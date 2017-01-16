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
#include "RTMFPConnection.h"

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
class FlowManager : public virtual Mona::Object {
public:
	FlowManager(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~FlowManager();

	enum CommandType {
		NETSTREAM_PLAY = 1,
		NETSTREAM_PUBLISH,
		NETSTREAM_PUBLISH_P2P,
		NETSTREAM_GROUP,
		NETSTREAM_CLOSE
	};

	// Add a command to the main stream (play/publish)
	virtual void					addCommand(CommandType command, const char* streamName, bool audioReliable=false, bool videoReliable=false)=0;

	// Return the name of the session
	virtual const std::string&		name() = 0;

	// Return the tag for this session (for RTMFPConnection)
	const std::string&				tag() { return _tag; }

	// Return the url or peerId of the session (for RTMFPConnection)
	virtual const std::string&		epd() = 0;

	// Return the id of the session (p2p or normal)
	const Mona::UInt32				sessionId() { return _sessionId; }

	RTMFP::SessionStatus			status; // Session status (stopped, connecting, connected or failed)

	// Subscribe to all events of the connection and add it to the list of known addresses
	virtual void					subscribe(std::shared_ptr<RTMFPConnection>& pConnection);

	// Unsubscribe to events of the connection pointed by address, 
	// Note: This function is only called by RTMFPConnection to avoid double subscription
	virtual void					unsubscribeConnection(const Mona::SocketAddress& address);

	// Read data asynchronously
	// peerId : id of the peer if it is a p2p connection, otherwise parameter is ignored
	// return : false if the connection is not established
	bool							readAsync(Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Latency (ping / 2)
	Mona::UInt16					latency();

	// A session is considered closed when it has failed or if it is in NEAR_CLOSED status since at least 90s
	bool							closed() { return status == RTMFP::FAILED || ((status == RTMFP::NEAR_CLOSED) && _closeTime.isElapsed(90000)); }

protected:

	// Analyze packets received from the server (must be connected)
	void						receive(Mona::BinaryReader& reader);

	// Handle data available or not event (asynchronous read only)
	virtual void				handleDataAvailable(bool isAvailable) = 0;

	// Handle play request (only for P2PSession)
	virtual bool				handlePlay(const std::string& streamName, Mona::UInt16 streamId, Mona::UInt64 flowId, double cbHandler) { return false; }

	// Handle a new writer creation
	virtual void				handleNewWriter(std::shared_ptr<RTMFPWriter>& pWriter) = 0;

	// Handle a Writer close message (type 5E)
	virtual void				handleWriterFailed(std::shared_ptr<RTMFPWriter>& pWriter) = 0;

	// Handle a P2P address exchange message (Only for RTMFPSession)
	virtual void				handleP2PAddressExchange(Mona::PacketReader& reader) = 0;

	// Close the conection properly or abruptly if parameter is true
	virtual void				close(bool abrupt);

	// On NetConnection success callback
	virtual void				onConnect() {}

	// On NetStream.Publish.Start (only for NetConnection)
	virtual void				onPublished(Mona::UInt16 streamId) {}

	//RTMFPWriter*				writer(Mona::UInt64 id);
	RTMFPFlow*					createFlow(Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef);

	// Create a flow for special signatures (NetGroup)
	virtual RTMFPFlow*			createSpecialFlow(Mona::Exception& ex, Mona::UInt64 id, const std::string& signature, Mona::UInt64 idWriterRef) = 0;

	// Manage the flows
	virtual void				manage();

	enum HandshakeType {
		BASE_HANDSHAKE = 0x0A,
		P2P_HANDSHAKE = 0x0F
	};

	Mona::Time											_lastKeepAlive; // last time a keepalive request has been received+
	std::shared_ptr<RTMFPConnection>					_pConnection; // Current connection object used for communication with the server/peer
	std::string											_tag;

	// External Callbacks to link with parent
	OnStatusEvent										_pOnStatusEvent;
	OnMediaEvent										_pOnMedia;
	OnSocketError										_pOnSocketError;

	// Events
	FlashConnection::OnStatus::Type						onStatus; // NetConnection or NetStream status event
	FlashConnection::OnMedia::Type						onMedia; // Received when we receive media (audio/video)
	FlashConnection::OnPlay::Type						onPlay; // Received when we receive media (audio/video)
	RTMFPConnection::OnMessage::Type					onMessage; // Received when a connection follow us a message
	RTMFPConnection::OnNewWriter::Type					onNewWriter; // Received when the connection create a new writer
	RTMFPConnection::OnWriterFailed::Type				onWriterFailed; // Received when the writer fail
	RTMFPConnection::OnWriterClose::Type				onWriterClose; // Received when the writer is closed

	// Job Members
	std::shared_ptr<FlashConnection>					_pMainStream; // Main Stream (NetConnection or P2P Connection Handler)
	std::map<Mona::UInt64, RTMFPFlow*>					_flows;
	Mona::UInt64										_mainFlowId; // Main flow ID, if it is closed we must close the session
	Invoker*											_pInvoker; // Main invoker pointer to get poolBuffers
	Mona::UInt32										_sessionId; // id of the session;

	FlashListener*										_pListener; // Listener of the main publication (only one by intance)

private:

	// Unsubscribe from all events of the connection
	virtual void										unsubscribeConnection(std::shared_ptr<RTMFPConnection>& pConnection);

	// Remove a flow from the list of flows
	void												removeFlow(RTMFPFlow* pFlow);

	std::map<Mona::SocketAddress, std::shared_ptr<RTMFPConnection>>				_mapConnections; // map of connections to all addresses of the session

	Mona::Time																	_closeTime; // Time since closure

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
