
#pragma once

#include "Mona/UDPSocket.h"
#include "RTMFPSender.h"
#include "AMFWriter.h"
#include "AMFReader.h"
#include "Mona/DiffieHellman.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "P2PConnection.h"
#include "FlowManager.h"

#define COOKIE_SIZE	0x40

// Callback typedef definitions
typedef void(*OnSocketError)(const char*);

/**************************************************
RTMFPConnection represents a connection to the
RTMFP Server
*/
class RTMFPConnection : public FlowManager {
public:
	RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPConnection();

	enum HandshakeType {
		BASE_HANDSHAKE = 0x0A,
		P2P_HANDSHAKE = 0x0F
	};

	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, const char* url, const char* host);

	void connect2Peer(const char* peerId);

	// Called by Invoker every second to manage connection (flush and ping)
	void manage();

	/******* Internal functions for writers *******/
	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter = NULL);

	void									flush() { flush(true); }

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->packet.size() : RTMFP_HEADER_SIZE); }

protected:
	// Send the connection message (after the answer of handshake1)
	virtual bool sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Analyze packets received from the server (must be connected)
	//virtual void receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Close the connection properly
	virtual void close();

	// Initialize the packet in the RTMFPSender
	Mona::UInt8* packet();

	// Finalize and send the current packet
	void flush(bool echoTime, Mona::UInt8 marker = 0x89);

	// Flush the connection
	void flush(Mona::UInt8 marker, Mona::UInt32 size, bool echoTime);

	// Dump
	void DumpResponse(const Mona::UInt8* data, Mona::UInt32 size);

	// Connection parameters
	Mona::SocketAddress						_outAddress; // current address used for sending
	std::string								_url; // RTMFP url of the application (base handshake)
	std::string								_peerId; // Id of the peer (P2P handshake)

	Mona::UInt32							_farId;

	// Job Members
	std::unique_ptr<Mona::UDPSocket>		_pSocket; // Sending socket established with server
	std::unique_ptr<Mona::UDPSocket>		_pSocketIn; // Receiving socket established with server (needed for middle to accept new socket connections)
	std::shared_ptr<RTMFPSender>			_pSender; // Current sender object
	Mona::PoolThread*						_pThread; // Thread used to send last message

													  // Encryption/Decription
	std::shared_ptr<RTMFPEngine>			_pEncoder;
	std::shared_ptr<RTMFPEngine>			_pDecoder;
	std::shared_ptr<RTMFPEngine>			_pDefaultDecoder; // used for id stream 0

private:

	// Map of addresses to P2P connections
	std::map<Mona::SocketAddress, P2PConnection>	mapPeers;

	// External Callbacks to link with parent
	OnSocketError	_pOnSocketError;

	// Handle message (after hanshake0)
	void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer);

	// Manage all handshake messages (marke 0x0B)
	void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer);

	// Send the first handshake message (with rtmfp url + tag)
	void sendHandshake0(HandshakeType type);

	// Send the second handshake message
	void sendHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader, Mona::UInt8 type);

	// Handle the first P2P handshake message
	void p2pHandshake0(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle the second P2P handshake message
	void p2pHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer);

	// Compute keys for encryption/decryption of the session (after handshake ok)
	// TODO: Create a Startable object for this
	bool computeKeys(Mona::Exception& ex, const std::string& farPubKey, const std::string& initiatorNonce, const Mona::UInt8* responderNonce, Mona::UInt32 responderNonceSize, std::shared_ptr<RTMFPEngine>& pDecoder, std::shared_ptr<RTMFPEngine>& pEncoder);

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