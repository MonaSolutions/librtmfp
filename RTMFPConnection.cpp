#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "Mona/Crypto.h"
#include "AMFReader.h"
#include "ParametersWriter.h"
#include "Mona/MapParameters.h"
#include "Mona/DNS.h"

#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(): _step(0),_pInvoker(NULL),_pThread(NULL),_tag(16),_pubKey(0x80),_nonce(0x8B),_timeReceived(0),_farId(0),_writerId(0),_bytesReceived(0),
		_pEncoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
		_pDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)) {
	onError = [this](const Exception& ex) {
		onSocketError(ex);
	};
	onPacket = [this](PoolBuffer& pBuffer,const SocketAddress& address) {
		_step++;
		_bytesReceived += pBuffer.size();

		// Decode the RTMFP data
		Exception ex;
		if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
			ex.set(Exception::PROTOCOL,"Invalid RTMFP packet");
			onSocketError(ex);
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
			onSocketError(ex);
	};
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
	onSocketConnected();

	sendNextHandshake(ex);
	return !ex;
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
		flush(ex, 0x0B, writer.size(), false);
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
	writer.write16(0); // unknown for now
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

	// Write connect command	
	AMFWriter writer(_pInvoker->poolBuffers);
	writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size
	BinaryWriter& packet(writer.packet);
	
	packet.write8(0x80); // flags

	packet.write7BitLongValue(0x02); // idFlow
	packet.write7BitLongValue(0x01); // stage
	packet.write7BitLongValue(0x01); // deltaNAck
	
	packet.write8(5).write(EXPAND("\x00\x54\x43\x04\x00")); // signature of NetConnection
	packet.write8(0);

	packet.write8(AMF::INVOCATION); // Invocation
	packet.write32(0); // time
	
	writer.amf0 = true;
	writer.writeString(EXPAND("connect"));
	writer.writeNumber(1.0);
	writer.writeNull();

	writer.beginObject();
	//TODO: writer.writeStringProperty("app",)
	writer.writeStringProperty("flashVer", EXPAND("WIN 19,0,0,185"));
	//TODO: writer.writeStringProperty("swfUrl")
	writer.writeStringProperty("tcUrl", _url);
	//TODO: writer.writeStringProperty("fpad")
	writer.writeBooleanProperty("fpad", false);
	writer.writeNumberProperty("capabilities",235);
	writer.writeNumberProperty("audioCodecs",3575);
	writer.writeNumberProperty("videoCodecs",252);
	writer.writeNumberProperty("videoFunction",1);
	writer.writeNumberProperty("videoCodecs",252);
	//TODO: writer.writeStringProperty("pageUrl")
	writer.writeNumberProperty("objectEncoding",3);
	writer.endObject();

	sendMessage(ex, 0x8D, 0x10, writer, true);
}

