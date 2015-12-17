#include "P2PConnection.h"
#include "Invoker.h"
#include "RTMFPFlow.h"
#include "Mona/Logs.h"
#include <set>

using namespace Mona;
using namespace std;

UInt32 P2PConnection::P2PSessionCounter = 0;

P2PConnection::P2PConnection(FlowManager& parent, string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const SocketAddress& hostAddress, const Buffer& pubKey, const Buffer& tag) :
	peerId(id), _parent(parent), _sessionId(++P2PSessionCounter), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {

	_hostAddress.set(hostAddress);

	BinaryWriter writer(_tag.data(), _tag.size());
	writer.write(tag.data(), tag.size()); // copy parent tag

	BinaryWriter writer2(_pubKey.data(), _pubKey.size());
	writer2.write(pubKey.data(), pubKey.size()); // copy parent public key
}

bool P2PConnection::connect(Exception & ex) {
	_outAddress = _hostAddress;

	_pSocket.reset(new UDPSocket(_pInvoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);
	if (!_pSocket->connect(ex, _hostAddress))
		return false;
	return true;
}

bool P2PConnection::bind(Exception& ex, const SocketAddress& address) {
	//_outAddress = address;

	_pSocket.reset(new UDPSocket(_pInvoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);
	_pSocket->bind(ex, address);
	return !ex;
}

void P2PConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		responderHandshake0(ex, reader, 0, _outAddress); break;
		break;
	case 0x38:
		responderHandshake1(ex, reader); break;
	case 0x70:
		initiatorHandshake70(ex, reader); break;
	case 0x71:
		initiatorHandshake71(ex, reader); break;
	case 0x78:
		initiatorHandshake2(ex, reader); break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected p2p handshake type : ", type);
		break;
	}
}

void P2PConnection::responderHandshake0(Exception& ex, BinaryReader& reader, UInt32 farId, const SocketAddress& address) {
	UInt8 peerIdSize = reader.read8();
	if(peerIdSize != 0x22) {
		ex.set(Exception::PROTOCOL, "Unexpected peer id size : ", peerIdSize, " (expected 34)");
		return;
	}
	if ((peerIdSize = reader.read8()) != 0x21) {
		ex.set(Exception::PROTOCOL, "Unexpected peer id size : ", peerIdSize, " (expected 33)");
		return;
	}
	if(reader.read8() != 0x0F) {
		ex.set(Exception::PROTOCOL, "Unexpected marker : ", *reader.current(), " (expected 0x0F)");
		return;
	}
	
	string peerId, tag;
	reader.read(0x20, peerId);
	reader.read(16, tag);
	INFO("P2P Connection request from peer ", Util::FormatHex((const UInt8*)peerId.data(), peerId.size(), LOG_BUFFER))

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
	_outAddress = address;
	_farId = farId;

	FlowManager::flush(0x0B, writer.size());
	_handshakeStep = 1;
}

void P2PConnection::responderHandshake1(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep > 2) {
	//if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 38 (step : ", _handshakeStep, ")");
		return;
	}

	UInt32 farId = reader.read32();

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
	if (!_parent.computeKeys(ex, _farKey, initiatorNonce, writer.data() + noncePos, 0x49, _pDecoder, _pEncoder))
		return;

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	FlowManager::flush(0x0B, writer.size());
	_handshakeStep = 2;
}

void P2PConnection::initiatorHandshake70(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 70 (step : ", _handshakeStep, ")");
		return;
	}
	string tagReceived, cookie;

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return;
	}
	reader.read(16, tagReceived);
	if (String::ICompare(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) { // compare to parent tag
		ex.set(Exception::PROTOCOL, "Unexpected tag received : ", tagReceived);
		return;
	}

	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL, "Unexpected cookie size : ", cookieSize);
		return;
	}
	reader.read(cookieSize, cookie);

	UInt32 keySize = (UInt32)reader.read7BitLongValue();
	if (keySize != 0x82) { // TODO: I'm not sure 1D02 is part of far key
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

	_nonce.resize(77,false);
	Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);
	// TODO: see if we need to add 58 at the end + the stable part of nonce

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	if (!ex) {
		FlowManager::flush(0x0B, writer.size());
		_handshakeStep = 2;
	}
}

