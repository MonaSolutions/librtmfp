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

#include "Base/Util.h"
#include "FlowManager.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "FlashConnection.h"
#include "RTMFPFlow.h"
#include "RTMFPSender.h"

using namespace Base;
using namespace std;

FlowManager::FlowManager(bool responder, Invoker& invoker, OnStatusEvent pOnStatusEvent) : _invoker(invoker), _pOnStatusEvent(pOnStatusEvent), 
	status(RTMFP::STOPPED), _tag(16, '\0'), _sessionId(0), _pListener(NULL), _mainFlowId(0), _initiatorTime(-1), _responder(responder), _nextRTMFPWriterId(2), _farId(0), _threadSend(0), _ping(0), _waitClose(false),
	_rttvar(0), _rto(Net::RTO_INIT) {

	_pMainStream.set();
	_pMainStream->onStatus = [this](const string& code, const string& description, UInt16 streamId, UInt64 flowId, double cbHandler) {
		DEBUG("onStatus (stream: ", streamId, ") : ", code, " - ", description)
		if (_pOnStatusEvent)
			_pOnStatusEvent(code.c_str(), description.c_str());

		if (code == "NetConnection.Connect.Success")
			onNetConnectionSuccess();
		else if (code == "NetStream.Publish.Start")
			onPublished(streamId);
		else if (code == "NetConnection.Connect.Closed" || code == "NetConnection.Connect.Rejected" || code == "NetStream.Publish.BadName") {
			close(false, RTMFP::SESSION_CLOSED);
			return false; // close the flow
		}
		return true;
	};
	_pMainStream->onPlay = [this](const string& streamName, UInt16 streamId, UInt64 flowId, double cbHandler) {
		return handlePlay(streamName, streamId, flowId, cbHandler);
	};

	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes
}

FlowManager::~FlowManager() {

	// remove the flows
	for (auto& it : _flows)
		delete it.second;
	_flows.clear();

	if (_pMainStream) {
		_pMainStream->onStatus = nullptr;
		_pMainStream->onMedia = nullptr;
		_pMainStream->onPlay = nullptr;
	}
}

shared<RTMFPWriter>& FlowManager::writer(UInt64 id, shared<RTMFPWriter>& pWriter) {
	auto it = _flowWriters.find(id);
	if (it != _flowWriters.end())
		pWriter = it->second;
	return pWriter;
}

const shared<RTMFPWriter>& FlowManager::createWriter(const Packet& signature, Base::UInt64 flowId) {

	RTMFPWriter* pWriter = new RTMFPWriter(0x89 + _responder,_nextRTMFPWriterId, flowId, signature, *this);
	auto itWriter = _flowWriters.emplace(piecewise_construct, forward_as_tuple(_nextRTMFPWriterId++), forward_as_tuple(pWriter)).first;
	pWriter->amf0 = false;

	DEBUG("New writer ", pWriter->id, " on connection ", name());
	return itWriter->second;
}

void FlowManager::clearWriters() {

	for (auto& it : _flowWriters)
		it.second->clear();
}

void FlowManager::flushWriters() {

	// Raise RTMFPWriter
	auto it = _flowWriters.begin();
	while (it != _flowWriters.end()) {
		shared<RTMFPWriter>& pWriter(it->second);
		pWriter->flush();
		if (pWriter->consumed()) {
			DEBUG("Writer ", pWriter->id, " of Session ", name(), " consumed")
			_flowWriters.erase(it++);
			continue;
		}
		++it;
	}
}

void FlowManager::sendCloseChunk(bool abrupt) {
	send(make_shared<RTMFPCmdSender>(abrupt ? 0x4C : 0x0C, 0x89 + _responder));
	_lastClose.update();
}

