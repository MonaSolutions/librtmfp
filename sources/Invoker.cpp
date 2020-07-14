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

#include "Invoker.h"
#include "RTMFPLogger.h"
#include "RTMFPSession.h"
#include "Base/BufferPool.h"
#include "Base/DNS.h"
#include "librtmfp.h"

// Method used to unlock a mutex during an instruction which can take time
#define UNLOCK_RUN_LOCK(MUTEX, INSTRUCTION)	MUTEX.unlock(); INSTRUCTION; MUTEX.lock()

using namespace Base;
using namespace std;

// GroupSpecifier reader
struct GroupSpecReader : Parameters {
	GroupSpecReader() {}

	bool parse(Exception& ex,  const char* groupspec) {

		if (strncmp("G:", groupspec, 2) != 0) {
			ex.set<Ex::Format>("Group ID not well formated, it must begin with 'G:'");
			return false;
		}

		const char* endMarker = NULL;

		// Create the reader of NetGroup ID
		Buffer buff;
		String::ToHex(groupspec + 2, buff);
		BinaryReader reader(buff.data(), buff.size());

		// Read each NetGroup parameters and save group version + end marker
		while (reader.available() > 0) {
			UInt8 size = reader.read8();
			if (size == 0) {
				endMarker = groupspec + 2 * reader.position();
				break;
			}
			else if (reader.available() < size) {
				ex.set<Ex::Format>("Unexpected end of group specifier");
				break;
			}
			
			string value;
			UInt8 type = reader.read8();
			if (size > 1)
				reader.read(size - 1, value);
			setString(String(String::Format<UInt8>("%02x", type)), value);
		}

		// Keep the meanful part of the group ID (before end marker)
		if (!endMarker) {
			ex.set<Ex::Format>("Group ID not well formated");
			return false;
		}
		
		groupTxt.assign(groupspec, endMarker);
		return true;
	}

	string groupTxt; // group specifier meanful part
};

// RTMFP Fallback connection wrapper from NetGroup to unicast
struct Invoker::FallbackConnection : virtual Object {
	FallbackConnection(UInt32 idConnection, UInt16 mediaId, const char* streamName, const RTMFPConfig* config, const char* url, 
		string& host, SocketAddress& address, PEER_LIST_ADDRESS_TYPE& addresses, shared<Buffer>&& rawUrl) : lastTime(0), mediaId(mediaId), idConnection(idConnection), idFallback(0),
		streamName(streamName), running(false), switched(false), url(url), host(host), rawUrl(move(rawUrl)), address(address), addresses(move(addresses)) {

		memcpy(&parameters, config, sizeof(RTMFPConfig));
	}
	virtual ~FallbackConnection() {}

	bool					switched; // True if the NetGroup Connection has started to send video
	bool					running; // True if the connection fallback has been started
	UInt32					lastTime; // last time received from fallback connection (for time patching)
	const UInt16			mediaId; // id of the main connection media for wrapping
	const UInt32			idConnection; // id of the main connection
	UInt32					idFallback; // id of the fallback connection (unicast)
	const std::string		streamName; // stream name to subscribe to
	const Time				timeCreation; // time when the fallback connection has been created
	RTMFPConfig				parameters; // RTMFP copy of parent parameters
	string					url; // RTMFP Unicast fallback url
	string					host;
	shared<Buffer>			rawUrl;
	SocketAddress			address;
	PEER_LIST_ADDRESS_TYPE	addresses;
};

// Reading Connection Buffer structure, contains the input media buffers from 1 session
struct Invoker::ConnectionBuffer : virtual Object {
	ConnectionBuffer() : mediaCount(0) {}
	virtual ~ConnectionBuffer() {}

	// Media stream buffer
	struct MediaBuffer : virtual Object {
		MediaBuffer() : firstRead(true), codecInfosRead(false), AACsequenceHeaderRead(false), timeOffset(0) {}

		// Packet structure
		struct RTMFPMediaPacket : Packet, virtual Object {
			RTMFPMediaPacket(const Packet& packet, UInt32 time, AMF::Type type) : time(time), type(type), Packet(std::move(packet)), pos(0) {}

			UInt32		time;
			AMF::Type	type;
			UInt32		pos;
		};
		deque<RTMFPMediaPacket>					mediaPackets;
		bool									firstRead;
		bool									codecInfosRead; // Player : False until the video codec infos have been read
		bool									AACsequenceHeaderRead; // False until the AAC sequence header infos have been read
		UInt32									timeOffset; // time offset used when a fallback connection has started
	};
	map<UInt16, MediaBuffer>					mapMedias; // Map of media players
	UInt16										mediaCount; // Counter of media streams (publisher/player) id
};

