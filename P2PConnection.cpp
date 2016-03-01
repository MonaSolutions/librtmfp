#include "P2PConnection.h"
#include "Invoker.h"
#include "RTMFPFlow.h"
#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

UInt32 P2PConnection::P2PSessionCounter = 2000000;

P2PConnection::P2PConnection(FlowManager& parent, string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const SocketAddress& hostAddress, const Buffer& pubKey, bool responder) :
	_responder(responder), peerId(id), _parent(parent), _sessionId(++P2PSessionCounter), attempt(0), _rawResponse(false), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {

	_outAddress = _hostAddress = hostAddress;

	_tag.resize(16);
	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes

	BinaryWriter writer2(_pubKey.data(), _pubKey.size());
	writer2.write(pubKey.data(), pubKey.size()); // copy parent public key
}

P2PConnection::~P2PConnection() {
	close();
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
	default:
		ex.set(Exception::PROTOCOL, "Unexpected p2p handshake type : ", type);
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

	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x70).write16(writer.size() - RTMFP_HEADER_SIZE - 3);

	// Before sending we set connection parameters
	_outAddress = _hostAddress = address;
	_farId = 0;

	FlowManager::flush(0x0B, writer.size());
	_handshakeStep = 1;
}

void P2PConnection::responderHandshake1(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep > 2) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 38 (step : ", _handshakeStep, ")");
		return;
	}

	_farId = reader.read32();

	string cookie, initiatorNonce;
	if (reader.read8() != 0x40) {
		ex.set(Exception::PROTOCOL, "Cookie size should be 64 bytes but found : ", *(reader.current() - 1));
		return;
	}
	reader.read(0x40, cookie);

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84) {
		ex.set(Exception::PROTOCOL, "Public key size should be 132 bytes but found : ", publicKeySize);
		return;
	}
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82) {
		ex.set(Exception::PROTOCOL, "Public key size should be 130 bytes but found : ", publicKeySize);
		return;
	}
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Expected signature 1D02 but found : ", Format<UInt16>("%.4x", signature));
		return;
	}
	reader.read(0x80, _farKey);

	// Reading peerId
	UInt8 id[32];
	EVP_Digest(reader.data() + idPos, 0x84, id, NULL, EVP_sha256(), NULL);
	INFO("peer ID calculated from public key : ", Util::FormatHex(id, sizeof(id), peerId))

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ex.set(Exception::PROTOCOL, "Responder Nonce size should be 76 bytes but found : ", nonceSize);
		return;
	}
	reader.read(0x4C, initiatorNonce);

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
	if (!_parent.computeKeys(ex, _farKey, initiatorNonce, _nonce.data(), 0x49, _sharedSecret, _pDecoder, _pEncoder))
		return;

	_handshakeStep = 2;
}

void P2PConnection::initiatorHandshake70(Exception& ex, BinaryReader& reader, const SocketAddress& address) {

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

	UInt32 keySize = (UInt32)reader.read7BitLongValue();
	if (keySize != 0x82) {
		ex.set(Exception::PROTOCOL, "Unexpected responder key size : ", keySize);
		return;
	}
	if (reader.read16() != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Unexpected signature before responder key (expected 1D02)");
		return;
	}
	reader.read(0x80, _farKey);

	// Write handshake1
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(_sessionId); // id

	writer.write7BitLongValue(cookieSize);
	writer.write(cookie); // Resend cookie

	writer.write7BitLongValue(_pubKey.size() + 4);
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

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
	_outAddress = _hostAddress = address;
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
	}

	_farId = reader.read32(); // session Id
	UInt8 nonceSize = reader.read8();
	if (nonceSize != 0x49) {
		ex.set(Exception::PROTOCOL, "Unexpected nonce size : ", nonceSize, " (expected 73)");
		return false;
	}
	string responderNonce;
	reader.read(nonceSize, responderNonce);
	if (String::ICompare(responderNonce, "\x03\x1A\x00\x00\x02\x1E\x00\x41\0E", 9) != 0) {
		ex.set(Exception::PROTOCOL, "Incorrect nonce : ", responderNonce);
		return false;
	}

	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ex.set(Exception::PROTOCOL, "Unexpected end of handshake 2 : ", endByte);
		return false;
	}

	// Compute P2P keys for decryption/encryption (TODO: refactorize key computing)
	string initiatorNonce((const char*)_nonce.data(), _nonce.size());
	if (!_parent.computeKeys(ex, _farKey, initiatorNonce, BIN responderNonce.data(), nonceSize, _sharedSecret, _pDecoder, _pEncoder, false))
		return false;

	_handshakeStep = 3;
	connected = true;
	NOTE("P2P Connection ", _sessionId, " is now connected to ", peerId)

	if (!_groupId.empty()) {
		// Send group connection request
		// Create 1st Netstream and flow
		string signature("\x00\x47\x52\x1C", 4);
		RTMFPFlow* pFlow = createFlow(2, signature);

		// Compile encrypted key
		UInt8 mdp1[Crypto::HMAC::SIZE];
		UInt8 mdp2[Crypto::HMAC::SIZE];
		Crypto::HMAC hmac;
		hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), _nonce.data(), _nonce.size(), mdp1);
		hmac.compute(EVP_sha256(), _groupId.data(), _groupId.size(), mdp1, Crypto::HMAC::SIZE, mdp2);

		INFO("Sending group connection request to peer")
		_rawResponse = true;
		pFlow->sendGroupPeerConnect(_groupId, mdp2, peerId);
	}
	else {
		// Start playing
		// Create 1st NetStream and flow
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		RTMFPFlow* pFlow = createFlow(2, signature);
		pFlow->setPeerId(peerId);

		INFO("Sending play request to peer for stream '", _streamName, "'")
		pFlow->sendPlay(_streamName, true);
	}	
	return true;
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

	(command == NETSTREAM_GROUP) ? _groupId = streamName : _streamName = streamName;
}

