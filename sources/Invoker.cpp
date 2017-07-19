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
#include "librtmfp.h"

// Method used to unlock a mutex during an instruction which can take time
#define UNLOCK_RUN_LOCK(MUTEX, INSTRUCTION)	MUTEX.unlock(); INSTRUCTION; MUTEX.lock()

using namespace Base;
using namespace std;

struct Invoker::FallbackConnection : virtual Base::Object {
	FallbackConnection(Base::UInt32 idConnection, Base::UInt32 idFallback, Base::UInt16 mediaId, const char* streamName) :
		lastTime(0), mediaId(mediaId), idConnection(idConnection), idFallback(idFallback), streamName(streamName) {}
	virtual ~FallbackConnection() {}

	Base::UInt32		lastTime; // last time received from fallback connection (for time patching)
	const Base::UInt16	mediaId;
	const Base::UInt32	idConnection;
	const Base::UInt32	idFallback;
	const std::string	streamName;
	const Base::Time	timeCreation; // time when the fallback connection has been created
};

// Connection Buffer structure, contains the media buffers
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

struct Invoker::MediaPacket : Runner, Packet, virtual Object {
	MediaPacket(Invoker& invoker, UInt32 RTMFPcontext, UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) :
		invoker(invoker), idConn(RTMFPcontext), idMedia(mediaId), time(time), Packet(move(packet)), lostRate(lostRate), type(type), Runner("MediaPacket") {}

	bool run(Base::Exception& ex) {
		invoker.bufferizeMedia(idConn, idMedia, time, *this, lostRate, type);
		return true;
	}

	UInt32 idConn;
	UInt16 idMedia;
	UInt32 time;
	const double lostRate;
	const AMF::Type type;
	Invoker& invoker;
};

/** Invoker **/

