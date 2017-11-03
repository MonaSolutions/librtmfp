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

// RTMFP Fallback connection wrapper from NetGroup to unicast
struct Invoker::FallbackConnection : virtual Base::Object {
	FallbackConnection(Base::UInt32 idConnection, Base::UInt32 idFallback, Base::UInt16 mediaId, const char* streamName) :
		lastTime(0), mediaId(mediaId), idConnection(idConnection), idFallback(idFallback), streamName(streamName), running(false), switched(false) {}
	virtual ~FallbackConnection() {}

	bool				switched; // True if the NetGroup Connection has started to send video
	bool				running; // True if the connection fallback has been started
	Base::UInt32		lastTime; // last time received from fallback connection (for time patching)
	const Base::UInt16	mediaId; // id of the main connection media for wrapping
	const Base::UInt32	idConnection; // id of the main connection
	const Base::UInt32	idFallback; // id of the fallback connection (unicast)
	const std::string	streamName; // stream name to subscribe to
	const Base::Time	timeCreation; // time when the fallback connection has been created
};

// Connection Buffer structure, contains the input media buffers from 1 session
struct Invoker::ConnectionBuffer : virtual Object {
	ConnectionBuffer() : mediaCount(0) {}
	virtual ~ConnectionBuffer() {
		for (auto &itBuffer : mapMedias)
			itBuffer.second.readSignal.set();
	}

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
		Signal									readSignal; // set this when receiving data
	};
	map<UInt16, MediaBuffer>					mapMedias; // Map of media players
	UInt16										mediaCount; // Counter of media streams (publisher/player) id
};

/** Invoker **/

