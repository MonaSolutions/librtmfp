#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "Mona/Crypto.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"

#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(void (__cdecl * onSocketError)(const char*), void (__cdecl * onStatusEvent)(const char*,const char*)): 
		_step(0),_pInvoker(NULL),_pThread(NULL),_tag(16),_pubKey(0x80),	_nonce(0x8B),_timeReceived(0),
		_farId(0),_bytesReceived(0),_nextRTMFPWriterId(0), _pLastWriter(NULL),
		_pEncoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
		_pDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)),
		_onSocketError(onSocketError), _onStatusEvent(onStatusEvent) {
	onError = [this](const Exception& ex) {
		_onSocketError(ex.error().c_str());
	};
	onPacket = [this](PoolBuffer& pBuffer,const SocketAddress& address) {
		_step++;
		_bytesReceived += pBuffer.size();

		// Decode the RTMFP data
		Exception ex;
		if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
			ex.set(Exception::PROTOCOL,"Invalid RTMFP packet");
			_onSocketError(ex.error().c_str());
			return;
		}
		
		BinaryReader reader(pBuffer.data(), pBuffer.size());
		UInt32 idStream = RTMFP::Unpack(reader);
		pBuffer->clip(reader.position());		
		_pDecoder->process((UInt8*)pBuffer.data(),pBuffer.size()); // TODO: make a Task

		if (_step == 1)
			sendNextHandshake(ex, pBuffer.data(), pBuffer.size());
		else
			handleMessage(ex, pBuffer);
		
		if (ex)
			_onSocketError(ex.error().c_str());
	};
	onStatus = [this](const std::string& code, const std::string& description) {
		if (code == "NetConnection.Connect.Success")
			connected=true;
		_onStatusEvent(code.c_str(), description.c_str());
	};
	onStreamCreated = [this](UInt16 idStream) {
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(idStream, pStream);

		// Stream created, now we create the flow before sending another request
		string signature;
		signature.append("\x00\x54\x43\x04", 4);
		Util::Write7BitValue(signature, idStream);
		UInt64 id = _flows.size();
		RTMFPFlow * pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *this);
		_waitingFlows[idStream] = pFlow;
		
		// TODO : manage other requests
		pFlow->sendPlay(_streamPlayed);
		_streamPlayed = "";
	};

	_pMainStream.reset(new FlashConnection());
	_pMainStream->OnStatus::subscribe(onStatus);
	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	/*_pMainStream->OnStop::subscribe(onStreamStop);*/
}

RTMFPConnection::~RTMFPConnection() {
	// Here no new sending must happen except "failSignal"
	for (auto& it : _flowWriters)
		it.second->clear();

	// delete waiting flows
	for(auto& it : _waitingFlows)
		delete it.second;
	_waitingFlows.clear();

	// delete flows
	for(auto& it : _flows)
		delete it.second;
	_flows.clear();
	
	// to remove OnStart and OnStop, and erase FlashWriters (before to erase flowWriters)
	if(_pMainStream) {
		_pMainStream->OnStatus::unsubscribe(onStatus);
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream.reset();
	}
	
	// delete flowWriters
	_flowWriters.clear();

	if(_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
	}
}

