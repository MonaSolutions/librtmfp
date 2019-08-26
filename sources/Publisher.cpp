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
#include "Base/Logs.h"
#include "GroupListener.h"
#include "Invoker.h"

using namespace Base;
using namespace std;

Int64 Publisher::TimeJump::operator()(UInt32 time, UInt32 size, UInt64& bytes) {

	if (!_lastTime) { // first time received
		_lastSecond.update();
		_lastTime = time;
		return 0;
	}
	
	if (time > _lastTime) { // if the time increase we save it
		_cumulatedTime += time - _lastTime;
		_lastTime = time;
	}
	_bytes += size;

	Int64 elapsed = _lastSecond.elapsed();
	if (_cumulatedTime && elapsed > 1000) {
		Int64 deltaPackets = _cumulatedTime - elapsed;
		_lastSecond.update();
		bytes = _bytes;
		_cumulatedTime = _bytes = 0;
		return (deltaPackets < 500)? 0 : deltaPackets + elapsed; // More than 1,5s of packets in 1s, we consider it is a time jump
	}
	return 0;
}

Publisher::Publisher(const string& name, Invoker& invoker, bool audioReliable, bool videoReliable, bool p2p) : _running(false), _new(false), _name(name), publishAudio(true), publishVideo(true),
	_audioReliable(audioReliable), _videoReliable(videoReliable), isP2P(p2p), _invoker(invoker), _lastTime(0) {

	INFO("Initialization of the publisher ", _name, " (audioReliable : ", _audioReliable, " - videoReliable : ", _videoReliable, ")");
}

Publisher::~Publisher() {
	// delete listeners
	if (!_listeners.empty()) {
		WARN("Publication ",_name," with subscribers is deleting")
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
	_running = false;
}

void Publisher::updateTime(AMF::Type type, UInt32 time, UInt32 size) {

	// Test a gap of packets
	if (_lastTime && _lastPacket.isElapsed(1000))
		WARN("More than 1s without receiving any packet from publication ", _name, " : ", _lastPacket.elapsed(), "ms")

	// Test time synchro issue
	Int64 deltaPackets;
	if (_lastTime && (deltaPackets = (Int64)_lastTime - (Int64)time) > 1000) {
		if (_lastSyncWarn.isElapsed(1000)) {
			WARN((type == AMF::TYPE_AUDIO) ? "Audio" : "Video", " packet of publication ", _name, " is more than 1s in the past : ", deltaPackets, "ms")
			_lastSyncWarn.update();
		}
	} 

	// Every 1s test if there is a publishing congestion
	UInt64 bytes(0);
	if (type == AMF::TYPE_AUDIO) {
		if ((deltaPackets = _audioJump(time, size, bytes)))
			WARN("Publication ", _name, " audio time jump : ", deltaPackets, "ms received in 1s (", bytes/125, " kbits)")
	}
	else if ((deltaPackets = _videoJump(time, size, bytes)))
		WARN("Publication ", _name, " video time jump : ", deltaPackets, "ms received in 1s (", bytes/125, " kbits)")

	_lastTime = time;
	_lastPacket.update();
}

void Publisher::pushAudio(UInt32 time, const Packet& packet) {
	if (!_running) {
		ERROR("Audio packet pushed on '", _name, "' publication stopped");
		return;
	}
	updateTime(AMF::TYPE_AUDIO, time, packet.size());

	// save audio codec packet for future listeners
	if (RTMFP::IsAACCodecInfos(packet.data(), packet.size())) {
		DEBUG("AAC codec infos received on publication ", _name)
		// AAC codec && settings codec informations
		_audioCodec = move(packet);
	}

	_new = true;
	auto it = _listeners.begin();
	while (it != _listeners.end())
		(it++)->second->pushAudio(time, packet, _audioReliable);  // listener can be removed in this call
}

void Publisher::pushVideo(UInt32 time, const Packet& packet) {
	if (!_running) {
		ERROR("Video packet pushed on '", _name, "' publication stopped");
		return;
	}

	updateTime(AMF::TYPE_VIDEO, time, packet.size());

	// save video codec packet for future listeners
	if (RTMFP::IsVideoCodecInfos(packet.data(), packet.size())) {
		INFO("Video codec infos received on publication ", _name)
		// video codec && settings codec informations
		_videoCodec = move(packet);
	}

	_new = true;
	auto it = _listeners.begin();
	while (it != _listeners.end())
		(it++)->second->pushVideo(time, packet, _videoReliable); // listener can be removed in this call
}

void Publisher::pushData(UInt32 time, const Packet& packet) {
	if (!_running) {
		ERROR("Data packet pushed on '", _name, "' publication stopped");
		return;
	}

	updateTime(AMF::TYPE_DATA, time, packet.size());

	_new = true;
	auto it = _listeners.begin();
	while (it != _listeners.end())
		(it++)->second->pushData(time, packet, true); // listener can be removed in this call
}

void Publisher::flush() {
	if (!_new)
		return;
	_new = false;
	map<string, Listener*>::const_iterator it;
	for (it = _listeners.begin(); it != _listeners.end(); ++it)
		it->second->flush();
}