Invoker::Invoker(bool createLogger) : Thread("Invoker"), _interruptCb(NULL), _interruptArg(NULL), handler(_handler), timer(_timer), sockets(_handler, threadPool), _lastIndex(0), _handler(wakeUp), _threadPush(0) {
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
			removeConnection(it);

		// To exit from the caller loop
		if (obj.blocking && Thread::running()) {
			obj.ready = true;
			_waitSignal.set();
		}
	};
	onPublishP2P = [this](PublishP2P& obj) {
		lock_guard<mutex>	lock(_mutexConnections);

		auto it = _mapConnections.find(obj.index);
		if (it != _mapConnections.end())
			it->second->startP2PPublisher(obj.streamName, obj.audioReliable, obj.videoReliable);
	};
	onConnect2Peer = [this](Connect2Peer& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto itConn = _mapConnections.find(obj.index);
		if (itConn != _mapConnections.end()) {

			// Start connecting to the peer and create the media buffer for this stream
			obj.mediaId = createMediaBuffer(obj.index, [&itConn, &obj](UInt16 mediaCount) {
				return itConn->second->connect2Peer(obj.peerId, obj.streamName, mediaCount);
			});
		}
	};
	onCreateStream = [this](CreateStream& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto itConn = _mapConnections.find(obj.index);
		if (itConn != _mapConnections.end()) {

			// Start connecting to the peer and create the media buffer for this stream
			obj.mediaId = createMediaBuffer(obj.index, [&itConn, &obj](UInt16 mediaCount) {
				return itConn->second->addStream(obj.publisher, obj.streamName, obj.audioReliable, obj.videoReliable, mediaCount);
			});
		}

		if (obj.publisher) // If publisher we wait for the onPublished() event
			return;
		
		// To exit from the caller loop
		obj.ready = true;
		_waitSignal.set();
	};
	onConnect2Group = [this](Connect2Group& obj) {
		lock_guard<mutex> lock(_mutexConnections);

		auto itConn = _mapConnections.find(obj.index);
		if (itConn != _mapConnections.end()) {
			// Start connecting to the group and create the media buffer for this stream
			obj.mediaId = createMediaBuffer(obj.index, [&itConn, &obj](UInt16 mediaCount) {
				return itConn->second->connect2Group(obj.streamName, obj.groupParameters, obj.audioReliable, obj.videoReliable, obj.groupHex, obj.groupTxt, mediaCount);
			});
		}

		if (obj.blocking) // If blocking we wait for the onConnected2Group() event
			return;

		// To exit from the caller loop
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

	if (createLogger) {
		_logger.reset(new RTMFPLogger());
		Logs::SetLogger(*_logger);
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
	onConnect = nullptr;
	onRemoveConnection = nullptr;
	onPublishP2P = nullptr;
	onConnect2Peer = nullptr;
	onCreateStream = nullptr;
	onConnect2Group = nullptr;
	_onDecoded = nullptr;
	_waitSignal.set();
}

// Start the socket manager if not started
bool Invoker::start() {
	if(running()) {
		ERROR("Invoker is already running, call stop method before");
		return false;
	}
	
	Exception ex;
	return Thread::start(ex);
}

void Invoker::manage() {
	
	_mutexConnections.lock();

	// Start waiting fallback connections
	auto itWait = _waitingFallback.begin();
	while (itWait != _waitingFallback.end()) {
		// If timeout reached we create the connection
		if (!itWait->second.switched && !itWait->second.running && itWait->second.timeCreation.isElapsed(TIMEOUT_FALLBACK_CONNECTION)) {

			INFO(TIMEOUT_FALLBACK_CONNECTION, "ms without data, starting fallback connection from ", itWait->first)
			startFallback(itWait->second);
			continue;
		}
		++itWait;
	}

	// Manage connections
	auto it = _mapConnections.begin();
	while(it != _mapConnections.end()) {
		int idConn = it->first;
		// unlock during manage to not lock the whole process
		UNLOCK_RUN_LOCK(_mutexConnections, bool erase = !it->second->manage());

		it = _mapConnections.lower_bound(idConn);
		if (it == _mapConnections.end() || it->first != idConn)
			continue; // connection deleted during manage()
		else if (erase) {
			// If there is a fallback running we ignore the request
			auto itFallback = _waitingFallback.find(idConn);
			if (itFallback == _waitingFallback.end() || !itFallback->second.running) {
				removeConnection(it++, true);
				continue;
			}
		}
		++it;
	}
	_mutexConnections.unlock();
}

bool Invoker::run(Exception& exc, const volatile bool& stopping) {
	//BufferPool bufferPool(timer);
	Allocator simpleAllocator;
	Buffer::SetAllocator(simpleAllocator);

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
	Thread::stop(); // to set running() to false (and not more allows to handler to queue Runner)
	// Stop onManage (useless now)
	_timer.set(onManage, 0);

	// Destroy the connections
	{
		lock_guard<mutex>	lock(_mutexConnections);
		if (_logger)
			Logs::SetDump(NULL); // we must set Dump to null because static dump object can be destroyed

		auto it = _mapConnections.begin();
		while (it != _mapConnections.end())
			removeConnection(it++);
	}

	// stop socket sending (it waits the end of sending last session messages)
	threadPool.join();

	// empty handler!
	_handler.flush();

	// release memory
	INFO("Invoker memory release");
	Buffer::SetAllocator();
	//bufferPool.clear();
	NOTE("Invoker stopped")
	if (_logger) {
		Logs::SetLogger(Logs::DefaultLogger()); // we must reset Logger to default to avoid crash if someone call Logs::Log() after Logger destruction
		_logger.reset();
	}
	return true;
}

void Invoker::setLogCallback(void(*onLog)(unsigned int, const char*, long, const char*)){
	if (_logger)
		_logger->setLogCallback(onLog);
}

void Invoker::setDumpCallback(void(*onDump)(const char*, const void*, unsigned int)) { 
	if (_logger)
		_logger->setDumpCallback(onDump);
}

void Invoker::setInterruptCallback(int(*interruptCb)(void*), void* argument) {
	_interruptCb = interruptCb;
	_interruptArg = argument;
}

bool Invoker::isInterrupted() {
	return !Thread::running() || (_interruptCb && _interruptCb(_interruptArg) == 1);
}

void Invoker::removeConnection(unsigned int index, bool blocking) {
	{
		lock_guard<mutex>	lock(_mutexConnections);
		if (_mapConnections.find(index) == _mapConnections.end()) {
			INFO("Connection at index ", index, " has already been removed")
			return;
		}
	}

	// Delete the session in the Handler thread and wait until operation is finished
	atomic<bool> ready(false);
	_handler.queue(onRemoveConnection, index, ready, blocking);

	if (blocking) {
		while (!ready && Thread::running())
			_waitSignal.wait(DELAY_BLOCKING_SIGNALS);
	}
}

void Invoker::removeConnection(map<int, shared_ptr<RTMFPSession>>::iterator it, bool abrupt) {

	INFO("Deleting connection ", it->first, "...")
	if (!abrupt)
		it->second->closeSession(); // we must close here because there can be shared pointers

	// Close possible fallback connection
	auto itWait = _waitingFallback.find(it->first);
	if (itWait != _waitingFallback.end()) {
		auto itConn = _mapConnections.find(itWait->second.idFallback);
		if (itConn != _mapConnections.end()) // can be deleted if an error occurs
			removeConnection(itConn);
		_waitingFallback.erase(itWait);
	}

	// Erase saved data
	{
		lock_guard<mutex> lock(_mutexRead);
		_connection2Buffer.erase(it->first);
	}

	it->second->onPublishP2P = nullptr;
	it->second->onConnectSucceed = nullptr;
	it->second->onConnected2Peer = nullptr;
	it->second->onStreamPublished = nullptr;
	it->second->onConnected2Group = nullptr;
	it->second->onNetGroupException = nullptr;
	_mapConnections.erase(it);
}

UInt32 Invoker::connect(const char* url, RTMFPConfig* parameters) {

	// Get hostname, port and publication name
	string host, publication, query;
	Util::UnpackUrl(url, host, publication, query);

	// Generate the raw url
	shared_ptr<Buffer> rawUrl(new Buffer());
	BinaryWriter urlWriter(*rawUrl);
	urlWriter.write7BitValue(strlen(url) + 1);
	urlWriter.write8('\x0A').write(url);

	// Extract the port
	size_t portPos = host.find_last_of(':'), ipv6End = host.find_last_of(']');
	if ((portPos != string::npos) && (ipv6End != string::npos) && portPos < ipv6End)
		portPos = string::npos;
	string port = (portPos != string::npos) ? host.substr(portPos + 1) : "1935";
	host = (portPos != string::npos) ? host.substr(0, portPos) : host;

	DEBUG("Trying to resolve the host address...")
	HostEntry hostEntry;
	SocketAddress address;
	PEER_LIST_ADDRESS_TYPE addresses;
	Exception ex;
	if (!address.set(ex, host, port)) {
		if (DNS::Resolve(ex, host, hostEntry)) { // list of addresses
			for (auto& itAddress : hostEntry.addresses()) {
				if (address.set(ex, itAddress, port))
					addresses.emplace(address, RTMFP::ADDRESS_PUBLIC);
			}
			address.reset();
		}
	}
	if (!address && addresses.empty()) {
		ERROR("Unable to resolve host address : ", ex)
		return 0;
	}	

	// Create the session
	bool ready(false);
	int idConn(0); // 
	{
		lock_guard<mutex> lock(_mutexConnections);
		shared_ptr<RTMFPSession> pConn(new RTMFPSession(idConn = ++_lastIndex, *this, parameters->pOnStatusEvent, parameters->pOnMedia));
		pConn->setFlashProperties(parameters->swfUrl, parameters->app, parameters->pageUrl, parameters->flashVer);
		if (parameters->isBlocking) {
			pConn->onConnectSucceed = [this, &ready]() {
				ready = true;
				_waitSignal.set();
			};
		}
		pConn->onNetGroupException = [this](UInt32 idConn) {
			auto itFallback = _waitingFallback.find(idConn);
			if (itFallback != _waitingFallback.end() && !itFallback->second.running) {
				INFO("Session ", idConn, " has been closed, starting fallback connection")
				startFallback(itFallback->second);
			}
		};
		_mapConnections.emplace(idConn, pConn);
	}

	_handler.queue(onConnect, idConn, url, host, address, addresses, rawUrl);

	// Wait for the connection to happen
	if (parameters->isBlocking) {
		while (!ready && !isInterrupted())
			_waitSignal.wait(DELAY_BLOCKING_SIGNALS);

		// unsubscribe from event if needed
		if (!isInterrupted()) {
			lock_guard<mutex> lock(_mutexConnections);

			auto it = _mapConnections.find(idConn);
			if (it != _mapConnections.end())
				it->second->onConnectSucceed = nullptr;
		}
		return !ready ? 0 : idConn;
	}
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

UInt16 Invoker::connect2Peer(UInt32 RTMFPcontext, const char* peerId, const char* streamName, bool blocking) {
	atomic<UInt16> mediaId(0), ready(false);
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return false;
		}
		if (blocking)
			it->second->onConnected2Peer = [this, &ready]() {
				ready = true;
				_waitSignal.set();
			};
	}

	_handler.queue(onConnect2Peer, RTMFPcontext, peerId, streamName, mediaId);

	// Wait for the connection to happen
	if (blocking) {
		while (!ready && !isInterrupted())
			_waitSignal.wait(DELAY_BLOCKING_SIGNALS);

		// unsubscribe from event if needed
		if (!isInterrupted()) {
			lock_guard<mutex> lock(_mutexConnections);

			auto it = _mapConnections.find(RTMFPcontext);
			if (it != _mapConnections.end())
				it->second->onConnected2Peer = nullptr;
		}
	}
	return mediaId;
}

