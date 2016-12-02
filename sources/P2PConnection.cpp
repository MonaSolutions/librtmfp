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

#include "P2PConnection.h"
#include "Invoker.h"
#include "RTMFPFlow.h"
#include "NetGroup.h"
#include "Mona/Logs.h"
#include "Listener.h"
#include "RTMFPConnection.h"

using namespace Mona;
using namespace std;

UInt32 P2PConnection::P2PSessionCounter = 2000000;

P2PConnection::P2PConnection(RTMFPConnection* parent, string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const PEER_LIST_ADDRESS_TYPE& addresses,
	const SocketAddress& host, bool responder, bool group) :
	_responder(responder), peerId(id), rawId("\x21\x0f"), _parent(parent), _sessionId(++P2PSessionCounter), attempt(0), _rawResponse(false), _pListener(NULL), _groupBeginSent(false), mediaSubscriptionSent(false),
	_lastIdSent(0), _pushOutMode(0), pushInMode(0), _fragmentsMap(MAX_FRAGMENT_MAP_SIZE), _idFragmentMap(0), groupReportInitiator(false), _groupConnectSent(false), _idMediaReportFlow(0), _isGroup(group),
	isGroupDisconnected(false), groupFirstReportSent(false), mediaSubscriptionReceived(false), _hostAddress(host), _knownAddresses(addresses), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onGroupHandshake = [this](const string& groupId, const string& key, const string& peerId) {
		handleGroupHandshake(groupId, key, peerId);
	};

	_tag.resize(16);
	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes

	if (peerId != "unknown")
		Util::UnformatHex(BIN peerId.data(), peerId.size(), rawId, true);

	_pMainStream->OnGroupMedia::subscribe((OnGroupMedia&)*this);
	_pMainStream->OnGroupReport::subscribe((OnGroupReport&)*this);
	_pMainStream->OnGroupPlayPush::subscribe((OnGroupPlayPush&)*this);
	_pMainStream->OnGroupPlayPull::subscribe((OnGroupPlayPull&)*this);
	_pMainStream->OnFragmentsMap::subscribe((OnFragmentsMap&)*this);
	_pMainStream->OnGroupBegin::subscribe((OnGroupBegin&)*this);
	_pMainStream->OnFragment::subscribe((OnFragment&)*this);
	_pMainStream->OnGroupHandshake::subscribe(onGroupHandshake);
}

P2PConnection::~P2PConnection() {
	DEBUG("Deletion of P2PConnection ", peerId, "(", _outAddress.toString(),")")

	_pMainStream->OnGroupMedia::unsubscribe((OnGroupMedia&)*this);
	_pMainStream->OnGroupReport::unsubscribe((OnGroupReport&)*this);
	_pMainStream->OnGroupPlayPush::unsubscribe((OnGroupPlayPush&)*this);
	_pMainStream->OnGroupPlayPull::unsubscribe((OnGroupPlayPull&)*this);
	_pMainStream->OnFragmentsMap::unsubscribe((OnFragmentsMap&)*this);
	_pMainStream->OnGroupBegin::unsubscribe((OnGroupBegin&)*this);
	_pMainStream->OnFragment::unsubscribe((OnFragment&)*this);
	_pMainStream->OnGroupHandshake::unsubscribe(onGroupHandshake);
	close(true);
	_parent = NULL;
}

void P2PConnection::close(bool full) {
	if (_died)
		return;

	closeGroup(full);

	if (_pListener) {
		_parent->stopListening(peerId);
		_pListener = NULL;
	}

	if (full) {
		FlowManager::close();
		_handshakeStep = 0;
	}
}

UDPSocket&	P2PConnection::socket() { 
	return _parent->socket(); 
}

void P2PConnection::updateHostAddress(const Mona::SocketAddress& address) {
	if (_hostAddress != address) {
		DEBUG("Updating host address of peer ", peerId, " to ", address.toString())
		_hostAddress = address;
	}
}