void P2PConnection::initiatorHandshake71(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 71 (step : ", _handshakeStep, ")");
		return;
	}

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if (String::ICompare(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) {  // compare to parent tag
		ex.set(Exception::PROTOCOL, "Unexpected tag received : ", tagReceived);
		return;
	}

	SocketAddress address;
	set<SocketAddress>		publicAddresses;
	set<SocketAddress>		localAddresses;
	set<SocketAddress>		redirectionAddresses;
	while (reader.available() && *reader.current() != 0xFF) {
		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")
		if ((addressType & 0x0F) == RTMFP::ADDRESS_PUBLIC)
			publicAddresses.emplace(address);
		else if ((addressType & 0x0F) == RTMFP::ADDRESS_LOCAL)
			localAddresses.emplace(address);
		else if ((addressType & 0x0F) == RTMFP::ADDRESS_REDIRECTION)
			redirectionAddresses.emplace(address);
		else
			ERROR("Unexpected address type : ", addressType)
	}
	//TODO: implement the redirection
}

bool P2PConnection::initiatorHandshake2(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 2) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 78 (step : ", _handshakeStep, ")");
		return false;
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
	if (!_parent.computeKeys(ex, _farKey, initiatorNonce, (const UInt8*)responderNonce.data(), nonceSize, _pDecoder, _pEncoder, false))
		return false;

	connected = true;

	// Create 1st NetStream and flow
	shared_ptr<FlashStream> pStream;
	_pMainStream->addStream(1, pStream);
	string signature("\x00\x54\x43\x04\xFA\x89\x00", 7);
	RTMFPFlow* pFlow = createFlow(2, signature);

	// Start playing Play
	pFlow->sendPlay(_streamName, true);
	return true;
}

void P2PConnection::flush(bool echoTime, UInt8 marker) {
	if (!_pSocket) // responder
		FlowManager::flush(echoTime, (marker==0x0B)? 0x0B : marker+1);
	else
		FlowManager::flush(echoTime, marker);
}

void P2PConnection::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {

	_streamName = streamName;
}

void P2PConnection::handleStreamCreated(UInt16 idStream) {
	ERROR("Stream creation not possible on a P2P Connection") // implementation error
}

bool P2PConnection::getPublishStream(const string& streamName,bool& audioReliable,bool& videoReliable) {
	ERROR("Cannot get publication stream on a P2P Connection") // implementation error
	return false;
}

// Only in responder mode
void P2PConnection::handlePlay(const string& streamName, FlashWriter& writer) {
	INFO("The peer ",peerId," is trying to play '", streamName,"'...")

	// TODO: find a better place to determine that the connection is ON in responder mode
	connected = true;

	bool audioReliable, videoReliable;
	if(!_parent.getPublishStream(streamName,audioReliable,videoReliable)) {
		// TODO : implement NetStream.Play.BadName
		return;
	}

	_streamName = streamName;

	// Create the publisher
	_pPublisher.reset(new Publisher(poolBuffers(), *_pInvoker, audioReliable, videoReliable));
	_pPublisher->setWriter(&writer);

	// Send the response
	writer.writeAMFStatus("NetStream.Play.Reset", "Playing and resetting " + streamName); // for entiere playlist
	writer.writeAMFStatus("NetStream.Play.Start", "Started playing " + streamName); // for item
	AMFWriter& amf(writer.writeAMFData("|RtmpSampleAccess"));

	// TODO: determinate if video and audio are available
	amf.writeBoolean(true); // audioSampleAccess
	amf.writeBoolean(true); // videoSampleAccess

	// TODO: flush?
}

void P2PConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	/*if(reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange first 3 bytes")
		return;
	}

	string tmp;
	//reader.read(0x20, peerId);
	Util::FormatHex((const UInt8*)peerId.data(), peerId.size(), tmp);
	SocketAddress address;
	RTMFP::ReadAddress(reader, address, reader.read8());
	
	string tag;
	reader.read(16, tag);
	INFO("P2P address exchange from peer ", peerId, " with address : ", address.toString())*/
}
