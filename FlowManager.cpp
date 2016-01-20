#include "FlowManager.h"
#include "Mona/PacketWriter.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"
#include "FlashConnection.h"
#include "RTMFPFlow.h"

using namespace Mona;
using namespace std;

FlowManager::RTMFPMediaPacket::RTMFPMediaPacket(const PoolBuffers& poolBuffers, const UInt8* data, UInt32 size, UInt32 time, bool audio) : pBuffer(poolBuffers, size + 15) {
	BinaryWriter writer(pBuffer->data(), size + 15);

	writer.write8(audio ? '\x08' : '\x09');
	// size on 3 bytes
	writer.write24(size);
	// time on 3 bytes
	writer.write24(time);
	// unknown 4 bytes set to 0
	writer.write32(0);
	// payload
	writer.write(data, size);
	// footer
	writer.write32(11 + size);

}

const char FlowManager::_FlvHeader[] = { 'F', 'L', 'V', 0x01,
0x05,				/* 0x04 == audio, 0x01 == video */
0x00, 0x00, 0x00, 0x09,
0x00, 0x00, 0x00, 0x00
};

FlowManager::FlowManager(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) :
_nextRTMFPWriterId(0),_firstRead(true),_firstWrite(true),_pLastWriter(NULL),_pInvoker(invoker),_timeReceived(0),_handshakeStep(0),_firstMedia(true),_timeStart(0), _pListener(NULL),
	_died(false), _pOnStatusEvent(pOnStatusEvent), _pOnMedia(pOnMediaEvent), _pOnSocketError(pOnSocketError), _pThread(NULL), _farId(0), _pubKey(0x80), _nonce(0x8B),
	_pEncoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
	_pDecoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)),
	_pDefaultDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)) {
	onStatus = [this](const string& code, const string& description, FlashWriter& writer) {
		_pOnStatusEvent(code.c_str(), description.c_str());

		if (code == "NetConnection.Connect.Success") {
			Exception ex;
			if (!onConnect(ex))
				onError(ex);
		}
		else if (code == "NetStream.Publish.Start")
			onPublished(writer);
		else if (code == "NetConnection.Connect.Closed" || code == "NetStream.Publish.BadName")
			close();
	};
	onStreamCreated = [this](UInt16 idStream) {
		handleStreamCreated(idStream);
	};
	onPlay = [this](const string& streamName, FlashWriter& writer) {
		return handlePlay(streamName, writer);
	};
	onMedia = [this](const std::string& peerId, const std::string& stream, UInt32 time, PacketReader& packet, double lostRate, bool audio) {

		if(_firstMedia) {
			_firstMedia=false;
			_timeStart=time; // to set to 0 the first packets
		}

		if (_pOnMedia) // Synchronous read
			_pOnMedia(peerId.c_str(), stream.c_str(), time-_timeStart, (const char*)packet.current(), packet.available(), audio);
		else { // Asynchronous read
			lock_guard<recursive_mutex> lock(_readMutex);
			_mediaPackets[peerId].emplace_back(new RTMFPMediaPacket(poolBuffers(), packet.current(), packet.available(), time-_timeStart, audio));
		}
	};
	onError = [this](const Exception& ex) {
		_pOnSocketError(ex.error());
	};
	onPacket = [this](PoolBuffer& pBuffer, const SocketAddress& address) {
		// Decode the RTMFP data
		Exception ex;
		if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
			ex.set(Exception::PROTOCOL, "Invalid RTMFP packet");
			_pOnSocketError(ex.error());
			return;
		}

		BinaryReader reader(pBuffer.data(), pBuffer.size());
		UInt32 idStream = RTMFP::Unpack(reader);
		pBuffer->clip(reader.position());

		// Handshake or session decoder?
		RTMFPEngine* pDecoder = getDecoder(idStream, address);

		if(!pDecoder->process((UInt8*)pBuffer.data(),pBuffer.size())) {
			WARN("Bad RTMFP CRC sum computing (idstream: ", idStream, ")")
			return;
		} else
			handleMessage(ex, pBuffer, address);

		if (ex)
			_pOnSocketError(ex.error());
	};

	_pFlowNull.reset(new RTMFPFlow(0,String::Empty,_pMainStream,poolBuffers(), *this));

	_pMainStream.reset(new FlashConnection());
	_pMainStream->OnStatus::subscribe(onStatus);
	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnMedia::subscribe(onMedia);
	_pMainStream->OnPlay::subscribe(onPlay);
}