// Writing Connection buffer structure, contains current packet buffer from 1 session
struct Invoker::WriteBuffer : virtual Object {
	WriteBuffer() : started(false), type(0), size(0), time(0), total(0) {}
	~WriteBuffer() {}

	shared<Buffer>				buffer; // bufferize input data to wait for the end of a packet 
	Base::unique<BinaryWriter>	writer; // Input writer pointing to the current writing buffer
	bool						started; // True if we have already read the FLV header
	UInt8						type; // current packet type
	UInt32						size; // current packet size
	UInt32						time; // current packet time
	UInt64						total; // total size since last flush
};

/** Invoker **/

Invoker::Invoker(void(*onLog)(unsigned int, const char*, long, const char*), void(*onDump)(const char*, const void*, unsigned int)) : Thread("Invoker"), handler(_handler), timer(_timer), sockets(_handler, threadPool), _lastIndex(0), _handler(wakeUp), _threadPush(0) {
	onPushAudio = [this](WritePacket& packet) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(packet.index);
		if (it != _mapConnections.end() && it->second->status < RTMFP::NEAR_CLOSED)
			it->second->writeAudio(packet, packet.time);
	};
	onPushVideo = [this](WritePacket& packet) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(packet.index);
		if (it != _mapConnections.end() && it->second->status < RTMFP::NEAR_CLOSED)
			it->second->writeVideo(packet, packet.time);
	};
	onPushData = [this](WritePacket& packet) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(packet.index);
		if (it != _mapConnections.end() && it->second->status < RTMFP::NEAR_CLOSED)
			it->second->writeData(packet, packet.time);
	};
	onFlushPublisher = [this](const WriteFlush& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end() && it->second->status < RTMFP::NEAR_CLOSED)
			it->second->writeFlush();
	};
	onFunction = [this](CallFunction& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end())
			it->second->callFunction(obj.function, obj.arguments, obj.peerId);
	};
	onClosePublication = [this](ClosePublication& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end())
			it->second->closePublication(obj.streamName.c_str());
	};
	onCloseStream = [this](CloseStream& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end())
			it->second->closeStream(obj.streamId);
	};
	onConnect = [this](ConnectAction& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end())
			it->second->connect(obj.url, obj.host, obj.address, obj.addresses, obj.rawUrl);
	};
	onRemoveConnection = [this](RemoveAction& obj) {
		lock_guard<mutex>	lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end())
			removeConnection(it, false);

		// To exit from the caller loop
		if (obj.blocking && Thread::running())
			obj.ready = true;
		_waitSignal.set();
	};
	onConnect2Peer = [this](Connect2Peer& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto itConn = _mapConnections.find(obj.index);
		if (itConn != _mapConnections.end() && Thread::running()) {

			// Start connecting to the peer and create the media buffer for this stream
			obj.mediaId = createMediaBuffer(obj.index, [&itConn, &obj](UInt16 mediaCount) {
				return itConn->second->connect2Peer(obj.peerId, obj.streamName, mediaCount);
			});
		}

		// To exit from the caller loop
		if (Thread::running())
			obj.ready = true;
		_waitSignal.set();
	};
	onCreateStream = [this](CreateStream& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto itConn = _mapConnections.find(obj.index);
		if (itConn != _mapConnections.end() && Thread::running()) {

			// Start connecting to the peer and create the media buffer for this stream
			// TODO: do not create a media buffer for publishers
			obj.mediaId = createMediaBuffer(obj.index, [&itConn, &obj](UInt16 mediaCount) {
				return itConn->second->addStream(obj.mask, obj.streamName, obj.audioReliable, obj.videoReliable, mediaCount);
			});
		}
		
		// To exit from the caller loop
		if (Thread::running())
			obj.ready = true;
		_waitSignal.set();
	};
	onConnect2Group = [this](Connect2Group& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto itConn = _mapConnections.find(obj.index);
		if (itConn != _mapConnections.end() && Thread::running()) {
			// Start connecting to the group and create the media buffer for this stream
			obj.mediaId = createMediaBuffer(obj.index, [&itConn, &obj](UInt16 mediaCount) {
				return itConn->second->connect2Group(obj.streamName, obj.groupParameters, obj.audioReliable, obj.videoReliable, obj.groupHex, obj.groupTxt, obj.groupName, mediaCount);
			});
		}

		// To exit from the caller loop
		if (Thread::running())
			obj.ready = true;
		_waitSignal.set();
	};
	_onDecoded = [this](RTMFPDecoder::Decoded& decoded) {
		_mutexConnections.lock();

		auto itConn = _mapConnections.find(decoded.idConnection);
		if (itConn != _mapConnections.end()) {
			UNLOCK_RUN_LOCK(_mutexConnections, itConn->second->receive(decoded));
		} else
			DEBUG("RTMFPDecoder callback without connection, possibly deleted")

		_mutexConnections.unlock();
	};

	if (onLog) {
		Logs::RemoveLogger("console"); // remove default logger
		Logs::AddLogger<RTMFPLogger>("LIBRTMFP", onLog, onDump);
	}
	DEBUG("Socket receiving buffer size of ", Net::GetRecvBufferSize(), " bytes");
	DEBUG("Socket sending buffer size of ", Net::GetSendBufferSize(), " bytes");
	DEBUG(threadPool.threads(), " threads in server threadPool");
	DEBUG("Librtmfp version ", (RTMFP_LIB_VERSION >> 24) & 0xFF, ".", (RTMFP_LIB_VERSION >> 16) & 0xFF, ".", RTMFP_LIB_VERSION & 0xFFFF);
}

