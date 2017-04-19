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
#include "Mona/UDPSocket.h"
#include "RTMFPDecoder.h"
#include "RTMFPHandshaker.h"
#include "Publisher.h"
#include <queue>

/**************************************************
RTMFPSession represents a connection to the
RTMFP Server
*/
struct NetGroup;
class RTMFPSession : public FlowManager {
public:
	RTMFPSession(Invoker& invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~RTMFPSession();

	// Close the session (safe-threaded)
	void closeSession();

	// Return address of the server (cleared if not connected)
	const Mona::SocketAddress&					address() { return _address; }

	// Return the socket object of the session
	virtual const std::shared_ptr<Mona::Socket>&	socket(Mona::IPAddress::Family family) { return ((family == Mona::IPAddress::IPv4) ? _pSocket : _pSocketIPV6)->socket(); }

	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, const char* url, const char* host);

	// Connect to a peer with asking server for the addresses and start playing streamName
	// return : the id of the media created
	Mona::UInt16 connect2Peer(const char* peerId, const char* streamName);

	// Connect to a peer (main function)
	bool connect2Peer(const std::string& peerId, const char* streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const Mona::SocketAddress& hostAddress, Mona::UInt16 mediaId=0);

	// Connect to the NetGroup with netGroup ID (in the form G:...)
	// return : the id of the media created
	Mona::UInt16 connect2Group(const char* streamName, RTMFPGroupConfig* parameters);

	// Create a stream (play/publish) in the main stream 
	// return : the id of the media created
	Mona::UInt16 addStream(bool publisher, const char* streamName, bool audioReliable = false, bool videoReliable = false);

