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

#include "Base/IOSocket.h"
#include "Base/Timer.h"
#include "AMF.h"
#include "RTMFP.h"
#include "RTMFPDecoder.h"
#include <queue>

#define DELAY_CONNECTIONS_MANAGER	75 // Delay between each onManage (in msec)
#define DELAY_SIGNAL_READ			100 // time to wait before each read request
#define DELAY_BLOCKING_SIGNALS		200 // time to wait before checking interrupted status in each waiting signals

struct RTMFPSession;
class RTMFPLogger;
struct RTMFPGroupConfig;
struct RTMFPConfig;
struct Invoker : private Base::Thread {

	// Create the Invoker
	// createLogger : if True it will associate a logger instance to the static log class, otherwise it will let the default logger
	Invoker(bool createLogger=true);
	virtual ~Invoker();

	// Start the socket manager if not started
	void			start();

	// Delete the RTMFP session at index (safe threaded)
	void			removeConnection(unsigned int index, bool blocking);

	// Try to read data from the connection RTMFPcontext and the media ID streamId
	// return : The number of bytes read
	int				read(Base::UInt32 RTMFPcontext, Base::UInt16 streamId, Base::UInt8 *buf, Base::UInt32 size);

	// Write media (netstream must be published)
	// return -1 if an error occurs, otherwise the size of data treated
	int				write(unsigned int RTMFPcontext, const Base::UInt8* data, Base::UInt32 size);

	// Connect to the server from url
	// return: the connection ID or 0 if an error occurs
	Base::UInt32	connect(const char* url, RTMFPConfig* parameters);

	// Connect to a peer and try to play the stream streamName
	// return: the stream ID for this peer or 0 if an error occurs
	Base::UInt16	connect2Peer(Base::UInt32 RTMFPcontext, const char* peerId, const char* streamName);

	// Connect to a netgroup and try to play the stream streamName
	// return: the stream ID for this group or 0 if an error occurs
	Base::UInt16	connect2Group(Base::UInt32 RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable, const char* fallbackUrl);

	// Create the stream media and try to play/publish streamName
	// param mask: 0 for player, otherwise publisher or p2p publisher mask
	// return: the stream ID or 0 if an error occurs
	Base::UInt16	addStream(Base::UInt32 RTMFPcontext, Base::UInt8 mask, const char* streamName, bool audioReliable, bool videoReliable);

	// Close a publication from a session
	// return: True if succeed, False otherwise
	bool			closePublication(Base::UInt32 RTMFPcontext, const char* streamName);

	// Close a stream of a session
	// return: True if succeed, False otherwise
	bool			closeStream(Base::UInt32 RTMFPcontext, Base::UInt16 streamId);

	// Call a remote function in session RTMFPcontext
	// return: True if succeed, False otherwise
	bool			callFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId);

	// Blocking function waiting for an event to callback
	// return: True if the event happened, False if an error occurs during waiting
	bool			waitForEvent(Base::UInt32 RTMFPcontext, Base::UInt8 mask);

	// Called by a connection to push a media packet
	void			pushMedia(Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type);

	// Bufferize an input packet from a stream/session pair (Thread-safe)
	void			bufferizeMedia(Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type);

	// Called by a connection to start decoding a packet from target
	void			decode(int idConnection, Base::UInt32 idSession, const Base::SocketAddress& address, const std::shared_ptr<RTMFP::Engine>& pEngine, std::shared_ptr<Base::Buffer>& pBuffer, Base::UInt16& threadRcv);

	/*** Set callback functions (WARN: not thread-safe) ***/
	void			setLogCallback(void(*onLog)(unsigned int, const char*, long, const char*));

	void			setDumpCallback(void(*onDump)(const char*, const void*, unsigned int));

	void			setInterruptCallback(int(*interruptCb)(void*), void* argument);

private:
	Base::Handler						_handler; // keep in first (must be build before sockets)
public:
	Base::ThreadPool					threadPool; // keep in first (must be build before sockets)
	Base::IOSocket						sockets;
	const Base::Timer&					timer; 
	const Base::Handler&				handler;