void P2PConnection::handleStreamCreated(UInt16 idStream) {
	ERROR("Stream creation not possible on a P2P Connection") // implementation error
}

Listener* P2PConnection::startListening(Exception& ex, const string& streamName, const std::string& peerId, FlashWriter& writer) {
	ERROR("Cannot start listening stream on a P2P Connection") // implementation error
	return NULL;
}

void P2PConnection::stopListening(const string& peerId) {
	ERROR("Cannot stop listening stream on a P2P Connection") // implementation error
}

// Set the p2p publisher as ready (used for blocking mode)
void P2PConnection::setP2pPublisherReady() {
	ERROR("Cannot set publisher ready on a P2P Connection") // implementation error
}

// Only in responder mode
bool P2PConnection::handlePlay(const string& streamName, FlashWriter& writer) {
	INFO("The peer ", peerId, " is trying to play '", streamName, "'...")

	Exception ex;
	if(!(_pListener = _parent.startListening(ex, streamName, peerId, writer))) {
		// TODO : See if we can send a specific answer
		WARN(ex.error())
		return false;
	}
	INFO("Stream ",streamName," found, sending start answer")

	// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
	_parent.setP2pPublisherReady();
	return true;
}

void P2PConnection::handleNewGroupPeer(const std::string& groupId, const string& peerId) {
	ERROR("Cannot handle new Group Peer on a P2P Connection") // target error (shouldn't happen)
}

void P2PConnection::handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id) {
	
	if (String::ICompare(groupId, _groupId) != 0) {
		ERROR("Unexpected group ID received : ", groupId)
		return;
	}
	string idReceived;
	INFO("Peer ID received : ", Util::FormatHex(BIN id.data(), PEER_ID_SIZE, idReceived))
	// TODO: compare to our ID

	// Compile encrypted key
	UInt8 mdp1[Crypto::HMAC::SIZE];
	UInt8 mdp2[Crypto::HMAC::SIZE];
	Crypto::HMAC hmac;
	hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), _nonce.data(), _nonce.size(), mdp1);
	hmac.compute(EVP_sha256(), _groupId.data(), _groupId.size(), mdp1, Crypto::HMAC::SIZE, mdp2);

	auto it = _flows.find(2);
	if (it == _flows.end()) {
		ERROR("Unable to find the flow 2 for NetGroup communication")
		return;
	}

	INFO("Sending group connection answer to peer")
	it->second->sendGroupPeerConnect(_groupId, mdp2, peerId);
}

void P2PConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	ERROR("Cannot handle P2P Address Exchange command on a P2P Connection") // target error (shouldn't happen)
}

void P2PConnection::close() {
	if (connected)
		writeMessage(0x5E, 0);

	if (_pListener) {
		_parent.stopListening(peerId);
		_pListener = NULL;
	}

	FlowManager::close();
}