FlowManager::~FlowManager() {

	// Here no new sending must happen except "failSignal"
	for (auto& it : _flowWriters)
		it.second->clear();

	// delete waiting flows
	for (auto& it : _waitingFlows)
		delete it.second;
	_waitingFlows.clear();

	// delete flows
	for (auto& it : _flows)
		delete it.second;
	_flows.clear();

	// delete media packets
	lock_guard<recursive_mutex> lock(_readMutex);
	_mediaPackets.clear();

	// delete flowWriters
	_flowWriters.clear();

	_pFlowNull.reset();

	if (_pMainStream) {
		_pMainStream->OnStatus::unsubscribe(onStatus);
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream->OnMedia::unsubscribe(onMedia);
		_pMainStream->OnPlay::unsubscribe(onPlay);
		_pMainStream.reset();
	}
}

void FlowManager::close() {
	if (connected) {
		writeMessage(0x4C, 0); // Close message
		flush(false, 0x89);
		connected = false;
	}

	_died = true;
}

void FlowManager::handleMessage(Exception& ex, const Mona::PoolBuffer& pBuffer, const SocketAddress&  address) {
	_outAddress.set(address);

	BinaryReader reader(pBuffer.data(), pBuffer->size());

	_timeReceived = reader.read16();
	if (Logs::GetLevel() >= 7)
		DUMP("RTMFP", reader.current(), reader.available(), "Request from ", _outAddress.toString())

	UInt8 marker = reader.read8();
	reader.shrink(reader.read16()); // length

	// Handshake
	if (marker == 0x0B) {
		manageHandshake(ex, reader);
		return;
	}

	// Connected message (normal or P2P)
	switch (marker | 0xF0) {
	case 0xFD:
	case 0xFE: {
		UInt16 time = RTMFP::TimeNow();
		UInt16 timeEcho = reader.read16();
		/*if (timeEcho>time) {
		if (timeEcho - time<30)
		time = 0;
		else
		time += 0xFFFF - timeEcho;
		timeEcho = 0;
		}
		peer.setPing((time-timeEcho)*RTMFP_TIMESTAMP_SCALE);*/
	}
	case 0xF9:
	case 0xFA:
		receive(ex, reader);
		break;
	default:
		WARN("Unexpected RTMFP marker : ", Format<UInt8>("%02x", marker));
	}
}

bool FlowManager::readAsync(const string& peerId, UInt8* buf, UInt32 size, int& nbRead) {
	
	nbRead = 0;
	if (_died)
		return false; // to stop the parent loop

	lock_guard<recursive_mutex> lock(_readMutex);
	if (!_mediaPackets[peerId].empty()) {
		// First read => send header
		if (_firstRead && size > sizeof(_FlvHeader)) { // TODO: make a real context with a recorded position
			memcpy(buf, _FlvHeader, sizeof(_FlvHeader));
			_firstRead = false;
			size -= sizeof(_FlvHeader);
			nbRead += sizeof(_FlvHeader);
		}

		UInt32 bufferSize = 0;
		auto& queue = _mediaPackets[peerId];
		while (!queue.empty() && (nbRead < size)) {

			std::shared_ptr<RTMFPMediaPacket> packet = queue.front();
			bufferSize = packet->pBuffer.size();
			if (bufferSize >(size - nbRead))
				return false;

			memcpy(buf + nbRead, packet->pBuffer.data(), bufferSize);
			queue.pop_front();
			nbRead += bufferSize;
		}
	}

	return true;
}