bool RTMFPConnection::connect(Exception& ex, Invoker* invoker, const char* host, int port, const char* url) {
	_pInvoker = invoker;
	_url = url;
	if (!_address.set(ex, host, port))
		return false;

	_pSocket.reset(new UDPSocket(invoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	if (!_pSocket->connect(ex, _address))
		return false;

	_pMainStream->setPort(port);
	sendNextHandshake(ex);
	return !ex;
}

void RTMFPConnection::playStream(Exception& ex, const char* streamName) {
	if(!_pInvoker) {
		_onSocketError("Can't play stream because RTMFPConnection is not initialized");
		return;
	}
	if(!connected) {
		_onSocketError("Can't play stream because RTMFPConnection is not connected");
		return;
	}

	_streamPlayed = streamName;


	map<UInt64,RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it==_flows.end() ? NULL : it->second;
	if(pFlow != NULL)
		pFlow->createStream(streamName);
}

void RTMFPConnection::sendNextHandshake(Exception& ex, const UInt8* data, UInt32 size) {
	BinaryWriter writer(packet(),RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size
	UInt8 idResponse = 0;

	if(_step==0)
		idResponse = sendHandshake0(writer);
	else {		
		BinaryReader reader(data, size);
		UInt16 time = reader.read16();
		UInt8 id = reader.read8();
		if(id != 0x0B) {
			ex.set(Exception::PROTOCOL,"Unexpected handshake id : ", id);
			return;
		}
		reader.shrink(reader.read16()); // length
		idResponse = sendHandshake1(ex, writer, reader);
	}

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(idResponse).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	//(UInt32&)farId = 0;
	if(!ex)
		flush(0x0B, writer.size(), false);
}

void RTMFPConnection::handleMessage(Exception& ex,const Mona::PoolBuffer& pBuffer) {
	UInt8 idResponse = 0;

	BinaryReader reader(pBuffer.data(), pBuffer->size());
	_timeReceived = reader.read16();
	UInt8 marker = reader.read8();
	reader.shrink(reader.read16()); // length

	if (_step==2) { // end of handshake
		if(marker != 0x0B) {
			ex.set(Exception::PROTOCOL,"Unexpected handshake id : ", marker);
			return;
		}
		sendConnect(ex, reader);
	} else {
		// with time echo
		if(marker == 0x4E) {
			UInt16 time = RTMFP::TimeNow();
			UInt16 timeEcho = reader.read16();
			if(timeEcho>time) {
				if(timeEcho-time<30)
					time=0;
				else
					time += 0xFFFF-timeEcho;
				timeEcho = 0;
			}
			//peer.setPing((time-timeEcho)*RTMFP_TIMESTAMP_SCALE);
		}
		else if(marker != 0x4A)
			WARN("RTMFPPacket marker unknown : ", Format<UInt8>("%02x",marker));

		receive(ex, reader);
	}
}

UInt8 RTMFPConnection::sendHandshake0(BinaryWriter& writer) {
	// (First packets are encoded with default key)

	writer.write16(_url.size()+1);
	writer.write8(0x0a); // type of handshake
	writer.write(_url);

	Util::Random(_tag.data(), 16); // random serie of 16 bytes
	writer.write(_tag);

	return 0x30;
}

UInt8 RTMFPConnection::sendHandshake1(Exception& ex, BinaryWriter& writer, BinaryReader& reader) {

	// Read & check handshake0's response
	UInt8 type = reader.read8();
	if(type != 0x70) {
		ex.set(Exception::PROTOCOL,"Unexpected handshake type : ", type);
		return 0;
	}
	UInt16 size = reader.read16();

	UInt8 tagSize = reader.read8();
	if(tagSize != 16) {
		ex.set(Exception::PROTOCOL,"Unexpected tag size : ", tagSize);
		return 0;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if(String::ICompare(tagReceived.c_str(),(const char*)_tag.data(),16)!=0) {
		ex.set(Exception::PROTOCOL,"Unexpected tag received : ", tagReceived);
		return 0;
	}	

	UInt8 cookieSize = reader.read8();
	if(cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL,"Unexpected cookie size : ", cookieSize);
		return 0;
	}
	string cookie;
	reader.read(cookieSize, cookie);
	
	string certificat;
	reader.read(77, certificat);

	// Write handshake1
	writer.write32(0x02000000); // id

	writer.write7BitLongValue(cookieSize);
	writer.write(cookie); // Resend cookie

	if (!_diffieHellman.initialize(ex))
		return 0;
	_diffieHellman.readPublicKey(ex, _pubKey.data());
	writer.write7BitLongValue(_pubKey.size()+4);
	writer.write7BitValue(_pubKey.size()+2);
	writer.write16(0x1D02); // unknown for now
	writer.write(_pubKey);

	Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);

	return 0x38;
}

void RTMFPConnection::sendConnect(Exception& ex, BinaryReader& reader) {

	// Read & check handshake1's response (cookie + session's creation)
	UInt8 type = reader.read8();
	if(type != 0x78) {
		ex.set(Exception::PROTOCOL,"Unexpected handshake type : ", type);
		return;
	}
	reader.read16(); // whole size

	_farId = reader.read32(); // id session?
	UInt32 size = (UInt32)reader.read7BitLongValue()-11;
	string nonce;
	reader.read(size+11, nonce);
	if(String::ICompare(nonce,"\x03\x1A\x00\x00\x02\x1E\x00",7)!=0) { // TODO: I think this is not fixed
		ex.set(Exception::PROTOCOL,"Nonce not expected : ", nonce);
		return;
	}

	string farPubKey = nonce.substr(11,size);
	UInt8 endByte = reader.read8();
	if(endByte!=0x58) {
		ex.set(Exception::PROTOCOL,"Unexpected end of handshake 2 : ", endByte);
		return;
	}

	// Compute keys for encryption/decryption
	if(!computeKeys(ex, farPubKey, nonce))
		return;

	string signature("\x00\x54\x43\x04\x00", 5);
	RTMFPFlow* pFlow = createFlow(2, signature);
	if(!pFlow)
		return;
	
	pFlow->sendConnect(_url, _pSocket->address().port());
}

void RTMFPConnection::receive(Exception& ex, BinaryReader& reader) {

	// Variables for request (0x10 and 0x11)
	UInt8 flags;
	RTMFPFlow* pFlow=NULL;
	UInt64 stage=0;
	UInt64 deltaNAck=0;

	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF;
	bool answer = false;

	// Can have nested queries
	while(type!=0xFF) {

		UInt16 size = reader.read16();	

		PacketReader message(reader.current(),size);

		switch(type) {
			case 0x0c :
				ex.set(Exception::PROTOCOL, "Failed on server side");
				writeMessage(0x0C, 0);
				break;

			/*case 0x4c :
				/// Session death!
				_failed = true; // to avoid the fail signal!!
				kill();
				return;*/

			/// KeepAlive
			case 0x01:
				/*if(!peer.connected)
					fail("Timeout connection client");
				else*/
				writeMessage(0x41,0);
				break;
			case 0x41 :
				_lastKeepAlive.update();
				break;

			/*case 0x5e : {
				// RTMFPFlow exception!
				UInt64 id = message.read7BitLongValue();
				
				RTMFPWriter* pRTMFPWriter = writer(id);
				if(pRTMFPWriter)
					pRTMFPWriter->fail("Writer rejected on session ",name());
				else
					WARN("RTMFPWriter ", id, " unfound for failed signal on session ", name());
				break;

			}
			case 0x18 :
				/// This response is sent when we answer with a Acknowledgment negative
				// It contains the id flow
				// I don't unsertand the usefulness...
				//pFlow = &flow(message.read8());
				//stage = pFlow->stageSnd();
				// For the moment, we considerate it like a exception
				fail("ack negative from server"); // send fail message immediatly
				break;*/

			case 0x51 : {
				/// Acknowledgment
				UInt64 id = message.read7BitLongValue();
				RTMFPWriter* pRTMFPWriter = writer(id);
				if(pRTMFPWriter)
					pRTMFPWriter->acknowledgment(message);
				else
					WARN("RTMFPWriter ",id," unfound for acknowledgment on connection ");
				break;
			}
			/// Request
			// 0x10 normal request
			// 0x11 special request, in repeat case (following stage request)
			case 0x10 : {
				flags = message.read8();
				UInt64 idFlow = message.read7BitLongValue();
				stage = message.read7BitLongValue()-1;
				deltaNAck = message.read7BitLongValue()-1;
				
				/*if (_failed)
					break;*/

				map<UInt64,RTMFPFlow*>::const_iterator it = _flows.find(idFlow);
				pFlow = it==_flows.end() ? NULL : it->second;

				// Header part if present
				if(flags & MESSAGE_HEADER) {
					string signature;
					message.read(message.read8(),signature);

					if(!pFlow)
						pFlow = createFlow(idFlow, signature);

					if(message.read8()>0) {

						// Fullduplex header part
						if(message.read8()!=0x0A)
							WARN("Unknown fullduplex header part for the flow ",idFlow)
						else 
							message.read7BitLongValue(); // Fullduplex useless here! Because we are creating a new RTMFPFlow!

						// Useless header part 
						UInt8 length=message.read8();
						while(length>0 && message.available()) {
							WARN("Unknown message part on flow ",idFlow);
							message.next(length);
							length=message.read8();
						}
						if(length>0) {
							ex.set(Exception::PROTOCOL, "Bad header message part, finished before scheduled");
							return;
						}
					}
				}
				
				if(!pFlow) {
					WARN("RTMFPFlow ",idFlow," unfound");
					/*if (_pFlowNull)
						((UInt64&)_pFlowNull->id) = idFlow;
					pFlow = _pFlowNull;*/
				}

			}	
			case 0x11 : {
				++stage;
				++deltaNAck;

				// has Header?
				if(type==0x11)
					flags = message.read8();

				// Process request
				if (pFlow/* && !_failed*/)
					pFlow->receive(stage, deltaNAck, message, flags);

				break;
			}
			default :
				ex.set(Exception::PROTOCOL,"RTMFPMessage type '", Format<UInt8>("%02x", type), "' unknown");
				return;
		}

		// Next
		reader.next(size); // TODO: maybe PacketReader was needed above to not move the cursor of "reader"
		type = reader.available()>0 ? reader.read8() : 0xFF;

		// Commit RTMFPFlow (pFlow means 0x11 or 0x10 message)
		if(pFlow && type!= 0x11) {
			pFlow->commit();
			if(pFlow->consumed()) {
				if (pFlow->critical()) {
					if (!connected) {
						// without connection, nothing must be sent!
						for (auto& it : _flowWriters)
							it.second->clear();
					}
					// TODO: commented because it replace other events (NetConnection.Connect.Rejected)
					// fail(); // If connection fails, log is already displayed, and so fail the whole session!
				}
				_flows.erase(pFlow->id);
				delete pFlow;
			}
			pFlow=NULL;
		}
	}
}

void RTMFPConnection::audioHandler(UInt32 time, PacketReader& message) {

}

void RTMFPConnection::videoHandler(UInt32 time, PacketReader& message) {

}

void RTMFPConnection::close() {
	if (_pSocket)
		_pSocket->close();
}

UInt8* RTMFPConnection::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE,false);
	return _pSender->packet.data();
}