private:
	// Safe-Threaded structure to publish packets
	struct WritePacket : RTMFP::MediaPacket {
		WritePacket(Base::UInt32 index, const Base::Packet& packet, Base::UInt32 time, AMF::Type type) : index(index), RTMFP::MediaPacket(packet, time, type) {}

		const Base::UInt32 index;
	};
	typedef Base::Event<void(WritePacket&)>			ON(PushAudio);
	typedef Base::Event<void(WritePacket&)>			ON(PushVideo);

	// Safe-Threaded structure to flush the publisher after publishing some packets
	struct WriteFlush : virtual Base::Object {
		WriteFlush(Base::UInt32 index) : index(index) {}
		const Base::UInt32 index;
	};
	typedef Base::Event<void(const WriteFlush&)>	ON(FlushPublisher);

	// Safe-Threaded structure to call a remote function
	struct CallFunction : virtual Base::Object {
		CallFunction(Base::UInt32 index, const char* function, int nbArgs, const char** args, const char* peerId) : index(index), function(function), peerId(peerId) {
			for (int i = 0; i < nbArgs; i++) {
				if (args[i])
					arguments.push(args[i]);
			}
		}
		std::queue<std::string>		arguments;
		const std::string			peerId;
		const std::string			function;
		const Base::UInt32			index;
	};
	typedef Base::Event<void(CallFunction&)>	ON(Function);

	// Safe-Threaded structure to close a publication from a session
	struct ClosePublication : virtual Base::Object {
		ClosePublication(Base::UInt32 index, const char* streamName) : index(index), streamName(streamName) {}

		const std::string		streamName;
		const Base::UInt32		index;
	};
	typedef Base::Event<void(ClosePublication&)>	ON(ClosePublication);

	// Safe-Threaded structure to close a stream in a session
	struct CloseStream : virtual Base::Object {
		CloseStream(Base::UInt32 index, Base::UInt16 streamId) : index(index), streamId(streamId) {}

		const Base::UInt16		streamId;
		const Base::UInt32		index;
	};
	typedef Base::Event<void(CloseStream&)>			ON(CloseStream);

	// Safe-Threaded structure to connect to a server
	struct ConnectAction : virtual Base::Object {
		ConnectAction(Base::UInt32 index, const char* url, const std::string& host, const Base::SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, std::shared_ptr<Base::Buffer>& rawUrl) :
			index(index), url(url), host(host), address(address), addresses(addresses), rawUrl(rawUrl) {}

		std::shared_ptr<Base::Buffer>	rawUrl;
		const std::string				url;
		const std::string				host;
		Base::SocketAddress				address;
		PEER_LIST_ADDRESS_TYPE			addresses;
		const Base::UInt32				index;
	};
	typedef Base::Event<void(ConnectAction&)>		ON(Connect);

	// Safe-Threaded structure to close and remove a session
	struct RemoveAction : virtual Base::Object {
		RemoveAction(Base::UInt32 index, std::atomic<bool>& ready, bool blocking) : index(index), ready(ready), blocking(blocking) {}

		Base::UInt32		index;
		std::atomic<bool>&	ready; // Warn, do not use this reference if not blocking
		bool				blocking;
	};
	typedef Base::Event<void(RemoveAction&)>		ON(RemoveConnection);

	struct CreateStream : virtual Base::Object {
		CreateStream(Base::UInt32 index, Base::UInt8 mask, const char* streamName, bool audioReliable, bool videoReliable, std::atomic<Base::UInt16>& mediaId, std::atomic<bool>&	ready) :
			index(index), mask(mask), streamName(streamName), audioReliable(audioReliable), videoReliable(videoReliable), mediaId(mediaId), ready(ready) {}

		const Base::UInt32			index;
		Base::UInt8					mask;
		std::string					streamName;
		bool						audioReliable;
		bool						videoReliable;
		std::atomic<Base::UInt16>&	mediaId;
		std::atomic<bool>&			ready;
	};
	typedef Base::Event<void(CreateStream&)>		ON(CreateStream);

	// Safe-Threaded structure to start the P2P publisher
	struct PublishP2P : virtual Base::Object {
		PublishP2P(Base::UInt32 index, const char* streamName, bool audioReliable, bool videoReliable) : 
			index(index), streamName(streamName), audioReliable(audioReliable), videoReliable(videoReliable) {}

		const Base::UInt32		index;
		std::string				streamName;
		bool					audioReliable; 
		bool					videoReliable;
	};
	typedef Base::Event<void(PublishP2P&)>			ON(PublishP2P);

	// Safe-Threaded structure to connect to a peer
	struct Connect2Peer : virtual Base::Object {
		Connect2Peer(Base::UInt32 index, const char* peerId, const char* streamName, std::atomic<bool>& ready, std::atomic<Base::UInt16>& mediaId) :
			index(index), peerId(peerId), streamName(streamName), ready(ready), mediaId(mediaId) {}

		const Base::UInt32			index;
		const std::string			streamName;
		const std::string			peerId;
		std::atomic<bool>&			ready;
		std::atomic<Base::UInt16>&	mediaId;
	};
	typedef Base::Event<void(Connect2Peer&)>		ON(Connect2Peer);

	// Safe-Threaded structure to connect to a NetGroup
	struct Connect2Group : virtual Base::Object {
		Connect2Group(Base::UInt32 index, const char* streamName, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable, const std::string& groupHex, const std::string& groupTxt, const std::string& groupName, 
			std::atomic<bool>& ready, std::atomic<Base::UInt16>& mediaId) : index(index), streamName(streamName), groupParameters(groupParameters), audioReliable(audioReliable), videoReliable(videoReliable), 
			groupHex(groupHex.c_str()), groupTxt(groupTxt.c_str()), groupName(groupName.c_str()), ready(ready), mediaId(mediaId) {}

		const Base::UInt32 index;
		const std::string streamName;
		RTMFPGroupConfig* groupParameters;
		const bool audioReliable;
		const bool videoReliable;
		const std::string groupHex;
		const std::string groupTxt;
		const std::string groupName;
		std::atomic<bool>& ready;
		std::atomic<Base::UInt16>& mediaId;
	};
	typedef Base::Event<void(Connect2Group&)>		ON(Connect2Group);

	virtual void		manage();
	bool				run(Base::Exception& exc, const volatile bool& stopping);

	// Remove the session pointed by the iterator, if this session has a fallback, delete it too
	// \param terminating : if true we are closing the Invoker so we do not delete the fallback recursively
	void				removeConnection(std::map<int, std::shared_ptr<RTMFPSession>>::iterator it, bool abrupt, bool terminating = false);

	// return True if the application is interrupted, otherwise False
	bool				isInterrupted();

	// Create the media buffer for connection RTMFPcontext if the condition return true
	// return: the media ID created or 0 if an error occurs
	Base::UInt16		createMediaBuffer(Base::UInt32 RTMFPcontext, std::function<bool(Base::UInt16)> condition);

	struct FallbackConnection;
	// Start a fallback play
	void				startFallback(FallbackConnection& fallback);

	Base::Timer														_timer; // manage timer
	Base::UInt32													_lastIndex; // last index of connection
	std::mutex														_mutexConnections;
	std::map<int, std::shared_ptr<RTMFPSession>>					_mapConnections;
	std::unique_ptr<RTMFPLogger>									_logger; // global logger for librtmfp
	Base::Signal													_waitSignal; // signal for blocking functions (TODO: make a signal for each connection)

	int																(*_interruptCb)(void*); // global interrupt callback function (NULL by default)
	void*															_interruptArg; // global interrup callback argument for interrupt function

	RTMFPDecoder::OnDecoded											_onDecoded; // Decoded callback

	std::map<Base::UInt32, FallbackConnection>						_waitingFallback; // map of waiting connection ID to fallback connection

	/* Members for Writting functions */
	enum { MAX_WRITE_BUFFER_SIZE = 0xFFFFFF }; // increase this at your own risks to handle bigger packets
	struct WriteBuffer;
	std::map<Base::UInt32, WriteBuffer>								_writeBuffers; // map of connection ID to writting buffer
	std::mutex														_mutexWrite; // mutex for write

	/* Data buffers for Readding */
	struct ConnectionBuffer;
	std::map<Base::UInt32, ConnectionBuffer>						_connection2Buffer; // map of connection ID to readding media buffers
	std::mutex														_mutexRead; // mutex for read

	/* MediaPacket temporary structure waiting buffering */
	struct ReadPacket : Base::Runner, RTMFP::MediaPacket {
		ReadPacket(Invoker& invoker, Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type) :
			invoker(invoker), idConn(RTMFPcontext), idMedia(mediaId), lostRate(lostRate), Base::Runner("ReadPacket"), RTMFP::MediaPacket(packet, time, type) {}

		bool run(Base::Exception& ex) {
			invoker.bufferizeMedia(idConn, idMedia, time, *this, lostRate, type);
			return true;
		}

		Base::UInt32 idConn;
		Base::UInt16 idMedia;
		const double lostRate;
		Invoker& invoker;
	};
	Base::UInt16 _threadPush;
};
