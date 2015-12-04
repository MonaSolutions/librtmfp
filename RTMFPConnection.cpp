#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"
#include <set>

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent):
		_pOnSocketError(pOnSocketError), _handshakeStep(0), _pThread(NULL), _tag(16), _pubKey(0x80), _nonce(0x8B), _timeReceived(0), _farId(0), 
	_pDefaultDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)),
	_pEncoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
	_pDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)), 
	FlowManager(invoker, pOnStatusEvent, pOnMediaEvent) {
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

		RTMFPEngine* pDecoder = _pDefaultDecoder.get(); // handshake decoder
		auto itPeer = mapPeers.find(address); // search P2P connection
		if (idStream > 0)
			pDecoder = (itPeer == mapPeers.end()) ? _pDecoder.get() : itPeer->second.pDecoder.get();

		if (!pDecoder->process((UInt8*)pBuffer.data(), pBuffer.size()))
			ex.set(Exception::CRYPTO, "Bad RTMFP CRC sum computing (idstream: ", idStream, ")");
		else {
			_outAddress.set(address);
			handleMessage(ex, pBuffer, itPeer);
		}

		if (ex)
			_pOnSocketError(ex.error());
	};
}

RTMFPConnection::~RTMFPConnection() {

	close();

	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
		_pSocket->close();
	}

	if (_pSocketIn) {
		_pSocketIn->OnPacket::unsubscribe(onPacket);
		_pSocketIn->OnError::unsubscribe(onError);
		_pSocketIn->close();
	}
}

bool RTMFPConnection::connect(Exception& ex, const char* url, const char* host) {
	if (!_pInvoker) {
		ex.set(Exception::APPLICATION, "Invoker is not initialized");
		return false;
	}

	 _url = url;
	string tmpHost = host;
	if (!strrchr(host, ':'))
		tmpHost += ":1935"; // default port

	if (!_hostAddress.set(ex, tmpHost) || !_outAddress.set(_hostAddress))
		return false;

	_pSocket.reset(new UDPSocket(_pInvoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	INFO("Connecting to ", _hostAddress.host().toString(), "...")
	if (!_pSocket->connect(ex, _hostAddress))
		return false;

	sendHandshake0(BASE_HANDSHAKE);
	return !ex;
}

void RTMFPConnection::connect2Peer(const char* peerId) {

	_peerId = peerId;
	Util::UnformatHex<string>(_peerId);

	INFO("Connecting to peer ", peerId, "...")

	sendHandshake0(P2P_HANDSHAKE);
}

void RTMFPConnection::handleMessage(Exception& ex, const Mona::PoolBuffer& pBuffer, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer) {
	BinaryReader reader(pBuffer.data(), pBuffer->size());

	_timeReceived = reader.read16();
	if (Logs::GetLevel() >= 7)
		DUMP("RTMFP", reader.current(), reader.available(), "Request from ", _outAddress.toString())

	UInt8 marker = reader.read8();
	reader.shrink(reader.read16()); // length

	// Handshake
	if (marker == 0x0B) {
		manageHandshake(ex, reader, itPeer);
		return;
	}

	// Connected message (normal or P2P)
	switch (marker|0xF0) {
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
		if (itPeer != mapPeers.end())
			itPeer->second.receive(ex, reader);
		else
			receive(ex, reader);
		break;
	default:
		WARN("Unexpected RTMFP marker : ", Format<UInt8>("%02x", marker));
	}
}

void RTMFPConnection::manageHandshake(Exception& ex, BinaryReader& reader, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0:
		ex.set(Exception::PROTOCOL, "Handshake0 has not been send"); // (should not happen)
		break;
	case 0x70:
	case 0x71:
		sendHandshake1(ex, reader, type); break;
	case 0x78:
		sendConnect(ex, reader); break;
	case 0x30:
		p2pHandshake0(ex, reader); break;
	case 0x38:
		p2pHandshake1(ex, reader, itPeer); break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected handshake type : ", type);
		break;
	}
}


void RTMFPConnection::sendHandshake0(HandshakeType type) {
	// (First packets are encoded with default key)
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write16((UInt16)((type == BASE_HANDSHAKE) ? _url.size() + 1 : _peerId.size() + 1));
	writer.write8(type); // handshake type
	writer.write((type == BASE_HANDSHAKE) ? _url : _peerId);

	Util::Random(_tag.data(), 16); // random serie of 16 bytes
	writer.write(_tag);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x30).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size(), false);
	_handshakeStep = 1; // TODO : see if we need to differentiate handshake steps for each type (basic and p2p)
}

