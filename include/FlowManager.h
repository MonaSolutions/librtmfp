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

#include "Publisher.h"
#include "FlashConnection.h"
#include "RTMFPHandshaker.h"
#include "BandWriter.h"
#include "Mona/DiffieHellman.h"

// Callback typedef definitions
typedef void(*OnStatusEvent)(const char*, const char*);
typedef void(*OnMediaEvent)(const char *, const char*, unsigned int, const char*, unsigned int, int);
typedef void(*OnSocketError)(const char*);

class Invoker;
class RTMFPFlow;
class RTMFPWriter;
class FlashListener;
/**************************************************
FlowManager is an abstract class used to manage 
lists of RTMFPFlow and RTMFPWriter
It is the base class of RTMFPSession and P2PSession
*/
class FlowManager : public BandWriter {
public:
	FlowManager(bool responder, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	virtual ~FlowManager();

	enum CommandType {
		NETSTREAM_PLAY = 1,
		NETSTREAM_PUBLISH,
		NETSTREAM_PUBLISH_P2P,
		NETSTREAM_GROUP,
		NETSTREAM_CLOSE
	};

	// Add a command to the main stream (play/publish)
	virtual void					addCommand(CommandType command, const char* streamName, bool audioReliable=false, bool videoReliable=false)=0;

	// Return the tag for this session (for RTMFPConnection)
	const std::string&				tag() { return _tag; }

	// Return the url or peerId of the session (for RTMFPConnection)
	virtual const std::string&		epd() = 0;

	// Return the id of the session (p2p or normal)
	const Mona::UInt32				sessionId() { return _sessionId; }

	RTMFP::SessionStatus			status; // Session status (stopped, connecting, connected or failed)

	// Read data asynchronously
	// peerId : id of the peer if it is a p2p connection, otherwise parameter is ignored
	// return : false if the connection is not established
	bool							readAsync(Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Latency (ping / 2)
	Mona::UInt16					latency() { return _ping >> 1; }

	// Return true if the session has failed (we will not send packets anymore)
	virtual bool					failed() { return (status == RTMFP::FAILED && _closeTime.isElapsed(19000)) || ((status == RTMFP::NEAR_CLOSED) && _closeTime.isElapsed(90000)); }

	// Return the pool buffers object
	virtual const Mona::PoolBuffers&	poolBuffers();

	// Called by RTMFPWriter to know if there is already a message in queue
	virtual bool					canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	virtual Mona::UInt32			availableToWrite();

	// Write a new message with type and length in parameter
	virtual Mona::BinaryWriter&		writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter = NULL);

	// Flush the current packet
	// marker is : 0B for handshake, 09 for raw request, 89 for AMF request
	virtual void					flush(bool echoTime, Mona::UInt8 marker);

	// Send the current packet
	virtual void					flush() { flush(status >= RTMFP::CONNECTED, (status >= RTMFP::CONNECTED) ? (0x89 + _responder) : 0x0B); }

	// Handle receiving packet
	virtual void					process(const Mona::SocketAddress& address, Mona::PoolBuffer& pBuffer);

	// Called when we received the first handshake 70 to update the address
	virtual bool					onPeerHandshake70(const Mona::SocketAddress& address, const std::string& farKey, const std::string& cookie);

	// Called when when sending the handshake 38 to build the peer ID if we are RTMFPSession
	virtual void					buildPeerID(const Mona::UInt8* data, Mona::UInt32 size) {}

	// Compute keys and init encoder and decoder
	bool							computeKeys(Mona::UInt32 farId);

	// Return the address of the session
	const Mona::SocketAddress&		address() { return _address; }

	// Remove the handshake properly
	virtual void					removeHandshake(std::shared_ptr<Handshake>& pHandshake)=0;

	// Return the diffie hellman object (related to main session)
	virtual bool					diffieHellman(Mona::DiffieHellman* &pDh)=0;

	// Return the nonce (generate it if not ready)
	Mona::Buffer&					getNonce();

	// Close the session properly or abruptly if parameter is true
	virtual void					close(bool abrupt);

protected:

	// Handle a writer closed (to release shared pointers)
	virtual void				handleWriterClosed(std::shared_ptr<RTMFPWriter>& pWriter)=0;

	// Create a new writer
	const std::shared_ptr<RTMFPWriter>&		createWriter(const std::string& signature, Mona::UInt64 flowId=0);

	// Analyze packets received from the server (must be connected)
	void						receive(Mona::BinaryReader& reader);

	// Handle data available or not event (asynchronous read only)
	virtual void				handleDataAvailable(bool isAvailable) = 0;

	// Handle play request (only for P2PSession)
	virtual bool				handlePlay(const std::string& streamName, Mona::UInt16 streamId, Mona::UInt64 flowId, double cbHandler) { return false; }

	// Handle a Writer close message (type 5E)
	virtual void				handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter) = 0;