bool Invoker::connect2FallbackUrl(UInt32 RTMFPcontext, RTMFPConfig* parameters, const char* fallbackUrl, Base::UInt16 mediaId) {
	bool ret(false);
	_mutexConnections.lock();
	auto itFbConn = _waitingFallback.lower_bound(RTMFPcontext);
	if (itFbConn != _waitingFallback.end() && itFbConn->first == RTMFPcontext)
		ERROR("A waiting fallback connection exist already for connection ", RTMFPcontext)
	else {

		// Connect to the fallback url
		short isBlocking = parameters->isBlocking; // save blocking state before connecting
		parameters->isBlocking = 1;
		UNLOCK_RUN_LOCK(_mutexConnections, UInt32 idFallback = connect(fallbackUrl, parameters));
		parameters->isBlocking = isBlocking;

		if (idFallback) {
			char* streamName(NULL);
			RTMFP_GetPublicationAndUrlFromUri(fallbackUrl, &streamName);
			_waitingFallback.emplace_hint(itFbConn, piecewise_construct, forward_as_tuple(RTMFPcontext), forward_as_tuple(RTMFPcontext, idFallback, mediaId, streamName));
		}
		// else not important, we continue the group connection
		ret = true;
	}
	_mutexConnections.unlock();
	return ret;
}