	// Asynchronous read (buffered)
	// return : False if an error occurs, true otherwise
	bool read(Mona::UInt16 mediaId, Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Write media (netstream must be published)
	// return false if the client is not ready to publish, otherwise true
	bool write(const Mona::UInt8* data, Mona::UInt32 size, int& pos);

	// Call a function of a server, peer or NetGroup
	// param peerId If set to 0 the call we be done to the server, if set to "all" to all the peers of a NetGroup, and to a peer otherwise
	// return 1 if the call succeed, 0 otherwise
	unsigned int callFunction(const char* function, int nbArgs, const char** args, const char* peerId = 0);

	// Start a P2P publisher with name
	// return : True if the creation succeed, false otherwise (there is already a publisher)
	bool startP2PPublisher(const char* streamName, bool audioReliable = false, bool videoReliable = false);

	// Close the publication
	// return : True if the publication has been closed, false otherwise (publication not found)
	bool closePublication(const char* streamName);

	// Called by Invoker every 50ms to manage connections (flush and ping)
	virtual void manage();
		
	// Return listener if started successfully, otherwise NULL (only for RTMFP connection)
	template <typename ListenerType, typename... Args>
	ListenerType* startListening(Mona::Exception& ex, const std::string& streamName, const std::string& peerId, Args... args) {
		if (!_pPublisher || _pPublisher->name() != streamName) {
			ex.set<Mona::Ex::Application>("No publication found with name ", streamName);
			return NULL;
		}

		_pPublisher->start();
		return _pPublisher->addListener<ListenerType, Args...>(ex, peerId, args...);
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
	virtual const Mona::Binary&		epd() { return _rawUrl; }

	bool							isPublisher() { return (bool)_pPublisher; }

	void							setDataAvailable(bool isAvailable) { handleDataAvailable(isAvailable); }

	// Called when when sending the handshake 38 to build the peer ID if we are RTMFPSession
	virtual void					buildPeerID(const Mona::UInt8* data, Mona::UInt32 size);

	// Called when we have received the handshake 38 and read peer ID of the far peer
	bool							onNewPeerId(const Mona::SocketAddress& address, std::shared_ptr<Handshake>& pHandshake, Mona::UInt32 farId, const std::string& rawId, const std::string& peerId);

	// Remove the handshake properly
	virtual void					removeHandshake(std::shared_ptr<Handshake>& pHandshake);

	// Close the session properly or abruptly if parameter is true
	virtual void					close(bool abrupt);
	
	// Return the diffie hellman object (related to main session)
	virtual Mona::DiffieHellman&	diffieHellman() { return _diffieHellman; }

	const RTMFPDecoder::OnDecoded&	getDecodeEvent() { return _onDecoded; }

	FlashStream::OnMedia			onMediaPlay; // received when a packet from any media stream is ready for reading

	// Blocking members (used for ffmpeg to wait for an event before exiting the function)
	Mona::Signal					connectSignal; // signal to wait connection
	Mona::Signal					p2pPublishSignal; // signal to wait p2p publish
	Mona::Signal					p2pPlaySignal; // signal to wait p2p publish
	Mona::Signal					publishSignal; // signal to wait publication
	Mona::Signal					readSignal; // signal to wait for asynchronous data
	std::atomic<bool>				p2pPublishReady; // true if the p2p publisher is ready
	std::atomic<bool>				p2pPlayReady; // true if the p2p player is ready
	std::atomic<bool>				publishReady; // true if the publisher is ready
	std::atomic<bool>				connectReady; // Ready if we have received the NetStream.Connect.Success event
	std::atomic<bool>				dataAvailable; // true if there is asynchronous data available

	// Publishing structures
	struct MediaPacket : virtual Mona::Object, Mona::Packet {
		MediaPacket(Mona::UInt32 time, const Mona::Packet& packet) : time(time), Mona::Packet(std::move(packet)) {}
		const Mona::UInt32 time;
	};
	typedef Mona::Event<void(MediaPacket&)> ON(PushAudio);
	typedef Mona::Event<void(MediaPacket&)> ON(PushVideo);
	typedef Mona::Event<void()>				ON(FlushPublisher);

protected:

	// Handle data available or not event
	void handleDataAvailable(bool isAvailable);

	// Handle a Writer close message (type 5E)
	virtual void handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter);

	// Handle a P2P address exchange message 0x0f from server (a peer is about to contact us)
	void handleP2PAddressExchange(Mona::BinaryReader& reader);

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
	// return : True if a stream has been created
	bool createWaitingStreams();

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
	Mona::Buffer													_rawUrl; // Header (size + 0A) + Url to be sent in handshake 30
	std::string														_rawId; // my peer ID (computed with HMAC-SHA256) in binary format
	std::string														_peerTxtId; // my peer ID in hex format

	std::unique_ptr<Publisher>										_pPublisher; // Unique publisher used by connection & p2p

	std::shared_ptr<RTMFPWriter>									_pMainWriter; // Main writer for the connection
	std::shared_ptr<RTMFPWriter>									_pGroupWriter; // Writer for the group requests
	std::shared_ptr<NetGroup>										_group;

	std::map<Mona::UInt32, FlowManager*>							_mapSessions; // map of session ID to Sessions

	std::shared_ptr<Mona::UDPSocket>								_pSocket; // Sending socket established with server
	std::shared_ptr<Mona::UDPSocket>								_pSocketIPV6; // Sending socket established with server

	Mona::DiffieHellman												_diffieHellman; // diffie hellman object used for key computing

	RTMFPDecoder::OnDecoded											_onDecoded; // Decoded callback
	Mona::UInt16													_threadRcv; // Thread used to decode last message
		
	OnMediaEvent													_pOnMedia; // External Callback to link with parent

	// Publish/Play commands
	struct StreamCommand : public Object {
		StreamCommand(bool isPublisher, const char* v, Mona::UInt16 id, bool aReliable, bool vReliable) : publisher(isPublisher), value(v), idMedia(id), audioReliable(aReliable), videoReliable(vReliable) {}

		bool			publisher;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
		Mona::UInt16	idMedia; // id generated by the session
	};
	std::queue<StreamCommand>										_waitingStreams;
	bool															_isWaitingStream; // True if a stream creation is waiting
	
	/* Asynchronous Read */
	struct MediaPlayer : public Object {
		MediaPlayer() : firstRead(true), firstMedia(true), timeStart(0), codecInfosRead(false) {}

		// Packet structure
		struct RTMFPMediaPacket : Mona::Packet, virtual Mona::Object {
			RTMFPMediaPacket(const Mona::Packet& packet, Mona::UInt32 time, AMF::Type type) : time(time), type(type), Packet(std::move(packet)), pos(0) {}

			Mona::UInt32	time;
			AMF::Type		type;
			Mona::UInt32	pos;
		};
		std::deque<std::shared_ptr<RTMFPMediaPacket>>	mediaPackets;
		bool											firstRead;
		bool											firstMedia;
		Mona::UInt32									timeStart;
		bool											codecInfosRead; // Player : False until the video codec infos have been read
	};
	std::map<Mona::UInt16, MediaPlayer>							_mapPlayers; // Map of media players
	Mona::UInt16												_mediaCount; // Counter of media streams (publisher/player) id
};