void FlowManager::close(bool abrupt, RTMFP::CLOSE_REASON reason) {
	if (status == RTMFP::FAILED)
		return;

	// Close handshake if exists
	if (_pHandshake)
		removeHandshake(_pHandshake);

	// Send the close message
	if (status >= RTMFP::CONNECTED) {
		// Trick to know the close reason
		if (reason != RTMFP::SESSION_CLOSED) {
			BinaryWriter(write(0x4d, 1)).write8(reason);
			RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(_pBuffer, _farId, _address)), _address);
		}
		sendCloseChunk(abrupt);
	}

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
	// switch to FARCLOSE_LINGER
	else if (status != RTMFP::FAILED && abrupt) {
		_closeTime.update(); // To wait 19s before deleting session
		status = RTMFP::FAILED;
	}
}

void FlowManager::receive(const Packet& packet) {
	if (status == RTMFP::FAILED)
		return;

	// Variables for request (0x10 and 0x11)
	UInt64 flowId;
	UInt8 flags;
	RTMFPFlow* pFlow = NULL;
	UInt64 stage = 0;

	BinaryReader reader(packet.data(), packet.size());
	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF, nextType(0xFF);

	// If it is a p2p responder and it is the first message we call onConnection()
	if (_responder && status < RTMFP::CONNECTED)
		onConnection();

	// Can have nested queries
	while (type != 0xFF) {

		UInt16 size = reader.read16();

		BinaryReader message(reader.current(), size);
		reader.next(size);
		nextType = reader.available()>0 ? reader.read8() : 0xFF;

		switch (type) {
		case 0x0f: // P2P address destinator exchange
			handleP2PAddressExchange(message);
			break;
		case 0xcc:
			INFO("CC message received (unknown for now) from connection ", name())
			break;
		case 0x0c:
			INFO("Session ", name(), " is closing (", _address, ")")
			if (status == RTMFP::FAILED)
				sendCloseChunk(true); // send back 4C message anyway
			else
				close(true, RTMFP::SESSION_CLOSED);
			break;
		case 0x4c : // Closing session abruptly
			if (status != RTMFP::FAILED) {
				INFO("Session ", name(), " is closing abruptly (", _address, ")")
				close(true, RTMFP::SESSION_CLOSED);
			}
			return;
		case 0x4d : // Custom closing session message
			if (status != RTMFP::FAILED) {
				UInt8	reason = message.read8();
				INFO("Session ", name(), " closure reason : ", reason, " - ", RTMFP::Reason2String(reason))
			}
			break;
		case 0x01: // KeepAlive
			if (status == RTMFP::CONNECTED)
				send(make_shared<RTMFPCmdSender>(0x41, (status >= RTMFP::CONNECTED) ? (0x89 + _responder) : 0x0B));
			break;
		case 0x41:
			_lastKeepAlive.update();
			break;

		case 0x5e : {
			// RTMFPWriter exception!
			UInt64 id = message.read7Bit<UInt64>();
			shared<RTMFPWriter> pWriter;
			if (writer(id, pWriter))
				handleWriterException(pWriter);
			else
				WARN("RTMFPWriter ", id, " unfound for failed signal on session ", name());
			break;
		}
		case 0x18:
			/// This response is sent when we answer with a Acknowledgment negative
			// It contains the id flow (message.read8())
			// I don't unsertand the usefulness...
			// For the moment, we considerate it like an exception
			WARN("Ack negative from ", name()); // send fail message immediatly
			close(true, RTMFP::SESSION_CLOSED);
			break;
		case 0x50:
		case 0x51: {
			/// Acknowledgment
			UInt64 id = message.read7Bit<UInt64>();
			UInt64 bufferSize = message.read7Bit<UInt64>();
			shared<RTMFPWriter> pWriter;
			if (writer(id, pWriter)) {
				if (bufferSize) {
					UInt64 ackStage(message.read7Bit<UInt64>());
					UInt32 lostCount(0);
					if (message.available()) {
						++lostCount;
						if (type == 0x50) {
							UInt8 i;
							do {
								UInt8 bits(message.read8());
								for (i = 0; i < 8; ++i) {
									if (bits & 1)
										break;
									++lostCount;
									bits >>= 1;
								}

							} while (i == 8 && message.available());
						}
						else
							lostCount += UInt32(message.read7Bit<UInt64>());
					}
					pWriter->acquit(ackStage, lostCount);
				}
				else if (!pWriter->closed()) {
					// no more place to write, reliability broken
					WARN("RTMFPWriter ", id, " can't deliver its data, buffer full on session ", name());
					close(false, RTMFP::SESSION_CLOSED);
					return;
				} // else ack on a closed writer, ignored
			}
			break;
		}
		/// Request
		// 0x10 normal request
		// 0x11 special request, in repeat case (following stage request)
		case 0x10: {
			flags = message.read8();
			flowId = message.read7Bit<UInt64>();
			stage = message.read7Bit<UInt64>() - 1;
			message.read7Bit<UInt64>(); //deltaNAck

			map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(flowId);
			pFlow = it == _flows.end() ? NULL : it->second;

			// Header part if present
			if (flags & RTMFP::MESSAGE_OPTIONS) {
				string signature;
				message.read(message.read8(), signature);

				UInt64 idWriterRef = 0;
				if (message.read8()>0) {

					// Fullduplex header part
					if (message.read8() != 0x0A)
						WARN("Unknown fullduplex header part for the flow ", flowId)
					else
						idWriterRef = message.read7Bit<UInt64>(); // RTMFPWriter ID related to this flow

					// Useless header part 
					UInt8 length = message.read8();
					while (length>0 && message.available()) {
						WARN("Unknown message part on flow ", flowId);
						message.next(length);
						length = message.read8();
					}
					if (length>0) {
						ERROR("Bad header message part, finished before scheduled");
						return;
					}
				}

				if (!pFlow) {
					if (status == RTMFP::NEAR_CLOSED)
						closeFlow(flowId); // do not accept flow creation in the near_closed status
					else
						pFlow = createFlow(flowId, signature, idWriterRef);
				}
			}

			if (!pFlow) {
				WARN("RTMFPFlow ", flowId, " unfound for connection ", name());
				break;
			}

		}
		case 0x11: {
			++stage;

			// has Header?
			if (type == 0x11)
				flags = message.read8();

			// Process request
			if (pFlow && (status != RTMFP::FAILED)) {
				pFlow->input(stage, flags, Packet(packet, message.current(), message.available()), nextType==0xFF);

				// Read congestion management (reliable mode)
				if (pFlow->fragmentation > Net::GetRecvBufferSize()) {
					if (status < RTMFP::NEAR_CLOSED) {
						WARN("Session ", name(), " input is congested (", pFlow->fragmentation, " > ", Net::GetRecvBufferSize(),")")
						close(false, RTMFP::INPUT_CONGESTED);
					}
					return;
				}
			}
			break;
		}
		default:
			ERROR("RTMFPMessage type '", String::Format<UInt8>("%02x", type), "' unknown on connection ", name());
			return;
		}

		// Next
		type = nextType;

		// Commit RTMFPFlow (pFlow means 0x11 or 0x10 message)
		if (stage && (status != RTMFP::FAILED) && type != 0x11) {
			if (pFlow) {
				vector<UInt64> losts;
				stage = pFlow->buildAck(losts, size = 0);
				size += Binary::Get7BitSize<UInt64>(pFlow->id) + Binary::Get7BitSize<UInt64>(0xFF7Fu) + Binary::Get7BitSize<UInt64>(stage);
				BinaryWriter writer(write(0x51, size));
				writer.write7Bit<UInt64>(pFlow->id).write7Bit<UInt64>(0xFF7F).write7Bit<UInt64>(stage);
				for (UInt64 lost : losts)
					writer.write7Bit<UInt64>(lost);
				if (pFlow->consumed())
					removeFlow(pFlow);
				pFlow = NULL;
			}
			else // commit everything (flow unknown)
				BinaryWriter(write(0x51, 1 + Binary::Get7BitSize<UInt64>(flowId) + Binary::Get7BitSize<UInt64>(stage))).write7Bit<UInt64>(flowId).write7Bit<UInt64>(0).write7Bit<UInt64>(stage);
			if (_pBuffer) {
				TRACE("Sending ack ", stage)
				RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(_pBuffer, _farId, _address)), _address);
				_pBuffer.reset();
			}
			stage = 0;
		}
	}
}

