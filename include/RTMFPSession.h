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
#include "Base/UDPSocket.h"
#include "RTMFPDecoder.h"
#include "RTMFPHandshaker.h"
#include "Publisher.h"
#include <queue>

/**************************************************
RTMFPSession represents a connection to the
RTMFP Server
*/
struct NetGroup;
struct RTMFPSession : public FlowManager {
	typedef Base::Event<void()>				ON(ConnectSucceed);
	typedef Base::Event<void(bool)>			ON(PublishP2P);
	typedef Base::Event<void()>				ON(Connected2Peer);
	typedef Base::Event<void()>				ON(StreamPublished);
	typedef Base::Event<void()>				ON(Connected2Group);

	RTMFPSession(Invoker& invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPSession();

	// Close the session (safe-threaded)
	void closeSession();

	// Return address of the server (cleared if not connected)
	const Base::SocketAddress&					address() { return _address; }

	// Return the socket object of the session
	virtual const std::shared_ptr<Base::Socket>&	socket(Base::IPAddress::Family family) { return ((family == Base::IPAddress::IPv4) ? socketIPV4 : socketIPV6).socket(); }

	// Connect to the specified url, return true if the command succeed
	bool connect(const std::string& url, const std::string& host, const Base::SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, std::shared_ptr<Base::Buffer>& rawUrl);

	// Connect to a peer with asking server for the addresses and start playing streamName
	// return : True if the peer has been added
	bool connect2Peer(const std::string& peerId, const std::string& streamName, Base::UInt16 mediaCount);

	// Connect to a peer (main function)
	// param delayed: if True we first try to connect directly to addresses and after 5s we start to contact the rendezvous service, if False we connect to all addresses
	bool connect2Peer(const std::string& peerId, const std::string& streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const Base::SocketAddress& hostAddress, Base::UInt16 mediaId=0);

	// Connect to the NetGroup with netGroup ID (in the form G:...)
	// return : True if the group has been added
	bool connect2Group(const std::string& streamName, RTMFPGroupConfig* parameters, bool audioReliable, bool videoReliable, const std::string& groupHex, const std::string& groupTxt, Base::UInt16 mediaCount);

	// Create a stream (play/publish) in the main stream 
	// return : True if the stream has been added
	bool addStream(bool publisher, const std::string& streamName, bool audioReliable, bool videoReliable, Base::UInt16 mediaCount);

	// Call a function of a server, peer or NetGroup
	// param peerId If set to 0 the call we be done to the server, if set to "all" to all the peers of a NetGroup, and to a peer otherwise
	// return 1 if the call succeed, 0 otherwise
	unsigned int callFunction(const std::string& function, std::queue<std::string>& arguments, const std::string& peerId);

	// Start a P2P publisher with name
	// return : True if the creation succeed, false otherwise (there is already a publisher)
	void startP2PPublisher(const std::string& streamName, bool audioReliable, bool videoReliable);

	// Close the publication
	// return : True if the publication has been closed, false otherwise (publication not found)
	bool closePublication(const char* streamName);

	// Called by Invoker every 50ms to manage connections (flush and ping)
	// return: False if the connection has failed, true otherwise
	bool manage();
		
	// Return listener if started successfully, otherwise NULL (only for RTMFP connection)
	template <typename ListenerType, typename... Args>
	ListenerType* startListening(Base::Exception& ex, const std::string& streamName, const std::string& peerId, Args... args) {
		if (!_pPublisher || _pPublisher->name() != streamName) {
			ex.set<Base::Ex::Application>("No publication found with name ", streamName);
			return NULL;
		}

		_pPublisher->start();
		return _pPublisher->addListener<ListenerType, Args...>(ex, peerId, args...);
	}

	// Remove the listener with peerId
	void stopListening(const std::string& peerId);

	// Set the p2p publisher as ready (used for blocking mode)
	void setP2pPublisherReady() { onPublishP2P(true); }

	// Set the p2p player as ready (used for blocking mode)
	void setP2PPlayReady() { onConnected2Peer(); }

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
	virtual const Base::Binary&		epd() { return *_rawUrl; }

	bool							isPublisher() { return (bool)_pPublisher; }

	// Called when when sending the handshake 38 to build the peer ID if we are RTMFPSession
	virtual void					buildPeerID(const Base::UInt8* data, Base::UInt32 size);

	// Called when we have received the handshake 38 and read peer ID of the far peer
	bool							onNewPeerId(const Base::SocketAddress& address, std::shared_ptr<Handshake>& pHandshake, Base::UInt32 farId, const std::string& peerId);

	// Remove the handshake properly
	virtual void					removeHandshake(std::shared_ptr<Handshake>& pHandshake);

	// Close the session properly or abruptly if parameter is true
	virtual void					close(bool abrupt);
	
	// Return the diffie hellman object (related to main session)
	virtual Base::DiffieHellman&	diffieHellman() { return _diffieHellman; }

	// Handle a decoded message
	void							receive(RTMFPDecoder::Decoded& decoded);

	// Called by NetGroup to close a peer connection and handshake of a non connected peer
	void							removePeer(const std::string& peerId);

	// Called by NetGroup when receiving a new address from a peer
	void							updatePeerAddress(const std::string& peerId, const Base::SocketAddress& address, RTMFP::AddressType type);

	/* Write functions */
	void writeAudio(const Base::Packet& packet, Base::UInt32 time);
	void writeVideo(const Base::Packet& packet, Base::UInt32 time);
	void writeFlush();

	FlashStream::OnMedia			onMediaPlay; // received when a packet from any media stream is ready for reading

protected:

	// Handle a Writer close message (type 5E)
	virtual void handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter);

