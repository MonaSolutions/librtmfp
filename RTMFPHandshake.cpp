#include "RTMFPHandshake.h"
#include "Mona/PacketWriter.h"
#include "Mona/Crypto.h"
#include "Invoker.h"

#include "Mona/Logs.h"
#include <set>

using namespace Mona;
using namespace std;

RTMFPHandshake::RTMFPHandshake(OnSocketError pOnSocketError) :
	_handshakeStep(0), _pInvoker(NULL), _pThread(NULL), _tag(16), _pubKey(0x80), _nonce(0x8B), _timeReceived(0), _died(false), _farId(0), _pLastWriter(NULL),
	_pEncoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
	_pDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)),
	_pOnSocketError(pOnSocketError) {
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
		_pDecoder->process((UInt8*)pBuffer.data(), pBuffer.size()); // TODO: make a Task

		handleMessage(ex, pBuffer);
		if (ex)
			_pOnSocketError(ex.error());
	};

	_pMainStream.reset(new FlashConnection());
}

RTMFPHandshake::~RTMFPHandshake() {

	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
		_pSocket->close();
	}

	// to remove OnStart and OnStop, and erase FlashWriters (before to erase flowWriters)
	if (_pMainStream) {
		_pMainStream.reset();
	}
}

bool RTMFPHandshake::connect(Exception& ex, Invoker* invoker, const char* url, const char* host, const char* publication, bool isPublisher, HandshakeType type) {
	_pInvoker = invoker;
	(type == BASE_HANDSHAKE) ? _url = url : _peerId = url;
	if (type == P2P_HANDSHAKE)
		Util::UnformatHex<string>(_peerId);
	_publication = publication;
	_isPublisher = isPublisher;
	string tmpHost = host;
	if (!strrchr(host, ':'))
		tmpHost += ":1935"; // default port

	if (!_address.set(ex, tmpHost))
		return false;

	_pSocket.reset(new UDPSocket(invoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	INFO("Connecting to ", _address.host().toString(), ":", _address.port(), "(", (type == BASE_HANDSHAKE) ? "url" : "peer id" ," : ", url, ")...")
	if (!_pSocket->connect(ex, _address))
		return false;

	_pMainStream->setPort(_address.port()); // Record port for setPeerInfo request
	sendHandshake0(type);
	return !ex;
}

void RTMFPHandshake::handleMessage(Exception& ex, const Mona::PoolBuffer& pBuffer) {
	BinaryReader reader(pBuffer.data(), pBuffer->size());
	_timeReceived = reader.read16();
	UInt8 marker = reader.read8();
	reader.shrink(reader.read16()); // length

	switch (_handshakeStep) {
	case 0:
		ex.set(Exception::PROTOCOL, "Handshake0 has not been send"); // (should not happen)
		break;
	case 1:
	case 2:
		if (marker != 0x0B) {
			ex.set(Exception::PROTOCOL, "Unexpected handshake id : ", marker);
			return;
		}
		if (_handshakeStep == 1)
			sendHandshake1(ex, reader);
		else
			sendConnect(ex, reader);
		break;
	default:
		// with time echo
		if (marker == 0x4E) {
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
		else if (marker != 0x4A)
			WARN("RTMFPPacket marker unknown : ", Format<UInt8>("%02x", marker));

		receive(ex, reader);
		break;
	}
}

void RTMFPHandshake::sendHandshake0(HandshakeType type) {
	// (First packets are encoded with default key)
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write16((UInt16) ((type==BASE_HANDSHAKE)? _url.size() + 1 : _peerId.size() + 1));
	writer.write8(type); // handshake type
	writer.write((type == BASE_HANDSHAKE) ? _url : _peerId);

	Util::Random(_tag.data(), 16); // random serie of 16 bytes
	writer.write(_tag);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x30).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size(), false);
	_handshakeStep = 1;
}

void RTMFPHandshake::sendHandshake1(Exception& ex, BinaryReader& reader) {

	// Read & check handshake0's response
	UInt8 type = reader.read8();
	if (type != 0x70 && type != 0x71) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type : ", type);
		return;
	}
	UInt16 size = reader.read16();

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
		writer.write16(0x1D02); // unknown for now
		writer.write(_pubKey);

		Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
		writer.write7BitValue(_nonce.size());
		writer.write(_nonce);

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
			DEBUG("Address added : ",address.toString()," (type : ",addressType,")")
			if ((addressType & 0x0F) == RTMFP::ADDRESS_PUBLIC)
				publicAddresses.emplace(address);
			else if ((addressType & 0x0F) == RTMFP::ADDRESS_LOCAL)
				localAddresses.emplace(address);
			else if ((addressType & 0x0F) == RTMFP::ADDRESS_REDIRECTION)
				redirectionAddresses.emplace(address);
			else
				ERROR("Unexpected address type : ", addressType)
		}
	}
}

bool RTMFPHandshake::sendConnect(Exception& ex, BinaryReader& reader) {
	// Read & check handshake1's response (cookie + session's creation)
	UInt8 type = reader.read8();
	if (type != 0x78) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type : ", type);
		return false;
	}
	reader.read16(); // whole size

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
	if (!computeKeys(ex, farPubKey, nonce))
		return false;

	_handshakeStep = 3;
	return true;
}

UInt8* RTMFPHandshake::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE, false);
	return _pSender->packet.data();
}

void RTMFPHandshake::flush(UInt8 marker, UInt32 size, bool echoTime) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(echoTime, marker);
}

void RTMFPHandshake::flush(bool echoTime, UInt8 marker) {
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
		//_pSender->address.set(peer.address);

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

const PoolBuffers& RTMFPHandshake::poolBuffers() {
	return _pInvoker->poolBuffers;
}

void RTMFPHandshake::DumpResponse(const UInt8* data, UInt32 size) {
	// executed just in debug mode, or in dump mode
	if (Logs::GetLevel() < 7)
		DUMP("RTMFP", data, size, "Response to ", _address.toString())
}

bool RTMFPHandshake::computeKeys(Exception& ex, const string& farPubKey, const string& nonce) {
	if (!_diffieHellman.initialized()) {
		ex.set(Exception::CRYPTO, "Diffiehellman object must be initialized before computing");
		return false;
	}

	// Compute Diffie-Hellman secret
	_diffieHellman.computeSecret(ex, (UInt8*)farPubKey.data(), farPubKey.size(), _sharedSecret);
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
	RTMFP::ComputeAsymetricKeys(_sharedSecret, (UInt8*)nonce.data(), (UInt16)nonce.size(), _nonce.data(), _nonce.size(), decryptKey, encryptKey);
	_pDecoder.reset(new RTMFPEngine(decryptKey, RTMFPEngine::DECRYPT));
	_pEncoder.reset(new RTMFPEngine(encryptKey, RTMFPEngine::ENCRYPT));

	return true;
}