void FlowManager::send(shared<RTMFPSender>&& pSender) {
	if (!_pSendSession)
		return;

	// Write congestion management
	if (!_waitClose && (status < RTMFP::NEAR_CLOSED) && _pSendSession->congested) {
		WARN("Session ", name(), " output is congested, closing...")
		_waitClose = true;
		return;
	}

	// continue even on _killing to repeat writers messages to flush it (reliable)
	pSender->address = _address;
	pSender->pSession = _pSendSession;
	_invoker.threadPool.queue(_threadSend, move(pSender));
}

Buffer& FlowManager::write(UInt8 type, UInt16 size) {
	if (!_pSendSession) 
		BinaryWriter(RTMFP::InitBuffer(_pBuffer, (status >= RTMFP::CONNECTED) ? (0x89 + _responder) : 0x0B)).write8(type).write16(size);
	else
		BinaryWriter(RTMFP::InitBuffer(_pBuffer, _pSendSession->initiatorTime, (status >= RTMFP::CONNECTED) ? (0x89 + _responder) : 0x0B)).write8(type).write16(size);
	return *_pBuffer;
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
		shared<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7Bit<UInt32>());
		DEBUG("Creating new Flow (", id, ") for NetStream ", idSession)

		// Search in mainstream
		if (_pMainStream->getStream(idSession, pStream))
			pFlow = new RTMFPFlow(id, pStream, *this, idWriterRef);
		else
			ex.set<Ex::Protocol>("RTMFPFlow ", id, " indicates a non-existent ", idSession, " NetStream on connection ", name());
	}
	if (!pFlow) {
		ERROR(ex)
		return NULL;
	}

	return _flows.emplace_hint(it, piecewise_construct, forward_as_tuple(id), forward_as_tuple(pFlow))->second;
}

