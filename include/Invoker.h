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
#define TIMEOUT_FALLBACK_CONNECTION	8000 // time to wait before connecting to fallback connection (Netgroup=>Unicast switch)
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
	bool			start();

	// Delete the RTMFP session at index (safe threaded)
	void			removeConnection(unsigned int index, bool blocking);

	// Try to read data from the connection RTMFPcontext and the media ID streamId
	// return : The number of bytes read
	int				read(Base::UInt32 RTMFPcontext, Base::UInt16 streamId, Base::UInt8 *buf, Base::UInt32 size);

	// Write media (netstream must be published)
	// return -1 if an error occurs
	int				write(unsigned int RTMFPcontext, const Base::UInt8* data, Base::UInt32 size);

	// Connect to the server from url
	// return: the connection ID or 0 if an error occurs
	Base::UInt32	connect(const char* url, RTMFPConfig* parameters);

	// Connect to a peer and try to play the stream streamName
	// return: the stream ID for this peer or 0 if an error occurs
	Base::UInt16	connect2Peer(Base::UInt32 RTMFPcontext, const char* peerId, const char* streamName, bool blocking);

	// Create a fallback connection to link to a NetGroup session
	// This connection will be run if the netgroup connection does not return data before the timeout
	// And if data arrives later we switch to the netgroup connection and delete the fallback one
	// return: True if the connection succeed, False otherwise
	bool			connect2FallbackUrl(Base::UInt32 RTMFPcontext, RTMFPConfig* parameters, const char* fallbackUrl, Base::UInt16 mediaId);

	// Connect to a netgroup and try to play the stream streamName
	// return: the stream ID for this group or 0 if an error occurs
	Base::UInt16	connect2Group(Base::UInt32 RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable);

	// Create the stream media and try to play/publish streamName
	// return: the stream ID or 0 if an error occurs
	Base::UInt16	addStream(Base::UInt32 RTMFPcontext, bool publisher, const char* streamName, bool audioReliable, bool videoReliable, bool blocking);

	// Start publishing the stream and wait for a P2P connection if blocking mode is set
	// return: True if publish succeed, False otherwise
	bool			publishP2P(Base::UInt32 RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking);

	// Close a publication from a session
	// return: True if succeed, False otherwise
	bool			closePublication(Base::UInt32 RTMFPcontext, const char* streamName);

	// Close a stream of a session
	// return: True if succeed, False otherwise
	bool			closeStream(Base::UInt32 RTMFPcontext, Base::UInt16 streamId);

	// Call a remote function in session RTMFPcontext
	// return: True if succeed, False otherwise
	bool			callFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId);

	// Called by a connection to push a media packet
	void			pushMedia(Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type);

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
	struct WritePacket : virtual Base::Object, Base::Packet {
		WritePacket(Base::UInt32 index, const Base::Packet& packet, Base::UInt32 time) : index(index), time(time), Base::Packet(std::move(packet)) {}
		const Base::UInt32 time;
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
		CreateStream(Base::UInt32 index, bool publisher, const char* streamName, bool audioReliable, bool videoReliable, std::atomic<Base::UInt16>& mediaId, std::atomic<bool>&	ready) :
			index(index), publisher(publisher), streamName(streamName), audioReliable(audioReliable), videoReliable(videoReliable), mediaId(mediaId), ready(ready) {}

		const Base::UInt32			index;
		bool						publisher;
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
		Connect2Peer(Base::UInt32 index, const char* peerId, const char* streamName, std::atomic<Base::UInt16>& mediaId) :
			index(index), peerId(peerId), streamName(streamName), mediaId(mediaId) {}

		const Base::UInt32			index;
		const std::string			streamName;
		const std::string			peerId;
		std::atomic<Base::UInt16>&	mediaId;
	};
	typedef Base::Event<void(Connect2Peer&)>		ON(Connect2Peer);

	// Safe-Threaded structure to connect to a NetGroup
	struct Connect2Group : virtual Base::Object {
		Connect2Group(Base::UInt32 index, const char* streamName, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable, const std::string& groupHex, const std::string& groupTxt, bool blocking, std::atomic<bool>& ready, std::atomic<Base::UInt16>& mediaId) :
			index(index), streamName(streamName), groupParameters(groupParameters), audioReliable(audioReliable), videoReliable(videoReliable), groupHex(groupHex), groupTxt(groupTxt), blocking(blocking), ready(ready), mediaId(mediaId) {}

		const Base::UInt32 index;
		const std::string streamName;
		RTMFPGroupConfig* groupParameters;
		const bool audioReliable;
		const bool videoReliable;
		const std::string& groupHex;
		const std::string& groupTxt;
		const bool blocking;
		std::atomic<bool>& ready;
		std::atomic<Base::UInt16>& mediaId;
	};
	typedef Base::Event<void(Connect2Group&)>		ON(Connect2Group);

	virtual void		manage();
	bool				run(Base::Exception& exc, const volatile bool& stopping);

	void				removeConnection(std::map<int, std::shared_ptr<RTMFPSession>>::iterator it, bool abrupt = false);

	// return True if the application is interrupted, otherwise False
	bool				isInterrupted();

	// Create the media buffer for connection RTMFPcontext if the condition return true
	// return: the media ID created or 0 if an error occurs
	Base::UInt16		createMediaBuffer(Base::UInt32 RTMFPcontext, std::function<bool(Base::UInt16)> condition);

	struct FallbackConnection;
	// Start a fallback play
	void				startFallback(FallbackConnection& fallback);

	Base::Timer														_timer; // manage timer
	int																_lastIndex; // last index of connection
	std::mutex														_mutexConnections;
	std::map<int, std::shared_ptr<RTMFPSession>>					_mapConnections;
	std::unique_ptr<RTMFPLogger>									_logger; // global logger for librtmfp
	Base::Signal													_waitSignal; // signal for blocking functions

	int																(*_interruptCb)(void*); // global interrupt callback function (NULL by default)
	void*															_interruptArg; // global interrup callback argument for interrupt function

	RTMFPDecoder::OnDecoded											_onDecoded; // Decoded callback

	std::map<Base::UInt32, FallbackConnection>						_waitingFallback; // map of waiting connection ID to fallback connection

	/* Data buffers for Readding */
	struct ConnectionBuffer;
	std::map <Base::UInt32, ConnectionBuffer>						_connection2Buffer; // map of connection ID to media buffers
	std::mutex														_mutexRead; // mutex for read

	/* MediaPacket temporary structure waiting buffering */
	struct ReadPacket : Base::Runner, Base::Packet, virtual Object {
		ReadPacket(Invoker& invoker, Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type) :
			invoker(invoker), idConn(RTMFPcontext), idMedia(mediaId), time(time), Packet(std::move(packet)), lostRate(lostRate), type(type), Base::Runner("MediaPacket") {}

		bool run(Base::Exception& ex) {
			invoker.bufferizeMedia(idConn, idMedia, time, *this, lostRate, type);
			return true;
		}

		Base::UInt32 idConn;
		Base::UInt16 idMedia;
		Base::UInt32 time;
		const double lostRate;
		const AMF::Type type;
		Invoker& invoker;
	};
	// Bufferize an input packet from a stream/session pair (Threaded)
	void bufferizeMedia(Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type);
	Base::UInt16 _threadPush;
};
