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

#include "RTMFPConnection.h"
#include "RTMFPSender.h"
#include "SocketHandler.h"
#include "FlowManager.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(const Mona::SocketAddress& address, SocketHandler* pHandler, FlowManager* session, bool responder, bool p2p) : 
	Connection(pHandler), _pSession(session), _responder(responder), _nonce(0x4C), _isP2P(p2p), _connectAttempt(0) {

	_address.set(address);
}

RTMFPConnection::~RTMFPConnection() {
	close();
}

void RTMFPConnection::close() {

	Connection::close();

	if (_pSession)
		_pSession = NULL;
}

void RTMFPConnection::flush(bool echoTime, UInt8 marker) {
	Connection::flush(echoTime, (_responder && marker != 0x0B)? (marker + 1) : marker); // If p2p responder and connected : marker++
}

void RTMFPConnection::handleMessage(const PoolBuffer& pBuffer) {

	BinaryReader reader(pBuffer.data(), pBuffer->size());
	reader.next(2); // TODO: CRC, don't share this part in onPacket() 

	if (Logs::GetLevel() >= 7)
		DUMP("RTMFP", reader.current(), reader.available(), "Request from ", _address.toString())

	UInt8 marker = reader.read8();
	_timeReceived = reader.read16();
	_lastReceptionTime.update();

	// Handshake
	if (marker == 0x0B) {
		manageHandshake(reader);
		return;
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
		if (_status < RTMFP::CONNECTED) {
			connected = true;
			_status = RTMFP::CONNECTED;
			OnConnected::raise(_address, _pSession->name()); // TODO: see if usefull
		}
		OnMessage::raise(reader);
		break;
	default:
		WARN("Unexpected RTMFP marker : ", Format<UInt8>("%02x", marker));
	}
}

void RTMFPConnection::manageHandshake(BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		handleHandshake30(reader); break; // P2P only (and send handshake 70)
	case 0x38:
		sendHandshake78(reader); break; // P2P only
	case 0x70:
		handleHandshake70(reader); break; // (and send handshake 38)
	case 0x78:
		sendConnect(reader); break;
	case 0x71:
		if (_status < RTMFP::CONNECTED)
			handleRedirection(reader);
		else
			handleP2pAddresses(reader);
		break;
	default:
		ERROR("Unexpected p2p handshake type : ", Format<UInt8>("%.2x", (UInt8)type))
		break;
	}
}

bool RTMFPConnection::computeKeys(const string& farPubKey, const Buffer& initiatorNonce, const UInt8* responderNonce, UInt32 responderNonceSize, Buffer& sharedSecret) {
	DiffieHellman* pDh = NULL;
	if (!_pParent->diffieHellman(pDh))
		return false;

	// Compute Diffie-Hellman secret
	Exception ex;
	pDh->computeSecret(ex, (UInt8*)farPubKey.data(), farPubKey.size(), sharedSecret);
	if (ex)
		return false;

	DUMP("RTMFP", sharedSecret.data(), sharedSecret.size(), "Shared secret :")

	PacketWriter packet(_pParent->poolBuffers());
	if (packet.size() > 0) {
		ex.set(Exception::CRYPTO, "RTMFPCookieComputing already executed");
		return false;
	}

	// Compute Keys
	UInt8 responseKey[Crypto::HMAC::SIZE];
	UInt8 requestKey[Crypto::HMAC::SIZE];
	RTMFP::ComputeAsymetricKeys(sharedSecret, (UInt8*)initiatorNonce.data(), (UInt16)initiatorNonce.size(), responderNonce, responderNonceSize, requestKey, responseKey);
	_pDecoder.reset(new RTMFPEngine(_responder ? requestKey : responseKey, RTMFPEngine::DECRYPT));
	_pEncoder.reset(new RTMFPEngine(_responder ? responseKey : requestKey, RTMFPEngine::ENCRYPT));

	return true;
}

