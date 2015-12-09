
#pragma once

#include "FlowManager.h"

#define COOKIE_SIZE	0x40

/**************************************************
P2PConnection represents a direct P2P connection 
with another peer
*/
class P2PConnection : public FlowManager {
	friend class RTMFPConnection;
public:
	P2PConnection(FlowManager& parent, std::string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const Mona::SocketAddress& hostAddress) :
		peerId(id), _parent(parent), _hostAddress(hostAddress), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {

	}

	virtual ~P2PConnection() {
		close();
	}

	virtual Mona::UDPSocket&	socket() { return (!_pSocket) ? _parent.socket() : FlowManager::socket(); }

	const std::string				peerId; // Peer Id of the peer connected

protected:

	// Close the connection properly
	virtual void close() {
		FlowManager::close();
	}

	bool connect(Mona::Exception& ex);

	// Manage all handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle the first P2P handshake message (called by parent)
	void p2pHandshake0(Mona::Exception& ex, const std::string& tag, const Mona::Buffer& pubKey, Mona::UInt32 farId, const Mona::SocketAddress& address);

	// Handle the second P2P handshake message
	void p2pHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader/*, const std::map<Mona::SocketAddress, P2PConnection>::iterator&  itPeer*/);

	// Flush the connection
	// marker values can be :
	// - 0B for handshake
	// - 0A for raw response in P2P mode (only for responder)
	// - 8A for AMF responde in P2P mode (only for responder)
	// - 4A for acknowlegment in P2P mode (TODO: see if it is needed)
	virtual void				flush(bool echoTime, Mona::UInt8 marker = 0x89);

private:
	const Mona::SocketAddress&				_hostAddress;
	FlowManager&							_parent; // RTMFPConnection related to
};