void Invoker::startFallback(FallbackConnection& fallback) {
	auto itFb = _mapConnections.find(fallback.idFallback);
	if (itFb == _mapConnections.end()) {
		WARN("Unable to start playing fallback connection, it is already closed")
		return;
	}
	
	// Start playing the fallback stream
	itFb->second->addStream(false, fallback.streamName.c_str(), false, false, 1); // fallback media id is always 1
	fallback.running = true;
}

UInt16 Invoker::connect2Group(UInt32 RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable) {

	if (strncmp("G:", groupParameters->netGroup, 2) != 0) {
		ERROR("Group ID not well formated, it must begin with 'G:'")
		return 0;
	}

	string value;
	bool groupV2 = false;
	const char* endMarker = NULL;

	// Create the reader of NetGroup ID
	Buffer buff;
	String::ToHex(groupParameters->netGroup + 2, buff);
	BinaryReader reader(buff.data(), buff.size());

	// Read each NetGroup parameters and save group version + end marker
	while (reader.available() > 0) {
		UInt8 size = reader.read8();
		if (size == 0) {
			endMarker = groupParameters->netGroup + 2 * reader.position();
			break;
		}
		else if (reader.available() < size)
			break;

		reader.read(size, value);
		if (!groupV2 && value == "\x7f\x02")
			groupV2 = true;
	}

	// Keep the meanful part of the group ID (before end marker)
	if (!endMarker) {
		ERROR("Group ID not well formated")
		return 0;
	}
	string groupTxt(groupParameters->netGroup, endMarker);

	// Compute the encrypted group specifier ID (2 consecutive sha256)
	UInt8 encryptedGroup[32];
	EVP_Digest(groupTxt.data(), groupTxt.size(), (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL);
	if (groupV2)
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
				return false;
		}
		if (parameters->isBlocking && groupParameters->isPublisher)
			it->second->onConnected2Group = [this, &ready]() {
				ready = true;
				_waitSignal.set();
			};
	}

	_handler.queue(onConnect2Group, RTMFPcontext, streamName, groupParameters, audioReliable, videoReliable, groupHex, groupTxt, parameters->isBlocking && groupParameters->isPublisher, ready, mediaId);

	// Wait for the connection to happen
	while (!ready && !isInterrupted())
		_waitSignal.wait(DELAY_BLOCKING_SIGNALS);

	// unsubscribe from event if needed
	if (!isInterrupted() && parameters->isBlocking && groupParameters->isPublisher) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it != _mapConnections.end())
			it->second->onConnected2Group = nullptr;
	}

	return ready? mediaId.load() : 0;
}