bool FlowManager::manage() {

	if (status != RTMFP::FAILED) {

		// Release the old flows
		auto itFlow = _flows.begin();
		while (itFlow != _flows.end()) {
			if (itFlow->second->consumed())
				removeFlow((itFlow++)->second);
			else
				++itFlow;
		}

		// Close the session if congestion
		if (_waitClose) {
			close(false, RTMFP::OUTPUT_CONGESTED);
			_waitClose = false;
		}
		// After 6 mn without any message we can considerate that the session has failed
		else if (_recvTime.isElapsed(360000)) {
			WARN(name(), " failed, reception timeout");
			close(true, RTMFP::KEEPALIVE_ATTEMPT);
			return false;
		}

		// Every 25s : ping
		if (_lastPing.isElapsed(25000) && status == RTMFP::CONNECTED) {
			send(make_shared<RTMFPCmdSender>(0x01, 0x89 + _responder));
			_lastPing.update();
		}

		// Every 5s : send back session close request
		if (status == RTMFP::NEAR_CLOSED && _lastClose.isElapsed(5000))
			sendCloseChunk(false);
	}

	// Send the waiting messages
	flushWriters();
	return true;
}

void FlowManager::removeFlow(RTMFPFlow* pFlow) {

	if (pFlow->id == _mainFlowId) {
		DEBUG("Main flow is closing, session ", name(), " will close")
		if (status != RTMFP::CONNECTED)
			clearWriters(); // without connection, nothing must be sent!
		_mainFlowId = 0;
		if (status <= RTMFP::CONNECTED)
			close(false, RTMFP::SESSION_CLOSED);
	}
	DEBUG("RTMFPFlow ", pFlow->id, " of session ", name(), " consumed")
	_flows.erase(pFlow->id);
	delete pFlow;
}

