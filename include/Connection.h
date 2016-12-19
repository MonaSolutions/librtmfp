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

#include "BandWriter.h"
#include "RTMFPWriter.h"
#include "RTMFP.h"
#include "RTMFPSender.h"

class SocketHandler;

namespace ConnectionEvents {
	struct OnNewWriter : Mona::Event<void(std::shared_ptr<RTMFPWriter>&)> {}; // called when a new writer is created
	struct OnWriterFailed : Mona::Event<void(std::shared_ptr<RTMFPWriter>&)> {}; // called when a writer fail
	struct OnWriterClose : Mona::Event<void(std::shared_ptr<RTMFPWriter>&)> {}; // called when a writer is closed
};

/**************************************************
Connection is the default connection Interface
used to send handshake messages
*/
class Connection : public BandWriter, 
	public ConnectionEvents::OnNewWriter,
	public ConnectionEvents::OnWriterFailed,
	public ConnectionEvents::OnWriterClose {
public:
	Connection(SocketHandler* pHandler);

	~Connection();

	// Manage the connection (send waiting requests)
	virtual void manage();

	// Close the connection properly
	virtual void close();

	// Called by the session to handle a writer fail message
	void handleWriterFailed(Mona::UInt64 id);

	// Called by the session to handle a writer acknowledgment
	void handleAcknowledgment(Mona::UInt64 id, Mona::PacketReader& message);

	// Send the first handshake message (with rtmfp url/peerId + tag)
	void									sendHandshake30(const std::string& epd, const std::string& tag);

	const Mona::SocketAddress&				address() { return _address; }

	void									clearWriters();

	// Read data received from server/peer
	void									process(Mona::PoolBuffer& buffer);

	// Return ping to calculate the latency
	Mona::UInt16							ping() { return _ping; }

	/******* Internal functions for writers *******/
	virtual const Mona::PoolBuffers&		poolBuffers();

	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

	virtual std::shared_ptr<RTMFPWriter>	changeWriter(RTMFPWriter& writer);

	virtual bool							getWriter(std::shared_ptr<RTMFPWriter>& pWriter, const std::string& signature);

	virtual bool							failed() const { return _status == RTMFP::FAILED; }

	virtual bool							canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->packet.size() : RTMFP_HEADER_SIZE); }

	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter = NULL);

	virtual void							flush() { flush(connected, connected ? 0x89 : 0x0B); }

	virtual const std::string&				name() { return _address.toString(); }

protected:

	// Handle message received (must be implemented)
	virtual void					handleMessage(const Mona::PoolBuffer& pBuffer) = 0;

	// Flush the connection
	// marker is : 0B for handshake, 09 for raw request, 89 for AMF request
	virtual void					flush(bool echoTime, Mona::UInt8 marker);

	// Clear the packet and flush the connection (just for handshake)
	void							flush(Mona::UInt8 marker, Mona::UInt32 size);

	// Read the p2p addresses to create p2p session
	void							handleP2pAddresses(Mona::BinaryReader& reader);

	// Update the ping value
	void							setPing(Mona::UInt16 time, Mona::UInt16 timeEcho);

	// Initialize the packet in the RTMFPSender
	Mona::UInt8*					packet();

	// Send the waiting messages
	void							flushWriters(); // TODO: make it private in the class parent Connection

	RTMFP::SessionStatus									_status; // Connection status (stopped, connecting, connected or failed)
	SocketHandler*											_pParent; // Pointer to the socket manager
	Mona::UInt32											_farId; // Session id

	Mona::SocketAddress										_address; // socket address related to this connection
	Mona::UInt16											_timeReceived; // last time received
	Mona::Time												_lastReceptionTime; // time of last "time" received

	// Encryption/Decryption
	std::shared_ptr<RTMFPEngine>							_pDecoder;
	std::shared_ptr<RTMFPEngine>							_pDefaultDecoder; // used for id stream 0
	std::shared_ptr<RTMFPEngine>							_pEncoder;
	
private:

	// Return the writer with this id
	std::shared_ptr<RTMFPWriter>&	writer(Mona::UInt64 id, std::shared_ptr<RTMFPWriter>& pWriter);

	std::map<Mona::UInt64, std::shared_ptr<RTMFPWriter>>	_flowWriters; // Map of writers identified by id
	RTMFPWriter*											_pLastWriter; // Write pointer used to check if it is possible to write
	Mona::UInt64											_nextRTMFPWriterId;
	std::shared_ptr<RTMFPSender>							_pSender; // Current sender object*/
	Mona::PoolThread*										_pThread; // Thread used to send last message

	std::recursive_mutex									_mutexConnections; // mutex for waiting p2p connections

	Mona::Time												_lastPing;
	Mona::UInt16											_ping;
};