void FlowManager::receive(Exception& ex, BinaryReader& reader) {

	// Variables for request (0x10 and 0x11)
	UInt8 flags;
	RTMFPFlow* pFlow = NULL;
	UInt64 stage = 0;
	UInt64 deltaNAck = 0;

	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF;
	bool answer = false;

	// TODO: find a better place to determine that the connection is ON in P2P responder mode
	if(!connected)
		connected = true;

	// Can have nested queries
	while (type != 0xFF) {

		UInt16 size = reader.read16();

		PacketReader message(reader.current(), size);

		switch (type) {
		case 0x0f: // P2P address destinator exchange
			handleP2PAddressExchange(ex, message);
			break;
		case 0xcc:
			INFO("CC message received (unknown for now)")
#if defined(_DEBUG)
			Logs::Dump(reader.current(), size);
#endif
			break;
		case 0x0c:
			ex.set(Exception::PROTOCOL, "Failed on server side");
			writeMessage(0x0C, 0);
			break;
		case 0x4c : // P2P closing session (only for p2p I think)
			INFO("P2P Session at ", _outAddress.toString(), " is closed")
			connected = false;
			close();
			return;
		case 0x01: // KeepAlive
			/*if(!peer.connected)
			fail("Timeout connection client");
			else*/
			writeMessage(0x41, 0);
			break;
		case 0x41:
			_lastKeepAlive.update();
			break;

		case 0x5e : {  // P2P closing flow (RTMFPFlow exception, only for p2p)
			UInt64 id = message.read7BitLongValue();

			RTMFPWriter* pRTMFPWriter = writer(id);
			if(pRTMFPWriter)
				pRTMFPWriter->fail("Writer terminated on connection");
			else
				WARN("RTMFPWriter ", id, " unfound for failed signal on connection");
			break;
		}
			/*case 0x18 :
			/// This response is sent when we answer with a Acknowledgment negative
			// It contains the id flow
			// I don't unsertand the usefulness...
			//pFlow = &flow(message.read8());
			//stage = pFlow->stageSnd();
			// For the moment, we considerate it like a exception
			fail("ack negative from server"); // send fail message immediatly
			break;*/

		case 0x51: {
			/// Acknowledgment
			UInt64 id = message.read7BitLongValue();
			RTMFPWriter* pRTMFPWriter = writer(id);
			if (pRTMFPWriter)
				pRTMFPWriter->acknowledgment(message);
			else
				WARN("RTMFPWriter ", id, " unfound for acknowledgment on connection ");
			break;
		}
		/// Request
		// 0x10 normal request
		// 0x11 special request, in repeat case (following stage request)
		case 0x10: {
			flags = message.read8();
			UInt64 idFlow = message.read7BitLongValue();
			stage = message.read7BitLongValue() - 1;
			deltaNAck = message.read7BitLongValue() - 1;

			/*if (_failed)
			break;*/

			map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(idFlow);
			pFlow = it == _flows.end() ? NULL : it->second;

			// Header part if present
			if (flags & MESSAGE_HEADER) {
				string signature;
				message.read(message.read8(), signature);

				if (!pFlow)
					pFlow = createFlow(idFlow, signature);

				if (message.read8()>0) {

					// Fullduplex header part
					if (message.read8() != 0x0A)
						WARN("Unknown fullduplex header part for the flow ", idFlow)
					else
						message.read7BitLongValue(); // Fullduplex useless here! Because we are creating a new RTMFPFlow!

													 // Useless header part 
					UInt8 length = message.read8();
					while (length>0 && message.available()) {
						WARN("Unknown message part on flow ", idFlow);
						message.next(length);
						length = message.read8();
					}
					if (length>0) {
						ex.set(Exception::PROTOCOL, "Bad header message part, finished before scheduled");
						return;
					}
				}
			}

			if (!pFlow) {
				WARN("RTMFPFlow ", idFlow, " unfound");
				if (_pFlowNull)
					((UInt64&)_pFlowNull->id) = idFlow;
				pFlow = _pFlowNull.get();
			}

		}
		case 0x11: {
			++stage;
			++deltaNAck;

			// has Header?
			if (type == 0x11)
				flags = message.read8();

			// Process request
			if (pFlow && !_died)
				pFlow->receive(stage, deltaNAck, message, flags);

			break;
		}
		default:
			ex.set(Exception::PROTOCOL, "RTMFPMessage type '", Format<UInt8>("%02x", type), "' unknown");
			return;
		}

		// Next
		reader.next(size); // TODO: maybe PacketReader was needed above to not move the cursor of "reader"
		type = reader.available()>0 ? reader.read8() : 0xFF;

		// Commit RTMFPFlow (pFlow means 0x11 or 0x10 message)
		if (pFlow && type != 0x11) {
			pFlow->commit();
			if (pFlow->consumed()) {
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
			pFlow = NULL;
		}
	}
}

RTMFPWriter* FlowManager::writer(UInt64 id) {
	auto it = _flowWriters.find(id);
	if (it == _flowWriters.end())
		return NULL;
	return it->second.get();
}

RTMFPFlow* FlowManager::createFlow(UInt64 id, const string& signature) {
	if (_died) {
		ERROR("Connection is died, no more RTMFPFlow creation possible");
		return NULL;
	}
	if (!_pMainStream)
		return NULL; // has failed! use FlowNull rather

	map<UInt64, RTMFPFlow*>::iterator it = _flows.lower_bound(id);
	if (it != _flows.end() && it->first == id) {
		WARN("RTMFPFlow ", id, " has already been created on connection")
			return it->second;
	}

	RTMFPFlow* pFlow;

	// get flash stream process engine related by signature
	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		INFO("Creating new Flow (", id, ") for NetConnection")
		pFlow = new RTMFPFlow(id, signature, poolBuffers(), *this, _pMainStream);
	}
	else if (signature.size()>6 && signature.compare(0, 6, "\x00\x54\x43\x04\xFA\x89", 6) == 0) { // Direct P2P Connection
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 6, signature.length() - 6).read7BitValue());
		INFO("Creating new Flow (2) for P2P NetStream ", idSession)
		_pMainStream->addStream(idSession, pStream);
		pFlow = new RTMFPFlow(2, signature, pStream, poolBuffers(), *this);
	}
	else if (signature.size()>3 && signature.compare(0, 4, "\x00\x54\x43\x04", 4) == 0) { // NetStream
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		INFO("Creating new Flow (", id, ") for NetStream ", idSession)

		// First : search in waiting flows
		auto it = _waitingFlows.find(idSession);
		if (it != _waitingFlows.end()) {
			pFlow = it->second;
			pFlow->setId(id);
			_waitingFlows.erase(it);
		}
		// 2nd : search in mainstream
		else if (_pMainStream->getStream(idSession, pStream))
			pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		else {
			ERROR("RTMFPFlow ", id, " indicates a non-existent ", idSession, " NetStream on connection ");
			return NULL;
		}

	}
	else {
		string tmp;
		ERROR("Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow")
		return NULL;
	}
	/*else if(signature.size()>2 && signature.compare(0,3,"\x00\x47\x43",3)==0)  // NetGroup
	pFlow = new RTMFPFlow(id, signature, _pInvoker->poolBuffers, *this, _pMainStream);*/

	return _flows.emplace_hint(it, piecewise_construct, forward_as_tuple(id), forward_as_tuple(pFlow))->second;
}

