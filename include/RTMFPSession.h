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

#include "P2PSession.h"
#include "Mona/HostEntry.h"
#include "RTMFPHandshaker.h"
#include <list>

/**************************************************
RTMFPSession represents a connection to the
RTMFP Server
*/
class NetGroup;
class RTMFPSession : public FlowManager {
public:
	RTMFPSession(Invoker& invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPSession();

	// Close the session (safe-threaded)
	void closeSession();

	// Return address of the server (cleared if not connected)
	const Mona::SocketAddress&			address() { return _address; }

	// Return the socket object of the session
	virtual Mona::UDPSocket&			socket(Mona::IPAddress::Family family) { return (family == Mona::IPAddress::IPv4) ? *_pSocket : *_pSocketIPV6; }

	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, const char* url, const char* host);

	// Connect to a peer with asking server for the addresses and start playing streamName
	void connect2Peer(const char* peerId, const char* streamName);

	// Connect to a peer (main function)
	void connect2Peer(const std::string& peerId, const char* streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const Mona::SocketAddress& hostAddress);

	// Connect to the NetGroup with netGroup ID (in the form G:...)
	void connect2Group(const char* streamName, RTMFPGroupConfig* parameters);

	// Asynchronous read (buffered)
	// return : False if the connection is not established, true otherwise
	bool read(const char* peerId, Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Write media (netstream must be published)
	// return false if the client is not ready to publish, otherwise true
	bool write(const Mona::UInt8* buf, Mona::UInt32 size, int& pos);

	// Call a function of a server, peer or NetGroup
	// param peerId If set to 0 the call we be done to the server, if set to "all" to all the peers of a NetGroup, and to a peer otherwise
	// return 1 if the call succeed, 0 otherwise
	unsigned int callFunction(const char* function, int nbArgs, const char** args, const char* peerId = 0);

	// Called by Invoker every second to manage connection (flush and ping)
	virtual void manage();

	// Add a command to the main stream (play/publish)
	virtual void addCommand(CommandType command, const char* streamName, bool audioReliable = false, bool videoReliable = false);
		
	// Return listener if started successfully, otherwise NULL (only for RTMFP connection)
	template <typename ListenerType, typename... Args>
	ListenerType* startListening(Mona::Exception& ex, const std::string& streamName, const std::string& peerId, Args... args) {
		if (!_pPublisher || _pPublisher->name() != streamName) {
			ex.set(Mona::Exception::APPLICATION, "No publication found with name ", streamName);
			return NULL;
		}

		_pPublisher->start();
		return _pPublisher->addListener<ListenerType, Args...>(ex, peerId, args...);
	}

	// Push the media packet to write into a file
	void pushMedia(const std::string& stream, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size, double lostRate, bool audio) { 
		Mona::PacketReader reader(data, size);
		onMedia(stream, time, reader, lostRate, audio); 
	}

	// Remove the listener with peerId
	void stopListening(const std::string& peerId);

	// Set the p2p publisher as ready (used for blocking mode)
	void setP2pPublisherReady() { p2pPublishSignal.set(); p2pPublishReady = true; }

	// Set the p2p player as ready (used for blocking mode)
	void setP2PPlayReady() { p2pPlaySignal.set(); p2pPlayReady = true; }

	// Called by P2PSession when we are connected to the peer
	bool addPeer2Group(const std::string& peerId);

	// Return the peer ID in text format
	const std::string&				peerId() { return _peerTxtId; }

	// Return the peer ID in bin format
	const std::string&				rawId() { return _rawId; }

	// Return the group Id in hexadecimal format
	const std::string&				groupIdHex();

	// Return the group Id in text format
	const std::string&				groupIdTxt();

	// Return the name of the session
	virtual const std::string&		name() { return _host; }

	// Return the raw url of the session (for RTMFPConnection)
	virtual const std::string&		epd() { return _rawUrl; }

	bool							isPublisher() { return (bool)_pPublisher; }

	void							setDataAvailable(bool isAvailable) { handleDataAvailable(isAvailable); }

	// Called when when sending the handshake 38 to build the peer ID if we are RTMFPSession
	virtual void					buildPeerID(const Mona::UInt8* data, Mona::UInt32 size);

	// Called when we have received the handshake 38 and read peer ID of the far peer
	bool							onNewPeerId(const Mona::SocketAddress& address, std::shared_ptr<Handshake>& pHandshake, Mona::UInt32 farId, const std::string& rawId, const std::string& peerId);

	// Remove the handshake properly
	virtual void					removeHandshake(std::shared_ptr<Handshake>& pHandshake);

	// Return the diffie hellman object (related to main session)
	virtual bool					diffieHellman(Mona::DiffieHellman* &pDh) { return _handshaker.diffieHellman(pDh); }

	// Close the session properly or abruptly if parameter is true
	virtual void					close(bool abrupt);

	// Blocking members (used for ffmpeg to wait for an event before exiting the function)
	Mona::Signal					connectSignal; // signal to wait connection
	Mona::Signal					p2pPublishSignal; // signal to wait p2p publish
	Mona::Signal					p2pPlaySignal; // signal to wait p2p publish
	Mona::Signal					publishSignal; // signal to wait publication
	Mona::Signal					readSignal; // signal to wait for asynchronous data
	bool							p2pPublishReady; // true if the p2p publisher is ready
	bool							p2pPlayReady; // true if the p2p player is ready
	bool							publishReady; // true if the publisher is ready
	bool							connectReady; // Ready if we have received the NetStream.Connect.Success event
	bool							dataAvailable; // true if there is asynchronous data available

protected:

	// Handle a writer closed (to release shared pointers)
	virtual void handleWriterClosed(std::shared_ptr<RTMFPWriter>& pWriter);
	
	// Handle stream creation
	bool handleStreamCreated(Mona::UInt16 idStream);

	// Handle data available or not event
	virtual void handleDataAvailable(bool isAvailable);

	// Handle a Writer close message (type 5E)
	virtual void handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter);