void RTMFPConnection::manage() {
	if (!_pSession)
		return;

	// Send waiting handshake 30 to server/peer
	switch (_status) {
	case RTMFP::HANDSHAKE30:
	case RTMFP::STOPPED: 
		// Send First handshake request (30)
		if (!(_pSession->status > RTMFP::HANDSHAKE30) && (!_connectAttempt || _lastAttempt.isElapsed(1500))) {
			if (_connectAttempt++ == 11) {
				INFO("Connection to ", name(), " has reached 11 attempt without answer, closing...")
				_status = RTMFP::FAILED;
				return;
			}
			TRACE("Sending new handshake 30 to ", _pSession->name(), " at address ", _address.toString())
			sendHandshake30(_pSession->epd(), _pSession->tag());
			if (_pSession->status == RTMFP::STOPPED)
				_pSession->status = RTMFP::HANDSHAKE30;
			_lastAttempt.update();
		}
		break;
	default:
		break;
	}

	Connection::manage();
}

void RTMFPConnection::handleHandshake30(BinaryReader& reader) {
	if (_status > RTMFP::HANDSHAKE30) {
		DEBUG("handshake 30 ignored, we are already in ", _status, " state")
		return;
	}

	FATAL_ASSERT(_pSession)

	if (_pSession->status > RTMFP::HANDSHAKE30) {
		DEBUG("Concurrent handshake 30 ignored, session is already in ", _pSession->status, " state")
		return;
	}

	// Here we decide which peer is responder or initator
	if (_pParent->peerId() < _pSession->name()) {
		DEBUG("Concurrent handshake 30 from ", _pSession->name(), " ignored, we have the biggest peer ID") // TODO: see how Flash deal with concurrent connections
		return;
	}
	else {
		DEBUG("Concurrent handshake 30 accepted, peer ID ", _pSession->name(), " is grater than ours")
		_responder = true;
		_pSession->unsubscribe(_address);
	}

	UInt64 peerIdSize = reader.read7BitLongValue();
	if (peerIdSize != 0x22)
		ERROR("Unexpected peer id size : ", peerIdSize, " (expected 34)")
	else if ((peerIdSize = reader.read7BitLongValue()) != 0x21)
		ERROR("Unexpected peer id size : ", peerIdSize, " (expected 33)")
	else if (reader.read8() != 0x0F)
		ERROR("Unexpected marker : ", *reader.current(), " (expected 0x0F)")
	else {

		string buff, peerId, tag;
		reader.read(0x20, buff);
		reader.read(16, tag);
		Util::FormatHex(BIN buff.data(), buff.size(), peerId);
		if (peerId != _pParent->peerId()) {
			WARN("Incorrect Peer ID in handshake 30 : ", peerId)
			return;
		}
		sendHandshake70(tag);
	}
}

void RTMFPConnection::sendHandshake70(const string& tag) {

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write8(16);
	writer.write(tag);

	UInt8 cookie[COOKIE_SIZE];
	Util::Random(cookie, COOKIE_SIZE);
	writer.write8(COOKIE_SIZE);
	writer.write(cookie, COOKIE_SIZE);

	Exception ex;
	DiffieHellman* pDh(NULL);
	if (!_pParent->diffieHellman(pDh))
		return;
	_pubKey.resize(pDh->publicKeySize(ex));
	pDh->readPublicKey(ex, _pubKey.data());
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x70).write16(writer.size() - RTMFP_HEADER_SIZE - 3);

	//_farId = 0; TODO: check this
	Connection::flush(0x0B, writer.size());
	_status = RTMFP::HANDSHAKE70;
	if (_pSession)
		_pSession->status = RTMFP::HANDSHAKE70;
}

