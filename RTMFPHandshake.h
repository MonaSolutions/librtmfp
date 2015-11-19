
#pragma once

#include "Mona/UDPSocket.h"
#include "RTMFPSender.h"
#include "AMFWriter.h"
#include "AMFReader.h"
#include "Mona/DiffieHellman.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "BandWriter.h"
#include "Publisher.h"

// Callback typedef definitions
typedef void(*OnSocketError)(const char*);

class Invoker;
/**************************************************
Parent class of RTMFPConnection
Dedicated to do the handshake before sending
connection and other AMF commands
*/
class RTMFPHandshake : public BandWriter {
public:
	RTMFPHandshake(OnSocketError pOnSocketError);

	~RTMFPHandshake();

	enum HandshakeType {
		BASE_HANDSHAKE = 0x0A,
		P2P_HANDSHAKE = 0x0F
	};

	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, Invoker* invoker, const char* url, const char* host, const char* publication, bool isPublisher, HandshakeType type);

	/******* Internal functions for writers *******/
	void									flush() { flush(true); }

	virtual const Mona::PoolBuffers&		poolBuffers();

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->packet.size() : RTMFP_HEADER_SIZE); }

	virtual bool							canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	virtual bool							failed() const { return _died; /* return _failed; */ }

protected:
	// Send the connection message (after the answer of handshake1)
	virtual bool sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Analyze packets received from the server (must be connected)
	virtual void receive(Mona::Exception& ex, Mona::BinaryReader& reader) = 0;

	// Initialize the packet in the RTMFPSender
	Mona::UInt8* packet();

	// Finalize and send the current packet
	void flush(bool echoTime, Mona::UInt8 marker = 0x89);

	// Flush the connection
	void flush(Mona::UInt8 marker, Mona::UInt32 size, bool echoTime);

	// Dump
	void DumpResponse(const Mona::UInt8* data, Mona::UInt32 size);

	// Connection parameters
	Mona::SocketAddress						_address; // host address
	std::string								_url; // RTMFP url of the application (base handshake)
	std::string								_peerId; // Id of the peer (P2P handshake)
	std::string								_publication; // Stream name
	bool									_isPublisher; // Publisher or Player?

	Mona::UInt32							_farId;
	bool									_died; // connection is died

	// Job Members
	std::shared_ptr<FlashConnection>		_pMainStream; // Main Stream (NetConnection)
	RTMFPWriter*							_pLastWriter; // Write pointer used to check if it is possible to write
	std::shared_ptr<Mona::UDPSocket>		_pSocket; // Unique socket established with server
	std::shared_ptr<RTMFPSender>			_pSender; // Current sender object
	Invoker*								_pInvoker; // Main invoker
	Mona::PoolThread*						_pThread; // Thread used to send last message

	// Encryption/Decription
	std::shared_ptr<RTMFPEngine>			_pEncoder;
	std::shared_ptr<RTMFPEngine>			_pDecoder;

private:

	// External Callbacks to link with parent
	OnSocketError	_pOnSocketError;

	// Handle message (after hanshake0)
	void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer);

	// Send the first handshake message (with rtmfp url + tag)
	void sendHandshake0(HandshakeType type);

	// Send the second handshake message
	void sendHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Compute keys for encryption/decryption of the session (after handshake ok)
	// TODO: Create a Startable object for this
	bool computeKeys(Mona::Exception& ex, const std::string& farPubKey, const std::string& nonce);

	Mona::UInt8								_handshakeStep; // Handshake step (3 possible states)
	Mona::UInt16							_timeReceived; // last time received

	// Events
	Mona::UDPSocket::OnError::Type			onError; // TODO: delete this if not needed
	Mona::UDPSocket::OnPacket::Type			onPacket; // Main input event, received on each raw packet

	// Encryption/Decription
	Mona::DiffieHellman						_diffieHellman;
	Mona::Buffer							_sharedSecret;
	Mona::Buffer							_tag;
	Mona::Buffer							_pubKey;
	Mona::Buffer							_nonce;
};