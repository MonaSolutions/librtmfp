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

#include "BandWriter.h"
#include "RTMFP.h"
#include "Mona/DiffieHellman.h"

class Invoker;
class RTMFPSession;
class FlowManager;

// Waiting handshake request
struct Handshake : public virtual Mona::Object {

	Handshake(FlowManager* session, const Mona::SocketAddress& host, const PEER_LIST_ADDRESS_TYPE& addresses, bool p2p) :
		status(RTMFP::STOPPED), pCookie(NULL), pTag(NULL), attempt(0), hostAddress(host), pSession(session), listAddresses(addresses), isP2P(p2p) {}

	bool					isP2P; // True if it is a p2p handshake
	const std::string*		pCookie; // pointer to the cookie buffer
	const std::string*		pTag; // pointer to the tag
	std::string				cookieReceived; // Value of the far peer cookie (initiator)
	FlowManager*			pSession; // Session related to (it can be null if we are in a handshake of a responder)
	Mona::UInt8				attempt; // Counter of connection attempts to the server
	Mona::Time				lastAttempt; // Last attempt to connect to the server
	Mona::Time				cookieCreation; // Time when the cookie has been created
	Mona::SocketAddress		hostAddress; // Address of the host server (if cleared : it is a direct connection)
	RTMFP::SessionStatus	status; // Status of the handshake
	PEER_LIST_ADDRESS_TYPE	listAddresses; // List of direct addresses (server or p2p addresses)

	// Coding keys
	std::string				farKey; // Far public key
	Mona::Buffer			pubKey; // Our public key
	Mona::Buffer			farNonce; // Far nonce
};

/**************************************************
RTMFPHandshaker handle the socket and the map of
socket addresses to RTMFPConnection
It is the entry point for all IO
*/
class RTMFPHandshaker : public BandWriter  {
public:

	RTMFPHandshaker(RTMFPSession* pSession);

	virtual ~RTMFPHandshaker();

	// Return the name of the session
	virtual const std::string&			name() { return _name; }

	// Start a new Handshake if possible and add it to the map of tags
	// return True if the connection is created
	bool								startHandshake(std::shared_ptr<Handshake>& pHandshake, const Mona::SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, FlowManager* pSession, bool responder, bool p2p);
	bool								startHandshake(std::shared_ptr<Handshake>& pHandshake, const Mona::SocketAddress& address, FlowManager* pSession, bool responder, bool p2p);

	// Create the handshake object if needed and send a handshake 70 to address
	void								sendHandshake70(const std::string& tag, const Mona::SocketAddress& address, const Mona::SocketAddress& host);

	// Called by Invoker every second to manage connection (flush and ping)
	void								manage();

	// Close the socket all connections
	void								close();

	// Initialize (if not already) the diffie hellman object and return it
	bool								diffieHellman(Mona::DiffieHellman * &pDh);

	// Handle receiving packet
	virtual void						process(const Mona::SocketAddress& address, Mona::PoolBuffer& pBuffer);

	// Return the socket object
	virtual Mona::UDPSocket&			socket(Mona::IPAddress::Family family);

	// Return true if the session has failed
	virtual bool						failed();

	// Return the pool buffers object
	virtual const Mona::PoolBuffers&	poolBuffers();
	
	// Clear the packet and flush the connection
	void								flush(Mona::UInt8 marker, Mona::UInt32 size);

	// Remove the handshake properly
	void								removeHandshake(std::shared_ptr<Handshake> pHandshake);

private:

	// Send the first handshake message (with rtmfp url/peerId + tag)
	void								sendHandshake30(const std::string& epd, const std::string& tag);

	// Handle the handshake 30 (p2p concurrent connection)
	void								handleHandshake30(Mona::BinaryReader& reader);

	// Handle a server redirection message or a p2p address exchange
	void								handleRedirection(Mona::BinaryReader& reader);

	// Send the 2nd handshake response (only in P2P mode)
	void								sendHandshake78(Mona::BinaryReader& reader);

	// Handle the handshake 70 (from peer or server)
	void								handleHandshake70(Mona::BinaryReader& reader);

	// Send the 2nd handshake request
	void								sendHandshake38(const std::shared_ptr<Handshake>& pHandshake, const std::string& cookie);

	// Send the first handshake response (only in P2P mode)
	void								sendHandshake70(const std::string& tag, std::shared_ptr<Handshake>& pHandshake);

	std::map<std::string, std::shared_ptr<Handshake>>		_mapTags; // map of Tag to waiting handshake
	std::map<std::string, std::shared_ptr<Handshake>>		_mapCookies; // map of Cookies to waiting handshake
	Mona::DiffieHellman										_diffieHellman; // diffie hellman object used for the shared object building

	RTMFPSession*						_pSession; // Pointer to the main RTMFP session for assocation with new connections
	const std::string					_name; // name of the session (handshaker)
};