void P2PConnection::setOutAddress(const Mona::SocketAddress& address) {
	if (_outAddress != address)
		_outAddress = address;
}

RTMFPFlow* P2PConnection::createSpecialFlow(Exception& ex, UInt64 id, const string& signature) {

	if (signature.size()>6 && signature.compare(0, 6, "\x00\x54\x43\x04\xFA\x89", 6) == 0) { // Direct P2P NetStream
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 6, signature.length() - 6).read7BitValue());
		DEBUG("Creating new Flow (2) for P2PConnection ", name())
		_pMainStream->addStream(idSession, pStream);
		RTMFPFlow* pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		pFlow->setPeerId(peerId);

		return pFlow;
	}
	else if ((signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)  // NetGroup Report stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x19", 4) == 0)  // NetGroup Data stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x1D", 4) == 0)  // NetGroup Message stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)  // NetGroup Media Report stream (fragments Map & Media Subscription)
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0)) {  // NetGroup Media stream
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true);

		DEBUG("Creating new flow (", id, ") for P2PConnection ", peerId)
		RTMFPFlow* pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		pFlow->setPeerId(peerId);

		if (signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)
			_idMediaReportFlow = pFlow->id; // Record the NetGroup Media Report id
		return pFlow;
	}
	string tmp;
	ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	return NULL;
}

void P2PConnection::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	FlowManager::initWriter(pWriter);

	if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0) {
		(UInt64&)pWriter->flowId = _idMediaReportFlow; // new Media Writer of NetGroup will be associated to the Media Report Flow
		_pMediaWriter = pWriter;
		return;
	}

	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // other new Writer are associated to the P2PConnection Report flow (first in _flow lists)

	if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)
		_pReportWriter = pWriter;
	else if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)
		_pMediaReportWriter = pWriter;
	else if (pWriter->signature.size() > 6 && pWriter->signature.compare(0, 7, "\x00\x54\x43\x04\xFA\x89\x01", 7) == 0)
		_pNetStreamWriter = pWriter; // TODO: maybe manage many streams
}

void P2PConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		DEBUG("Handshake 30 has already been received, request ignored") // responderHandshake0 is called by RTMFPConnection
		break;
	case 0x38:
		responderHandshake1(ex, reader); break;
	case 0x78:
		initiatorHandshake2(ex, reader); break;
	case 0x70:
		// special case, we have sent handshake 70 and we receive handshake 70, notify the parent
		TRACE("Unexpected p2p handshake type 70 (possible double sessions)")
		_parent->onP2PHandshake70(ex, reader, _outAddress);
		break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected p2p handshake type : ", Format<UInt8>("%.2x", (UInt8)type));
		break;
	}
}

void P2PConnection::responderHandshake0(Exception& ex, string tag, const SocketAddress& address) {

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write8(16);
	writer.write(tag);

	UInt8 cookie[COOKIE_SIZE];
	Util::Random(cookie, COOKIE_SIZE);
	writer.write8(COOKIE_SIZE);
	writer.write(cookie, COOKIE_SIZE);

	writer.write7BitValue(_parent->publicKey().size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_parent->publicKey());

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x70).write16(writer.size() - RTMFP_HEADER_SIZE - 3);

	// Before sending we set connection parameters
	_outAddress = _targetAddress = address;
	_farId = 0;

	FlowManager::flush(0x0B, writer.size());
	_handshakeStep = 1;
}