Invoker::~Invoker() {

	TRACE("Closing global invoker...")

	// terminate the tasks
	if (running())
		stop();

	onPushAudio = nullptr;
	onPushVideo = nullptr;
	onFlushPublisher = nullptr;
	onFunction = nullptr;
	onClosePublication = nullptr;
	onCloseStream = nullptr;
	onConnect = nullptr;
	onRemoveConnection = nullptr;
	onConnect2Peer = nullptr;
	onCreateStream = nullptr;
	onConnect2Group = nullptr;
	_onDecoded = nullptr;

	_waitSignal.set();
}

// Start the socket manager if not started
void Invoker::start() {
	if(running()) {
		ERROR("Invoker is already running, call stop method before");
		return;
	}
	
	Thread::start();
}

void Invoker::manage() {
	
	_mutexConnections.lock();

	if (!_waitingFallback.empty()) {

		// Start waiting fallback connections
		int timeoutFb = RTMFP::Parameters().getNumber<int>("timeoutFallback");
		auto itWait = _waitingFallback.begin();
		while (itWait != _waitingFallback.end()) {
			// If timeout reached we create the connection
			if (!itWait->second.idFallback && !itWait->second.switched && !itWait->second.running && itWait->second.timeCreation.isElapsed(timeoutFb)) {

				INFO(timeoutFb, "ms without data, starting fallback connection from ", itWait->first);
				startFallback(itWait->second);
				continue;
			}
			++itWait;
		}
	}

	// Manage connections
	for (auto& it : _mapConnections) {
		UNLOCK_RUN_LOCK(_mutexConnections, it.second->manage()); // unlock during manage to not lock the whole process
	}
	_mutexConnections.unlock();
}

bool Invoker::run(Exception& exc, const volatile bool& stopping) {
	Buffer::Allocator::Set<BufferPool>();

	Timer::OnTimer onManage;

#if !defined(_DEBUG)
	try
#endif
	{ // Encapsulate sessions!

		onManage = [&](UInt32 count) {
			manage(); // client manage (script, etc..)
			return DELAY_CONNECTIONS_MANAGER;
		}; // manage every 2 seconds!
		_timer.set(onManage, DELAY_CONNECTIONS_MANAGER);
		while (!stopping) {
			if (wakeUp.wait(_timer.raise()))
				_handler.flush();
		}

		// Sessions deletion!
	}
#if !defined(_DEBUG)
	catch (exception& ex) {
		FATAL("Invoker, ", ex.what());
	}
	catch (...) {
		FATAL("Invoker, unknown error");
	}
#endif
	Logs::SetLevel(0); // /!\ To ensure that no possible static Logs variables will be used then (handle brutal program exit)

	// Stop onManage (useless now)
	_timer.set(onManage, 0);

	// do a handler flush here too because there could be few remaining tasks
	_handler.flush();
	Thread::stop(); // to set running() to false (and not more allows to handler to queue Runner)	

	// Destroy the connections
	{
		lock_guard<mutex>	lock(_mutexConnections);
		//Logs::RemoveLogger("LIBRTMFP");

		auto it = _mapConnections.begin();
		while (it != _mapConnections.end())
			removeConnection(it++, false, true);
	}

	// stop socket sending (it waits the end of sending last session messages)
	threadPool.join();

	// last handler!
	_handler.flush(true);

	// release memory
	Buffer::Allocator::Set();
	return true;
}

