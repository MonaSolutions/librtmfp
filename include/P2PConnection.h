
#pragma once

#include "FlowManager.h"
#include "Mona/StopWatch.h"

#define COOKIE_SIZE	0x40

/**************************************************
P2PConnection represents a direct P2P connection 
with another peer
*/
class RTMFPConnection;
class NetGroup;
class FlashListener;
class P2PConnection : public FlowManager,
	public FlashEvents::OnGroupMedia,
	public FlashEvents::OnGroupReport,
	public FlashEvents::OnGroupPlayPush,
	public FlashEvents::OnGroupPlayPull,
	public FlashEvents::OnFragmentsMap,
	public FlashEvents::OnGroupBegin,
	public FlashEvents::OnFragment {
	friend class RTMFPConnection;
public:
	P2PConnection(RTMFPConnection* parent, std::string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const Mona::SocketAddress& hostAddress, const Mona::Buffer& pubKey, bool responder);

	virtual ~P2PConnection();

	virtual Mona::UDPSocket&	socket();

	// Add a command to the main stream (play/publish/netgroup)
	virtual void addCommand(CommandType command, const char* streamName, bool audioReliable = false, bool videoReliable = false);

	// Set the tag used for this connection (responder mode)
	void setTag(const std::string& tag) { _tag = tag; }

	// Update the Group Fragments map
	void updateFragmentsMap(Mona::UInt64 id, const Mona::UInt8* data, Mona::UInt32 size);

	// Return True if bit number is available in the fragments map (for push out mode)
	bool checkMask(Mona::UInt8 bitNumber);

	// Set the group
	void setGroup(std::shared_ptr<NetGroup> group) { _group = group; }
	void resetGroup() { _group.reset(); }

	// Return the tag used for this p2p connection (initiator mode)
	const std::string&	getTag() { return _tag; }

	const Mona::SocketAddress& peerAddress() { return _outAddress; }

	// Manage all handshake messages (marker 0x0B)
	virtual void manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle the first P2P responder handshake message (called by RTMFPConnection)
	void responderHandshake0(Mona::Exception& ex, std::string tag, const Mona::SocketAddress& address);

	// Handle the second P2P responder handshake message
	void responderHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the second P2P initiator handshake message in a middle mode (local)
	void initiatorHandshake70(Mona::Exception& ex, Mona::BinaryReader& reader, const Mona::SocketAddress& address);

	// Send the third P2P initiator handshake message
	bool initiatorHandshake2(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Write the Group publication infos
	void sendGroupMedia(const std::string& stream, const Mona::UInt8* data, Mona::UInt32 size);

	// Send the group report (message 0A)
	void sendGroupReport(const Mona::UInt8* data, Mona::UInt32 size);

	// If packet is pushable : create the flow if necessary and send media
	void sendMedia(const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt64 fragment, bool pull=false);

	// Send the report message
	void sendFragmentsMap(const Mona::UInt8* data, Mona::UInt32 size);

	// Set the Group Publish Push mode (after a message 23)
	void setPushMode(Mona::UInt8 mode);

	// Update the Group Play Push mode
	void sendPushMode(Mona::UInt8 mode);

	// Send the group begin message (02 + 0E messages)
	void sendGroupBegin();

	// Send the UnpublishNotify and closeStream messages
	//void closeGroupStream(Mona::UInt8 type, Mona::UInt64 fragmentCounter, Mona::UInt32 lastTime);

	// Flush the connection
	// marker values can be :
	// - 0B for handshake
	// - 0A for raw response in P2P mode (only for responder)
	// - 8A for AMF responde in P2P mode (only for responder)
	// - 4A for acknowlegment in P2P mode (TODO: see if it is needed)
	virtual void				flush(bool echoTime, Mona::UInt8 marker);

	virtual void				initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*			createSpecialFlow(Mona::UInt64 id, const std::string& signature);

	// Does the connection is terminated? => can be deleted by parent
	bool						consumed() { return _died; }

	Mona::UInt8						attempt; // Number of try to contact the responder (only for initiator)
	Mona::Stopwatch					lastTry; // Last time handshake 30 has been sent to the server (only for initiator)

	std::string						peerId; // Peer Id of the peer connected
	static Mona::UInt32				P2PSessionCounter; // Global counter for generating incremental P2P sessions id

	bool							publicationInfosSent; // True if it is the publisher and if the publications infos have been sent
	Mona::UInt64					lastGroupReport; // Time in msec of First Group report received

	Mona::UInt8						pushInMode; // Group Play Push mode

protected:
	// Handle play request (only for P2PConnection)
	virtual bool				handlePlay(const std::string& streamName, FlashWriter& writer);

	// Handle a NetGroup connection message from a peer connected (only for P2PConnection)
	virtual void				handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id);

	// Handle a P2P address exchange message (Only for RTMFPConnection)
	virtual void				handleP2PAddressExchange(Mona::Exception& ex, Mona::PacketReader& reader);
	
	// Close the conection properly
	virtual void				close();

private:
	// Return true if the new fragment is pushable (according to the Group push mode)
	bool						isPushable(Mona::UInt8 rest);

	RTMFPConnection*			_parent; // RTMFPConnection related to
	FlashListener*				_pListener; // Listener of the main publication (only one by intance)
	Mona::UInt32				_sessionId; // id of the P2P session;
	std::string					_farKey; // Key of the server/peer
	std::string					_farNonce; // Nonce of the distant peer

	// Play/Publish command
	std::string					_streamName; // playing stream name
	bool						_responder; // is responder?

	bool						_rawResponse; // next message is a raw response? TODO: make it nicer

	// Group members
	bool						_groupConnectSent; // True if group connection request has been sent to peer
	bool						_groupBeginSent; // True if the group messages 02 + 0E have been sent
	std::shared_ptr<NetGroup>	_group; // Group pointer if netgroup connection

	Mona::UInt8					_pushOutMode; // Group Publish Push mode

	RTMFPFlow*					_pMediaFlow; // Flow for media packets
	RTMFPFlow*					_pFragmentsFlow; // Flow for fragments Map messages and media related messages
	RTMFPFlow*					_pReportFlow; // Flow for report messages

	Mona::Buffer				_fragmentsMap; // Last Fragments Map received
	Mona::UInt64				_idFragmentMap; // Last Fragments Map id
};