Invoker::Invoker(bool createLogger) : Thread("Invoker"), _interruptCb(NULL), _interruptArg(NULL), handler(_handler), timer(_timer), sockets(_handler, threadPool), _lastIndex(0), _handler(wakeUp), _threadPush(0) {

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

bool Invoker::getConnection(unsigned int index, std::shared_ptr<RTMFPSession>& pConn) {
	lock_guard<mutex>	lock(_mutexConnections);
	auto it = _mapConnections.find(index);
	if(it == _mapConnections.end()) {
		WARN("There is no connection at specified index ", index)
		return false;
	}

	pConn = it->second;
	return true;
}

void Invoker::removeConnection(unsigned int index) {
	lock_guard<mutex>	lock(_mutexConnections);
	auto it = _mapConnections.find(index);
	if(it == _mapConnections.end()) {
		INFO("Connection at index ", index, " as already been removed")
		return;
	}
	removeConnection(it);
}

void Invoker::removeConnection(map<int, shared_ptr<RTMFPSession>>::iterator it, bool abrupt) {

	INFO("Deleting connection ", it->first, "...")
	if (!abrupt)
		it->second->closeSession(); // we must close here because there can be shared pointers

	// Erase fallback connections
	_waitingFallback.erase(it->first);
	_connection2Fallback.erase(it->first);

	// Erase saved data
	{
		lock_guard<mutex> lock(_mutexRead);
		_connection2Buffer.erase(it->first);
	}

	_mapConnections.erase(it);
}

unsigned int Invoker::empty() {
	lock_guard<mutex>	lock(_mutexConnections);
	return _mapConnections.empty();
}

void Invoker::manage() {
	
	_mutexConnections.lock();

	// Start waiting fallback connections
	auto itWait = _waitingFallback.begin();
	while (itWait != _waitingFallback.end()) {
		// If timeout reached we create the connection
		if (itWait->second->timeCreation.isElapsed(TIMEOUT_FALLBACK_CONNECTION)) {
			INFO(TIMEOUT_FALLBACK_CONNECTION, "ms without data, starting fallback connection from ", itWait->first)

			auto itFb = _mapConnections.find(itWait->second->idFallback);
			FATAL_CHECK(itFb != _mapConnections.end()) // implementation error

			// Start playing the fallback stream
			itFb->second->addStream(false, itWait->second->streamName.c_str(), false, false, 0); // media count == 0, we do not create a buffer for this connection
			_connection2Fallback.emplace(itWait->first, itWait->second);
			_waitingFallback.erase(itWait++);
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
		else if (erase)
			removeConnection(it++, true);
		else
			++it;
	}
	_mutexConnections.unlock();
}

bool Invoker::run(Exception& exc, const volatile bool& stopping) {
	BufferPool bufferPool(timer);
	Buffer::SetAllocator(bufferPool);

	Timer::OnTimer onManage;

#if !defined(_DEBUG)
	try
#endif
	{ // Encapsulate sessions!

		onManage = ([&](UInt32 count) {
			manage(); // client manage (script, etc..)
			return DELAY_CONNECTIONS_MANAGER;
		}); // manage every 2 seconds!
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
	bufferPool.clear();
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

UInt32 Invoker::connect(const char* url, RTMFPConfig* parameters) {

	_mutexConnections.lock();
	// Get hostname, port and publication name
	string host, publication, query;
	Util::UnpackUrl(url, host, publication, query);

	Exception ex;
	shared_ptr<RTMFPSession> pConn(new RTMFPSession(*this, parameters->pOnSocketError, parameters->pOnStatusEvent, parameters->pOnMedia));
	auto itConn = _mapConnections.emplace(++_lastIndex, pConn).first;
	int ret = pConn->_id = _lastIndex;
	if (!pConn->connect(ex, url, host.c_str())) {
		ERROR("Error in connect : ", ex)
		removeConnection(itConn);
		ret = 0;
	}
	else if (parameters->isBlocking) {
		while (!pConn->connectReady) {
			// sleep until connection is ready or timeout reached (unlock timeout to not lock the whole process)
			UNLOCK_RUN_LOCK(_mutexConnections, pConn->connectSignal.wait(200));
			if (isInterrupted()) {
				UNLOCK_RUN_LOCK(_mutexConnections, removeConnection(pConn->_id));
				ret = 0;
				break;
			}
		}
	}
	_mutexConnections.unlock();
	return ret;
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
	_mutexConnections.lock();
	auto itConn = _mapConnections.find(RTMFPcontext);
	if (itConn == _mapConnections.end()) {
		WARN("There is no connection at specified index ", RTMFPcontext)
		return 0;
	}

	// Start connecting to the peer and create the media buffer for this stream
	UInt16 mediaId = createMediaBuffer(RTMFPcontext, [&itConn, peerId, streamName](UInt16 mediaCount) { return itConn->second->connect2Peer(peerId, streamName, mediaCount); });
	if (!mediaId)
		return 0;

	if (blocking) {
		while (((itConn = _mapConnections.find(RTMFPcontext)) != _mapConnections.end()) && !itConn->second->p2pPlayReady) {
			UNLOCK_RUN_LOCK(_mutexConnections, itConn->second->p2pPlaySignal.wait(200));
			if (isInterrupted()) {
				mediaId = 0;
				break;
			}
		}
	}
	_mutexConnections.unlock();
	return mediaId;
}

UInt16 Invoker::connect2Group(UInt32 RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, bool audioReliable, bool videoReliable, const char* fallbackUrl) {

	_mutexConnections.lock();
	UInt16 mediaId(0);
	auto it = _mapConnections.find(RTMFPcontext);
	if (it == _mapConnections.end())
		ERROR("Unable to find the connection ", RTMFPcontext)
	else {

		// Start connecting to the group and create the media buffer for this stream
		mediaId = createMediaBuffer(RTMFPcontext, [&it, streamName, groupParameters, audioReliable, videoReliable](UInt16 mediaCount) {
			 return it->second->connect2Group(streamName, groupParameters, audioReliable, videoReliable, mediaCount);
		});

		// Create fallback and start waiting
		if (mediaId && fallbackUrl) {

			if (_connection2Fallback.find(RTMFPcontext) != _connection2Fallback.end()) {
				ERROR("A fallback connection exist already for connection ", RTMFPcontext)
				mediaId = 0;
			}
			auto itFbConn = _waitingFallback.lower_bound(RTMFPcontext);
			if (itFbConn != _waitingFallback.end() && itFbConn->first == RTMFPcontext) {
				ERROR("A waiting fallback connection exist already for connection ", RTMFPcontext)
				mediaId = 0;
			}
			else {

				 // Connect to the fallback url
				 short isBlocking = parameters->isBlocking; // save blocking state before connecting
				 parameters->isBlocking = 1;
				 UNLOCK_RUN_LOCK(_mutexConnections, UInt32 idFallback = connect(fallbackUrl, parameters));
				 parameters->isBlocking = isBlocking;

				 if (idFallback) {
					 char* streamName(NULL);
					 RTMFP_GetPublicationAndUrlFromUri(fallbackUrl, &streamName);
					 _waitingFallback.emplace_hint(itFbConn, piecewise_construct, forward_as_tuple(RTMFPcontext), forward_as_tuple(new FallbackConnection(RTMFPcontext, idFallback, mediaId, streamName)));
				 }
				// else not important, we continue the group connection
			}
		}

		// Connect to the group
		if (mediaId && parameters->isBlocking && groupParameters->isPublisher) {
			while (((it = _mapConnections.find(RTMFPcontext)) != _mapConnections.end()) && !it->second->publishReady) {

				UNLOCK_RUN_LOCK(_mutexConnections, it->second->publishSignal.wait(200));
				if (isInterrupted()) {
					mediaId = 0;
					break;
				}
			}
		}
	}
	_mutexConnections.unlock();
	return mediaId;
}

UInt16 Invoker::addStream(UInt32 RTMFPcontext, bool publisher, const char* streamName, bool audioReliable, bool videoReliable, bool blocking) {

	UInt16 mediaId(0);
	_mutexConnections.lock();
	auto it = _mapConnections.find(RTMFPcontext);
	if (it == _mapConnections.end())
		ERROR("Unable to find the connection ", RTMFPcontext)
	else {

		// Create the stream and the media buffer for this stream
		mediaId = createMediaBuffer(RTMFPcontext, [&it, publisher, streamName, audioReliable, videoReliable](UInt16 mediaCount) {
			return it->second->addStream(publisher, streamName, audioReliable, videoReliable, mediaCount);
		});

		if (mediaId && publisher && blocking) {
			while (((it = _mapConnections.find(RTMFPcontext)) != _mapConnections.end()) && !it->second->publishReady) {

				UNLOCK_RUN_LOCK(_mutexConnections, it->second->publishSignal.wait(200));
				if (isInterrupted()) {
					mediaId = 0;
					break;
				}
			}
		}
	}

	_mutexConnections.unlock();
	return mediaId;
}

bool Invoker::publishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {

	_mutexConnections.lock();
	bool ret(false);
	auto it = _mapConnections.find(RTMFPcontext);
	if (it == _mapConnections.end())
		ERROR("Unable to find the connection ", RTMFPcontext)
	else {

		ret = it->second->startP2PPublisher(streamName, audioReliable > 0, videoReliable > 0);

		if (ret && blocking) {
			while (((it = _mapConnections.find(RTMFPcontext)) != _mapConnections.end()) && !it->second->p2pPublishReady) {

				UNLOCK_RUN_LOCK(_mutexConnections, it->second->p2pPublishSignal.wait(200));
				if (isInterrupted()) {
					ret = false;
					break;
				}
			}
		}
	}

	_mutexConnections.unlock();
	return ret;
}

int Invoker::read(UInt32 RTMFPcontext, UInt16 mediaId, UInt8* buf, UInt32 size, int& nbRead) {

	_mutexRead.lock();
	int ret = 0;
	Time noData;
	while (!nbRead) {

		{
			lock_guard<mutex> lock(_mutexConnections);
			if (isInterrupted())
				break;
		}

		auto itBuffer = _connection2Buffer.find(RTMFPcontext);
		if (itBuffer == _connection2Buffer.end()) {
			DEBUG("Unable to find the buffer for connection ", RTMFPcontext, ", it can be closed")
			break;
		}

		auto itMedia = itBuffer->second.mapMedias.find(mediaId);
		if (itMedia == itBuffer->second.mapMedias.end()) {
			WARN("Unable to find buffer media ", mediaId, " of connection ", RTMFPcontext)
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
			ret = 1;
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
	return ret;
}

void Invoker::pushMedia(UInt32 RTMFPcontext, UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {

	Exception ex;
	shared_ptr<MediaPacket> pPacket(new MediaPacket(*this, RTMFPcontext, mediaId, time, packet, lostRate, type));
	AUTO_ERROR(threadPool.queue(ex, pPacket, _threadPush), "Invoker PushMedia")
}

void Invoker::bufferizeMedia(UInt32 RTMFPcontext, UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {

	bool newStream = false;
	UInt32 eraseFallback(0), savedTime(0);
	{
		// Search if a fallback connection exist
		lock_guard<mutex> lockConn(_mutexConnections);
		auto itFallback = _connection2Fallback.begin();
		while (itFallback != _connection2Fallback.end()) {
			// Fallback connection? => patch the mediaId & idConnection and save the time
			if (itFallback->second->idFallback == RTMFPcontext) {
				mediaId = itFallback->second->mediaId; // switch media ID 
				RTMFPcontext = itFallback->second->idConnection; // switch connection ID
				itFallback->second->lastTime = time;
				break;
			}
			// Group Connection => first media packet, update the time and delete fallback
			else if (itFallback->first == RTMFPcontext) {
				INFO("First packet received, deleting fallback connection ", itFallback->second->idFallback, " to ", RTMFPcontext)
				savedTime = itFallback->second->lastTime;
				newStream = true;
				eraseFallback = itFallback->second->idFallback;
				_connection2Fallback.erase(itFallback);
				break;
			}
			++itFallback;
		}

		// Search if a waiting fallback connection exist
		auto itWait = _waitingFallback.find(RTMFPcontext);
		if (itWait != _waitingFallback.end()) {
			INFO("First packet received, deleting waiting fallback connection ", itWait->second->idFallback, " to ", RTMFPcontext)
			eraseFallback = itWait->second->idFallback;
			_waitingFallback.erase(itWait);
		}
	}
	// Remove fallback connection if needed
	if (eraseFallback)
		removeConnection(eraseFallback);

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
