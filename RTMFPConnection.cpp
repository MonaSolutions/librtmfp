#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "Mona/Crypto.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"

#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPMediaPacket::RTMFPMediaPacket(const PoolBuffers& poolBuffers,const UInt8* data,UInt32 size,UInt32 time, bool audio): pBuffer(size+15) {
	BinaryWriter writer(pBuffer.data(), size+15);

	writer.write8(audio? '\x08' : '\x09');
	// size on 3 bytes
	writer.write24(size);
	// time on 3 bytes
	writer.write24(time);
	// unknown 4 bytes set to 0
	writer.write32(0);
	// payload
	writer.write(data, size);
	// footer
	writer.write32(11+size);

}

const char RTMFPConnection::_FlvHeader[] = { 'F', 'L', 'V', 0x01,
  0x05,				/* 0x04 == audio, 0x01 == video */
  0x00, 0x00, 0x00, 0x09,
  0x00, 0x00, 0x00, 0x00
};

RTMFPConnection::RTMFPConnection(void (*onSocketError)(const char*), void (*onStatusEvent)(const char*,const char*), void (*onMediaEvent)(unsigned int, const char*, unsigned int,int)): 
		_handshakeStep(0),_pInvoker(NULL),_pThread(NULL),_tag(16),_pubKey(0x80),	_nonce(0x8B),_timeReceived(0),
		_farId(0),_bytesReceived(0),_nextRTMFPWriterId(0),_pLastWriter(NULL),_firstRead(true),_publishingStream(0),
		_pEncoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
		_pDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)),
		_onSocketError(onSocketError), _onStatusEvent(onStatusEvent), _onMedia(onMediaEvent) {
	onError = [this](const Exception& ex) {
		_onSocketError(ex.error().c_str());
	};
	onPacket = [this](PoolBuffer& pBuffer,const SocketAddress& address) {
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

		handleMessage(ex, pBuffer);
		if (ex)
			_onSocketError(ex.error().c_str());
	};
	onStatus = [this](const std::string& code, const std::string& description) {
		if (code == "NetConnection.Connect.Success") {
			connected = true;
			if (_isPublisher)
				sendCommand(CommandType::NETSTREAM_PUBLISH, _publication.c_str());
			else
				sendCommand(CommandType::NETSTREAM_PLAY, _publication.c_str());
		} else if (code == "NetStream.Publish.Start")
			published=true;

		_onStatusEvent(code.c_str(), description.c_str());
	};
	onStreamCreated = [this](UInt16 idStream) {
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(idStream, pStream);

		// Stream created, now we create the flow before sending another request
		string signature;
		signature.append("\x00\x54\x43\x04", 4);
		RTMFP::Write7BitValue(signature, idStream);
		UInt64 id = _flows.size();
		RTMFPFlow * pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *this);
		_waitingFlows[idStream] = pFlow;
		
		// Send createStream command and add command type to waiting commands
		if (_waitingCommands.empty()) {
			ERROR("created stream without command")
			return;
		}
		const StreamCommand command = _waitingCommands.back();
		switch(command.type) {
			case NETSTREAM_PLAY:
				pFlow->sendPlay(command.value);
				break;
			case NETSTREAM_PUBLISH:
				pFlow->sendPublish(command.value);
				_publishingStream = idStream;
				break;
		}		
		_waitingCommands.pop_back();
	};
	onMedia = [this](UInt32 time,PacketReader& packet,double lostRate,bool audio) {
		if (_onMedia) // Synchronous read
			_onMedia(time,(const char*)packet.current(),packet.available(), audio);
		else { // Asynchronous read
			lock_guard<recursive_mutex> lock(_readMutex);
			_mediaPackets.emplace_back(new RTMFPMediaPacket(poolBuffers(),packet.current(),packet.size(), time, audio));
		}
	};

	_pMainStream.reset(new FlashConnection());
	_pMainStream->OnStatus::subscribe(onStatus);
	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnMedia::subscribe(onMedia);
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

	// delete media packets
	lock_guard<recursive_mutex> lock(_readMutex);
	_mediaPackets.clear();
	
	// to remove OnStart and OnStop, and erase FlashWriters (before to erase flowWriters)
	if(_pMainStream) {
		_pMainStream->OnStatus::unsubscribe(onStatus);
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream->OnMedia::unsubscribe(onMedia);
		_pMainStream.reset();
	}
	
	// delete flowWriters
	_flowWriters.clear();

	if(_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
	}
}

