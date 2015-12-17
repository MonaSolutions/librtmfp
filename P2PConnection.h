
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
	P2PConnection(FlowManager& parent, std::string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const Mona::SocketAddress& hostAddress, const Mona::Buffer& pubKey, const Mona::Buffer& tag);

	virtual ~P2PConnection() {
		close();
	}

	virtual Mona::UDPSocket&	socket() { return (!_pSocket) ? _parent.socket() : FlowManager::socket(); }

	// Add a command to the main stream (play/publish)
	virtual void addCommand(CommandType command, const char* streamName, bool audioReliable=false, bool videoReliable=false);

	// Return true if the stream exists, otherwise false (only for RTMFP connection)
	virtual bool getPublishStream(const std::string& streamName, bool& audioReliable, bool& videoReliable);

	const std::string				peerId; // Peer Id of the peer connected
	static Mona::UInt32				P2PSessionCounter; // Global counter for generating incremental P2P sessions id

	// Close the connection properly
	virtual void close() { FlowManager::close(); }

	// Connection the new socket to the server
	bool connect(Mona::Exception& ex);

	// Bind the peer address for new packets
	bool bind(Mona::Exception& ex, const Mona::SocketAddress& address);

	// Manage all handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle the first P2P responder handshake message (called by RTMFPConnection)
	void responderHandshake0(Mona::Exception& ex, Mona::BinaryReader& reader, Mona::UInt32 farId, const Mona::SocketAddress& address);

	// Handle the second P2P responder handshake message
	void responderHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the second P2P initiator handshake message in a middle mode (local)
	void initiatorHandshake70(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the second P2P initiator handshake message in a middle mode (local)
	void initiatorHandshake71(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the third P2P initiator handshake message
	bool initiatorHandshake2(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Flush the connection
	// marker values can be :
	// - 0B for handshake
	// - 0A for raw response in P2P mode (only for responder)
	// - 8A for AMF responde in P2P mode (only for responder)
	// - 4A for acknowlegment in P2P mode (TODO: see if it is needed)
	virtual void				flush(bool echoTime, Mona::UInt8 marker);

protected:
	// Handle stream creation (only for RTMFP connection)
	virtual void				handleStreamCreated(Mona::UInt16 idStream);

	// Handle play request (only for P2PConnection)
	virtual void				handlePlay(const std::string& streamName, FlashWriter& writer);

	// Handle a P2P address exchange message (Only for P2PConnection)
	virtual void				handleP2PAddressExchange(Mona::Exception& ex, Mona::PacketReader& reader);

private:
	FlowManager&				_parent; // RTMFPConnection related to
	Mona::UInt32				_sessionId; // id of the P2P session;
	std::string					_farKey; // Key of the server/peer

	// Play/Publish command
	std::string					_streamName;
};
