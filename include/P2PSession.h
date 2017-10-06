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

#include "FlowManager.h"
#include "PeerMedia.h"
#include "RTMFP.h"

struct RTMFPSession;
struct RTMFPGroupConfig;

/**************************************************
P2PSession represents a direct P2P connection 
with another peer
*/
struct P2PSession : FlowManager, virtual Base::Object {
	typedef Base::Event<void(P2PSession*, Base::BinaryReader&, bool)>																		ON(PeerGroupReport); // called when receiving a Group Report message from the peer
	typedef Base::Event<bool(const std::string&, std::shared_ptr<PeerMedia>&, const std::string&, const std::string&, Base::BinaryReader&)> ON(NewMedia); // called when a new PeerMedia is called (new stream available for the peer)
	typedef Base::Event<void(const std::string&, Base::UInt64)>																				ON(ClosedMedia); // called when the peer publisher close the GroupMedia
	typedef Base::Event<void(P2PSession*)>																									ON(PeerGroupBegin); // called when receiving a Group Begin message from the peer
	typedef Base::Event<void(const std::string&)>																							ON(PeerClose); // called when the peer is closing
	typedef Base::Event<bool(const std::string&)>																							ON(PeerGroupAskClose); // called when a peer ask to close its session (return True to accept closing)

	P2PSession(RTMFPSession* parent, std::string id, Invoker& invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, 
		const Base::SocketAddress& host, bool responder, bool group, Base::UInt16 mediaId=0);

	virtual ~P2PSession();

	// Set the stream name to send play command when connected
	void setStreamName(const std::string& streamName) { _streamName = streamName; }

	// Set the tag used for this connection (responder mode)
	void							setTag(const std::string& tag) { _tag = tag; }

	// Call a function on the peer side
	// return 0 if it fails, 1 otherwise
	unsigned int					callFunction(const std::string& function, std::queue<std::string>& arguments);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*				createSpecialFlow(Base::Exception& ex, Base::UInt64 id, const std::string& signature, Base::UInt64 idWriterRef);

	// Close the group writers but keep the connection open if full is false
	virtual void					close(bool abrupt);

	// Return the name of the session
	virtual const std::string&		name() { return peerId; }

	// Return the raw peerId of the session (for RTMFPConnection)
	virtual const Base::Binary&		epd() { return rawId; }

	// Return the known addresses of the peer (for RTMFPSession)
	const PEER_LIST_ADDRESS_TYPE&	addresses() { return _knownAddresses; }
	
	// Return the socket object of the session
	virtual const std::shared_ptr<Base::Socket>&		socket(Base::IPAddress::Family family);

	std::shared_ptr<Handshake>&		handshake() { return _pHandshake; }

	void							setAddress(const Base::SocketAddress& address) { _address = address; }

	// Called when receiving handshake 38 to decide if answering
	bool							onHandshake38(const Base::SocketAddress& address, std::shared_ptr<Handshake>& pHandshake);

	/*** NetGroup functions ***/

	// Write the Group publication infos
	std::shared_ptr<PeerMedia>&		getPeerMedia(const std::string& streamKey);

	// Send the group report (message 0A)
	void							sendGroupReport(const Base::UInt8* data, Base::UInt32 size);

	// Send the group begin message (02 + 0E messages), return true if the message has been sent
	bool							sendGroupBegin();

	// Send the Group Peer Connect request
	void							sendGroupPeerConnect();

	// called by a PeerMedia to create the media writer
	bool							createMediaWriter(std::shared_ptr<RTMFPWriter>& pWriter, Base::UInt64 flowIdRef);

	// Ask a peer from the group to disconnect
	// return : True if the request has been sent
	bool							askPeer2Disconnect();

	// Manage the flows
	virtual bool					manage() { return FlowManager::manage(); }
	
	// Remove the handshake properly
	virtual void					removeHandshake(std::shared_ptr<Handshake>& pHandshake);

	// Return the diffie hellman object (related to main session)
	virtual Base::DiffieHellman&	diffieHellman();
	
	// Add host or address when receiving address
	// Update handhsake if present
	virtual void					addAddress(const Base::SocketAddress& address, RTMFP::AddressType type);

	/*** Public members ***/

	Base::Buffer					rawId; // Peer Id in binary format + header (210f)
	std::string						peerId; // Peer Id of the peer connected
	Base::SocketAddress				hostAddress; // Host address (server address)

	// NetGroup members
	bool							groupFirstReportSent; // True if the first group report has been sent
	bool							groupReportInitiator; // True if we are the initiator of last Group Report (to avoid endless exchanges)

protected:

	// Handle play request (only for P2PSession)
	virtual bool					handlePlay(const std::string& streamName, Base::UInt16 streamId, Base::UInt64 flowId, double cbHandler);

	// Handle a Writer close message (type 5E)
	virtual void					handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter);

	// Called when we are connected to the peer/server
	virtual void					onConnection();

private:

	// Handle a NetGroup connection message from a peer connected (only for P2PSession)
	bool							handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id);

	// Build the group connection key (after connection suceed)
	void							buildGroupKey();

	static Base::UInt32										P2PSessionCounter; // Global counter for generating incremental P2P sessions id
	RTMFPSession*											_parent; // RTMFPConnection related to
	PEER_LIST_ADDRESS_TYPE									_knownAddresses; // list of known addresses of the peer/server
	std::string												_streamName; // playing stream name
	Base::UInt16											_peerMediaId; // playing Id (if P2P direct player)

	// Group members
	std::shared_ptr<Base::Buffer>							_groupConnectKey; // Encrypted key used to connect to the peer
	std::shared_ptr<Base::Buffer>							_groupExpectedKey; // Encrypted key expected from far peer
	bool													_groupConnectSent; // True if group connection request has been sent to peer
	bool													_groupBeginSent; // True if the group messages 02 + 0E have been sent
	bool													_isGroup; // True if this peer connection it part of a NetGroup
	Base::Time												_lastTryDisconnect; // Last time we ask peer to disconnect

	std::shared_ptr<RTMFPWriter>							_pReportWriter; // Writer for report messages
	std::shared_ptr<RTMFPWriter>							_pNetStreamWriter; // Writer for NetStream P2P direct messages

	std::map<Base::UInt64, std::shared_ptr<PeerMedia>>		_mapWriter2PeerMedia; // map of writer id to peer media
	std::map<std::string, std::shared_ptr<PeerMedia>>		_mapStream2PeerMedia; // map of stream key to peer media
	std::map<Base::UInt64, std::shared_ptr<PeerMedia>>		_mapFlow2PeerMedia; // map of flow id to peer media
};
