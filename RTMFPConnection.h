
#pragma once

#include "Mona/UDPSocket.h"
#include "RTMFPSender.h"
#include "AMFWriter.h"
#include "AMFReader.h"
#include "Mona/DiffieHellman.h"

#define MESSAGE_HEADER			0x80

class Invoker;
class RTMFPConnection {
public:
	RTMFPConnection(void (__cdecl * onSocketError)(const char*), void (__cdecl * onStatusEvent)(const char*,const char*));

	~RTMFPConnection() {

		if(_pSocket) {
			_pSocket->OnPacket::unsubscribe(onPacket);
			_pSocket->OnError::unsubscribe(onError);
		}
	}
	
	bool connect(Mona::Exception& ex, Invoker* invoker, const char* host, int port, const char* url);

	void playStream(Mona::Exception& ex, const char* streamName);

	void close();

private:

	void (__cdecl * _onSocketError)(const char*);
	void (__cdecl * _onStatusEvent)(const char*,const char*);

	enum PlayStreamType {
		PLAYSTREAM_STOPPED,
		PLAYSTREAM_CREATING,
		PLAYSTREAM_CREATED,
		PLAYSTREAM_PLAYING
	};

	// Send the next handshake
	void sendNextHandshake(Mona::Exception& ex, const Mona::UInt8* data=NULL, Mona::UInt32 size=0);

	// Handle message (after hanshake is done)
	void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer);

	// Send the first handshake message (with rtmfp url + tag)
	Mona::UInt8 sendHandshake0(Mona::BinaryWriter& writer);

	// Send the second handshake message
	Mona::UInt8 sendHandshake1(Mona::Exception& ex, Mona::BinaryWriter& writer, Mona::BinaryReader& reader);

	// Send the third handshake message
	void sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Analyze packets received from the server
	void receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Treat audio received
	void audioHandler(Mona::UInt32 time, Mona::PacketReader& message);

	// Treat video received 
	void videoHandler(Mona::UInt32 time, Mona::PacketReader& message);

	// Process a request from Server (types 10&11)
	void process(Mona::Exception& ex, Mona::UInt64 stage, Mona::UInt64 deltaNAck, Mona::PacketReader& message, Mona::UInt8 flags);

	// Treat message received in an invocation packet
	void invocationHandler(Mona::Exception& ex, const std::string& name, AMFReader& message);

	// Initialize the packet in the RTMFPSender
	Mona::UInt8* packet();

	// Write only a type and send it directly
	void writeType(Mona::Exception& ex, Mona::UInt8 type, Mona::UInt8 length, bool echoTime);

	// Send a message (must be connected)
	void sendMessage(Mona::Exception& ex, Mona::UInt8 marker, Mona::UInt8 idResponse, AMFWriter& writer, bool echoTime);

	// Flush the connection
	void flush(Mona::Exception& ex, Mona::UInt8 marker, Mona::UInt32 size,bool echoTime);

	// Finalize and send the current packet
	void flush(Mona::Exception& ex,bool echoTime,Mona::UInt8 marker);

	// Compute keys for encryption/decryption of the session (after handshake ok)
	bool computeKeys(Mona::Exception& ex, const std::string& farPubKey, const std::string& nonce);

	Mona::UInt8							_step;
	PlayStreamType						_playStreamStep;

	Mona::UInt16						_timeReceived; // last time received
	Mona::UInt32						_farId;
	double								_idStreamPlayed; // Id of playing stream
	Mona::UInt64						_writerId; // writer Id received in acknowledgment
	Mona::UInt64						_stage;

	Mona::UInt64						_bytesReceived; // Number of bytes received
	Mona::Time							_lastKeepAlive; // last time a keepalive request has been received

	std::string							_url; // RTMFP url of the application
	std::string							_streamPlayed; // Stream name of the stream to be played

	Mona::UDPSocket::OnError::Type		onError;
	Mona::UDPSocket::OnPacket::Type		onPacket;
	
	Mona::SocketAddress					_address;
	std::shared_ptr<Mona::UDPSocket>	_pSocket;
	std::shared_ptr<RTMFPSender>		_pSender;
	const Invoker*						_pInvoker;
	Mona::PoolThread*					_pThread;

	// Encryption/Decription
	std::shared_ptr<RTMFPEngine>		_pEncoder;
	std::shared_ptr<RTMFPEngine>		_pDecoder;
	Mona::DiffieHellman					_diffieHellman;
	Mona::Buffer						_sharedSecret;
	Mona::Buffer						_tag;
	Mona::Buffer						_pubKey;
	Mona::Buffer						_nonce;
};