void RTMFPConnection::handleHandshake70(BinaryReader& reader) {
	string tagReceived, cookie, farKey;

	if (_status > RTMFP::HANDSHAKE30) {
		DEBUG("Handshake 70 ignored, we are already in ", _status, " state")
		return;
	}

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ERROR("Unexpected tag size : ", tagSize)
		return;
	}
	reader.read(16, tagReceived);
	if (memcmp(tagReceived.c_str(), _pSession->tag().data(), 16) != 0) {
		ERROR("Unexpected tag received")
		return;
	}

	// Normal NetConnection
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ERROR("Unexpected cookie size : ", cookieSize)
		return;
	}
	reader.read(cookieSize, cookie);

	if (!_isP2P) {
		string certificat;
		reader.read(77, certificat);
		DEBUG("Server Certificate : ", Util::FormatHex(BIN certificat.data(), 77, LOG_BUFFER))
	}
	else {
		UInt32 keySize = (UInt32)reader.read7BitLongValue() - 2;
		if (keySize != 0x80 && keySize != 0x7F) {
			ERROR("Unexpected responder key size : ", keySize)
			return;
		}
		if (reader.read16() != 0x1D02) {
			ERROR("Unexpected signature before responder key (expected 1D02)")
			return;
		}
		reader.read(keySize, farKey);
	}

	// Handshake 70 accepted? => We send the handshake 38
	if (_pParent->onPeerHandshake70(tagReceived, farKey, cookie, _address, false, _isP2P))
		sendHandshake38(farKey, cookie);
	else
		close(); // destroy the connection
}

void RTMFPConnection::sendHandshake38(const string& farKey, const string& cookie) {
	if (!farKey.empty())
		_farKey = farKey;

	// Write handshake
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(0x02000000); // id

	writer.write7BitLongValue(cookie.size());
	writer.write(cookie); // Resend cookie

	// TODO: refactorize
	Exception ex;
	DiffieHellman* pDh(NULL);
	if (!_pParent->diffieHellman(pDh))
		return;
	_pubKey.resize(pDh->publicKeySize(ex));
	pDh->readPublicKey(ex, _pubKey.data());
	writer.write7BitLongValue(_pubKey.size() + 4);

	UInt32 idPos = writer.size();
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	if (!_isP2P) {
		string rawId("0x21\x0f", PEER_ID_SIZE+2), peerId;
		EVP_Digest(writer.data() + idPos, writer.size() - idPos, BIN(rawId.data() + 2), NULL, EVP_sha256(), NULL);
		INFO("Peer ID : \n", Util::FormatHex(BIN(rawId.data() + 2), PEER_ID_SIZE, peerId))
		OnIdBuilt::raise(rawId, peerId);
	}

	_nonce.resize(0x4C, false);
	BinaryWriter nonceWriter(_nonce.data(), 0x4C);
	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	Util::Random(_nonce.data() + 5, 64); // nonce 64 random bytes
	nonceWriter.next(64);
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);
	writer.write8(0x58);

	// _farId = 0; TODO: not sure

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	if (!ex) {
		Connection::flush(0x0B, writer.size());
		_status = RTMFP::HANDSHAKE38;
		_pSession->status = RTMFP::HANDSHAKE38;
	}
}


