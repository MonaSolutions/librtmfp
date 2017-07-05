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

#define DELAY_CONNECTIONS_MANAGER	50 // Delay between each onManage (in msec)
#define TIMEOUT_FALLBACK_CONNECTION	3500 // time to wait before connecting to fallback connection (Netgroup=>Unicast switch)
#define DELAY_SIGNAL_READ			100 // time to wait before each read request

class RTMFPSession;
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

	bool			getConnection(unsigned int index, std::shared_ptr<RTMFPSession>& pConn);

	void			removeConnection(unsigned int index);

	unsigned int	empty();

	// Try to read data from the connection RTMFPcontext and the media ID streamId
	// return : -1 if an error occurs, 0 if the stream is closed, otherwise 1
	int				read(Base::UInt32 RTMFPcontext, Base::UInt16 streamId, Base::UInt8 *buf, Base::UInt32 size, int& nbRead);

	// Connect to the server from url
	// return: the connection ID or 0 if an error occurs
	Base::UInt32	connect(const char* url, RTMFPConfig* parameters);

	// Connect to a peer and try to play the stream streamName
	// return: the stream ID for this peer or 0 if an error occurs
	Base::UInt16	connect2Peer(Base::UInt32 RTMFPcontext, const char* peerId, const char* streamName, bool blocking);

	// Connect to a netgroup and try to play the stream streamName
	// param fallbackUrl an rtmfp url used if no P2P connection is established after timeout
	// return: the stream ID for this group or 0 if an error occurs
	Base::UInt16	connect2Group(Base::UInt32 RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable, const char* fallbackUrl);

	// Create the stream media and try to play/publish streamName
	// return: the stream ID or 0 if an error occurs
	Base::UInt16	addStream(Base::UInt32 RTMFPcontext, bool publisher, const char* streamName, bool audioReliable, bool videoReliable, bool blocking);

	// Start publishing the stream and wait for a P2P connection if blocking mode is set
	// return: True if publish succeed, False otherwise
	bool			publishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking);

	// Called by a connection to push a media packet
	void			pushMedia(Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type);

	/*** Set callback functions ***/
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
	virtual void		manage();
	bool				run(Base::Exception& exc, const volatile bool& stopping);

	void				removeConnection(std::map<int, std::shared_ptr<RTMFPSession>>::iterator it, bool abrupt = false);

	// return True if the application is interrupted, otherwise False
	bool				isInterrupted();

	// Create the media buffer for connection RTMFPcontext if the condition return true
	// return: the media ID created or 0 if an error occurs
	Base::UInt16		createMediaBuffer(Base::UInt32 RTMFPcontext, std::function<bool(Base::UInt16)> condition);

	Base::Timer										_timer; // manage timer
	int												_lastIndex; // last index of connection
	std::mutex										_mutexConnections;
	std::map<int, std::shared_ptr<RTMFPSession>>	_mapConnections;
	std::unique_ptr<RTMFPLogger>					_logger; // global logger for librtmfp

	int												(*_interruptCb)(void*); // global interrupt callback function (NULL by default)
	void*											_interruptArg; // global interrup callback argument for interrupt function

	/* Netgroup/Unicast Fallback connections */
	struct FallbackConnection;
	std::map<Base::UInt32, std::shared_ptr<FallbackConnection>>		_connection2Fallback; // map of connection ID to fallback connection
	std::map<Base::UInt32, std::shared_ptr<FallbackConnection>>		_waitingFallback; // map of waiting connection ID to fallback connection

	/* Data buffers for Readding */
	struct ConnectionBuffer;
	std::map <Base::UInt32, ConnectionBuffer>	_connection2Buffer; // map of connection ID to media buffers
	std::mutex									_mutexRead; // mutex for read

	/* MediaPacket temporary structure waiting buffering */
	struct MediaPacket;
	void bufferizeMedia(Base::UInt32 RTMFPcontext, Base::UInt16 mediaId, Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type); // bufferize packet for connection and stream
	Base::UInt16 _threadPush;
};
