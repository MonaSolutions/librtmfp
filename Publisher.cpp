#include "Publisher.h"
#include "RTMFP.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"
#include "Listener.h"

using namespace Mona;
using namespace std;

Publisher::Publisher(const string& name, const PoolBuffers& poolBuffers, bool audioReliable, bool videoReliable) : _running(false), _new(false), _name(name), publishAudio(true), publishVideo(true),
	_audioReliable(audioReliable), _videoReliable(videoReliable), _poolBuffers(poolBuffers), _audioCodecBuffer(poolBuffers), _videoCodecBuffer(poolBuffers) {

	INFO("Initialization of the publisher ", _name, " (audioReliable : ", _audioReliable, " - videoReliable : ", _videoReliable, ")")
}

Publisher::~Publisher() {
	// delete listeners
	if (!_listeners.empty()) {
		ERROR("Publication ",_name," with subscribers is deleting")
		while (!_listeners.empty())
			removeListener(_listeners.begin()->first);
	}
	if (_running)
		ERROR("Publication ",_name," running is deleting")
	DEBUG("Publication ",_name," deleted");
}

Listener* Publisher::addListener(Exception& ex, const string& identifier, FlashWriter& writer) {
	auto it = _listeners.lower_bound(identifier);
	if (it != _listeners.end() && it->first == identifier) {
		ex.set(Exception::APPLICATION, "Already subscribed to ", _name);
		return NULL;
	}
	if (it != _listeners.begin())
		--it;
	Listener* pListener = new Listener(*this, identifier, writer);
	_listeners.emplace_hint(it, identifier, pListener);
	return pListener;
}

void Publisher::removeListener(const string& identifier) {
	auto it = _listeners.find(identifier);
	if (it == _listeners.end()) {
		WARN("Already unsubscribed of publication ", _name);
		return;
	}
	Listener* pListener = it->second;
	_listeners.erase(it);
	delete pListener;
}

void Publisher::start() {
	if (_running)
		return;
	INFO("Publication ", _name, " started")
	_running = true;  // keep before startPublishing()
	for (auto& it : _listeners) {
		it.second->startPublishing();
		it.second->flush(); // flush possible messages in startPublishing
	}
}

void Publisher::stop() {
	if (!_running)
		return; // already done
	INFO("Publication ", _name, " stopped")
	for (auto& it : _listeners) {
		it.second->stopPublishing();
		it.second->flush(); // flush possible last media + messages in stopPublishing
	}
	/*_properties.clear();
	_propertiesWriter.clear();
	_videoQOS.reset();
	_audioQOS.reset();
	_dataQOS.reset();
	_lastTime = 0;*/
	_running = false;
	_videoCodecBuffer.release();
	_audioCodecBuffer.release();
}

bool Publisher::publish(const Mona::UInt8* data, Mona::UInt32 size, int& pos) {
	lock_guard<recursive_mutex> lock(_mediaMutex); // TODO: check if it is better to use a Task and wait
	PacketReader packet(data, size);
	if (packet.available()<14) {
		DEBUG("Packet too small")
		return true;
	}

	const UInt8* cur = packet.current();
	if (*cur == 'F' && *(++cur) == 'L' && *(++cur) == 'V') { // header
		packet.next(13);
		pos = +13;
	}

	while (packet.available()) {
		if (packet.available() < 11) // smaller than flv header
			break;

		UInt8 type = packet.read8();
		UInt32 bodySize = packet.read24();
		UInt32 time = packet.read24();
		packet.next(4); // ignored

		if (packet.available() < bodySize + 4)
			break; // we will wait for further data

		TRACE(((type == 0x08) ? "Audio" : ((type == 0x09) ? "Video" : "Unknown")), " packet read - size : ", bodySize, " - time : ", time)
		if (type == AMF::AUDIO || type == AMF::VIDEO)
			_mediaPackets.emplace_back(_poolBuffers, (AMF::ContentType)type, time, packet.current(), bodySize);
		else
			WARN("Unhandled packet type : ", type)
		packet.next(bodySize);
		UInt32 sizeBis = packet.read32();
		pos += bodySize + 15;
		if (sizeBis != bodySize + 11) {
			ERROR("Unexpected size found after payload : ", sizeBis, " (expected: ", bodySize + 11, ")")
			break;
		}
	}
	return true;
}

void Publisher::pushPackets() {
	lock_guard<recursive_mutex> lock(_mediaMutex);

	// Send all packets
	while (!_mediaPackets.empty()) {
		OutMediaPacket& media = _mediaPackets.front();
		if (media.type == AMF::AUDIO)
			pushAudio(media.time, media.pBuffer.data(), media.pBuffer.size());
		else
			pushVideo(media.time, media.pBuffer.data(), media.pBuffer.size());
		_mediaPackets.pop_front();
	}
	flush();
}


void Publisher::pushAudio(UInt32 time, const UInt8* data, UInt32 size) {
	if (!_running) {
		ERROR("Audio packet pushed on '", _name, "' publication stopped");
		return;
	}

	//	TRACE("Time Audio ",time)

	/*if (lostRate)
		INFO((UInt8)(lostRate * 100), "% of audio information lost on publication ", _name);
	_audioQOS.add(packet.available() + 4, ping, lostRate); // 4 for time encoded*/

	// save audio codec packet for future listeners
	if (RTMFP::IsAACCodecInfos(data, size)) {
		DEBUG("AAC codec infos received on publication ", _name)
		// AAC codec && settings codec informations
		_audioCodecBuffer->resize(size, false);
		memcpy(_audioCodecBuffer->data(), data, size);
	}

	_new = true;
	auto it = _listeners.begin();
	while (it != _listeners.end()) {
		(it++)->second->pushAudio(time, data, size);  // listener can be removed in this call
	}
}

void Publisher::pushVideo(UInt32 time, const UInt8* data, UInt32 size) {
	if (!_running) {
		ERROR("Video packet pushed on '", _name, "' publication stopped");
		return;
	}

	//  TRACE("Time Video ",time," => ",Util::FormatHex(packet.current(),16,LOG_BUFFER))

	// save video codec packet for future listeners
	if (RTMFP::IsH264CodecInfos(data, size)) {
		DEBUG("H264 codec infos received on publication ", _name)
		// h264 codec && settings codec informations
		_videoCodecBuffer->resize(size, false);
		memcpy(_videoCodecBuffer->data(), data, size);
	}

	/*_videoQOS.add(packet.available() + 4, ping, lostRate); // 4 for time encoded
	if (lostRate) {
		INFO((UInt8)(lostRate * 100), "% video fragments lost on publication ", _name);
		// here we are on a new frame which don't follow the previous,
		// so I-Frame, P-Frame, P-Frame, ... sequence is broken
		// we have to wait the next keyframe before to redistribute it to the listeners
		_broken = true;
	}
	if (_broken) {
		++_droppedFrames;
		if (!MediaCodec::IsKeyFrame(packet))
			return;
		_broken = false;
	}*/

	_new = true;
	auto it = _listeners.begin();
	while (it != _listeners.end()) {
		(it++)->second->pushVideo(time, data, size); // listener can be removed in this call
	}
}

void Publisher::flush() {
	if (!_new)
		return;
	_new = false;
	map<string, Listener*>::const_iterator it;
	for (it = _listeners.begin(); it != _listeners.end(); ++it)
		it->second->flush();
}