void P2PConnection::responderHandshake1(Exception& ex, BinaryReader& reader) {

	if (_responder && _handshakeStep > 1) {
		DEBUG("Handshake message ignored, we have already received handshake 38 (step : ", _handshakeStep, ")")
		return;
	}

	_farId = reader.read32();

	string cookie;
	if (reader.read8() != 0x40) {
		ex.set(Exception::PROTOCOL, "Cookie size should be 64 bytes but found : ", *(reader.current() - 1));
		return;
	}
	reader.read(0x40, cookie);

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84)
		DEBUG("Public key size should be 132 bytes but found : ", publicKeySize);
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82)
		DEBUG("Public key size should be 130 bytes but found : ", publicKeySize);
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Expected signature 1D02 but found : ", Format<UInt16>("%.4x", signature));
		return;
	}
	reader.read(publicKeySize-2, _farKey);

	// Read peerId and update the parent
	UInt8 id[PEER_ID_SIZE];
	EVP_Digest(reader.data() + idPos, 0x84, id, NULL, EVP_sha256(), NULL);
	if (peerId == "unknown")
		rawId.append(STR id, PEER_ID_SIZE);
	DEBUG("peer ID calculated from public key : ", Util::FormatHex(id, PEER_ID_SIZE, peerId))
	_parent->onPeerConnect(_knownAddresses, _hostAddress, peerId, rawId.data(), _outAddress);

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ex.set(Exception::PROTOCOL, "Responder Nonce size should be 76 bytes but found : ", nonceSize);
		return;
	}
	reader.read(nonceSize, _farNonce);
	_farNonce.resize(nonceSize);

	UInt8 endByte;
	if ((endByte = reader.read8()) != 0x58) {
		ex.set(Exception::PROTOCOL, "Unexpected end byte : ", endByte, " (expected 0x58)");
		return;
	}

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(_sessionId); // TODO: see if we need to do a << 24
	writer.write8(0x49); // nonce is 73 bytes long
	BinaryWriter nonceWriter(_nonce.data(), 0x49);
	nonceWriter.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
	Util::Random(_nonce.data()+9, 64); // nonce 64 random bytes
	writer.write(_nonce.data(), 0x49);
	writer.write8(0x58);

	// Important: send this before computing encoder key because we need the default encoder
	// TODO: ensure that the default encoder is used for handshake (in repeated messages)
	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	FlowManager::flush(0x0B, writer.size());

	// Compute P2P keys for decryption/encryption
	if (!_parent->computeKeys(ex, _farKey, _farNonce, _nonce.data(), 0x49, _sharedSecret, _pDecoder, _pEncoder))
		return;

	DEBUG("Initiator Nonce : ", Util::FormatHex(BIN _farNonce.data(), _farNonce.size(), LOG_BUFFER))
	DEBUG("Responder Nonce : ", Util::FormatHex(BIN _nonce.data(), 0x49, LOG_BUFFER))

	_handshakeStep = 2;
	_parent->addPeer2Group(_outAddress, peerId); // TODO: see if we must check the result

	// We are intiator but have received handshake 38, try to send the next RTMFP message
	if (!_responder) {
		DEBUG("We have received handshake 38 but we are initiator, sending next message...")
		connected = true;
		if (_isGroup)
			sendGroupPeerConnect();
		else {
			// Start playing
			INFO("Sending play request to peer for stream '", _streamName, "'")
			string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
			new RTMFPWriter(FlashWriter::OPENED, signature, *this); // writer is automatically associated to _pNetStreamWriter
			AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation("play", true);
			amfWriter.amf0 = true; // Important for p2p unicast play
			amfWriter.writeString(_streamName.c_str(), _streamName.size());
			_pNetStreamWriter->flush();
		}
	}
}