	// Handle a P2P address exchange message (Only for RTMFPSession)
	virtual void				handleP2PAddressExchange(Mona::PacketReader& reader) {}

	// On NetConnection.Connect.Success callback (only for RTMFPSession)
	virtual void				onNetConnectionSuccess() {}

	// On NetStream.Publish.Start (only for NetConnection)
	virtual void				onPublished(Mona::UInt16 streamId) {}

	//RTMFPWriter*				writer(Mona::UInt64 id);
	RTMFPFlow*					createFlow(Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*			createSpecialFlow(Mona::Exception& ex, Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef) = 0;

	// Manage the flows
	virtual void				manage();

	// Called when we are connected to the peer/server
	virtual void				onConnection() = 0;

	enum HandshakeType {
		BASE_HANDSHAKE = 0x0A,
		P2P_HANDSHAKE = 0x0F
	};

	Mona::Time											_lastKeepAlive; // last time a keepalive request has been received+
	std::string											_tag;

	// External Callbacks to link with parent
	OnStatusEvent										_pOnStatusEvent;
	OnMediaEvent										_pOnMedia;
	OnSocketError										_pOnSocketError;

	// Events
	FlashConnection::OnStatus::Type						onStatus;
	FlashConnection::OnMedia::Type						onMedia;
	FlashConnection::OnPlay::Type						onPlay;

	// Job Members
	std::shared_ptr<FlashConnection>					_pMainStream; // Main Stream (NetConnection or P2P Connection Handler)
	std::map<Mona::UInt64, RTMFPFlow*>					_flows;
	Mona::UInt64										_mainFlowId; // Main flow ID, if it is closed we must close the session
	Invoker*											_pInvoker; // Main invoker pointer to get poolBuffers
	Mona::UInt32										_sessionId; // id of the session;

	FlashListener*										_pListener; // Listener of the main publication (only one by intance)
	bool												_responder; // is responder?
	std::shared_ptr<Handshake>							_pHandshake; // Handshake object if not connected

	Mona::Buffer										_sharedSecret; // shared secret for crypted communication
	Mona::Buffer										_farNonce; // far nonce (saved for p2p group key building)
	Mona::Buffer										_nonce; // Our Nonce for key exchange, can be of size 0x4C or 0x49 for responder

private:

	// Remove all messages from the writers (before closing)
	void												clearWriters();

	// Read the handshake 78 response and notify the session for the connection
	void												sendConnect(Mona::BinaryReader& reader);

	// Remove a flow from the list of flows
	void												removeFlow(RTMFPFlow* pFlow);

	// Return the writer with this id
	std::shared_ptr<RTMFPWriter>&						writer(Mona::UInt64 id, std::shared_ptr<RTMFPWriter>& pWriter);

	// Send the waiting messages
	void												flushWriters();

	// Update the ping value
	void												setPing(Mona::UInt16 time, Mona::UInt16 timeEcho);

	// Send the close message (0C if normal, 4C if abrupt)
	void												sendCloseChunk(bool abrupt);

	Mona::Time																	_closeTime; // Time since closure
	Mona::Time																	_lastPing; // Time since last ping sent
	Mona::Time																	_lastClose; // Time since last close chunk
	Mona::UInt16																_ping; // ping value

	// writers members
	std::map<Mona::UInt64, std::shared_ptr<RTMFPWriter>>						_flowWriters; // Map of writers identified by id
	RTMFPWriter*																_pLastWriter; // Write pointer used to check if it is possible to write
	Mona::UInt64																_nextRTMFPWriterId; // Writer id to use for the next writer to create

	// Asynchronous read
	struct RTMFPMediaPacket : public Mona::Object {

		RTMFPMediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 time, bool audio);

		Mona::PoolBuffer	pBuffer;
		Mona::UInt32		pos;
	};
	std::map<std::string, std::deque<std::shared_ptr<RTMFPMediaPacket>>>		_mediaPackets;
	std::recursive_mutex														_readMutex;
	bool																		_firstRead;
	static const char															_FlvHeader[];

	// Read
	bool																		_firstMedia;
	Mona::UInt32																_timeStart;
	bool																		_codecInfosRead; // Player : False until the video codec infos have been read
};
