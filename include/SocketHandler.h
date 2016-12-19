/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "Mona/UDPSocket.h"
#include "Mona/PoolBuffer.h"
#include "Mona/DiffieHellman.h"
#include "RTMFPConnection.h"
#include "DefaultConnection.h"

namespace SHandlerEvents {
	// Can be called by a separated thread!
	struct OnPeerHandshake30 : Mona::Event<void(const std::string&, const Mona::SocketAddress&)> {};
	struct OnPeerHandshake70 : Mona::Event<void(const std::string&, const Mona::SocketAddress&, const std::string&, const std::string&)> {};
	struct OnNewPeerId : Mona::Event<bool(std::shared_ptr<RTMFPConnection>&, const std::string&, const std::string&)> {};
	struct OnConnection : Mona::Event<void(std::shared_ptr<RTMFPConnection>&, const std::string&)> {}; // called when a connection succeed
	struct OnP2PAddresses : Mona::Event<bool(const std::string&, const PEER_LIST_ADDRESS_TYPE&)> {}; // called when we receive addresses of a P2P session, must return true if peer is found and in stopped mode
};

class Invoker;
class RTMFPSession;

/**************************************************
SocketHandler handle the socket and the map of
socket addresses to RTMFPConnection
It is the entry point for all IO
*/
class SocketHandler : public virtual Mona::Object, 
	public SHandlerEvents::OnNewPeerId,
	public SHandlerEvents::OnConnection,
	public SHandlerEvents::OnP2PAddresses,
	public SHandlerEvents::OnPeerHandshake30,
	public SHandlerEvents::OnPeerHandshake70,
	public ConnectionEvents::OnIdBuilt {
public:
	#define MAP_ADDRESS2CONNECTION	std::map<Mona::SocketAddress, std::shared_ptr<RTMFPConnection>>

	SocketHandler(Invoker* invoker, RTMFPSession* pSession);

	~SocketHandler();

	// Return the socket object of the session
	virtual Mona::UDPSocket&			socket() { return *_pSocket; }

	// Return poolbuffers object to allocate buffers
	const Mona::PoolBuffers&			poolBuffers();

	// Add a connection to the map
	std::shared_ptr<RTMFPConnection>	addConnection(const Mona::SocketAddress& address, FlowManager* session, bool responder, bool p2p);

	// Accept all connexions (P2P writer or NetGroup)
	void								setAcceptAll() { _acceptAll = true; }

	// Called by Invoker every second to manage connection (flush and ping)
	void								manage();

	// Close the socket all connections
	void								close();

	/* Public functions for RTMFPConnection */

	// Return the main session peer Id
	const std::string&					peerId();

	// Initialize (if not already) the diffie hellman object and return it
	bool								diffieHellman(Mona::DiffieHellman * &pDh);

	// Called by RTMFPConnection when we discover a new peer ID, return true if the p2p session has been created
	bool								onNewPeerId(const std::string& rawId, const std::string& peerId, const Mona::SocketAddress& address);

	// Add a p2p connection request to send to the server
	void								addP2PConnection(const std::string& rawId, const std::string& peerId, const std::string& tag, const Mona::SocketAddress& hostAddress);

	// Called when receiving addresses from a peer
	void								onP2PAddresses(const std::string& tagReceived, const PEER_LIST_ADDRESS_TYPE& addresses, const Mona::SocketAddress& hostAddress);

	// Called when receiving handshake 30 on an unknown address
	void								onPeerHandshake30(const std::string& id, const std::string& tag, const Mona::SocketAddress& address);

	// Called when receiving handshake 70
	void								onPeerHandshake70(const std::string& tagReceived, const std::string& farkey, const std::string& cookie, const Mona::SocketAddress& address, bool createConnection);

private:
	// Delete the connection with the address given
	void								deleteConnection(const MAP_ADDRESS2CONNECTION::iterator& itConnection);

	// Waiting P2P request
	struct WaitingPeer : public Mona::Object {

		WaitingPeer(const std::string& rawPeerId, const std::string& id, const Mona::SocketAddress& address) : 
			rawId(rawPeerId.c_str(), PEER_ID_SIZE+2), /*attempt(0),*/ peerId(id.c_str()), hostAddress(address), received(false) {}

		std::string			rawId;
		std::string			peerId;
		bool				received;
		/*Mona::UInt8			attempt; // Counter of connection attempts to the server
		Mona::Time			lastAttempt; // Last attempt to connect to the server*/
		Mona::SocketAddress	hostAddress; // Address of the server
	};
	std::map<std::string, WaitingPeer>		_mapTag2Peer; // map of Tag to P2P waiting request

	MAP_ADDRESS2CONNECTION					_mapAddress2Connection; // map of address to RTMFP connection
	std::unique_ptr<DefaultConnection>		_pDefaultConnection; // Default connection to send handshake messages

	std::recursive_mutex					_mutexConnections; // main mutex for connections (normal or p2p)
	std::unique_ptr<Mona::UDPSocket>		_pSocket; // Sending socket established with server
	Invoker*								_pInvoker; // Pointer to the main invoker class (to get poolbuffers)
	RTMFPSession*							_pMainSession; // Pointer to the main RTMFP session for assocation with new connections
	bool									_acceptAll; // True if we must accept packets from unknown addresses (P2P publisher or NetGroup)

	Mona::DiffieHellman						_diffieHellman; // diffie hellman object used for the shared object building

	// Events subscriptions
	Mona::UDPSocket::OnPacket::Type			onPacket; // Main input event, received on each raw packet
	Mona::UDPSocket::OnError::Type			onError; // Main input event, received on socket error
	ConnectionEvents::OnConnected::Type		onConnection; // received when we are connected to a peer or server
};