void P2PConnection::initiatorHandshake70(Exception& ex, BinaryReader& reader, const SocketAddress& address) {

	if (_knownAddresses.find(address) == _knownAddresses.end())
		_knownAddresses.emplace(address, RTMFP::ADDRESS_PUBLIC);

	if (_handshakeStep > 1) {
		WARN("Handshake 70 already received, ignored")
		return;
	}

	string cookie;
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL, "Unexpected cookie size : ", cookieSize);
		return;
	}
	reader.read(cookieSize, cookie);

	UInt32 keySize = (UInt32)reader.read7BitLongValue() - 2;
	if (keySize != 0x80 && keySize != 0x7F) {
		ex.set(Exception::PROTOCOL, "Unexpected responder key size : ", keySize);
		return;
	}
	if (reader.read16() != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Unexpected signature before responder key (expected 1D02)");
		return;
	}
	reader.read(keySize, _farKey);

	// Write handshake1
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(_sessionId); // id

	writer.write7BitLongValue(cookieSize);
	writer.write(cookie); // Resend cookie

	writer.write7BitLongValue(_parent->publicKey().size() + 4);
	writer.write7BitValue(_parent->publicKey().size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_parent->publicKey());

	_nonce.resize(0x4C,false);
	BinaryWriter nonceWriter(_nonce.data(), 0x4C);
	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	Util::Random(_nonce.data()+5, 64); // nonce is a serie of 64 random bytes
	nonceWriter.next(64);
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);
	writer.write8(0x58);

	// Before sending we set connection parameters
	_outAddress = _targetAddress = address;
	_farId = 0;

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	if (!ex) {
		FlowManager::flush(0x0B, writer.size());
		_handshakeStep = 2;
	}
}

bool P2PConnection::initiatorHandshake2(Exception& ex, BinaryReader& reader) {
	
	if (_handshakeStep > 2) {
		WARN("Handshake 78 already received, ignored")
		return true;
	} else if(!_responder) {

		_farId = reader.read32(); // session Id
		UInt8 nonceSize = reader.read8();
		if (nonceSize != 0x49) {
			ex.set(Exception::PROTOCOL, "Unexpected nonce size : ", nonceSize, " (expected 73)");
			return false;
		}
		reader.read(nonceSize, _farNonce);
		_farNonce.resize(nonceSize);
		if (String::ICompare(_farNonce, "\x03\x1A\x00\x00\x02\x1E\x00\x41\0E", 9) != 0) {
			ex.set(Exception::PROTOCOL, "Incorrect responder nonce : ", _farNonce);
			return false;
		}

		UInt8 endByte = reader.read8();
		if (endByte != 0x58) {
			ex.set(Exception::PROTOCOL, "Unexpected end of handshake 2 : ", endByte);
			return false;
		}

		// Compute P2P keys for decryption/encryption (TODO: refactorize key computing)
		string initiatorNonce((const char*)_nonce.data(), _nonce.size());
		if (!_parent->computeKeys(ex, _farKey, initiatorNonce, BIN _farNonce.data(), nonceSize, _sharedSecret, _pDecoder, _pEncoder, false))
			return false;

		DUMP("RTMFP", _nonce.data(), _nonce.size(), "Initiator Nonce  :")
		DUMP("RTMFP", BIN _farNonce.data(), _farNonce.size(), "Responder Nonce :")

		_parent->addPeer2Group(_outAddress, peerId);
	} else
		DEBUG("We have received handshake 78 but we are responder, sending next message...")
	_handshakeStep = 3;
	connected = true;
	NOTE("P2P Connection ", _sessionId, " is now connected to ", peerId)

	if (_isGroup)
		sendGroupPeerConnect();
	else {
		// Start playing
		INFO("Sending play request to peer for stream '", _streamName, "'")
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		new RTMFPWriter(FlashWriter::OPENED, signature, *this); // writer is automatically associated to _pNetStreamWriter
		AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation("play", true);
		amfWriter.amf0 = true; // Important for p2p unicast play
		amfWriter.writeString(_streamName.c_str(), _streamName.size());
		_pNetStreamWriter->flush();
	}
	return true;
}

unsigned int P2PConnection::callFunction(const char* function, int nbArgs, const char** args) {

	if (!_pNetStreamWriter) {
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		new RTMFPWriter(FlashWriter::OPENED, signature, *this); // writer is automatically associated to _pNetStreamWriter
	}
	_pMainStream->callFunction(*_pNetStreamWriter, function, nbArgs, args);
	return 0;
}