int Invoker::isInterrupted(UInt32 RTMFPcontext) {
	if (!Thread::running())
		return ERROR_APP_INTERRUPT;

	{
		lock_guard<mutex> lock(_mutexConnections);
		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end())
			return ERROR_CONN_INTERRUPT; 
		bool interrupted = it->second->isInterrupted();
		if (interrupted || it->second->status >= RTMFP::NEAR_CLOSED) {
			removeConnection(it, interrupted);
			return _mapConnections.empty()? ERROR_LAST_INTERRUPT : ERROR_CONN_INTERRUPT;
		}
	}
	return 0;
}

int Invoker::removeConnection(unsigned int index, bool blocking) {
	{
		lock_guard<mutex>	lock(_mutexConnections);
		if (_mapConnections.find(index) == _mapConnections.end()) {
			INFO("Connection at index ", index, " has already been removed")
			return 0;
		}
		if (!blocking && _mapConnections.size() == 1)
			return ERROR_LAST_INTERRUPT;
	}

	// Delete the session in the Handler thread and wait until operation is finished
	atomic<bool> ready(false);
	_handler.queue(onRemoveConnection, index, ready, blocking);

	if (blocking) {
		while (!ready) {
			int code(0);
			if ((code = isInterrupted(index)))
				return code;
			_waitSignal.wait(DELAY_BLOCKING_SIGNALS);
		}
		{
			lock_guard<mutex>	lock(_mutexConnections);
			if (_mapConnections.empty())
				return ERROR_LAST_INTERRUPT;
		}
	}
	return 1;
}

void Invoker::removeConnection(map<int, shared<RTMFPSession>>::iterator it, bool abrupt, bool terminating) {

	INFO("Deleting connection ", it->first, "...")

	if (!abrupt)
		it->second->closeSession(); // we must close here because there can be shared pointers

	// Release handlers
	it->second->onConnectionEvent = nullptr;
	it->second->onNetGroupException = nullptr;

	// Save the id and delete the connection
	int id = it->first;
	_mapConnections.erase(it);

	// Close possible fallback connection
	auto itWait = _waitingFallback.find(id);
	if (itWait != _waitingFallback.end()) {
		if (!terminating) { // to avoid corrupting the closing loop
			auto itConn = _mapConnections.find(itWait->second.idFallback);
			if (itConn != _mapConnections.end()) // can be deleted if an error occurs
				removeConnection(itConn, abrupt);
		}
		_waitingFallback.erase(itWait);
	}

	// Erase possible saved data
	{
		lock_guard<mutex> lock(_mutexRead);
		_connection2Buffer.erase(id);
	}

	// Erase possible writing buffer
	{
		lock_guard<mutex> lock(_mutexWrite);
		_writeBuffers.erase(id);
	}

	_waitSignal.set(); // can release from waiting event
}

UInt32 Invoker::connect(const char* url, RTMFPConfig* parameters) {

	string host;
	shared<Buffer> rawUrl(SET);
	SocketAddress address;
	PEER_LIST_ADDRESS_TYPE addresses;
	if (!RTMFP::ReadUrl(url, host, address, addresses, rawUrl))
		return 0;

	// Create the session
	UInt32 idConn(0);
	{
		lock_guard<mutex> lock(_mutexConnections);
		shared<RTMFPSession> pConn(SET, idConn = ++_lastIndex, *this, *parameters);
		pConn->setFlashProperties(parameters->swfUrl, parameters->app, parameters->pageUrl, parameters->flashVer);
		pConn->onConnectionEvent = [this](UInt32 index, UInt8 mask) {
			_waitSignal.set(); // release from waiting function
		};
		_mapConnections.emplace(idConn, pConn);
	}

	_handler.queue(onConnect, idConn, url, host, address, addresses, rawUrl);
	return idConn;
}

UInt16 Invoker::createMediaBuffer(UInt32 RTMFPcontext, function<bool(UInt16)> condition) {

	lock_guard<mutex> lockRead(_mutexRead);
	// Create the connection buffer if needed
	auto itBuffer = _connection2Buffer.lower_bound(RTMFPcontext);
	if (itBuffer == _connection2Buffer.end() || itBuffer->first != RTMFPcontext)
		itBuffer = _connection2Buffer.emplace_hint(itBuffer, piecewise_construct, forward_as_tuple(RTMFPcontext), forward_as_tuple());

	// Run the condition function
	ConnectionBuffer &connBuffer = itBuffer->second;
	if (!condition(connBuffer.mediaCount + 1))
		return 0;

	// Add the media buffer
	connBuffer.mapMedias.emplace(piecewise_construct, forward_as_tuple(++connBuffer.mediaCount), forward_as_tuple());
	return connBuffer.mediaCount;
}