void RTMFPConnection::sendHandshake1(Exception& ex, BinaryReader& reader, UInt8 type) {

	if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected Handshake 70 received at step ", _handshakeStep);
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
	if (String::ICompare(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) {
		ex.set(Exception::PROTOCOL, "Unexpected tag received : ", tagReceived);
		return;
	}

	// Normal NetConnection
	if (type == 0x70) {
		UInt8 cookieSize = reader.read8();
		if (cookieSize != 0x40) {
			ex.set(Exception::PROTOCOL, "Unexpected cookie size : ", cookieSize);
			return;
		}
		string cookie;
		reader.read(cookieSize, cookie);

		string certificat;
		reader.read(77, certificat);

		// Write handshake1
		BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
		writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

		writer.write32(0x02000000); // id

		writer.write7BitLongValue(cookieSize);
		writer.write(cookie); // Resend cookie

		if (!_diffieHellman.initialize(ex))
			return;
		_diffieHellman.readPublicKey(ex, _pubKey.data());
		writer.write7BitLongValue(_pubKey.size() + 4);
		writer.write7BitValue(_pubKey.size() + 2);
		writer.write16(0x1D02); // (signature)
		writer.write(_pubKey);

		Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
		writer.write7BitValue(_nonce.size());
		writer.write(_nonce);
		// TODO: see if we need to add 58 at the end + the stable part of nonce/certificate

		BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
		if (!ex) {
			flush(0x0B, writer.size(), false);
			_handshakeStep = 2;
		}
	}
	else {
		SocketAddress address;
		std::set<SocketAddress>		publicAddresses;
		std::set<SocketAddress>		localAddresses;
		std::set<SocketAddress>		redirectionAddresses;
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
}

bool RTMFPConnection::sendConnect(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 2) {
		ex.set(Exception::PROTOCOL, "Unexpected Handshake 78 received at step ", _handshakeStep);
		return false;
	}

	_farId = reader.read32(); // id session?
	UInt32 size = (UInt32)reader.read7BitLongValue() - 11;
	string nonce;
	reader.read(size + 11, nonce);
	if (String::ICompare(nonce, "\x03\x1A\x00\x00\x02\x1E\x00", 7) != 0) { // TODO: I think this is not fixed
		ex.set(Exception::PROTOCOL, "Nonce not expected : ", nonce);
		return false;
	}

	string farPubKey = nonce.substr(11, size);
	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ex.set(Exception::PROTOCOL, "Unexpected end of handshake 2 : ", endByte);
		return false;
	}

	// Compute keys for encryption/decryption
	if (!computeKeys(ex, farPubKey, nonce, _nonce.data(), _nonce.size(), _pDecoder, _pEncoder))
		return false;

	// To bind new connections on this address
	/*_pSocketIn.reset(new UDPSocket(_pInvoker->sockets, true));
	_pSocketIn->OnError::subscribe(onError);
	_pSocketIn->OnPacket::subscribe(onPacket);
	if (!_pSocketIn->bind(ex, _pSocket->address()))
		return false;*/

	
	string signature("\x00\x54\x43\x04\x00", 5);
	RTMFPFlow* pFlow = createFlow(2, signature);
	if (!pFlow)
		return false;

	pFlow->sendConnect(_url, _pSocket->address().port());
	_handshakeStep = 3;
	return true;
}

void RTMFPConnection::p2pHandshake0(Exception& ex, BinaryReader& reader) {

	if (!connected) {
		ex.set(Exception::PROTOCOL, "Handshake 30 received before connection succeed");
		return;
	}

	UInt8 peerIdSize = reader.read8();
	if (peerIdSize != 0x22)
		ex.set(Exception::PROTOCOL, "Unexpected peer id size : ", peerIdSize, " (expected 34)");
	else if ((peerIdSize = reader.read8()) != 0x21)
		ex.set(Exception::PROTOCOL, "Unexpected peer id size : ", peerIdSize, " (expected 33)");
	else if (reader.read8() != 0x0F)
		ex.set(Exception::PROTOCOL, "Unexpected marker : ", *reader.current(), " (expected 0x0F)");
	else {
		string peerId, tag;
		reader.read(0x20, peerId);
		reader.read(16, tag);

		INFO("P2P Connection request from peer ", Util::FormatHex((const UInt8*)peerId.data(), peerId.size(), LOG_BUFFER))
		auto it = mapPeers.lower_bound(_outAddress);
		if (it != mapPeers.end()) {
			ex.set(Exception::PROTOCOL, "A P2P connection already exists on address ", _outAddress.toString(), " (id : ", it->second.peerId,")");
			return;
		}
		mapPeers.emplace_hint(it, piecewise_construct, forward_as_tuple(_outAddress), forward_as_tuple(*this, peerId, _pInvoker, _pOnStatusEvent, _pOnMedia));

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
		flush(0x0B, writer.size(), false);
		_handshakeStep = 2;
	}
}