void P2PConnection::flush(bool echoTime, UInt8 marker) {
	if (_rawResponse && marker != 0x0B)
		marker = 0x09;
	if (_responder)
		FlowManager::flush(echoTime, (marker == 0x0B) ? 0x0B : marker + 1);
	else
		FlowManager::flush(echoTime, marker);
}

void P2PConnection::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {

	_streamName = streamName;
}

// Only in responder mode
bool P2PConnection::handlePlay(const string& streamName, FlashWriter& writer) {
	DEBUG("The peer ", peerId, " is trying to play '", streamName, "'...")

	Exception ex;
	if(!(_pListener = _parent->startListening<FlashListener, FlashWriter&>(ex, streamName, peerId, writer))) {
		// TODO : See if we can send a specific answer
		WARN(ex.error())
		return false;
	}
	INFO("Stream ",streamName," found, sending start answer")

	// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
	_parent->setP2pPublisherReady();
	return true;
}

void P2PConnection::handleProtocolFailed() {
	close(true);
}

void P2PConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	ERROR("Cannot handle P2P Address Exchange command on a P2P Connection") // target error (shouldn't happen)
}

void P2PConnection::handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id) {
	if (!_isGroup)
		return;

	// Is it a reconnection? => add the peer to group
	if (isGroupDisconnected && !_parent->addPeer2Group(_targetAddress, peerId))
		return;

	if (String::ICompare(groupId, _parent->groupIdHex()) != 0) {
		ERROR("Unexpected group ID received : ", groupId, "\nExpected : ", _parent->groupIdHex())
		return;
	}
	string idReceived;
	Util::FormatHex(BIN id.data(), PEER_ID_SIZE, idReceived);
	if (String::ICompare(idReceived, _parent->peerId()) != 0) {
		ERROR("Our peer ID was expected but received : ", idReceived)
		return;
	}

	// Send the group connection request to peer if not already sent
	if (!_groupConnectSent)
		sendGroupPeerConnect();
}

void P2PConnection::handleWriterFailed(RTMFPWriter* pWriter) {

	if (pWriter == _pMediaReportWriter.get()) {
		DEBUG(peerId, " has closed the subscription media writer")
		pWriter->close();
		mediaSubscriptionSent = mediaSubscriptionReceived = false;
		return;
	}
	else if (pWriter == _pMediaWriter.get()) {
		DEBUG(peerId, " has closed the media writer")
		pWriter->close();
		return;
	}

	Exception ex;
	pWriter->fail(ex, "Writer terminated on connection ", peerId);
	if (ex)
		WARN(ex.error())
}

void P2PConnection::sendGroupBegin() {
	if (!_groupBeginSent) {
		if (!_pReportWriter) {
			ERROR("Unable to find the Report flow (2) for NetGroup communication")
			return;
		}

		DEBUG("Sending Group Begin message")
		_pReportWriter->writeGroupBegin();
		_pReportWriter->flush();
		_groupBeginSent = true;
	}
}

void P2PConnection::sendGroupMedia(const string& stream, const UInt8* data, UInt32 size, RTMFPGroupConfig* groupConfig) {

	DEBUG("Sending the Media Subscription for stream '", stream, "' to peer ", peerId)
	if (!_pMediaReportWriter) {
		string signature("\x00\x47\x52\x11", 4);
		new RTMFPWriter(FlashWriter::OPENED, signature, *this); // writer is automatically associated to _pMediaReportWriter
	}
	_pMediaReportWriter->writeGroupMedia(stream, data, size, groupConfig);
	mediaSubscriptionSent = true;
}

void P2PConnection::sendGroupReport(const UInt8* data, UInt32 size) {

	if (!_pReportWriter) {
		ERROR("Unable to find the Report flow (2) for NetGroup communication")
		return;
	}
	_pReportWriter->writeRaw(data, size);
	if (!groupFirstReportSent)
		groupFirstReportSent = true;
	sendGroupBegin(); // (if not already sent)
}