int Invoker::connect2Peer(UInt32 RTMFPcontext, const char* peerId, const char* streamName) {
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return 0;
		}
	}

	atomic<UInt16> mediaId(0);
	atomic<bool> ready(false);
	_handler.queue(onConnect2Peer, RTMFPcontext, peerId, streamName, ready, mediaId);

	// Wait for the connection to happen
	while (!ready) {
		int code(0);
		if ((code = isInterrupted(RTMFPcontext)))
			return code;
		_waitSignal.wait(DELAY_BLOCKING_SIGNALS);
	}

	return ready ? mediaId.load() : 0;
}

void Invoker::startFallback(FallbackConnection& fallback) {

	if (fallback.idFallback) {
		WARN("Fallback for connection ", fallback.idConnection, " is already started")
		return;
	}

	// Create the session
	shared<RTMFPSession> pConn(SET, fallback.idFallback = ++_lastIndex, *this, fallback.parameters);
	pConn->setFlashProperties(fallback.parameters.swfUrl, fallback.parameters.app, fallback.parameters.pageUrl, fallback.parameters.flashVer);
	pConn->onConnectionEvent = [this, &fallback](UInt32 index, UInt8 mask) {

		// When fallback connection is connected we start playing
		if (mask & RTMFP_CONNECTED) {
			lock_guard<mutex> lock(_mutexConnections);
			auto itFb = _mapConnections.find(index);
			if (itFb == _mapConnections.end()) {
				WARN("Unable to start playing fallback connection, it is already closed")
				return;
			}

			itFb->second->addStream(RTMFP_UNDEFINED, fallback.streamName.c_str(), false, false, 1); // fallback media id is always 1
			fallback.running = true;
		}
	};

	// Connect & add the fallback connection to map of connections
	if (pConn->connect(fallback.url.c_str(), fallback.host, fallback.address, fallback.addresses, fallback.rawUrl))
		_mapConnections.emplace(fallback.idFallback, pConn); // Note: mutex is already locked here
}

int Invoker::connect2Group(UInt32 RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable, const char* fallbackUrl) {

	Exception ex;
	GroupSpecReader groupReader;
	if (!groupReader.parse(ex, groupParameters->netGroup)) {
		ERROR("Error during connection to group : ", ex)
		return 0;
	}
	string groupName = groupReader.getString("0e", "");

	// Compute the encrypted group specifier ID (2 consecutive sha256)
	UInt8 encryptedGroup[32];
	EVP_Digest(groupReader.groupTxt.data(), groupReader.groupTxt.size(), (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL);
	if (String::ICompare(groupReader.getString("7f"), "\x2")==0) // group v2?
		EVP_Digest(encryptedGroup, 32, (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL); // v2 groupspec needs 2 sha256s
	String groupHex(String::Hex(encryptedGroup, 32));
	DEBUG("Encrypted Group Id : ", groupHex)

	atomic<UInt16> mediaId(0);
	atomic<bool> ready(false);
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return 0;
		}
		it->second->onNetGroupException = [this](UInt32 idConn) {
			lock_guard<mutex> lock(_mutexConnections);
			auto itFallback = _waitingFallback.find(idConn);
			if (itFallback != _waitingFallback.end() && !itFallback->second.running) {
				INFO("Session ", idConn, " has been closed, starting fallback connection")
				startFallback(itFallback->second);
			}
		};
	}

	_handler.queue(onConnect2Group, RTMFPcontext, streamName, groupParameters, audioReliable, videoReliable, groupHex, groupReader.groupTxt, groupName, ready, mediaId);

	// Wait for the connection to happen
	while (!ready) {
		int code(0);
		if ((code = isInterrupted(RTMFPcontext)))
			return code;
		_waitSignal.wait(DELAY_BLOCKING_SIGNALS);
	}

	// Unicast fallback (only for viewers)
	if (!groupParameters->isPublisher && fallbackUrl) {
		lock_guard<mutex> lock(_mutexConnections);
		auto itFbConn = _waitingFallback.lower_bound(RTMFPcontext);
		if (itFbConn != _waitingFallback.end() && itFbConn->first == RTMFPcontext)
			ERROR("A waiting fallback connection exist already for connection ", RTMFPcontext)
		else {
			string host;
			shared<Buffer> rawUrl(SET);
			SocketAddress address;
			PEER_LIST_ADDRESS_TYPE addresses;
			char* streamName(NULL);

			// Extract name, host and addresses from url and create the fallback instance
			RTMFP_GetPublicationAndUrlFromUri(fallbackUrl, &streamName);
			if (RTMFP::ReadUrl(fallbackUrl, host, address, addresses, rawUrl))
				_waitingFallback.emplace_hint(itFbConn, piecewise_construct, forward_as_tuple(RTMFPcontext), 
					forward_as_tuple(RTMFPcontext, mediaId, streamName, parameters, fallbackUrl, host, address, addresses, move(rawUrl)));
		}
	}

	return mediaId.load();
}