bool RTMFPConnection::connect(Exception& ex, Invoker* invoker, const char* url, const char* host, const char* publication, bool isPublisher) {
	_pInvoker = invoker;
	_url = url;
	_publication = publication;
	_isPublisher = isPublisher;
	string tmpHost = host;
	if (!strrchr(host, ':'))
		tmpHost += ":1935"; // default port

	if (!_address.set(ex, tmpHost))
		return false;

	_pSocket.reset(new UDPSocket(invoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	INFO("Connecting to ", _address.host().toString(), ":", _address.port(), " (url : ",url,")...")
	if (!_pSocket->connect(ex, _address))
		return false;

	_pMainStream->setPort(_address.port()); // Record port for setPeerInfo request
	sendHandshake0();
	return !ex;
}

// TODO: see if we always need to manage a list of commands
void RTMFPConnection::sendCommand(CommandType command, const char* streamName) {
	if(!_pInvoker) {
		_onSocketError("Can't play stream because RTMFPConnection is not initialized");
		return;
	}
	if(!connected) {
		_onSocketError("Can't play stream because RTMFPConnection is not connected");
		return;
	}

	map<UInt64,RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it==_flows.end() ? NULL : it->second;
	if(pFlow != NULL) {
		_waitingCommands.emplace_front(command, streamName);
		pFlow->createStream();
	} else
		ERROR("Unable to find the main flow");
}

bool RTMFPConnection::read(UInt8* buf,UInt32 size, Mona::UInt32& nbRead) {
	nbRead = 0;

	lock_guard<recursive_mutex> lock(_readMutex);
	if (!_mediaPackets.empty()) {
		// First read => send header
		if (_firstRead && size > sizeof(_FlvHeader)) { // TODO: make a real context with a recorded position
			memcpy(buf, _FlvHeader, sizeof(_FlvHeader));
			_firstRead = false;
			size -= sizeof(_FlvHeader);
			nbRead += sizeof(_FlvHeader);
		}

		UInt32 bufferSize = 0;
		while (!_mediaPackets.empty() && (nbRead < size)) {

			std::shared_ptr<RTMFPMediaPacket> packet = _mediaPackets.front();
			bufferSize = packet->pBuffer.size();
			if (bufferSize > (size - nbRead))
				return false;

			memcpy(buf + nbRead, packet->pBuffer.data(), bufferSize);
			_mediaPackets.pop_front();
			nbRead += bufferSize;
		}
	}

	return true;
}

UInt32 RTMFPConnection::write(const UInt8* buf,UInt32 size) {
	if(!published) {
		ERROR("Can't write data because NetStream is not published")
		return 0;
	}
	std::shared_ptr<FlashStream> pStream;
	if(!_pMainStream || !_pMainStream->getStream(_publishingStream,pStream)) {
		ERROR("Unable to find publishing NetStream ", _publishingStream)
		return 0;
	}

	PacketReader packet(buf, size);
	if(packet.available()<14) {
		DEBUG("Packet too small")
		return 0;
	}
	
	string tmp;
	if (packet.read(3, tmp) == "FLV") // header
		packet.next(13);

	while(packet.available()) {
		if (packet.available() < 11) // smaller than flv header
			break;

		UInt8 type = packet.read8();
		UInt32 bodySize = packet.read24();
		UInt32 time = packet.read32();

		if(packet.available() < bodySize+4)
			break; // we will wait further data

		if (type == AMF::AUDIO || type == AMF::VIDEO)
			pStream->writeMedia(type, time, packet.current(), bodySize);
		packet.next(bodySize+15);
	}

	return size-packet.available();
}

void RTMFPConnection::handleMessage(Exception& ex,const Mona::PoolBuffer& pBuffer) {
	BinaryReader reader(pBuffer.data(), pBuffer->size());
	_timeReceived = reader.read16();
	UInt8 marker = reader.read8();
	reader.shrink(reader.read16()); // length

	switch(_handshakeStep) {
		case 0:
			ERROR("Handshake0 has not been send") // (should not happen)
			break;
		case 1:
		case 2:
			if(marker != 0x0B) {
				ex.set(Exception::PROTOCOL,"Unexpected handshake id : ", marker);
				return;
			}
			if(_handshakeStep==1)
				sendHandshake1(ex, reader);
			else
				sendConnect(ex, reader);
			break;
		default:
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
			break;
	}
}

void RTMFPConnection::sendHandshake0() {
	// (First packets are encoded with default key)
	BinaryWriter writer(packet(),RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size

	writer.write16((UInt16)_url.size()+1);
	writer.write8(0x0a); // type of handshake
	writer.write(_url);

	Util::Random(_tag.data(), 16); // random serie of 16 bytes
	writer.write(_tag);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x30).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size(), false);
	_handshakeStep=1;
}

void RTMFPConnection::sendHandshake1(Exception& ex, BinaryReader& reader) {

	// Read & check handshake0's response
	UInt8 type = reader.read8();
	if(type != 0x70) {
		ex.set(Exception::PROTOCOL,"Unexpected handshake type : ", type);
		return;
	}
	UInt16 size = reader.read16();

	UInt8 tagSize = reader.read8();
	if(tagSize != 16) {
		ex.set(Exception::PROTOCOL,"Unexpected tag size : ", tagSize);
		return;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if(String::ICompare(tagReceived.c_str(),(const char*)_tag.data(),16)!=0) {
		ex.set(Exception::PROTOCOL,"Unexpected tag received : ", tagReceived);
		return;
	}	

	UInt8 cookieSize = reader.read8();
	if(cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL,"Unexpected cookie size : ", cookieSize);
		return;
	}
	string cookie;
	reader.read(cookieSize, cookie);
	
	string certificat;
	reader.read(77, certificat);

	// Write handshake1
	BinaryWriter writer(packet(),RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size

	writer.write32(0x02000000); // id

	writer.write7BitLongValue(cookieSize);
	writer.write(cookie); // Resend cookie

	if (!_diffieHellman.initialize(ex))
		return;
	_diffieHellman.readPublicKey(ex, _pubKey.data());
	writer.write7BitLongValue(_pubKey.size()+4);
	writer.write7BitValue(_pubKey.size()+2);
	writer.write16(0x1D02); // unknown for now
	writer.write(_pubKey);

	Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	if(!ex) {
		flush(0x0B, writer.size(), false);
		_handshakeStep = 2;
	}
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
	_handshakeStep = 3;
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
	RTMFP::ComputeAsymetricKeys(_sharedSecret,(UInt8*)nonce.data(),(UInt16)nonce.size(),_nonce.data(),_nonce.size(),decryptKey,encryptKey);
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
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		INFO("Creating new Flow (",id,") for NetStream ", idSession)

		// First : search in waiting flows
		auto it = _waitingFlows.find(idSession);
		if(it!=_waitingFlows.end()) {
			pFlow = it->second;
			pFlow->setId(id);
			_waitingFlows.erase(it);
		}
		// 2nd : search in mainstream
		else if (_pMainStream->getStream(idSession,pStream))
			pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		else {
			ERROR("RTMFPFlow ",id," indicates a non-existent ",idSession," NetStream on connection ");
			return NULL;
		}
		
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
	if (_lastPing.isElapsed(25000) && connected) {
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