bool P2PConnection::sendMedia(const UInt8* data, UInt32 size, UInt64 fragment, bool pull) {
	if ((!pull && !isPushable((UInt8)fragment%8)))
		return false;

	if (!_pMediaWriter) {
		string signature("\x00\x47\x52\x12", 4);
		new RTMFPWriter(FlashWriter::OPENED, signature, *this); // writer is automatically associated to _pMediaWriter
	}
	_pMediaWriter->writeRaw(data, size);
	_pMediaWriter->flush();
	return true;
}

void P2PConnection::sendFragmentsMap(UInt64 lastFragment, const UInt8* data, UInt32 size) {
	if (_pMediaReportWriter && lastFragment != _lastIdSent) {
		DEBUG("Sending Fragments Map message (type 22) to peer ", peerId, " (", lastFragment,")")
		_pMediaReportWriter->writeRaw(data, size);
		_pMediaReportWriter->flush();
		_lastIdSent = lastFragment;
	}
}

void P2PConnection::setPushMode(UInt8 mode) {
	_pushOutMode = mode;
}

bool P2PConnection::isPushable(UInt8 rest) {
	return (_pushOutMode & (1 << rest)) > 0;
}

void P2PConnection::sendPushMode(UInt8 mode) {
	if (_pMediaReportWriter && pushInMode != mode) {
		string masks;
		if (mode > 0) {
			for (int i = 0; i < 8; i++) {
				if ((mode & (1 << i)) > 0)
					String::Append(masks, (masks.empty() ? "" : ", "), i, ", ", Format<UInt8>("%.1X", i + 8));
			}
		}

		DEBUG("Setting Group Push In mode to ", Format<UInt8>("%.2x", mode), " (", masks,") for peer ", peerId, " - last fragment : ", _idFragmentMap)
		_pMediaReportWriter->writeGroupPlay(mode);
		_pMediaReportWriter->flush();
		pushInMode = mode;
	}
}

void P2PConnection::updateFragmentsMap(UInt64 id, const UInt8* data, UInt32 size) {
	if (id <= _idFragmentMap) {
		DEBUG("Wrong Group Fragments map received from peer ", peerId, " : ", id, " <= ", _idFragmentMap)
		return;
	}

	_idFragmentMap = id;
	if (!size)
		return; // 0 size protection

	if (size > MAX_FRAGMENT_MAP_SIZE)
		WARN("Size of fragment map > max size : ", size)
	_fragmentsMap.resize(size);
	BinaryWriter writer(_fragmentsMap.data(), size);
	writer.write(data, size);
}

bool P2PConnection::checkMask(UInt8 bitNumber) {
	if (!_idFragmentMap)
		return false;

	if (_idFragmentMap % 8 == bitNumber)
		return true;

	// Determine the last fragment with bit mask
	UInt64 lastFragment = _idFragmentMap - (_idFragmentMap % 8);
	lastFragment += ((_idFragmentMap % 8) > bitNumber) ? bitNumber : bitNumber - 8;

	DEBUG("Searching ", lastFragment, " into ", Format<UInt8>("%.2x", *_fragmentsMap.data()), " ; (current id : ", _idFragmentMap, ") ; result = ",
		((*_fragmentsMap.data()) & (1 << (8 - _idFragmentMap + lastFragment))) > 0, " ; bit : ", bitNumber, " ; address : ", _targetAddress.toString(), " ; latency : ", latency())

	return ((*_fragmentsMap.data()) & (1 << (8 - _idFragmentMap + lastFragment))) > 0;
}

