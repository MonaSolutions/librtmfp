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
#include "SocketHandler.h"
#include <list>

/**************************************************
RTMFPSession represents a connection to the
RTMFP Server
*/
class NetGroup;
class RTMFPSession : public FlowManager {
public:
	RTMFPSession(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPSession();

	// Close the conection properly or abruptly if parameter is true
	virtual void close(bool abrupt);

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
	const Mona::UInt8*				rawId() { return _rawId; }

	// Return the server address (for NetGroup)
	const Mona::SocketAddress&		serverAddress() { FATAL_ASSERT(_pConnection) return _pConnection->address(); }

	// Return the pool buffer (for NetGroup)
	const Mona::PoolBuffers&		poolBuffers();

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
	
	// Handle stream creation
	bool handleStreamCreated(Mona::UInt16 idStream);

	// Handle data available or not event
	virtual void handleDataAvailable(bool isAvailable);

	// Handle a Writer close message (type 5E)
	virtual void handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter);

	// Handle a P2P address exchange message 0x0f from server (a peer is about to contact us)
	void handleP2PAddressExchange(Mona::PacketReader& reader);

	// Handle a new writer creation
	virtual void handleNewWriter(std::shared_ptr<RTMFPWriter>& pWriter);

	// On NetConnection.Connect.Success callback
	virtual void onConnect();

	// On NetStream.Publish.Start (only for NetConnection)
	virtual void onPublished(Mona::UInt16 streamId);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*	createSpecialFlow(Mona::Exception& ex, Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef);

	// Called when the server send us the ID of a peer in the NetGroup : connect to it
	void handleNewGroupPeer(const std::string& peerId);

private:

	// If there is at least one request of command : create the stream
	void createWaitingStreams();

	// Send waiting Connections (P2P or normal)
	void sendConnections();

	// Send handshake for group connection
	void sendGroupConnection(const std::string& netGroup);

	static Mona::UInt32												RTMFPSessionCounter; // Global counter for generating incremental sessions id

	std::unique_ptr<SocketHandler>									_pSocketHandler; // Socket handler object, manage the IO and contain all RTMFPConnection

	std::string														_port; // server port
	std::string														_host; // server host name
	std::set<std::string>											_waitingPeers; // queue of tag from waiting p2p connection request (initiators)
	std::deque<std::string>											_waitingGroup; // queue of waiting connections to groups
	std::mutex														_mutexConnections; // mutex for waiting connections (normal or p2p)
	std::map<std::string, std::shared_ptr<P2PSession>>				_mapPeersById; // P2P connections by Id

	std::string														_url; // RTMFP url of the application (base handshake)
	std::string														_rawUrl; // Header (420A) + Url to be sent in handshake 30
	Mona::UInt8														_rawId[PEER_ID_SIZE+2]; // my peer ID (computed with HMAC-SHA256) in binary format
	std::string														_peerTxtId; // my peer ID in hex format

	std::unique_ptr<Publisher>										_pPublisher; // Unique publisher used by connection & p2p

	std::shared_ptr<RTMFPWriter>									_pMainWriter; // Main writer for the connection
	std::shared_ptr<RTMFPWriter>									_pGroupWriter; // Writer for the group requests
	std::shared_ptr<NetGroup>										_group;


	FlashConnection::OnStreamCreated::Type							onStreamCreated; // Received when stream has been created and is waiting for a command
	FlashConnection::OnNewPeer::Type								onNewPeer; // Received when a we receive the ID of a new peer from the server in a NetGroup
	SocketHandler::OnPeerHandshake30::Type							onPeerHandshake30; // Received when a handshake 30 is received (P2P connection request)
	SocketHandler::OnPeerHandshake70::Type							onPeerHandshake70; // Received when a handshake 70 from an unknwon address is received (P2P connection answer)
	SocketHandler::OnNewPeerId::Type								onNewPeerId; // Received when a p2p initiator send us its peer id (we must create the P2PSession)
	SocketHandler::OnIdBuilt::Type									onPeerIdBuilt; // Received when we have built our peer id
	SocketHandler::OnConnection::Type								onConnection; // Received when a connection succeed
	SocketHandler::OnP2PAddresses::Type								onP2PAddresses; // Received P2P addresses have been received

	// Publish/Play commands
	struct StreamCommand : public Object {
		StreamCommand(CommandType t, const char* v, bool aReliable, bool vReliable) : type(t), value(v), audioReliable(aReliable), videoReliable(vReliable) {}

		CommandType		type;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
	};
	std::list<StreamCommand>										_waitingCommands;
	//std::recursive_mutex											_mutexCommands;
	Mona::UInt16													_nbCreateStreams; // Number of streams to create
};