bool FlowManager::computeKeys(UInt32 farId) {
	// Compute Diffie-Hellman secret
	Exception ex;
	shared<Buffer> pSharedSecret(SET, DiffieHellman::SIZE);
	UInt8 sizeSecret = diffieHellman().computeSecret(ex, _pHandshake->farKey->data(), _pHandshake->farKey->size(), pSharedSecret->data());
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
	shared<Buffer>& initiatorNonce = (_responder)? _pHandshake->farNonce : _nonce;
	shared<Buffer>& responderNonce = (_responder)? _nonce : _pHandshake->farNonce;
	RTMFP::ComputeAsymetricKeys(_sharedSecret, BIN initiatorNonce->data(), initiatorNonce->size(), BIN responderNonce->data(), responderNonce->size(), requestKey, responseKey);
	_pDecoder.set(_responder ? requestKey : responseKey);
	_pEncoder.set(_responder ? responseKey : requestKey);
	_pSendSession.set(farId, _pEncoder, socket(_address.family()), _pSendSession ? _pSendSession->initiatorTime.load() : 0); // important, initialize the sender session

	// Save nonces just in case we are in a NetGroup connection
	_farNonce = _pHandshake->farNonce;

	TRACE(_responder ? "Initiator" : "Responder", " Nonce : ", String::Hex(BIN _farNonce->data(), _farNonce->size()))
	TRACE(_responder ? "Responder" : "Initiator", " Nonce : ", String::Hex(BIN _nonce->data(), _nonce->size()))

	_farId = farId; // important, save far ID
	return true;
}

void FlowManager::receive(const SocketAddress& address, const Packet& packet) {
	BinaryReader reader(packet.data(), packet.size());
	UInt8 marker = reader.read8();
	UInt16 time = reader.read16();
	_recvTime.update();

	if (address != _address) {
		DEBUG("Session ", name(), " has change its address from ", _address, " to ", address)

		// If address family change socket will change
		if (address.family() != _address.family())
			_pSendSession.set(_farId, _pEncoder, socket(_address.family()), _pSendSession ? _pSendSession->initiatorTime.load() : 0);
		_address.set(address);
	}

	if (time != _initiatorTime) { // to avoid to use a repeating packet version
		_initiatorTime = time;
		if (_pSendSession)
			_pSendSession->initiatorTime = Time::Now() - (time*RTMFP::TIMESTAMP_SCALE);
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
			if (reader.available() < 0x81)
				WARN("Session ", name(), " - Unexpected size received from handshake 79 : ", reader.available())
			else
				handleCookieChange(reader);
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
		setPing(RTMFP::TimeNow(), reader.read16());
	}
	case 0xF9:
	case 0xFA:
		receive(Packet(packet, reader.current(), reader.available()));
		break;
	default:
		WARN("Unexpected RTMFP marker : ", String::Format<UInt8>("%02x", marker));
	}
}

void FlowManager::handleCookieChange(BinaryReader& reader) {

	DEBUG("Handshake 79 received, we have sent wrong cookie to far peer");
	UInt8 byte40 = reader.read8();
	if (byte40 != 0x40) {
		WARN("Session ", name(), " - Unexpected first byte in handshake 79 : ", String::Format<UInt8>("%2x", byte40), ", expected 40")
		return;
	}
	string cookie, theirCookie;
	reader.read(0x40, cookie);
	if (cookie != _pHandshake->cookieReceived) {
		WARN("Session ", name(), " - Unexpected cookie received in handshake 79")
		return;
	}
	INFO("Session ", name(), " - Cookie has changed")
	reader.read(0x40, _pHandshake->cookieReceived);
}

