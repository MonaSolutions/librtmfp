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

#include "Publisher.h"
#include "RTMFP.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"
#include "GroupListener.h"
#include "Invoker.h"

using namespace Mona;
using namespace std;

Publisher::Publisher(const string& name, Invoker& invoker, bool audioReliable, bool videoReliable) : _running(false), _new(false), _name(name), publishAudio(true), publishVideo(true),
	_audioReliable(audioReliable), _videoReliable(videoReliable), _audioCodecBuffer(invoker.poolBuffers), _videoCodecBuffer(invoker.poolBuffers),
	_pos(0), _invoker(invoker), Task((TaskHandler&)invoker) {

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
	_reader.reset(new PacketReader(data, size));
	_pos = pos;
	if (_reader->available()<14) {
		DEBUG("Packet too small")
		return true;
	}

	// Wait for packets to be sent
	waitHandle();
	pos = _pos; // update the position
	return true;
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
		INFO("H264 codec infos received on publication ", _name)
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

void Publisher::handle(Exception& ex) { 
	if (!_reader)
		return; // TODO: shouldn't happen

	const UInt8* cur = _reader->current();
	if (*cur == 'F' && *(++cur) == 'L' && *(++cur) == 'V') { // header
		_reader->next(13);
		_pos = 13;
	}

	// Send all packets
	while (_reader->available()) {
		if (_reader->available() < 11) // smaller than flv header
			break;

		UInt8 type = _reader->read8();
		UInt32 bodySize = _reader->read24();
		UInt32 time = _reader->read24();
		_reader->next(4); // ignored

		if (_reader->available() < bodySize + 4)
			break; // we will wait for further data

		//TRACE(((type == 0x08) ? "Audio" : ((type == 0x09) ? "Video" : "Unknown")), " packet read - size : ", bodySize, " - time : ", time)
		if (type == AMF::AUDIO)
			pushAudio(time, _reader->current(), bodySize);
		else if (type == AMF::VIDEO)
			pushVideo(time, _reader->current(), bodySize);
		else
			WARN("Unhandled packet type : ", type)
		_reader->next(bodySize);
		UInt32 sizeBis = _reader->read32();
		_pos += bodySize + 15;
		if (sizeBis != bodySize + 11) {
			ERROR("Unexpected size found after payload : ", sizeBis, " (expected: ", bodySize + 11, ")")
			break;
		}
	}
	flush();
}