void RTMFPConnection::sendHandshake78(BinaryReader& reader) {

	if (_status > RTMFP::HANDSHAKE70) {
		DEBUG("Handshake 38 ignored, we are already in ", _status, " state")
		return;
	}

	// Save the far id
	_farId = reader.read32();

	string cookie;
	if (reader.read8() != 0x40) {
		ERROR("Cookie size should be 64 bytes but found : ", *(reader.current() - 1))
		return;
	}
	reader.read(0x40, cookie);

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84)
		DEBUG("Public key size should be 132 bytes but found : ", publicKeySize)
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82)
		DEBUG("Public key size should be 130 bytes but found : ", publicKeySize)
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ERROR("Expected signature 1D02 but found : ", Format<UInt16>("%.4x", signature))
		return;
	}
	reader.read(publicKeySize - 2, _farKey);

	// Read peerId and update the parent
	string rawId("\x21\x0f"), peerId;
	UInt8 id[PEER_ID_SIZE];
	EVP_Digest(reader.data() + idPos, publicKeySize + 2, id, NULL, EVP_sha256(), NULL);
	rawId.append(STR id, PEER_ID_SIZE);
	Util::FormatHex(id, PEER_ID_SIZE, peerId);
	DEBUG("peer ID calculated from public key : ", peerId)

	// Create the session, if already exists and connected we ignore the request
	if (!_pParent->onNewPeerId(rawId, peerId, _address) && _pSession->status > RTMFP::HANDSHAKE38) {
		DEBUG("Handshake 38 ignored, session is already in state ", _pSession->status)
		close();
		return;
	}

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ERROR("Responder Nonce size should be 76 bytes but found : ", nonceSize)
		return;
	}
	reader.read(nonceSize, _farNonce);
	_farNonce.resize(nonceSize);

	UInt8 endByte;
	if ((endByte = reader.read8()) != 0x58) {
		ERROR("Unexpected end byte : ", endByte, " (expected 0x58)")
		return;
	}

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(_pSession->sessionId()); // TODO: see if we need to do a << 24
	writer.write8(0x49); // nonce is 73 bytes long
	BinaryWriter nonceWriter(_nonce.data(), 0x49);
	nonceWriter.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
	Util::Random(_nonce.data() + 9, 64); // nonce 64 random bytes
	writer.write(_nonce.data(), 0x49);
	writer.write8(0x58);

	// Important: send this before computing encoder key because we need the default encoder
	// TODO: ensure that the default encoder is used for handshake (in repeated messages)
	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	Connection::flush(0x0B, writer.size());

	// Compute P2P keys for decryption/encryption
	if (!computeKeys(_farKey, _farNonce, _nonce.data(), 0x49, _sharedSecret))
		return;

	DEBUG("Initiator Nonce : ", Util::FormatHex(BIN _farNonce.data(), _farNonce.size(), LOG_BUFFER))
	DEBUG("Responder Nonce : ", Util::FormatHex(BIN _nonce.data(), 0x49, LOG_BUFFER))

	_status = RTMFP::HANDSHAKE78;
	_pSession->status = RTMFP::HANDSHAKE78;
}

void RTMFPConnection::sendConnect(BinaryReader& reader) {

	if (_pSession->status > RTMFP::HANDSHAKE38) {
		DEBUG("Handshake 78 ignored, the session is already in ", _pSession->status, " state")
		return;
	}

	_farId = reader.read32(); // id session?
	UInt32 nonceSize = (UInt32)reader.read7BitLongValue();
	if ((_isP2P && nonceSize != 73) || (!_isP2P && nonceSize < 0x8A)) {
		ERROR("Incorrect nonce size : ", nonceSize, " (expected ", _isP2P? 73 : 138," bytes)")
		return;
	}

	reader.read(nonceSize, _farNonce);
	if (memcmp(_farNonce.data(), "\x03\x1A\x00\x00\x02\x1E\x00", 7) != 0) {
		ERROR("Far nonce received is not well formated : ", Util::FormatHex(BIN _farNonce.data(), nonceSize, LOG_BUFFER))
		return;
	}

	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ERROR("Unexpected end of handshake 78 : ", endByte)
		return;
	}

	// Compute keys for encryption/decryption
	if (!_isP2P)
		_farKey.assign(STR (_farNonce.data() + 11), nonceSize - 11);
	if (!computeKeys(_farKey, _nonce, BIN _farNonce.data(), _farNonce.size(), _sharedSecret))
		return;

	connected = true;
	_status = RTMFP::CONNECTED;
	OnConnected::raise(_address, _pSession->name());
}


void RTMFPConnection::handleRedirection(BinaryReader& reader) {
	if (_status > RTMFP::HANDSHAKE30) {
		DEBUG("Redirection message ignored, we have already received handshake 71")
		return;
	}

	DEBUG("Server redirection messsage, sending back the handshake 30")
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ERROR("Unexpected tag size : ", tagSize)
		return;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if (memcmp(tagReceived.c_str(), (const char*)_pSession->tag().data(), 16) != 0) {
		ERROR("Unexpected tag received")
		return;
	}
	SocketAddress address;
	while (reader.available() && *reader.current() != 0xFF) {
		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")
		if (address.family() == IPAddress::IPv6) {
			DEBUG("Ignored address ", address.toString(), ", IPV6 not supported yet") // TODO: support IPV6
			continue;
		}

		// Ask parent to create a new connection and send handshake 30 back
		_pParent->addConnection(address, _pSession, false, false);
	}
}
