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

#include "Connection.h"

class FlowManager;

namespace ConnectionEvents {
	struct OnMessage : Mona::Event<void(Mona::BinaryReader&)> {}; // called when we receive an RTMFP message
	struct OnIdBuilt : Mona::Event<void(const std::string&, const std::string&)> {}; // called when our Peer ID has been built
	struct OnConnected : Mona::Event<void(const Mona::SocketAddress&, const std::string&)> {}; // called when the connection is ready
};

/**************************************************
RTMFPConnection is an RTMFP connection to
a socket address (can be P2P or normal)
It realizes the RTMFP handshake and dispatch
decoded packets to the listener
*/
class RTMFPConnection : public Connection,
	public ConnectionEvents::OnMessage,
	public ConnectionEvents::OnIdBuilt,
	public ConnectionEvents::OnConnected {
public:
	RTMFPConnection(const Mona::SocketAddress& address, SocketHandler* pHandler, FlowManager* session, bool responder, bool p2p);

	~RTMFPConnection();

	// Manage the connection (send waiting requests)
	virtual void manage();

	// Close the connection properly
	virtual void close();

	// Change the session (used by RTMFPSession to set the P2PSession after creation)
	void setSession(FlowManager* pSession) { _pSession = pSession; }

	// Send the 2nd handshake request
	void							sendHandshake38(const std::string& farKey, const std::string& cookie);

	// Send the first handshake response (only in P2P mode)
	void							sendHandshake70(const std::string& tag);

	// Return the shared secret (used by P2PSession to calculate the group key)
	const Mona::Buffer&				sharedSecret() { return _sharedSecret; }

	// Return the far nonce (used by P2PSession to calculate the group key)
	const Mona::Buffer&				farNonce() { return _farNonce; }

protected:

	// Handle message received
	virtual void					handleMessage(const Mona::PoolBuffer& pBuffer);

	// Flush the connection
	// marker is : 0B for handshake, 09 for raw request, 89 for AMF request
	virtual void					flush(bool echoTime, Mona::UInt8 marker);

private:

	// Compute keys and init encoder and decoder
	bool							computeKeys(const std::string& farPubKey, const Mona::Buffer& initiatorNonce, const Mona::UInt8* responderNonce, Mona::UInt32 responderNonceSize, Mona::Buffer& sharedSecret);

	// Manage handshake messages (marker 0x0B)
	virtual void					manageHandshake(Mona::BinaryReader& reader);

	// Handle the handshake 30 (p2p concurrent connection)
	void							handleHandshake30(Mona::BinaryReader& reader);

	// Send the 2nd handshake response (only in P2P mode)
	void							sendHandshake78(Mona::BinaryReader& reader);

	// Handle the handshake 70 (from peer or server)
	void							handleHandshake70(Mona::BinaryReader& reader);

	// Read the 2nd handshake response and notify the session for the connection
	void							sendConnect(Mona::BinaryReader& reader);

	// Read the redirection addresses and send new handshake 30 if not connected
	void							handleRedirection(Mona::BinaryReader& reader);

	bool													_responder; // True if this is a responder connection
	bool													_isP2P; // True if this is a P2P connection

	Mona::UInt8												_connectAttempt; // Counter of connection attempts to the server
	Mona::Time												_lastAttempt; // Last attempt to connect to the server

	Mona::Buffer											_sharedSecret; // shared secret for crypted communication
	std::string												_farKey; // Far public key
	Mona::Buffer											_pubKey; // Our public key
	Mona::Buffer											_farNonce; // Far nonce
	Mona::Buffer											_nonce; // Our Nonce for key exchange, can be of size 0x4C or 0x49 for responder

	FlowManager*											_pSession; // Pointer to the session (normal or p2p)

	std::recursive_mutex									_mutexConnections; // mutex for waiting p2p connections*/
};