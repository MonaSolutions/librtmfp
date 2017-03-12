/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Mona/Util.h"
#include "FlowManager.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "FlashConnection.h"
#include "RTMFPFlow.h"
#include "RTMFPSender.h"

using namespace Mona;
using namespace std;

FlowManager::FlowManager(bool responder, Invoker& invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) :
	_pLastWriter(NULL), _firstRead(true), _invoker(invoker), _firstMedia(true), _timeStart(0), _codecInfosRead(false), _pOnStatusEvent(pOnStatusEvent), _pOnMedia(pOnMediaEvent), _pOnSocketError(pOnSocketError),
	status(RTMFP::STOPPED), _tag(16, '0'), _sessionId(0), _pListener(NULL), _mainFlowId(0), _responder(responder), _nextRTMFPWriterId(2), BandWriter(invoker) {

	_pMainStream.reset(new FlashConnection());
	_pMainStream->onStatus = [this](const string& code, const string& description, UInt16 streamId, UInt64 flowId, double cbHandler) {
		_pOnStatusEvent(code.c_str(), description.c_str());

		if (code == "NetConnection.Connect.Success")
			onNetConnectionSuccess();
		else if (code == "NetStream.Publish.Start")
			onPublished(streamId);
		else if (code == "NetConnection.Connect.Closed" || code == "NetConnection.Connect.Rejected" || code == "NetStream.Publish.BadName") {
			close(false);
			return false; // close the flow
		}
		return true;
	};
	_pMainStream->onPlay = [this](const string& streamName, UInt16 streamId, UInt64 flowId, double cbHandler) {
		return handlePlay(streamName, streamId, flowId, cbHandler);
	};
	onMedia = _pMainStream->onMedia = [this](const string& stream, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {

		if (!_codecInfosRead) {
			if (type == AMF::TYPE_VIDEO && RTMFP::IsVideoCodecInfos(packet.data(), packet.size())) {
				INFO("Video codec infos found, starting to read")
				_codecInfosRead = true;
			}
			else {
				if (type == AMF::TYPE_VIDEO)
					DEBUG("Video frame dropped to wait first key frame");
				return;
			}
		}

		if (_firstMedia) {
			_firstMedia = false;
			_timeStart = time; // to set to 0 the first packets
		}
		else if (time < _timeStart) {
			DEBUG("Packet ignored because it is older (", time, ") than start time (", _timeStart, ")")
			return;
		}

		if (_pOnMedia) // Synchronous read
			_pOnMedia(name().c_str(), stream.c_str(), time - _timeStart, STR packet.data(), packet.size(), type);
		else { // Asynchronous read
			{
				lock_guard<recursive_mutex> lock(_readMutex); // TODO: use the 'stream' parameter
				_mediaPackets.emplace_back(new RTMFPMediaPacket(packet, time - _timeStart, type));
			}
			handleDataAvailable(true);
		}
	};
	/*TODO: onError = [this](const Exception& ex) {
		string buffer;
		_pOnSocketError(String::Format(buffer, ex.error(), " on connection ", name()).c_str());
	};*/

	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes
}

FlowManager::~FlowManager() {

	// remove the flows
	for (auto& it : _flows)
		delete it.second;
	_flows.clear();

	// delete media packets
	lock_guard<recursive_mutex> lock(_readMutex);
	_mediaPackets.clear();

	if (_pMainStream) {
		_pMainStream->onStatus = nullptr;
		_pMainStream->onMedia = nullptr;
		_pMainStream->onPlay = nullptr;
	}
}

BinaryWriter& FlowManager::writeMessage(UInt8 type, UInt16 length, RTMFPWriter* pWriter) {

	_pLastWriter = pWriter;

	UInt16 size = length + 3; // for type and size

	if (size>availableToWrite()) {
		BandWriter::flush(false, 0x89); // send packet (and without time echo)

		if (size > availableToWrite()) {
			ERROR("RTMFPMessage truncated because exceeds maximum UDP packet size on connection");
			size = availableToWrite();
		}
	}

	if (!_pSender)
		_pSender.reset(new RTMFPSender(socket(_address.family()), _pEncoder));
	return _pSender->write8(type).write16(length);
}

shared_ptr<RTMFPWriter>& FlowManager::writer(UInt64 id, shared_ptr<RTMFPWriter>& pWriter) {
	auto it = _flowWriters.find(id);
	if (it != _flowWriters.end())
		pWriter = it->second;
	return pWriter;
}

UInt32 FlowManager::availableToWrite() { 
	return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->size() : RTMFP_HEADER_SIZE); 
}