void FlowManager::setPing(UInt16 time, UInt16 timeEcho) {
	if (timeEcho > time) {
		if (timeEcho - time < 30)
			time = 0;
		else
			time += 0xFFFF - timeEcho;
		timeEcho = 0;
	}
	UInt16 value = (time - timeEcho) * RTMFP::TIMESTAMP_SCALE;
	if(value == 0)
		value = 1;
	else if (value > 0xFFFF)
		value = 0xFFFF;

	// Smoothed Round Trip time https://tools.ietf.org/html/rfc2988

	if (!_rttvar)
		_rttvar = value / 2.0;
	else
		_rttvar = ((3 * _rttvar) + Base::abs(_ping - UInt16(value))) / 4.0;

	if (_ping == 0)
		_ping = UInt16(value);
	else if (value != _ping)
		_ping = UInt16((7 * _ping + value) / 8.0);

	_rto = (UInt32)(_ping + (4 * _rttvar) + 200);
	if (_rto < Net::RTO_MIN)
		_rto = Net::RTO_MIN;
	else if (_rto > Net::RTO_MAX)
		_rto = Net::RTO_MAX;
}

void FlowManager::sendConnect(BinaryReader& reader) {

	if (status > RTMFP::HANDSHAKE38) {
		DEBUG("Handshake 78 ignored, the session is already connected (state=", status, ")")
		return;
	}
	else if (status < RTMFP::HANDSHAKE38) {
		WARN("Handshake 78 ignored, the session is not in handshake 38 state (", status, ")")
		return;
	}

	UInt32 farId = reader.read32(); // id session
	UInt32 nonceSize = (UInt32)reader.read7Bit<UInt64>();
	if ((_pHandshake->isP2P && nonceSize != 73) || (!_pHandshake->isP2P && nonceSize < 0x8A)) {
		ERROR("Incorrect nonce size : ", nonceSize, " (expected ", _pHandshake->isP2P ? 73 : 138, " bytes)")
		return;
	}

	shared<Buffer> pFarNonce(SET, nonceSize);
	reader.read(nonceSize, *pFarNonce);
	if (memcmp(pFarNonce->data(), "\x03\x1A\x00\x00\x02\x1E\x00", 7) != 0) {
		ERROR("Far nonce received is not well formated : ", String::Hex(pFarNonce->data(), nonceSize))
		return;
	}
	_pHandshake->farNonce = move(pFarNonce);

	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ERROR("Unexpected end of handshake 78 : ", endByte)
		return;
	}

	// If we are in client->server session far key = nonce+11
	if (!_pHandshake->isP2P) {
		_pHandshake->farKey.set(nonceSize - 11);
		BinaryWriter writer(_pHandshake->farKey->data(), _pHandshake->farKey->size());
		writer.write(_pHandshake->farNonce->data() + 11, nonceSize - 11);
	}

	// Compute keys for encryption/decryption and start connect requests
	if (computeKeys(farId))
		onConnection();
}

bool FlowManager::onPeerHandshake70(const SocketAddress& address, const shared<Buffer>& farKey, const string& cookie) {
	if (status > RTMFP::HANDSHAKE30) {
		DEBUG("Handshake 70 ignored for session ", name(), ", we are already in state ", status)
		return false;
	}

	// update address & generate the session
	_address.set(address);
	_pSendSession.set(0, _pEncoder, socket(_address.family()), _pSendSession ? _pSendSession->initiatorTime.load() : 0);
	return true;
};

const shared<Buffer>& FlowManager::getNonce() {
	if (_nonce)
		return _nonce; // already computed
	
	shared<Buffer> pBuffer(SET);
	BinaryWriter nonceWriter(*pBuffer);
	if (_responder) {
		
		nonceWriter.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
		nonceWriter.writeRandom(0x40); // nonce 64 random bytes
		return _nonce = pBuffer;
	}

	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	nonceWriter.writeRandom(64); // nonce 64 random bytes
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	return _nonce = pBuffer;
}

void FlowManager::closeFlow(UInt64 flowId) {
	if (status < RTMFP::CONNECTED)
		return;

	BinaryWriter(write(0x5e, 1 + Binary::Get7BitSize<UInt64>(flowId))).write7Bit<UInt64>(flowId).write8(0);
	RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(_pBuffer, _farId, _address)), _address);
}