	// Handle a P2P address exchange message 0x0f from server (a peer is about to contact us)
	void handleP2PAddressExchange(Mona::PacketReader& reader);

	// On NetConnection.Connect.Success callback
	virtual void onNetConnectionSuccess();

	// On NetStream.Publish.Start (only for NetConnection)
	virtual void onPublished(Mona::UInt16 streamId);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*	createSpecialFlow(Mona::Exception& ex, Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef);

	// Called when the server send us the ID of a peer in the NetGroup : connect to it
	void handleNewGroupPeer(const std::string& rawId, const std::string& peerId);

	// Called when we are connected to the peer/server
	virtual void onConnection();

private:

	// If there is at least one request of command : create the stream
	void createWaitingStreams();

	// Send waiting Connections (P2P or normal)
	void sendConnections();

	// Send handshake for group connection
	void sendGroupConnection(const std::string& netGroup);

	static Mona::UInt32												RTMFPSessionCounter; // Global counter for generating incremental sessions id

	RTMFPHandshaker													_handshaker; // Handshake manager

	std::string														_host; // server host name
	std::deque<std::string>											_waitingGroup; // queue of waiting connections to groups
	std::mutex														_mutexConnections; // mutex for waiting connections (normal or p2p)
	std::map<std::string, std::shared_ptr<P2PSession>>				_mapPeersById; // P2P connections by Id

	std::string														_url; // RTMFP url of the application (base handshake)
	std::string														_rawUrl; // Header (420A) + Url to be sent in handshake 30
	std::string														_rawId; // my peer ID (computed with HMAC-SHA256) in binary format
	std::string														_peerTxtId; // my peer ID in hex format

	std::unique_ptr<Publisher>										_pPublisher; // Unique publisher used by connection & p2p

	std::shared_ptr<RTMFPWriter>									_pMainWriter; // Main writer for the connection
	std::shared_ptr<RTMFPWriter>									_pGroupWriter; // Writer for the group requests
	std::shared_ptr<NetGroup>										_group;

	std::map<Mona::UInt32, FlowManager*>							_mapSessions; // map of session ID to Sessions

	std::unique_ptr<Mona::UDPSocket>								_pSocket; // Sending socket established with server
	std::unique_ptr<Mona::UDPSocket>								_pSocketIPV6; // Sending socket established with server

	FlashConnection::OnStreamCreated::Type							onStreamCreated; // Received when stream has been created and is waiting for a command
	FlashConnection::OnNewPeer::Type								onNewPeer; // Received when a we receive the ID of a new peer from the server in a NetGroup
	Mona::UDPSocket::OnPacket::Type									onPacket; // Main input event, received on each raw packet
	Mona::UDPSocket::OnError::Type									onError; // Main input event, received on socket error

	// Publish/Play commands
	struct StreamCommand : public Object {
		StreamCommand(CommandType t, const char* v, bool aReliable, bool vReliable) : type(t), value(v), audioReliable(aReliable), videoReliable(vReliable) {}

		CommandType		type;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
	};
	std::list<StreamCommand>										_waitingCommands;
	Mona::UInt16													_nbCreateStreams; // Number of streams to create
};