void FlowManager::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	while (++_nextRTMFPWriterId == 0 || !_flowWriters.emplace(_nextRTMFPWriterId, pWriter).second);
	(UInt64&)pWriter->id = _nextRTMFPWriterId;
	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // newWriter will be associated to the NetConnection flow (first in _flow lists)
	if (!pWriter->signature.empty())
		DEBUG("New writer ", pWriter->id, " on connection ");
}

const Mona::PoolBuffers& FlowManager::poolBuffers() { 
	return _pInvoker->poolBuffers; 
}

BinaryWriter& FlowManager::writeMessage(UInt8 type, UInt16 length, RTMFPWriter* pWriter) {

	// No sending formated message for a failed session!
	/*if (_failed)
	return DataWriter::Null.packet;*/

	_pLastWriter = pWriter;

	UInt16 size = length + 3; // for type and size

	if (size>availableToWrite()) {
		flush(false, 0x89); // send packet (and without time echo)

		if (size > availableToWrite()) {
			ERROR("RTMFPMessage truncated because exceeds maximum UDP packet size on connection");
			size = availableToWrite();
		}
		_pLastWriter = NULL;
	}

	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	return _pSender->packet.write8(type).write16(length);
}

UInt8* FlowManager::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE, false);
	return _pSender->packet.data();
}

void FlowManager::flush(UInt8 marker, UInt32 size) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(false, marker);
}