UInt16 Invoker::addStream(UInt32 RTMFPcontext, bool publisher, const char* streamName, bool audioReliable, bool videoReliable, bool blocking) {
	atomic<UInt16> mediaId(0);
	atomic<bool> ready(false);
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
				return false;
		}
		if (blocking && publisher)
			it->second->onStreamPublished = [this, &ready]() { // publication accepted
				ready = true;
				_waitSignal.set();
			};
	}

	_handler.queue(onCreateStream, RTMFPcontext, publisher, streamName, audioReliable, videoReliable, mediaId, ready);

	// Wait for the stream to be created or the publication to be accepted
	while (!ready && !isInterrupted())
		_waitSignal.wait(DELAY_BLOCKING_SIGNALS);

	// unsubscribe from event if needed
	if (!isInterrupted() && blocking && publisher) {
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it != _mapConnections.end())
			it->second->onStreamPublished = nullptr;
	}

	return ready? mediaId.load() : 0;
}

bool Invoker::publishP2P(UInt32 RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {
	bool ret(false), ready(false);
	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return false;
		}
		if (blocking)
			it->second->onPublishP2P = [this, &ret, &ready](bool succeed) {
				ret = succeed;
				ready = true;
				_waitSignal.set();
			};
	}

	_handler.queue(onPublishP2P, RTMFPcontext, streamName, audioReliable > 0, videoReliable > 0);

	// Wait for a peer to connect to us
	if (blocking) {
		while (!ready && !isInterrupted())
			_waitSignal.wait(DELAY_BLOCKING_SIGNALS);

		// unsubscribe from event
		if (!isInterrupted()) {
			lock_guard<mutex> lock(_mutexConnections);
			auto it = _mapConnections.find(RTMFPcontext);
			if (it != _mapConnections.end())
				it->second->onPublishP2P = nullptr;
		}
	}
	return ret;
}