const shared_ptr<RTMFPWriter>& FlowManager::createWriter(const Packet& signature, Mona::UInt64 flowId) {

	RTMFPWriter* pWriter = new RTMFPWriter(FlashWriter::OPENED, signature, *this, flowId);
	auto itWriter = _flowWriters.emplace(piecewise_construct, forward_as_tuple(_nextRTMFPWriterId), forward_as_tuple(pWriter)).first;
	(UInt64&)pWriter->id = _nextRTMFPWriterId++;
	pWriter->amf0 = false;

	DEBUG("New writer ", pWriter->id, " on connection ", name());
	return itWriter->second;
}

void FlowManager::clearWriters() {

	for (auto& it : _flowWriters)
		it.second->clear();
}

void FlowManager::flushWriters() {
	// Every 25s : ping
	if (_lastPing.isElapsed(25000) && status == RTMFP::CONNECTED) {
		writeMessage(0x01, 0);
		BandWriter::flush(false, 0x89);
		_lastPing.update();
	}

	// Every 5s : send back session close request
	if (status == RTMFP::NEAR_CLOSED && _lastClose.isElapsed(5000))
		sendCloseChunk(false);

	// Raise RTMFPWriter
	auto it = _flowWriters.begin();
	while (it != _flowWriters.end()) {
		shared_ptr<RTMFPWriter>& pWriter(it->second);
		Exception ex;
		if (!pWriter->manage(ex)) {
			INFO("Closing session ", name(), " : ", ex)
			close(false);
			break;
		}
		if (pWriter->consumed()) {
			handleWriterClosed(pWriter);
			DEBUG("Writer ", pWriter->id, " of Session ", name(), " consumed")
			_flowWriters.erase(it++);
			continue;
		}
		++it;
	}
}

void FlowManager::sendCloseChunk(bool abrupt) {
	writeMessage(abrupt ? 0x4C : 0x0C, 0);
	flush(false, 0x89);
	_lastClose.update();
}

void FlowManager::close(bool abrupt) {
	if (status == RTMFP::FAILED)
		return;

	// Send the close message
	if (status >= RTMFP::CONNECTED)
		sendCloseChunk(abrupt);

	// Close writers
	if (!_flowWriters.empty()) {
		for (auto& it : _flowWriters) {
			it.second->clear(); // Here no new sending must happen except "failSignal"
		}
		if (abrupt)
			_flowWriters.clear();
	}

	if (status <= RTMFP::CONNECTED) {
		_closeTime.update(); // To wait (90s or 19s) before deleting session
		status = abrupt ? RTMFP::FAILED : RTMFP::NEAR_CLOSED;
	}
	// switch from NEARCLOSED to FARCLOSE_LINGER
	else if (status == RTMFP::NEAR_CLOSED && abrupt) {
		_closeTime.update(); // To wait 19s before deleting session
		status = RTMFP::FAILED;
	}
}

bool FlowManager::readAsync(UInt8* buf, UInt32 size, int& nbRead) {
	if (nbRead != 0)
		ERROR("Parameter nbRead must equal zero in readAsync()")
	else if (status == RTMFP::CONNECTED) {

		bool available = false;
		lock_guard<recursive_mutex> lock(_readMutex);
		if (!_mediaPackets.empty()) {
			// First read => send header
			BinaryWriter writer(buf, size);
			if (_firstRead && size > 13) {
				writer.write(EXPAND("FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00"));
				_firstRead = false;
			}

			// While media packets are available and buffer is not full
			while (!_mediaPackets.empty() && (writer.size() < size - 15)) {

				// Read next packet
				std::shared_ptr<RTMFPMediaPacket>& packet = _mediaPackets.front();
				UInt32 bufferSize = packet->size() - packet->pos;
				UInt32 toRead = (bufferSize > (size - writer.size() - 15)) ? size - writer.size() - 15 : bufferSize;

				// header
				if (!packet->pos) {
					writer.write8(packet->type);
					writer.write24(packet->size()); // size on 3 bytes
					writer.write24(packet->time); // time on 3 bytes
					writer.write32(0); // unknown 4 bytes set to 0
				}
				writer.write(packet->data() + packet->pos, toRead); // payload

				// If packet too big : save position and exit, else write footer
				if (bufferSize > toRead) {
					packet->pos += toRead;
					break;
				}
				writer.write32(11 + packet->size()); // footer, size on 4 bytes
				_mediaPackets.pop_front();
			}
			// Finally update the nbRead & available
			nbRead = writer.size();
			available = !_mediaPackets.empty();
		}
		if (!available)
			handleDataAvailable(false); // change the available status
		return true;
	} 

	return false;
}