void RTMFPConnection::receive(Exception& ex, BinaryReader& reader) {

	// Variables for request (0x10 and 0x11)
	UInt8 flags;
	UInt64 stage=0;
	UInt64 deltaNAck=0;

	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF;
	bool answer = false;

	// Can have nested queries
	while(type!=0xFF) {

		UInt16 size = reader.read16();	

		PacketReader message(reader.current(),size);

		switch(type) {
			/*case 0x0c :
				fail("failed on client side");
				break;

			case 0x4c :
				/// Session death!
				_failed = true; // to avoid the fail signal!!
				kill();
				return;

			/// KeepAlive
			case 0x01 :
				if(!peer.connected)
					fail("Timeout connection client");
				else
					writeMessage(0x41,0);
			case 0x41 :
				_timesKeepalive=0;
				break;

			case 0x5e : {
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
				_writerId = message.read7BitLongValue();
				UInt64 bufferSize = message.read7BitLongValue();

				if(bufferSize==0) {
					// In fact here, we should send a 0x18 message (with id flow),
					// but it can create a loop... We prefer the following behavior
					ex.set(Exception::PROTOCOL, "Negative acknowledgment");
					return;
				}

				//UInt64 stageAckPrec = _stageAck;
				UInt64 stageReaden = message.read7BitLongValue();
				//UInt64 stage = _stageAck+1;

				/*if(stageReaden>_stage) {
					ERROR("Acknowledgment received ",stageReaden," superior than the current sending stage ",_stage," on writer ",id);
					_stageAck = _stage;
				} else if(stageReaden<=_stageAck) {
					// already acked
					if(packet.available()==0)
						DEBUG("Acknowledgment ",stageReaden," obsolete on writer ",id);
				} else
					_stageAck = stageReaden;*/

				UInt64 maxStageRecv = stageReaden;
				UInt32 pos=message.position();

				while(message.available()>0)
					maxStageRecv += message.read7BitLongValue()+message.read7BitLongValue()+2;
				if(pos != message.position()) {
					// TRACE(stageReaden,"..x"Util::FormatHex(reader.current(),reader.available()));
					message.reset(pos);
				}

				//TODO: implement a message buffer and update this code with RTMFPWriter::acknowledgment()
				UInt64 lostCount = 0;
				UInt64 lostStage = 0;
				while(message.available()) {
					lostCount = message.read7BitLongValue()+1;
					lostStage = stageReaden+1;
					stageReaden = lostStage+lostCount+message.read7BitLongValue();
				}
					
				AMFWriter writer(_pInvoker->poolBuffers);
				writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size
				writer.packet.write7BitLongValue(_writerId).write7BitLongValue(_bytesReceived);
				writer.packet.write7BitLongValue(stageReaden); // Stage readen
				sendMessage(ex, 0x8D, 0x51, writer, true);
				
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
					break;

				map<UInt64,RTMFPFlow*>::const_iterator it = _flows.find(idFlow);
				pFlow = it==_flows.end() ? NULL : it->second;*/

				// Header part if present
				if(flags & MESSAGE_HEADER) {
					string signature;
					message.read(message.read8(),signature);

					/*if(!pFlow)
						pFlow = createFlow(idFlow,signature);*/

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
				
				/*if(!pFlow) {
					WARN("RTMFPFlow ",idFlow," unfound");
					if (_pFlowNull)
						((UInt64&)_pFlowNull->id) = idFlow;
					pFlow = _pFlowNull;
				}*/

			}	
			case 0x11 : {
				++stage;
				++deltaNAck;

				// has Header?
				if(type==0x11)
					flags = message.read8();

				// Process request
				//TODO: deal with fragments 
				//pFlow->receive(stage, deltaNAck, message, flags);
				UInt32 time(0);
				AMF::ContentType type = (AMF::ContentType)message.read8();
				switch(type) {
					case AMF::AUDIO:
					case AMF::VIDEO:
						time = message.read32();
					default:
						message.next(4);
						break;
				}
				switch(type) {
					case AMF::AUDIO:
						audioHandler(time,message/*, lostRate*/);
						break;
					case AMF::VIDEO:
						videoHandler(time,message/*, lostRate*/);
						break;

					case AMF::DATA_AMF3:
						message.next();
					case AMF::DATA: {
						/*AMFReader reader(message);
						dataHandler(reader, lostRate);*/
						break;
					}

					case AMF::EMPTY:
						break;

					case AMF::INVOCATION_AMF3:
						message.next();
					case AMF::INVOCATION: {
						string name;
						AMFReader reader(message);
						reader.readString(name);
						double number(0);
						reader.readNumber(number);
						//writer.setCallbackHandle(number);
						reader.readNull();
						invocationHandler(ex, name, reader);
						break;
					}

					case AMF::RAW:
						// rawHandler(packet.read16(), packet, writer);
						break;

					default:
						ex.set(Exception::PROTOCOL, "Unpacking type '",Format<UInt8>("%02x",(UInt8)type),"' unknown");
						return;
				}

				break;
			}
			default :
				ex.set(Exception::PROTOCOL,"RTMFPMessage type '", Format<UInt8>("%02x", type), "' unknown");
				return;
		}

		// Next
		reader.next(size); // TODO: maybe PacketReader was needed above to not move the cursor of "reader"
		type = reader.available()>0 ? reader.read8() : 0xFF;
	}
}