int Invoker::addStream(UInt32 RTMFPcontext, UInt8 mask, const char* streamName, bool audioReliable, bool videoReliable) {
	atomic<UInt16> mediaId(0);
	atomic<bool> ready(false);
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return 0;
		}
	}

	_handler.queue(onCreateStream, RTMFPcontext, mask, streamName, audioReliable, videoReliable, mediaId, ready);

	// Wait for the stream to be created or the publication to be accepted
	while (!ready) {
		int code(0);
		if ((code = isInterrupted(RTMFPcontext)))
			return code;
		_waitSignal.wait(DELAY_BLOCKING_SIGNALS);
	}

	return ready? mediaId.load() : 0;
}

int Invoker::waitForEvent(UInt32 RTMFPcontext, UInt8 mask) {
	for (;;) {
		if (!Thread::running())
			return ERROR_APP_INTERRUPT;

		{
			lock_guard<mutex> lock(_mutexConnections);
			auto it = _mapConnections.find(RTMFPcontext);
			if (it == _mapConnections.end())
				return ERROR_CONN_INTERRUPT;
			bool interrupted = it->second->isInterrupted();
			if (interrupted || it->second->status >= RTMFP::NEAR_CLOSED) {
				removeConnection(it, interrupted);
				return _mapConnections.empty() ? ERROR_LAST_INTERRUPT : ERROR_CONN_INTERRUPT;
			}
			else if (it->second->flags & mask) // Event handled?
				break;
		}
		_waitSignal.wait(DELAY_BLOCKING_SIGNALS);
	}

	return 1;
}

bool Invoker::closePublication(UInt32 RTMFPcontext, const char* streamName) {
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return false;
		}
	}

	_handler.queue(onClosePublication, RTMFPcontext, streamName);
	return true;
}

bool Invoker::closeStream(UInt32 RTMFPcontext, UInt16 streamId) {
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
				return false;
		}
	}

	_handler.queue(onCloseStream, RTMFPcontext, streamId);
	return true;
}


bool Invoker::callFunction(UInt32 RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId) {
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return false;
		}
	}

	_handler.queue(onFunction, RTMFPcontext, function, nbArgs, args, peerId);
	return true;
}