void FlowManager::receive(const Packet& packet) {

	// Variables for request (0x10 and 0x11)
	UInt8 flags;
	RTMFPFlow* pFlow = NULL;
	UInt64 stage = 0;
	UInt64 deltaNAck = 0;

	BinaryReader reader(packet.data(), packet.size());
	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF;
	bool answer = false;

	// If it is a p2p responder and it is the first message we call onConnection()
	if (_responder && status < RTMFP::CONNECTED)
		onConnection();

	// Can have nested queries
	while (type != 0xFF) {

		UInt16 size = reader.read16();

		BinaryReader message(reader.current(), size);

		switch (type) {
		case 0x0f: // P2P address destinator exchange
			handleP2PAddressExchange(message);
			break;
		case 0xcc:
			INFO("CC message received (unknown for now) from connection ", name())
#if defined(_DEBUG)
			DUMP("LIBRTMFP", reader.current(), size, "CC message :");
#endif
			break;
		case 0x0c:
			INFO("Session ", name(), " is closing");
			if (status == RTMFP::FAILED)
				sendCloseChunk(true); // send back 4C message anyway
			else
				close(true);
			break;
		case 0x4c : // Closing session abruptly
			INFO("Session ", name(), " is closing abruptly")
			close(true);
			return;
		case 0x01: // KeepAlive
			if (status == RTMFP::CONNECTED)
				writeMessage(0x41, 0);
			break;
		case 0x41:
			_lastKeepAlive.update();
			break;

		case 0x5e : {  // P2P closing flow (RTMFPFlow exception, only for p2p)
			UInt64 id = message.read7BitLongValue();
			shared_ptr<RTMFPWriter> pWriter;
			if (writer(id, pWriter))
				handleWriterException(pWriter);
			else
				WARN("RTMFPWriter ", id, " unfound for failed signal on session ", name());
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
			shared_ptr<RTMFPWriter> pWriter;
			if (writer(id, pWriter)) {
				Exception ex;
				if (!pWriter->acknowledgment(ex, message))
					WARN(ex, " on connection ", name())
			}
			else
				WARN("RTMFPWriter ", id, " unfound for acknowledgment on session ", name())
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

			if (status == RTMFP::FAILED)
				break;

			map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(idFlow);
			pFlow = it == _flows.end() ? NULL : it->second;

			// Header part if present
			if (flags & RTMFP::MESSAGE_OPTIONS) {
				string signature;
				message.read(message.read8(), signature);

				UInt64 idWriterRef = 0;
				if (message.read8()>0) {

					// Fullduplex header part
					if (message.read8() != 0x0A)
						WARN("Unknown fullduplex header part for the flow ", idFlow)
					else
						idWriterRef = message.read7BitLongValue(); // RTMFPWriter ID related to this flow

					// Useless header part 
					UInt8 length = message.read8();
					while (length>0 && message.available()) {
						WARN("Unknown message part on flow ", idFlow);
						message.next(length);
						length = message.read8();
					}
					if (length>0) {
						ERROR("Bad header message part, finished before scheduled");
						return;
					}
				}

				if (!pFlow)
					pFlow = createFlow(idFlow, signature, idWriterRef);
			}

			if (!pFlow) {
				WARN("RTMFPFlow ", idFlow, " unfound for connection ", name());
				break;
			}

		}
		case 0x11: {
			++stage;

			// has Header?
			if (type == 0x11)
				flags = message.read8();

			// Process request
			if (pFlow && (status != RTMFP::FAILED))
				pFlow->input(stage, flags, Packet(packet, message.current(), message.available()));

			break;
		}
		default:
			ERROR("RTMFPMessage type '", String::Format<UInt8>("%02x", type), "' unknown on connection ", name());
			return;
		}

		// Next
		reader.next(size);
		type = reader.available()>0 ? reader.read8() : 0xFF;

		// Commit RTMFPFlow (pFlow means 0x11 or 0x10 message)
		if (pFlow && (status != RTMFP::FAILED) && type != 0x11) {
			pFlow->commit();
			if (pFlow->consumed())
				removeFlow(pFlow);
			pFlow = NULL;
		}
	}
}