bool Invoker::closePublication(Base::UInt32 RTMFPcontext, const char* streamName) {
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

bool Invoker::closeStream(Base::UInt32 RTMFPcontext, Base::UInt16 streamId) {
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


bool Invoker::callFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId) {
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

	{
		lock_guard<mutex> lock(_mutexConnections);

		auto it = _mapConnections.find(RTMFPcontext);
		if (it == _mapConnections.end()) {
			ERROR("Unable to find the connection ", RTMFPcontext)
			return -1;
		}
		else if (it->second->status >= RTMFP::NEAR_CLOSED)
			return -1; // to stop the caller loop
	}

	int pos = 0;
	BinaryReader reader(data, size);
	if (reader.available()<14) {
		DEBUG("Packet too small")
		return -1;
	}

	const UInt8* cur = reader.current();
	if (*cur == 'F' && *(++cur) == 'L' && *(++cur) == 'V') { // header (TODO: do the check only once)
		reader.next(13);
		pos = 13;
	}

	// Send all packets
	while (reader.available()) {
		if (reader.available() < 11) // smaller than flv header
			break;

		UInt8 type = reader.read8();
		UInt32 bodySize = reader.read24();
		UInt32 time = reader.read24();
		reader.next(4); // ignored

		if (reader.available() < bodySize + 4)
			break; // we will wait for further data

		if (type == AMF::TYPE_AUDIO)
			_handler.queue(onPushAudio, RTMFPcontext, Packet(reader.current(), bodySize), time, AMF::TYPE_AUDIO);
		else if (type == AMF::TYPE_VIDEO)
			_handler.queue(onPushVideo, RTMFPcontext, Packet(reader.current(), bodySize), time, AMF::TYPE_VIDEO);
		else
			WARN("Unhandled packet type : ", type)
			reader.next(bodySize);
		UInt32 sizeBis = reader.read32();
		pos += bodySize + 15;
		if (sizeBis != bodySize + 11) {
			ERROR("Unexpected size found after payload : ", sizeBis, " (expected: ", bodySize + 11, ")")
			break;
		}
	}
	_handler.queue(onFlushPublisher, RTMFPcontext);
	return pos;
}

int Invoker::read(UInt32 RTMFPcontext, UInt16 mediaId, UInt8* buf, UInt32 size) {

	_mutexRead.lock();
	int nbRead = 0;
	Time noData;
	while (!nbRead && !isInterrupted()) {

		auto itBuffer = _connection2Buffer.find(RTMFPcontext);
		if (itBuffer == _connection2Buffer.end()) {
			WARN("Unable to find the buffer for connection ", RTMFPcontext, ", it can be closed")
			nbRead = -1;
			break;
		}

		auto itMedia = itBuffer->second.mapMedias.find(mediaId);
		if (itMedia == itBuffer->second.mapMedias.end()) {
			WARN("Unable to find buffer media ", mediaId, " of connection ", RTMFPcontext)
			nbRead = -1;
			break;
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
					break;
				}
				writer.write32(11 + packet.size()); // footer, size on 4 bytes
				itMedia->second.mediaPackets.pop_front();
			}
			// Finally update the nbRead & available
			nbRead += writer.size();
		}
		else {
			if (noData.isElapsed(1000)) {
				DEBUG("Nothing available during last second...")
				noData.update();
			}
			UNLOCK_RUN_LOCK(_mutexRead, itMedia->second.readSignal.wait(DELAY_SIGNAL_READ));
		}
	}
	_mutexRead.unlock();
	return nbRead;
}

void Invoker::pushMedia(UInt32 RTMFPcontext, UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {

	Exception ex;
	shared_ptr<ReadPacket> pPacket(new ReadPacket(*this, RTMFPcontext, mediaId, time, packet, lostRate, type));
	AUTO_ERROR(threadPool.queue(ex, pPacket, _threadPush), "Invoker PushMedia")
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

			if (itFallback->second.running) {
				INFO("First packet received, switching from fallback connection ", itFallback->second.idFallback, " to ", RTMFPcontext)
				savedTime = itFallback->second.lastTime;
				newStream = true;
				// Close the stream
				UNLOCK_RUN_LOCK(_mutexConnections, closeStream(itFallback->second.idFallback, 1));
				itFallback->second.running = false; // we reset the fallback state, can restart when the connection close
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
			INFO("Video codec infos found, starting to read")
				itMedia->second.codecInfosRead = true;
		}
		// AAC : we wait for the sequence header packet 
		else if (!itMedia->second.AACsequenceHeaderRead && type == AMF::TYPE_AUDIO && (packet.size() > 1 && (*packet.data() >> 4) == 0x0A)) { // TODO: save the codec type
			if (!RTMFP::IsAACCodecInfos(packet.data(), packet.size())) {
				DEBUG("AAC frame dropped to wait first key frame (sequence header)");
				return;
			}
			INFO("AAC codec infos found, starting to read audio part")
				itMedia->second.AACsequenceHeaderRead = true;
		}

		itMedia->second.mediaPackets.emplace_back(packet, time + itMedia->second.timeOffset, type);
		itMedia->second.readSignal.set(); // signal that data is available
	}
}

void Invoker::decode(int idConnection, UInt32 idSession, const SocketAddress& address, const shared_ptr<RTMFP::Engine>& pEngine, shared_ptr<Buffer>& pBuffer, UInt16& threadRcv) {

	shared_ptr<RTMFPDecoder> pDecoder(new RTMFPDecoder(idConnection, idSession, address, pEngine, pBuffer, handler));
	pDecoder->onDecoded = _onDecoded;
	Exception ex;
	AUTO_ERROR(threadPool.queue(ex, pDecoder, threadRcv), "RTMFP Decode")
}
