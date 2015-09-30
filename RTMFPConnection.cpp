#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "Mona/Crypto.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(): _step(0),_pInvoker(NULL),_pThread(NULL),_tag(16),_pubKey(0x80),_nonce(0x4C),
		_pEncoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
		_pDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)) {
	onError = [this](const Exception& ex) {
		onSocketError(ex);
	};
	onPacket = [this](PoolBuffer& pBuffer,const SocketAddress& address) {
		_step++;

		// Decode the RTMFP data
		Exception ex;
		if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
			ex.set(Exception::PROTOCOL,"Invalid RTMFP packet");
			onSocketError(ex);
			return;
		}
		
		BinaryReader reader(pBuffer.data(), pBuffer.size());
		UInt32 idStream = RTMFP::Unpack(reader);
		pBuffer->clip(reader.position());		
		_pDecoder->process((UInt8*)pBuffer.data(),pBuffer.size());

		if (!sendNextHandshake(ex, pBuffer.data(), pBuffer.size()))
			onSocketError(ex);
	};
}

bool RTMFPConnection::connect(Exception& ex, Invoker* invoker, const char* host, int port, const char* url) {
	_pInvoker = invoker;
	_url = url;
	if (!_address.set(ex, host, port))
		return false;

	_pSocket.reset(new UDPSocket(invoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	if (!_pSocket->connect(ex, _address))
		return false;
	onSocketConnected();

	return sendNextHandshake(ex);
}

bool RTMFPConnection::sendNextHandshake(Exception& ex, const UInt8* data, UInt32 size) {
	BinaryWriter writer(packet(),RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE+3); // header + type and size
	UInt8 idResponse = 0;

	switch(_step) {
		case 0:
			idResponse = sendHandshake0(writer);
			break;
		case 1: 
		{
			BinaryReader reader(data, size);
			UInt16 time = reader.read16();
			UInt8 id = reader.read8();
			reader.shrink(reader.read16()); // length

			idResponse = sendHandshake1(ex, writer, reader);
		}
			break;
		default: 
			ex.set(Exception::PROTOCOL,"Unimplemented RTMFP step : ", _step);
			onSocketError(ex);
			break;
	}

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(idResponse).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	//(UInt32&)farId = 0;
	if(!ex)
		flush(ex, 0x0B, writer.size());
	return !ex;
}

UInt8 RTMFPConnection::sendHandshake0(BinaryWriter& writer) {
	// (First packets are encoded with default key)

	writer.write16(_url.size()+1);
	writer.write8(0x0a); // type of handshake
	writer.write(_url);

	Util::Random(_tag.data(), 16); // random serie of 16 bytes
	writer.write(_tag);

	return 0x30;
}

UInt8 RTMFPConnection::sendHandshake1(Exception& ex, BinaryWriter& writer, BinaryReader& reader) {

	// Read & check handshake0's response
	UInt8 type = reader.read8();
	if(type != 0x70) {
		ex.set(Exception::PROTOCOL,"Unexpected handshake type : ", type);
		return 0;
	}
	UInt16 size = reader.read16();

	UInt8 tagSize = reader.read8();
	if(tagSize != 16) {
		ex.set(Exception::PROTOCOL,"Unexpected tag size : ", tagSize);
		return 0;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if(String::ICompare(tagReceived.c_str(),(const char*)_tag.data(),16)!=0) {
		ex.set(Exception::PROTOCOL,"Unexpected tag received : ", tagReceived);
		return 0;
	}	

	UInt8 cookieSize = reader.read8();
	if(cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL,"Unexpected cookie size : ", cookieSize);
		return 0;
	}
	string cookie;
	reader.read(cookieSize, cookie);
	
	string certificat;
	reader.read(77, certificat);

	// Write handshake1
	writer.write32(0x02000000); // id

	writer.write7BitLongValue(cookieSize);
	writer.write(cookie); // Resend cookie

	Util::Random(_pubKey.data(), _pubKey.size()); // TODO: find a best way to define the public key
	writer.write7BitLongValue(_pubKey.size()+4);
	writer.write7BitValue(_pubKey.size()+2);
	writer.write16(0); // unknown for now
	writer.write(_pubKey);

	Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
	writer.write7BitValue(_nonce.size()+2);
	writer.write(_nonce);

	return 0x38;
}

void RTMFPConnection::close() {
	if (_pSocket)
		_pSocket->close();
}

UInt8* RTMFPConnection::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE,false);
	return _pSender->packet.data();
}

void RTMFPConnection::flush(Exception& ex, UInt8 marker, UInt32 size) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(ex, marker);
}

void RTMFPConnection::flush(Exception& ex,/*bool echoTime,*/UInt8 marker) {
	//_pLastWriter=NULL;
	if(!_pSender)
		return;
	if (/*!died && */_pSender->available()) {
		BinaryWriter& packet(_pSender->packet);
	
		// After 30 sec, send packet without echo time
		/*if(peer.lastReceptionTime.isElapsed(30000))
			echoTime = false;

		if(echoTime)
			marker+=4;
		else*/
			packet.clip(2);

		BinaryWriter writer(packet.data()+6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		/*if (echoTime)
			writer.write16(_timeSent+RTMFP::Time(peer.lastReceptionTime.elapsed()));*/

		/*_pSender->farId = farId;
		_pSender->address.set(peer.address);*/

		if(packet.size() > RTMFP_MAX_PACKET_SIZE) {
			ex.set(Exception::PROTOCOL, "Message exceeds max RTMFP packet size on connection (",packet.size(),">",RTMFP_MAX_PACKET_SIZE,")");
			return;
		}

		//dumpResponse(packet.data() + 6, packet.size() - 6);

		_pThread = _pSocket->send<RTMFPSender>(ex, _pSender,_pThread);
	}
	_pSender.reset();
}