	// Handle a P2P address exchange message 0x0f from server (a peer is about to contact us)
	void handleP2PAddressExchange(Base::BinaryReader& reader);

	// On NetConnection.Connect.Success callback
	virtual void onNetConnectionSuccess();

	// On NetStream.Publish.Start (only for NetConnection)
	virtual void onPublished(Base::UInt16 streamId);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*	createSpecialFlow(Base::Exception& ex, Base::UInt64 id, const std::string& signature, Base::UInt64 idWriterRef);

	// Called when the server send us the ID of a peer in the NetGroup : connect to it
	void handleNewGroupPeer(const std::string& rawId, const std::string& peerId);

	// Called when we are connected to the peer/server
	virtual void onConnection();

private:
	friend struct Invoker;

	// Send handshake for group connection
	void sendGroupConnection(const std::string& netGroup);

	static Base::UInt32												RTMFPSessionCounter; // Global counter for generating incremental sessions id

	int																_id; // RTMFPSession ID set by the Invoker
	RTMFPHandshaker													_handshaker; // Handshake manager

	std::string														_host; // server host name
	std::map<std::string, std::shared_ptr<P2PSession>>				_mapPeersById; // P2P connections by Id

	std::string														_url; // RTMFP url of the application (base handshake)
	std::shared_ptr<Base::Buffer>									_rawUrl; // Header (size + 0A) + Url to be sent in handshake 30
	std::string														_rawId; // my peer ID (computed with HMAC-SHA256) in binary format
	std::string														_peerTxtId; // my peer ID in hex format

	std::unique_ptr<Publisher>										_pPublisher; // Unique publisher used by connection & p2p

	std::shared_ptr<RTMFPWriter>									_pMainWriter; // Main writer for the connection
	std::shared_ptr<RTMFPWriter>									_pGroupWriter; // Writer for the group requests
	std::shared_ptr<NetGroup>										_group;

	std::map<Base::UInt32, FlowManager*>							_mapSessions; // map of session ID to Sessions

	Base::UDPSocket													socketIPV4; // Sending socket established with server
	Base::UDPSocket													socketIPV6; // Sending socket established with server

	Base::DiffieHellman												_diffieHellman; // diffie hellman object used for key computing

	Base::UInt16													_threadRcv; // Thread used to decode last message
		
	OnMediaEvent													_pOnMedia; // External Callback to link with parent

	// Publish/Play commands
	struct StreamCommand : public Object {
		StreamCommand(bool isPublisher, const std::string& value, Base::UInt16 id, bool aReliable, bool vReliable) : publisher(isPublisher), value(value), idMedia(id), audioReliable(aReliable), videoReliable(vReliable) {}

		bool			publisher;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
		Base::UInt16	idMedia; // id generated by the session
	};
	std::queue<StreamCommand>										_waitingStreams;
};