void RTMFPConnection::invocationHandler(Exception& ex, const string& name, AMFReader& message) {
	if(name == "_result") {
		MapParameters params;
		ParameterWriter paramWriter(params);
		message.read(AMFReader::OBJECT, paramWriter);

		string code,level,description;
		params.getString("level",level);
		if(level == "status") {
			params.getString("code",code);
			params.getString("description", description);
			onStatusEvent(code.c_str(),description.c_str());

			if (code == "NetConnection.Connect.Success") {
				AMFWriter writer(_pInvoker->poolBuffers);
				writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size
				BinaryWriter& packet(writer.packet);
	
				packet.write8(0x00); // flags

				packet.write7BitLongValue(0x02); // idFlow
				packet.write7BitLongValue(0x02); // stage
				packet.write7BitLongValue(0x01); // deltaNAck
	
				packet.write8(AMF::INVOCATION_AMF3);
				packet.write32(0); // TODO: what it is?
				packet.write8(0); // TODO: same question
				writer.writeString(EXPAND("setPeerInfo"));

				Exception ex;
				HostEntry host;
				DNS::ThisHost(ex, host);
				string buf;
				for(auto it : host.addresses()) {
					if (it.family() == IPAddress::IPv4)
						writer.writeString(String::Format(buf, it.toString(), ":", _pSocket->address().port()).c_str(), buf.size());
				}
				sendMessage(ex, 0x8D, 0x10, writer, true);
			}
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

void RTMFPConnection::sendMessage(Exception& ex, UInt8 marker, UInt8 idResponse, AMFWriter& writer, bool echotime) {
	BinaryWriter(writer.packet.data() + RTMFP_HEADER_SIZE, 3).write8(idResponse).write16(writer.packet.size() - RTMFP_HEADER_SIZE - 3);
	//(UInt32&)farId = 0;
	BinaryWriter newWriter(packet(),RTMFP_MAX_PACKET_SIZE);
	// Copy (TODO : make a list of messages)
	newWriter.write(writer.packet.data(), writer.packet.size());
	if(!ex)
		flush(ex, marker, writer.packet.size(), echotime);
}

void RTMFPConnection::flush(Exception& ex, UInt8 marker, UInt32 size,bool echoTime) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(ex, echoTime, marker);
}

void RTMFPConnection::flush(Exception& ex,bool echoTime,UInt8 marker) {
	//_pLastWriter=NULL;
	if(!_pSender)
		return;
	if (/*!died && */_pSender->available()) {
		BinaryWriter& packet(_pSender->packet);
	
		// After 30 sec, send packet without echo time
		/*if(peer.lastReceptionTime.isElapsed(30000))
			echoTime = false;*/

		/*if(echoTime)
			marker+=4;
		else*/ 
		if(!echoTime)
			packet.clip(2);

		BinaryWriter writer(packet.data()+6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		if (echoTime)
			writer.write16(_timeReceived); // TODO: +RTMFP::Time(peer.lastReceptionTime.elapsed()));

		_pSender->farId = _farId;
		//_pSender->address.set(peer.address);

		if(packet.size() > RTMFP_MAX_PACKET_SIZE) {
			ex.set(Exception::PROTOCOL, "Message exceeds max RTMFP packet size on connection (",packet.size(),">",RTMFP_MAX_PACKET_SIZE,")");
			return;
		}

		_pThread = _pSocket->send<RTMFPSender>(ex, _pSender,_pThread);
	}
	_pSender.reset();
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