RTMFPFlow* FlowManager::createFlow(UInt64 id, const string& signature, UInt64 idWriterRef) {
	if (status == RTMFP::FAILED) {
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

	// Get flash stream process engine related by signature
	Exception ex;
	RTMFPFlow* pFlow = createSpecialFlow(ex, id, signature, idWriterRef);
	if (!pFlow && signature.size()>3 && signature.compare(0, 4, "\x00\x54\x43\x04", 4) == 0) { // NetStream (P2P or normal)
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		DEBUG("Creating new Flow (", id, ") for NetStream ", idSession)

		// Search in mainstream
		if (_pMainStream->getStream(idSession, pStream))
			pFlow = new RTMFPFlow(id, signature, pStream, *this, idWriterRef);
		else
			ex.set<Ex::Protocol>("RTMFPFlow ", id, " indicates a non-existent ", idSession, " NetStream on connection ", name());
	}
	if (!pFlow) {
		ERROR(ex)
		return NULL;
	}

	return _flows.emplace_hint(it, piecewise_construct, forward_as_tuple(id), forward_as_tuple(pFlow))->second;
}

void FlowManager::manage() {

	// Release the old flows
	auto itFlow = _flows.begin();
	while (itFlow != _flows.end()) {
		if (itFlow->second->consumed())
			removeFlow((itFlow++)->second);
		else
			++itFlow;
	}

	// Send the waiting messages
	flushWriters();
}

void FlowManager::removeFlow(RTMFPFlow* pFlow) {

	if (pFlow->id == _mainFlowId) {
		DEBUG("Main flow is closing, session ", name(), " will close")
		if (status != RTMFP::CONNECTED) {
			// without connection, nothing must be sent!
			clearWriters();
		}
		_mainFlowId = 0;
		close(false);
	}
	DEBUG("RTMFPFlow ", pFlow->id, " of session ", name(), " consumed")
	_flows.erase(pFlow->id);
	delete pFlow;
}

bool FlowManager::computeKeys(UInt32 farId) {
	// Compute Diffie-Hellman secret
	Exception ex;
	shared_ptr<Buffer> pSharedSecret(new Buffer(DiffieHellman::SIZE));
	UInt8 sizeSecret = diffieHellman().computeSecret(ex, _pHandshake->farKey.data(), _pHandshake->farKey.size(), pSharedSecret->data());
	if (ex) {
		WARN(ex)
		return false;
	}
	pSharedSecret->resize(sizeSecret);
	_sharedSecret = pSharedSecret;
	DUMP("LIBRTMFP", _sharedSecret.data(), _sharedSecret.size(), "Shared secret :")

	// Compute Keys
	UInt8 responseKey[Crypto::SHA256_SIZE];
	UInt8 requestKey[Crypto::SHA256_SIZE];
	Packet& initiatorNonce = (_responder)? _pHandshake->farNonce : _nonce;
	Packet& responderNonce = (_responder)? _nonce : _pHandshake->farNonce;
	RTMFP::ComputeAsymetricKeys(_sharedSecret, BIN initiatorNonce.data(), initiatorNonce.size(), BIN responderNonce.data(), responderNonce.size(), requestKey, responseKey);
	_pDecoder.reset(new RTMFPEngine(_responder ? requestKey : responseKey, RTMFPEngine::DECRYPT));
	_pEncoder.reset(new RTMFPEngine(_responder ? responseKey : requestKey, RTMFPEngine::ENCRYPT));

	// Save nonces just in case we are in a NetGroup connection
	_farNonce = _pHandshake->farNonce;

	TRACE(_responder ? "Initiator" : "Responder", " Nonce : ", String::Hex(BIN _farNonce.data(), _farNonce.size()))
	TRACE(_responder ? "Responder" : "Initiator", " Nonce : ", String::Hex(BIN _nonce.data(), _nonce.size()))

	_farId = farId; // important, save far ID
	return true;
}

void FlowManager::flush(bool echoTime, Mona::UInt8 marker) {

	// Reset last writer pointer before flush
	_pLastWriter = NULL;
	BandWriter::flush(echoTime, marker);
}

void FlowManager::receive(const SocketAddress& address, const Packet& packet) {
	if (Logs::GetLevel() >= 7)
		DUMP("LIBRTMFP", packet.data(), packet.size(), "Request from ", address)

	BinaryReader reader(packet.data(), packet.size());
	UInt8 marker = reader.read8();
	_timeReceived = reader.read16();
	_lastReceptionTime.update();

	if (address != _address) {
		DEBUG("Session ", name(), " has change its address from ", _address, " to ", address)
		_address.set(address);
	}

	// Handshake
	if (marker == 0x0B) {
		UInt8 type = reader.read8();
		UInt16 length = reader.read16();
		reader.shrink(length); // resize the buffer to ignore the padding bytes

		switch (type) {
		case 0x78:
			sendConnect(reader);
			return;
		case 0x79:
			WARN("Handshake 79 received, we have sent wrong cookie to far peer"); // TODO: handle?
			return;
		default:
			WARN("Unexpected Handshake marker : ", String::Format<UInt8>("%02X", type));
			return;
		}
	}

	// Connected message (normal or P2P)
	switch (marker | 0xF0) {
	case 0xFD:
		if (!_responder) {
			DEBUG("Responder is sending wrong marker, request ignored")
			return;
		}
	case 0xFE: {
		if ((marker | 0xF0) == 0xFE && _responder) {
			DEBUG("Initiator is sending wrong marker, request ignored")
			return;
		}
		UInt16 time = RTMFP::TimeNow();
		UInt16 timeEcho = reader.read16();
		setPing(time, timeEcho);
	}
	case 0xF9:
	case 0xFA:
		receive(Packet(packet, reader.current(), reader.available()));
		break;
	default:
		WARN("Unexpected RTMFP marker : ", String::Format<UInt8>("%02x", marker));
	}
}

void FlowManager::setPing(UInt16 time, UInt16 timeEcho) {
	if (timeEcho > time) {
		if (timeEcho - time < 30)
			time = 0;
		else
			time += 0xFFFF - timeEcho;
		timeEcho = 0;
	}
	UInt16 value = (time - timeEcho) * RTMFP_TIMESTAMP_SCALE;
	_ping = (value == 0 ? 1 : value);
}

void FlowManager::sendConnect(BinaryReader& reader) {

	if (status > RTMFP::HANDSHAKE38) {
		DEBUG("Handshake 78 ignored, the session is already in ", status, " state")
		return;
	}

	UInt32 farId = reader.read32(); // id session
	UInt32 nonceSize = (UInt32)reader.read7BitLongValue();
	if ((_pHandshake->isP2P && nonceSize != 73) || (!_pHandshake->isP2P && nonceSize < 0x8A)) {
		ERROR("Incorrect nonce size : ", nonceSize, " (expected ", _pHandshake->isP2P ? 73 : 138, " bytes)")
		return;
	}

	shared_ptr<Buffer> pFarNonce(new Buffer());
	reader.read(nonceSize, *pFarNonce);
	if (memcmp(pFarNonce->data(), "\x03\x1A\x00\x00\x02\x1E\x00", 7) != 0) {
		ERROR("Far nonce received is not well formated : ", String::Hex(pFarNonce->data(), nonceSize))
		return;
	}
	_pHandshake->farNonce.set(pFarNonce);

	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ERROR("Unexpected end of handshake 78 : ", endByte)
		return;
	}

	// If we are in client->server session far key = nonce+11
	if (!_pHandshake->isP2P)
		_pHandshake->farKey.set(_pHandshake->farNonce, _pHandshake->farNonce.data() + 11, nonceSize - 11);

	// Compute keys for encryption/decryption and start connect requests
	if (computeKeys(farId))
		onConnection();
}

bool FlowManager::onPeerHandshake70(const SocketAddress& address, const Packet& farKey, const string& cookie) {
	if (status > RTMFP::HANDSHAKE30) {
		DEBUG("Handshake 70 ignored for session ", name(), ", we are already in state ", status)
		return false;
	}

	// update address
	_address.set(address);
	return true;
};

const Packet& FlowManager::getNonce() {
	if (_nonce)
		return _nonce; // already computed
	
	shared<Buffer> pBuffer(new Buffer());
	BinaryWriter nonceWriter(*pBuffer);
	if (_responder) {
		
		nonceWriter.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
		nonceWriter.writeRandom(0x40); // nonce 64 random bytes
		return _nonce.set(pBuffer);
	}

	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	nonceWriter.writeRandom(64); // nonce 64 random bytes
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	return _nonce.set(pBuffer);
}
