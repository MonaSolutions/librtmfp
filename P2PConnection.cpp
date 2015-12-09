#include "P2PConnection.h"
#include "Invoker.h"

using namespace Mona;
using namespace std;

bool P2PConnection::connect(Mona::Exception & ex) {

	_pSocket.reset(new UDPSocket(_pInvoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);
	if (!_pSocket->connect(ex, _hostAddress))
		return false;
	return true;
}

void P2PConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x38:
		p2pHandshake1(ex, reader); break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected handshake type : ", type);
		break;
	}
}

void P2PConnection::p2pHandshake0(Exception& ex, const std::string& tag, const Buffer& pubKey, UInt32 farId, const SocketAddress& address) {
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write8(16);
	writer.write(tag);

	UInt8 cookie[COOKIE_SIZE];
	Util::Random(cookie, COOKIE_SIZE);
	writer.write8(COOKIE_SIZE);
	writer.write(cookie, COOKIE_SIZE);

	writer.write7BitValue(pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(pubKey);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x70).write16(writer.size() - RTMFP_HEADER_SIZE - 3);

	// Before sending we set connection parameters
	_outAddress = address;
	_farId = farId;

	FlowManager::flush(0x0B, writer.size(), false);
	_handshakeStep = 1;
}

void P2PConnection::p2pHandshake1(Exception& ex, BinaryReader& reader/*, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer*/) {

	if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 38 (step : ", _handshakeStep, ")");
		return;
	}

	UInt32 farId = reader.read32();

	string cookie, initiatorKey, initiatorNonce;

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
	if ((publicKeySize = reader.read7BitValue()) != 0x82) {
		ex.set(Exception::PROTOCOL, "Public key size should be 130 bytes but found : ", publicKeySize);
		return;
	}
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Expected signature 1D02 but found : ", Format<UInt16>("%.4x", signature));
		return;
	}
	reader.read(0x80, initiatorKey);

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

	writer.write32(_farId);
	writer.write8(0x49); // nonce is 73 bytes long
	UInt32 noncePos = writer.size();
	writer.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
	UInt8 nonce[64];
	Util::Random(nonce, sizeof(nonce)); // nonce 64 random bytes
	writer.write(nonce, sizeof(nonce));
	// TODO: check if we need to add 58 here

	// Compute P2P keys for decryption/encryption (TODO: refactorize key computing)
	if (!_parent.computeKeys(ex, initiatorKey, initiatorNonce, writer.data() + noncePos, 0x49, _pDecoder, _pEncoder))
		return;

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	FlowManager::flush(0x0B, writer.size(), false);
	_handshakeStep = 2;
}

void P2PConnection::flush(bool echoTime, Mona::UInt8 marker) {
	if (!_pSocket) // responder
		FlowManager::flush(echoTime, (marker==0x0B)? 0x0B : marker+1);
	else
		FlowManager::flush(echoTime, marker);
}