void RTMFPConnection::p2pHandshake1(Exception& ex, BinaryReader& reader, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer) {

	if (!connected) {
		ex.set(Exception::PROTOCOL, "Handshake 38 received before connection succeed");
		return;
	}

	UInt32 id = reader.read32();

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

	writer.write32(id);
	writer.write8(0x49); // nonce is 73 bytes long
	UInt32 noncePos = writer.size();
	writer.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
	UInt8 nonce[64];
	Util::Random(nonce, sizeof(nonce)); // nonce 64 random bytes
	writer.write(nonce, sizeof(nonce));
	// TODO: check if we need to add 58 here

	if (itPeer == mapPeers.end()) {
		ex.set(Exception::PROTOCOL, "Unable to find the peer with address ", _outAddress.toString());
		return;
	}

	// Compute P2P keys for decryption/encryption
	if (!computeKeys(ex, initiatorKey, initiatorNonce, writer.data() + noncePos, 0x49, itPeer->second.pDecoder, itPeer->second.pEncoder))
		return;

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size(), false);
	_handshakeStep = 2;
}

BinaryWriter& RTMFPConnection::writeMessage(UInt8 type, UInt16 length, RTMFPWriter* pWriter) {

	// No sending formated message for a failed session!
	/*if (_failed)
	return DataWriter::Null.packet;*/

	_pLastWriter = pWriter;

	UInt16 size = length + 3; // for type and size

	if (size>availableToWrite()) {
		flush(false); // send packet (and without time echo)

		if (size > availableToWrite()) {
			ERROR("RTMFPMessage truncated because exceeds maximum UDP packet size on connection");
			size = availableToWrite();
		}
		_pLastWriter = NULL;
	}

	if (!_pSender) {
		auto it = mapPeers.find(_outAddress); // search P2P encoder
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, (it == mapPeers.end()) ? _pEncoder : it->second.pEncoder));
	}
	return _pSender->packet.write8(type).write16(length);
}

UInt8* RTMFPConnection::packet() {
	if (!_pSender) {
		auto it = mapPeers.find(_outAddress); // search P2P encoder
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, (it == mapPeers.end()) ? _pEncoder : it->second.pEncoder));
	}
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE, false);
	return _pSender->packet.data();
}

void RTMFPConnection::flush(UInt8 marker, UInt32 size, bool echoTime) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(echoTime, marker);
}

void RTMFPConnection::flush(bool echoTime, UInt8 marker) {
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

		DumpResponse(packet.data() + 6, packet.size() - 6);

		Exception ex;
		_pThread = _pSocket->send<RTMFPSender>(ex, _pSender, _pThread);
		if (ex)
			ERROR("RTMFP flush, ", ex.error());
	}
	_pSender.reset();
}

void RTMFPConnection::close() {

	if (connected) {
		if (_pPublisher)
			_pPublisher.reset();

		writeMessage(0x4C, 0); // Close message
		flush(false);
		connected = false;
	}

	FlowManager::close();
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

void RTMFPConnection::DumpResponse(const UInt8* data, UInt32 size) {
	// executed just in debug mode, or in dump mode
	if (Logs::GetLevel() >= 7)
		DUMP("RTMFP", data, size, "Response to ", _outAddress.toString())
}

bool RTMFPConnection::computeKeys(Exception& ex, const string& farPubKey, const string& initiatorNonce, const UInt8* responderNonce, UInt32 responderNonceSize, shared_ptr<RTMFPEngine>& pDecoder, shared_ptr<RTMFPEngine>& pEncoder) {
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
	UInt8 encryptKey[Crypto::HMAC::SIZE];
	UInt8 decryptKey[Crypto::HMAC::SIZE];
	RTMFP::ComputeAsymetricKeys(_sharedSecret, (UInt8*)initiatorNonce.data(), (UInt16)initiatorNonce.size(), responderNonce, responderNonceSize, decryptKey, encryptKey);
	pDecoder.reset(new RTMFPEngine(decryptKey, RTMFPEngine::DECRYPT));
	pEncoder.reset(new RTMFPEngine(encryptKey, RTMFPEngine::ENCRYPT));

	return true;
}

