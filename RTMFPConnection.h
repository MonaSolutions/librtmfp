
#pragma once

#include "P2PConnection.h"

/**************************************************
RTMFPConnection represents a connection to the
RTMFP Server
*/
class RTMFPConnection : public FlowManager {
public:
	RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPConnection();

	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, const char* url, const char* host);

	// Connect to a peer of the RTMFP server (Direct P2P)
	bool connect2Peer(Mona::Exception& ex, const char* peerId);

	// Called by Invoker every second to manage connection (flush and ping)
	void manage();

protected:

	// Send the connection message (after the answer of handshake1)
	virtual bool sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle message (after hanshake0)
	virtual void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer, const Mona::SocketAddress& address);

	// Manage handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Return the decoder engine for the following address (can be P2P or Normal connection)
	virtual RTMFPEngine*	getDecoder(Mona::UInt32 idStream, const Mona::SocketAddress& address);

private:

	// Send the second handshake message
	void sendHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader, Mona::UInt8 type);

	// Handle the first P2P handshake message
	void p2pHandshake0(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send waiting P2P connections
	void											sendP2PConnections();
	std::deque<std::string>							_waitingPeers;
	std::recursive_mutex							_mutexPeers;

	// Map of addresses to P2P connections
	std::map<Mona::SocketAddress, P2PConnection>	_mapPeersByAddress;
	std::map<std::string, P2PConnection>			_mapPeersById;

	std::string								_url; // RTMFP url of the application (base handshake)
	Mona::Buffer							_pubKey;
	Mona::Buffer							_nonce;
};