int Invoker::write(unsigned int RTMFPcontext, const UInt8* data, UInt32 size) {

	int code(0);
	if ((code = isInterrupted(RTMFPcontext)))
		return code;

	// Find the writing buffer for current session
	lock_guard<mutex> lock(_mutexWrite);
	auto itBuffer = _writeBuffers.lower_bound(RTMFPcontext);
	if (itBuffer == _writeBuffers.end() || itBuffer->first != RTMFPcontext)
		itBuffer = _writeBuffers.emplace_hint(itBuffer, piecewise_construct, forward_as_tuple(RTMFPcontext), forward_as_tuple());
	WriteBuffer& writeBuffer = itBuffer->second;

	// FLV header
	BinaryReader reader(data, size);
	if (!writeBuffer.started) {
		if (size < 13 || data[0] != 'F' || data[1] != 'L' || data[2] != 'V') {
			ERROR("Invoker::write() - FLV Header not found or size < 13 (", size, ")")
			return -1;
		}
		reader.next(13);
		writeBuffer.started = true;
	}

	// Send all packets
	while (reader.available()) {

		// Packet Header
		if (!writeBuffer.buffer) {
			if (reader.available() < 11)
				break; // wait for at least the header part

			writeBuffer.type = reader.read8();
			if ((writeBuffer.size = reader.read24()) > MAX_WRITE_BUFFER_SIZE) {
				ERROR("Invoker::write() - Packet size is too big : ", MAX_WRITE_BUFFER_SIZE, " not supported")
				return -1;
			}
			writeBuffer.time = reader.read24() | (reader.read8() << 24);
			reader.next(3); // ignored (Stream ID)

			writeBuffer.buffer.set(writeBuffer.size);
			writeBuffer.writer.set(writeBuffer.buffer->data(), writeBuffer.buffer->size());
		}

		// Read current part
		UInt32 toRead = min(writeBuffer.size, reader.available()); 
		if (toRead) {
			writeBuffer.writer->write(reader.current(), toRead);
			writeBuffer.total += toRead;
			writeBuffer.size -= toRead;
			reader.next(toRead);
		}

		// Packet splitted or missing previous size bytes?
		if (writeBuffer.size || reader.available() < 4)
			break; // wait for the end of the packet

		UInt32 previousSize = reader.read32();
		if (previousSize != (writeBuffer.buffer->size() + 11))
			WARN("Invoker::write() - Unexpected previous size found for session ", RTMFPcontext, " : ", previousSize, ", expected : ", writeBuffer.buffer->size() + 11)

		// Packet complete, send it to the session
		if (writeBuffer.type == AMF::TYPE_AUDIO)
			_handler.queue(onPushAudio, RTMFPcontext, Packet(writeBuffer.buffer), writeBuffer.time, AMF::TYPE_AUDIO);
		else if (writeBuffer.type == AMF::TYPE_VIDEO)
			_handler.queue(onPushVideo, RTMFPcontext, Packet(writeBuffer.buffer), writeBuffer.time, AMF::TYPE_VIDEO);
		else if (writeBuffer.type == AMF::TYPE_DATA)
			_handler.queue(onPushData, RTMFPcontext, Packet(writeBuffer.buffer), writeBuffer.time, AMF::TYPE_DATA);
		else
			WARN("Invoker::write() - Unhandled packet type : ", writeBuffer.type)
		
		writeBuffer.writer.reset(); writeBuffer.buffer.reset();
	}

	// Flush if size >= RTMFP packet size
	if (writeBuffer.total >= RTMFP::SIZE_PACKET - 11) {
		_handler.queue(onFlushPublisher, RTMFPcontext);
		writeBuffer.total = 0;
	}
	return reader.position();
}

int Invoker::read(UInt32 RTMFPcontext, UInt16 mediaId, UInt8* buf, UInt32 size) {

	int nbRead = 0;
	Time noData;
	while (!nbRead) {

		int code(0);
		if ((code = isInterrupted(RTMFPcontext)))
			return code;

		_mutexRead.lock();
		auto itBuffer = _connection2Buffer.find(RTMFPcontext);
		if (itBuffer == _connection2Buffer.end()) {
			WARN("Unable to find the buffer for connection ", RTMFPcontext, ", it can be closed")
			_mutexRead.unlock();
			return ERROR_CONN_INTERRUPT;
		}

		auto itMedia = itBuffer->second.mapMedias.find(mediaId);
		if (itMedia == itBuffer->second.mapMedias.end()) {
			WARN("Unable to find buffer media ", mediaId, " of connection ", RTMFPcontext)
			nbRead = -1;
			_mutexRead.unlock();
			return ERROR_CONN_INTERRUPT;
		}

		if (!itMedia->second.mediaPackets.empty()) {
			// First read => send header
			BinaryWriter writer(buf, size);
			if (itMedia->second.firstRead && size > 13) {
				writer.write(EXPAND("FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00"));
				itMedia->second.firstRead = false;
			}

			// While media packets are available and buffer is not full
			while (!itMedia->second.mediaPackets.empty() && (writer.size() < size - 15)) {

				// Read next packet
				ConnectionBuffer::MediaBuffer::RTMFPMediaPacket& packet = itMedia->second.mediaPackets.front();
				UInt32 bufferSize = packet.size() - packet.pos;
				UInt32 toRead = (bufferSize > (size - writer.size() - 15)) ? size - writer.size() - 15 : bufferSize;

				// header
				if (!packet.pos) {
					writer.write8(packet.type);
					writer.write24(packet.size()); // size on 3 bytes
					writer.write24(packet.time); // time on 3 bytes
					writer.write8(packet.time >> 24); // timestamp upper
					writer.write24(0); // stream ID on 3 bytes, always 0
				}
				writer.write(packet.data() + packet.pos, toRead); // payload

				// If packet too big : save position and exit, else write footer
				if (bufferSize > toRead) {
					packet.pos += toRead;
					_mutexRead.unlock();
					break;
				}
				writer.write32(11 + packet.size()); // footer, size on 4 bytes
				itMedia->second.mediaPackets.pop_front();
			}
			// Finally update the nbRead & available
			nbRead += writer.size();
			_mutexRead.unlock();
		}
		else {
			_mutexRead.unlock();
			if (noData.isElapsed(1000)) {
				DEBUG("Nothing available during last second...")
				noData.update();
			}
			_waitSignal.wait(DELAY_SIGNAL_READ);
		}
	}
	return nbRead;
}

