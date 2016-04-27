
#pragma once

#include "P2PConnection.h"
#include <list>

/**************************************************
RTMFPConnection represents a connection to the
RTMFP Server
*/
class NetGroup;
class RTMFPConnection : public FlowManager {
public:
	RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPConnection();

	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, const char* url, const char* host);

	// Connect to a peer of the RTMFP server (Direct P2P) and start playing streamName
	void connect2Peer(const char* peerId, const char* streamName);

	// Connect to the NetGroup with netGroup ID (in the form G:...)
	void connect2Group(const char* netGroup, const char* streamName, bool publisher, double availabilityUpdatePeriod, Mona::UInt16 windowDuration);

	// Asynchronous read (buffered)
	// return false if end of buf has been reached
	bool read(const char* peerId, Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Write media (netstream must be published)
	// return false if the client is not ready to publish, otherwise true
	bool write(const Mona::UInt8* buf, Mona::UInt32 size, int& pos);

	// Called by Invoker every second to manage connection (flush and ping)
	void manage();

	virtual Mona::UDPSocket& socket() { return *_pSocket; }

	// Add a command to the main stream (play/publish)
	virtual void addCommand(CommandType command, const char* streamName, bool audioReliable = false, bool videoReliable = false);
		
	// Return listener if started successfully, otherwise NULL (only for RTMFP connection)
	template <typename ListenerType, typename... Args>
	ListenerType* startListening(Mona::Exception& ex, const std::string& streamName, const std::string& peerId, Args... args) {
		if (!_pPublisher || _pPublisher->name() != streamName) {
			ex.set(Exception::APPLICATION, "No publication found with name ", streamName);
			return NULL;
		}

		_pPublisher->start();
		return _pPublisher->addListener<ListenerType, Args...>(ex, peerId, args...);
	}

	// Remove the listener with peerId
	void stopListening(const std::string& peerId);

	// Set the p2p publisher as ready (used for blocking mode)
	void setP2pPublisherReady() { p2pPublishSignal.set(); p2pPublishReady = true; }

	// Called by P2PConnection when the responder receive the caller peerId to update the group if needed
	void updatePeerId(const Mona::SocketAddress& peerAddress, const std::string& peerId);

	// Return the peer ID (for p2p childs)
	virtual Mona::UInt8* peerId() { return _peerId; }

	// Blocking members (used for ffmpeg to wait for an event before exiting the function)
	Mona::Signal							connectSignal; // signal to wait connection
	Mona::Signal							p2pPublishSignal; // signal to wait p2p publish
	Mona::Signal							publishSignal; // signal to wait publication
	bool									p2pPublishReady; // true if the p2p publisher is ready
	bool									publishReady; // true if the publisher is ready
	bool									connectReady; // Ready if we have received the NetStream.Connect.Success event

protected:

	// Send the connection message (after the answer of handshake1)
	virtual bool sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);
	
	// Handle stream creation
	void handleStreamCreated(Mona::UInt16 idStream);
	
	// Handle play request (only for P2PConnection)
	virtual bool handlePlay(const std::string& streamName, FlashWriter& writer);

	// Handle new peer in a Netgroup : connect to the peer
	void handleNewGroupPeer(const std::string& groupId, const std::string& peerId);

	// Handle a NetGroup connection message from a peer connected (only for P2PConnection)
	virtual void handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id);

	// Handle a P2P address exchange message
	void handleP2PAddressExchange(Mona::Exception& ex, Mona::PacketReader& reader);

	// Handle message (after hanshake0)
	virtual void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer, const Mona::SocketAddress& address);

	// Manage handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Return the decoder engine for the following address (can be P2P or Normal connection)
	virtual RTMFPEngine*	getDecoder(Mona::UInt32 idStream, const Mona::SocketAddress& address);

	// On NetConnection success callback
	virtual bool onConnect(Mona::Exception& ex);

	// On NetStream.Publish.Start (only for NetConnection)
	virtual void onPublished(FlashWriter& writer);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*			createSpecialFlow(Mona::UInt64 id, const std::string& signature);

private:

	// Finish the initiated p2p connection (when handshake 70 is received)
	bool handleP2PHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the p2p requests to each available address
	// TODO: see if we need to implement it
	bool sendP2pRequests(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the second handshake message
	void sendHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle the first P2P responder handshake message (P2P connection initiated by peer)
	void responderHandshake0(Mona::Exception& ex, Mona::BinaryReader& reader);

	// If there is at least one request of command : create the stream
	void createWaitingStreams();

	// Send waiting Connections (P2P or normal)
	void sendConnections();

	// Send handshake for group connection
	void sendGroupConnection(const std::string& netGroup);

	bool															_waitConnect; // True if we are waiting for a normal connection request to be sent
	std::deque<std::string>											_waitingPeers; // queue of tag from waiting p2p connection request (initiators)
	std::deque<std::string>											_waitingGroup; // queue of waiting connections to groups
	std::recursive_mutex											_mutexConnections; // mutex for waiting connections (normal or p2p)

	std::map<Mona::SocketAddress, std::shared_ptr<P2PConnection>>	_mapPeersByAddress; // P2P connections by Address
	std::map<std::string, std::shared_ptr<P2PConnection>>			_mapPeersByTag; // Initiator connections waiting an answer (70 or 71)

	std::string														_url; // RTMFP url of the application (base handshake)
	Mona::UInt8														_peerId[0x20]; // my peer ID (computed with HMAC-SHA256)

	std::unique_ptr<Mona::UDPSocket>								_pSocket; // Sending socket established with server
	std::unique_ptr<Publisher>										_pPublisher; // Unique publisher used by connection & p2p
	FlashListener*													_pListener; // Listener of the main publication (only one by intance)

	std::shared_ptr<NetGroup>										_group;


	FlashConnection::OnStreamCreated::Type							onStreamCreated; // Received when stream has been created and is waiting for a command
	FlashConnection::OnNewPeer::Type								onNewPeer; // Received when a we receive the ID of a new peer in a NetGroup

	// Publish/Play commands
	struct StreamCommand {
		StreamCommand(CommandType t, const char* v, bool aReliable, bool vReliable) : type(t), value(v), audioReliable(aReliable), videoReliable(vReliable) {}

		CommandType		type;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
	};
	std::list<StreamCommand>										_waitingCommands;
	std::recursive_mutex											_mutexCommands;
	Mona::UInt16													_nbCreateStreams; // Number of streams to create
};