void FlowManager::flush(bool echoTime, UInt8 marker) {
	_pLastWriter = NULL;
	if (!_pSender)
		return;
	if (!_died && _pSender->available()) {
		BinaryWriter& packet(_pSender->packet);

		// After 30 sec, send packet without echo time
		/*if(peer.lastReceptionTime.isElapsed(30000))
		echoTime = false;*/

		if (echoTime)
			marker += 4;
		else
			packet.clip(2);

		BinaryWriter writer(packet.data() + 6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		if (echoTime)
			writer.write16(_timeReceived); // TODO: +RTMFP::Time(peer.lastReceptionTime.elapsed()));

		_pSender->farId = _farId;
		_pSender->address.set(_outAddress); // set the right address for sending

		if (packet.size() > RTMFP_MAX_PACKET_SIZE)
			ERROR(Exception::PROTOCOL, "Message exceeds max RTMFP packet size on connection (", packet.size(), ">", RTMFP_MAX_PACKET_SIZE, ")");

		// executed just in debug mode, or in dump mode
		if (Logs::GetLevel() >= 7)
			DUMP("RTMFP", packet.data()+6, packet.size()-6, "Response to ", _outAddress.toString(), " (farId : ", _farId, ")")

		Exception ex;
		_pThread = socket().send<RTMFPSender>(ex, _pSender, _pThread);

		if (ex)
			ERROR("RTMFP flush, ", ex.error());
	}
	_pSender.reset();
}

bool FlowManager::computeKeys(Exception& ex, const string& farPubKey, const string& initiatorNonce, const UInt8* responderNonce, UInt32 responderNonceSize, shared_ptr<RTMFPEngine>& pDecoder, shared_ptr<RTMFPEngine>& pEncoder, bool isResponder) {
	if (!_diffieHellman.initialized()) {
		ex.set(Exception::CRYPTO, "Diffiehellman object must be initialized before computing");
		return false;
	}

	// Compute Diffie-Hellman secret
	_diffieHellman.computeSecret(ex, (UInt8*)farPubKey.data(), farPubKey.size(), _sharedSecret);
	if (ex)
		return false;

	DEBUG("Shared secret : ", Util::FormatHex(_sharedSecret.data(), _sharedSecret.size(), LOG_BUFFER))

	PacketWriter packet(_pInvoker->poolBuffers);
	if (packet.size() > 0) {
		ex.set(Exception::CRYPTO, "RTMFPCookieComputing already executed");
		return false;
	}

	// Compute Keys
	UInt8 responseKey[Crypto::HMAC::SIZE];
	UInt8 requestKey[Crypto::HMAC::SIZE];
	RTMFP::ComputeAsymetricKeys(_sharedSecret, (UInt8*)initiatorNonce.data(), (UInt16)initiatorNonce.size(), responderNonce, responderNonceSize, requestKey, responseKey);
	pDecoder.reset(new RTMFPEngine(isResponder? requestKey : responseKey, RTMFPEngine::DECRYPT));
	pEncoder.reset(new RTMFPEngine(isResponder? responseKey : requestKey, RTMFPEngine::ENCRYPT));

	return true;
}

void FlowManager::sendHandshake0(HandshakeType type, const string& epd, const string& tag) {
	// (First packets are encoded with default key)
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	if(type == P2P_HANDSHAKE) {
		writer.write7BitLongValue(epd.size() + 2);
		writer.write7BitLongValue(epd.size() + 1);
	} else
		writer.write16((UInt16)(epd.size() + 1));
	writer.write8(type); // handshake type
	writer.write(epd);

	writer.write(tag);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x30).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size());
	_handshakeStep = 1; // TODO : see if we need to differentiate handshake steps for each type (basic and p2p)
}

void FlowManager::flushWriters() {
	// Every 25s : ping
	if (_lastPing.isElapsed(25000) && connected) {
		_outAddress.set(_hostAddress); // To avoid sending to the wrong address
		writeMessage(0x01, 0); // TODO: send only if needed
		flush(false, 0x89);
		_lastPing.update();
	}

	// Raise RTMFPWriter
	auto it = _flowWriters.begin();
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

shared_ptr<RTMFPWriter> FlowManager::changeWriter(RTMFPWriter& writer) {
	auto it = _flowWriters.find(writer.id);
	if (it == _flowWriters.end()) {
		ERROR("RTMFPWriter ", writer.id, " change impossible on connection")
		return shared_ptr<RTMFPWriter>(&writer);
	}
	shared_ptr<RTMFPWriter> pWriter(it->second);
	it->second.reset(&writer);
	return pWriter;
}