void Invoker::pushMedia(UInt32 RTMFPcontext, UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {

	threadPool.queue<ReadPacket>(_threadPush, *this, RTMFPcontext, mediaId, time, packet, lostRate, type);
}

void Invoker::bufferizeMedia(UInt32 RTMFPcontext, UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {

	bool newStream = false;
	UInt32 savedTime(0);
	{
		// Search if a fallback connection exist
		lock_guard<mutex> lockConn(_mutexConnections);		
		auto itFallback = _waitingFallback.find(RTMFPcontext);
		if (itFallback == _waitingFallback.end()) {

			// Fallback connection found? => patch the mediaId & idConnection and save the time
			for (auto& itWait : _waitingFallback) {
				if (itWait.second.idFallback == RTMFPcontext) {
					mediaId = itWait.second.mediaId; // switch media ID 
					RTMFPcontext = itWait.second.idConnection; // switch connection ID
					itWait.second.lastTime = time;
					break;
				}
			}
		}
		// NetGroup Connection with fallback url => first media packet, update the time and stop fallback
		else if (!itFallback->second.switched) {

			// Update the offset time
			if (itFallback->second.running) {
				INFO("First packet received, switching from fallback connection ", itFallback->second.idFallback, " to ", RTMFPcontext)
				savedTime = itFallback->second.lastTime;
				newStream = true;
				itFallback->second.running = false; // we reset the fallback state, can restart when the connection close
			}
			// Stop fallback if started
			if (itFallback->second.idFallback) {
				UNLOCK_RUN_LOCK(_mutexConnections, removeConnection(itFallback->second.idFallback, false));
				itFallback->second.idFallback = 0;
			}
			itFallback->second.switched = true; // no more fallback timeout
		}
	}		

	// Add the new packet
	{
		lock_guard<mutex> lock(_mutexRead);
		auto itBuffer = _connection2Buffer.find(RTMFPcontext);
		if (itBuffer == _connection2Buffer.end()) {
			DEBUG("Connection to ", RTMFPcontext, " has been removed, impossible to push the packet")
			return;
		}

		auto itMedia = itBuffer->second.mapMedias.find(mediaId);
		if (itMedia == itBuffer->second.mapMedias.end()) {
			DEBUG("Media ", mediaId, " from connection ", RTMFPcontext, " has been removed, impossible to push the packet")
			return;
		}
		// reset the media stream?
		if (newStream) {
			itMedia->second.timeOffset = (savedTime > time)? savedTime - time : 0;
			itMedia->second.codecInfosRead = false;
			itMedia->second.AACsequenceHeaderRead = false;
		}

		if (!itMedia->second.codecInfosRead && type == AMF::TYPE_VIDEO) {
			if (!RTMFP::IsVideoCodecInfos(packet.data(), packet.size())) {
				DEBUG("Video frame dropped to wait first key frame");
				return;
			}
			INFO("Video codec infos found, starting to read");
			itMedia->second.codecInfosRead = true;
		}
		// AAC : we wait for the sequence header packet 
		else if (!itMedia->second.AACsequenceHeaderRead && type == AMF::TYPE_AUDIO && (packet.size() > 1 && (*packet.data() >> 4) == 0x0A)) { // TODO: save the codec type
			if (!RTMFP::IsAACCodecInfos(packet.data(), packet.size())) {
				DEBUG("AAC frame dropped to wait first key frame (sequence header)");
				return;
			}
			INFO("AAC codec infos found, starting to read audio part");
			itMedia->second.AACsequenceHeaderRead = true;
		}

		itMedia->second.mediaPackets.emplace_back(packet, time + itMedia->second.timeOffset, type);
		_waitSignal.set(); // signal that data is available
	}
}

void Invoker::decode(int idConnection, UInt32 idSession, const SocketAddress& address, const shared<RTMFP::Engine>& pEngine, shared<Buffer>& pBuffer, UInt16& threadRcv) {

	shared<RTMFPDecoder> pDecoder(SET, idConnection, idSession, address, pEngine, pBuffer, handler);
	pDecoder->onDecoded = _onDecoded;
	threadPool.queue(threadRcv, move(pDecoder));
}