bool P2PConnection::hasFragment(UInt64 index) {
	if (!_idFragmentMap || (_idFragmentMap < index)) {
		TRACE("Searching ", index, " impossible into ", peerId, ", current id : ", _idFragmentMap)
		return false; // No Fragment or index too recent
	}
	else if (_idFragmentMap == index) {
		TRACE("Searching ", index, " OK into ", peerId, ", current id : ", _idFragmentMap)
		return true; // Fragment is the last one or peer has all fragments
	}
	else if (_setPullBlacklist.find(index) != _setPullBlacklist.end()) {
		TRACE("Searching ", index, " impossible into ", peerId, " a request has already failed")
		return false;
	}

	UInt32 offset = (UInt32)((_idFragmentMap - index - 1) / 8);
	UInt32 rest = ((_idFragmentMap - index - 1) % 8);
	if (offset > _fragmentsMap.size()) {
		TRACE("Searching ", index, " impossible into ", peerId, ", out of buffer (", offset, "/", _fragmentsMap.size(), ")")
		return false; // Fragment deleted from buffer
	}

	TRACE("Searching ", index, " into ", Format<UInt8>("%.2x", *(_fragmentsMap.data() + offset)), " ; (current id : ", _idFragmentMap, ", offset : ", offset, ") ; result = ",
		(*(_fragmentsMap.data() + offset) & (1 << rest)) > 0)

	return (*(_fragmentsMap.data() + offset) & (1 << rest)) > 0;
}

void P2PConnection::sendPull(UInt64 index) {
	if (_pMediaReportWriter) {
		TRACE("Sending pull request for fragment ", index, " to peer ", peerId);
		_pMediaReportWriter->writeGroupPull(index);
	}
}

void P2PConnection::closeGroup(bool full) {

	OnPeerClose::raise(peerId, pushInMode, full);

	if (full && _pReportWriter) {
		_groupConnectSent = false;
		_groupBeginSent = false;
		groupFirstReportSent = false;
		_pReportWriter->close();
	}
	mediaSubscriptionSent = mediaSubscriptionReceived = false;
	pushInMode = 0;
	if (_pMediaReportWriter)
		_pMediaReportWriter->close();
	if (_pMediaWriter)
		_pMediaWriter->close();
}

void P2PConnection::sendGroupPeerConnect() {
	if (!_pReportWriter) {
		string signature("\x00\x47\x52\x1C", 4);
		new RTMFPWriter(FlashWriter::OPENED, signature, *this);  // writer is automatically associated to _pReportWriter
	}

	// Compile encrypted key
	if (!_groupConnectKey) {
		UInt8 mdp1[Crypto::HMAC::SIZE];
		_groupConnectKey.reset(new Buffer(Crypto::HMAC::SIZE));
		Crypto::HMAC hmac;
		hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), BIN _farNonce.data(), _farNonce.size(), mdp1);
		hmac.compute(EVP_sha256(), _parent->groupIdTxt().data(), _parent->groupIdTxt().size(), mdp1, Crypto::HMAC::SIZE, _groupConnectKey->data());
	}

	DEBUG("Sending group connection request to peer ", peerId)
	_pReportWriter->writePeerGroup(_parent->groupIdHex(), _groupConnectKey->data(), rawId.c_str());
	_pReportWriter->flush();
	_groupConnectSent = true;
	sendGroupBegin();
}

void P2PConnection::addPullBlacklist(UInt64 idFragment) {
	// TODO: delete old blacklisted fragments
	_setPullBlacklist.emplace(idFragment);
}

bool P2PConnection::ReadAddresses(BinaryReader& reader, PEER_LIST_ADDRESS_TYPE& addresses, SocketAddress& hostAddress) {

	// Read all addresses
	SocketAddress address;
	while (reader.available()) {

		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		if (address.family() == IPAddress::IPv4) { // TODO: Handle ivp6

			switch (addressType & 0x0F) {
			case RTMFP::ADDRESS_LOCAL:
			case RTMFP::ADDRESS_PUBLIC:
				addresses.emplace(address, (RTMFP::AddressType)addressType);
				break;
			case RTMFP::ADDRESS_REDIRECTION:
				hostAddress = address; break;
			}
			TRACE("IP Address : ", address.toString(), " - type : ", addressType)
		}
	}
	return !addresses.empty();
}
