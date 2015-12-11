
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
	virtual void addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable);

	const std::string				peerId; // Peer Id of the peer connected
	static Mona::UInt32				P2PSessionCounter; // Global counter for generating incremental P2P sessions id

	// Close the connection properly
	virtual void close() { FlowManager::close(); }

	// Connection the new socket to the server
	bool connect(Mona::Exception& ex);

	// Manage all handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle the first P2P responder handshake message (called by RTMFPConnection)
	void responderHandshake0(Mona::Exception& ex, const std::string& tag, Mona::UInt32 farId, const Mona::SocketAddress& address);

	// Handle the second P2P handshake message
	void responderHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the second P2P initiator handshake message
	void initiatorHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the third P2P initiator handshake message
	bool initiatorHandshake2(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Flush the connection
	// marker values can be :
	// - 0B for handshake
	// - 0A for raw response in P2P mode (only for responder)
	// - 8A for AMF responde in P2P mode (only for responder)
	// - 4A for acknowlegment in P2P mode (TODO: see if it is needed)
	virtual void				flush(bool echoTime, Mona::UInt8 marker = 0x89);

private:
	FlowManager&				_parent; // RTMFPConnection related to
	Mona::UInt32				_sessionId; // id of the P2P session;
	std::string					_farKey; // Key of the server/peer

	// Play/Publish command
	CommandType					_command;
	std::string					_streamName;
	bool						_audioReliable;
	bool						_videoReliable;
};
