
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

	// Connect to a peer of the RTMFP server (Direct P2P) and start playing streamName
	bool connect2Peer(Mona::Exception& ex, const char* peerId, const char* streamName);

	// Asynchronous read (buffered)
	// return false if end of buf has been reached
	bool read(Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Write media (netstream must be published)
	// return false if the client is not ready to publish, otherwise true
	bool write(const Mona::UInt8* buf, Mona::UInt32 size, int& pos);

	// Called by Invoker every second to manage connection (flush and ping)
	void manage();

	virtual Mona::UDPSocket& socket() { return *_pSocket; }

	// Add a command to the main stream (play/publish)
	virtual void addCommand(CommandType command, const char* streamName, bool audioReliable = false, bool videoReliable = false);
		
	// Return true if the stream exists, otherwise false (only for RTMFP connection)
	virtual bool getPublishStream(const std::string& streamName, bool& audioReliable, bool& videoReliable);

	Mona::Signal							connectSignal; // signal to wait connection

protected:

	// Send the connection message (after the answer of handshake1)
	virtual bool sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);
	
	// Handle stream creation (only for RTMFP connection)
	virtual void handleStreamCreated(Mona::UInt16 idStream);
	
	// Handle play request (only for P2PConnection)
	virtual void handlePlay(const std::string& streamName, FlashWriter& writer);

	// Handle a P2P address exchange message (Only for RTMFPConnection)
	virtual void handleP2PAddressExchange(Mona::Exception& ex, Mona::PacketReader& reader);

	// Handle message (after hanshake0)
	virtual void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer, const Mona::SocketAddress& address);

	// Manage handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Return the decoder engine for the following address (can be P2P or Normal connection)
	virtual RTMFPEngine*	getDecoder(Mona::UInt32 idStream, const Mona::SocketAddress& address);

	virtual void onConnect();

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
	void											sendConnections();

	bool															_waitConnect; // True if we are waiting for a normal connection request to be sent
	std::deque<std::string>											_waitingPeers; // queue of tag from waiting p2p connection request (initiators)
	std::recursive_mutex											_mutexConnections; // mutex for waiting connections (normal or p2p)

	std::map<Mona::SocketAddress, std::shared_ptr<P2PConnection>>	_mapPeersByAddress; // P2P connections by Address
	std::map<std::string, std::shared_ptr<P2PConnection>>			_mapPeersByTag; // Initiator connections waiting an answer (70 or 71)

	std::map<std::string, std::pair<bool,bool>>						_mapP2pPublications; // map of p2p stream publication names to their parameters

	std::string														_url; // RTMFP url of the application (base handshake)
	Mona::UInt8														_peerId[0x20]; // my peer ID (computed with HMAC-SHA256)

	std::unique_ptr<Mona::UDPSocket>								_pSocket; // Sending socket established with server

	// Publish/Play commands
	struct StreamCommand {
		StreamCommand(CommandType t, const char* v, bool aReliable, bool vReliable) : type(t), value(v), audioReliable(aReliable), videoReliable(vReliable) {}

		CommandType		type;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
	};
	std::deque<StreamCommand>										_waitingCommands;
	std::recursive_mutex											_mutexCommands;
	Mona::UInt16													_nbCreateStreams; // Number of streams to create
};