void RTMFPConnection::writeType(UInt8 marker, UInt8 type, bool echoTime) {

	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	_pSender->packet.write8(type).write16(0);
	flush(marker, _pSender->packet.size(), echoTime);
}

void RTMFPConnection::sendMessage(UInt8 marker, UInt8 idResponse, AMFWriter& writer, bool echoTime) {
	BinaryWriter(writer.packet.data() + RTMFP_HEADER_SIZE, 3).write8(idResponse).write16(writer.packet.size() - RTMFP_HEADER_SIZE - 3);
	//(UInt32&)farId = 0;
	BinaryWriter newWriter(packet(),RTMFP_MAX_PACKET_SIZE);
	// Copy (TODO : make a list of messages)
	newWriter.write(writer.packet.data(), writer.packet.size());
	
	flush(marker, writer.packet.size(), echoTime);
}

void RTMFPConnection::flush(UInt8 marker, UInt32 size,bool echoTime) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(echoTime, marker);
}

void RTMFPConnection::flush(bool echoTime,UInt8 marker) {
	_pLastWriter=NULL;
	if(!_pSender)
		return;
	if (/*!died && */_pSender->available()) {
		BinaryWriter& packet(_pSender->packet);
	
		// After 30 sec, send packet without echo time
		/*if(peer.lastReceptionTime.isElapsed(30000))
			echoTime = false;*/

		if(echoTime)
			marker+=4;
		else
		if(!echoTime)
			packet.clip(2);

		BinaryWriter writer(packet.data()+6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		if (echoTime)
			writer.write16(_timeReceived); // TODO: +RTMFP::Time(peer.lastReceptionTime.elapsed()));

		_pSender->farId = _farId;
		//_pSender->address.set(peer.address);

		if(packet.size() > RTMFP_MAX_PACKET_SIZE)
			ERROR(Exception::PROTOCOL, "Message exceeds max RTMFP packet size on connection (",packet.size(),">",RTMFP_MAX_PACKET_SIZE,")");

		DumpResponse(packet.data() + 6, packet.size() - 6);

		Exception ex;
		_pThread = _pSocket->send<RTMFPSender>(ex, _pSender,_pThread);
		if (ex)
			ERROR("RTMFP flush, ", ex.error());
	}
	_pSender.reset();
}

void RTMFPConnection::DumpResponse(const UInt8* data, UInt32 size) {
	// executed just in debug mode, or in dump mode
	if (Logs::GetLevel() < 7)
		DUMP("RTMFP",data, size, "Response to ", _address.toString())
}

bool RTMFPConnection::computeKeys(Exception& ex, const string& farPubKey, const string& nonce) {
	if(!_diffieHellman.initialized()) {
		ex.set(Exception::CRYPTO, "Diffiehellman object must be initialized before computing");
		return false;
	}

	// Compute Diffie-Hellman secret
	_diffieHellman.computeSecret(ex,(UInt8*)farPubKey.data(),farPubKey.size(),_sharedSecret);
	if (ex)
		return false;

	PacketWriter packet(_pInvoker->poolBuffers);
	if (packet.size() > 0) {
		ex.set(Exception::CRYPTO, "RTMFPCookieComputing already executed");
		return false;
	}

	// Compute Keys
	UInt8 encryptKey[Crypto::HMAC::SIZE];
	UInt8 decryptKey[Crypto::HMAC::SIZE];
	RTMFP::ComputeAsymetricKeys(_sharedSecret,(UInt8*)nonce.data(),nonce.size(),_nonce.data(),_nonce.size(),decryptKey,encryptKey);
	_pDecoder.reset(new RTMFPEngine(decryptKey,RTMFPEngine::DECRYPT));
	_pEncoder.reset(new RTMFPEngine(encryptKey,RTMFPEngine::ENCRYPT));

	return true;
}

BinaryWriter& RTMFPConnection::writeMessage(UInt8 type, UInt16 length, RTMFPWriter* pWriter) {

	// No sending formated message for a failed session!
	/*if (_failed)
		return DataWriter::Null.packet;*/

	_pLastWriter=pWriter;

	UInt16 size = length + 3; // for type and size

	if(size>availableToWrite()) {
		flush(false); // send packet (and without time echo)
		
		if(size > availableToWrite()) {
			ERROR("RTMFPMessage truncated because exceeds maximum UDP packet size on connection");
			size = availableToWrite();
		}
		_pLastWriter=NULL;
	}

	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	return _pSender->packet.write8(type).write16(length);
}

RTMFPWriter* RTMFPConnection::writer(UInt64 id) {
	auto it = _flowWriters.find(id);
	if(it==_flowWriters.end())
		return NULL;
	return it->second.get();
}

const PoolBuffers&		RTMFPConnection::poolBuffers() { 
	return _pInvoker->poolBuffers; 
}

RTMFPFlow* RTMFPConnection::createFlow(UInt64 id,const string& signature) {
	/*if(died) {
		ERROR("Session ", name(), " is died, no more RTMFPFlow creation possible");
		return NULL;
	}*/
	if (!_pMainStream)
		return NULL; // has failed! use FlowNull rather

	map<UInt64,RTMFPFlow*>::iterator it = _flows.lower_bound(id);
	if(it!=_flows.end() && it->first==id) {
		WARN("RTMFPFlow ",id," has already been created on connection")
		return it->second;
	}

	RTMFPFlow* pFlow;

	// get flash stream process engine related by signature
	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		INFO("Creating new Flow (",id,") for NetConnection")
		pFlow = new RTMFPFlow(id, signature, _pInvoker->poolBuffers, *this, _pMainStream);
	} else if (signature.size()>3 && signature.compare(0, 4, "\x00\x54\x43\x04", 4) == 0) { // NetStream
		/*shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		if (!_pMainStream->getStream(idSession,pStream)) {
			ERROR("RTMFPFlow ",id," indicates a non-existent ",idSession," NetStream on connection")
			return NULL;
		}
		pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *this);*/
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		INFO("Creating new Flow (",id,") for NetStream ", idSession)
		auto it = _waitingFlows.find(idSession);
		if(it==_waitingFlows.end()) {
			ERROR("Creating flow impossibe because NetStream ",idSession," does not exists in connection")
			return NULL;
		}
		pFlow = it->second;
		pFlow->setId(id);
		_waitingFlows.erase(it);
	} 
	else {
		ERROR("Unhandled signature type : ", signature, " , cannot create RTMFPFlow")
		return NULL;
	}
		/*else if(signature.size()>2 && signature.compare(0,3,"\x00\x47\x43",3)==0)  // NetGroup
		pFlow = new RTMFPFlow(id, signature, _pInvoker->poolBuffers, *this, _pMainStream);*/

	return _flows.emplace_hint(it, piecewise_construct, forward_as_tuple(id), forward_as_tuple(pFlow))->second;
}

void RTMFPConnection::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	while (++_nextRTMFPWriterId == 0 || !_flowWriters.emplace(_nextRTMFPWriterId, pWriter).second);
	(UInt64&)pWriter->id = _nextRTMFPWriterId;
	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // newWriter will be associated to the NetConnection flow (first in _flow lists)
	if (!pWriter->signature.empty())
		DEBUG("New writer ", pWriter->id, " on connection ");
}

void RTMFPConnection::manage() {
	if (!_pMainStream)
		return;

	// Every 25s : ping
	if (_lastPing.isElapsed(25000)) {
		writeMessage(0x01, 0); // TODO: send only if needed
		flush(false);
		_lastPing.update();
	}

	// Raise RTMFPWriter
	auto it=_flowWriters.begin();
	while (it != _flowWriters.end()) {
		shared_ptr<RTMFPWriter>& pWriter(it->second);
		Exception ex;
		pWriter->manage(ex);
		if (ex) {
			if (pWriter->critical) {
				// TODO: fail(ex.error());
				break;
			}
			continue;
		}
		if (pWriter->consumed()) {
			_flowWriters.erase(it++);
			continue;
		}
		++it;
	}
}
