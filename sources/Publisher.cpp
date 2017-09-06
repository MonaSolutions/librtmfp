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

Publisher::Publisher(const string& name, Invoker& invoker, bool audioReliable, bool videoReliable, bool p2p) : _running(false), _new(false), _name(name), publishAudio(true), publishVideo(true),
	_audioReliable(audioReliable), _videoReliable(videoReliable), isP2P(p2p), _invoker(invoker), _lastTime(0) {

	INFO("Initialization of the publisher ", _name, " (audioReliable : ", _audioReliable, " - videoReliable : ", _videoReliable, ")");
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
	_running = false;
}

void Publisher::updateTime(UInt32 time) {

	// Test time synchro issue
	if (_lastTime > time) {
		if (_lastSyncWarn.isElapsed(1000) && (_lastTime - time) > 1000) {
			WARN("Packet time of publication ", _name, " is more than 1s in the past : ", time, "ms < ", _lastTime, "ms")
			_lastSyncWarn.update();
		}
	}
	// Test a gap of packets
	if (_lastTime && _lastPacket.isElapsed(1000))
		WARN("More than 1s without receiving any packet from ", _name, " : ", _lastPacket.elapsed(), "ms")
	_lastTime = time;
	_lastPacket.update();
}

void Publisher::pushAudio(UInt32 time, const Packet& packet) {
	if (!_running) {
		ERROR("Audio packet pushed on '", _name, "' publication stopped");
		return;
	}
	updateTime(time);

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

	updateTime(time);

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

void Publisher::flush() {
	if (!_new)
		return;
	_new = false;
	map<string, Listener*>::const_iterator it;
	for (it = _listeners.begin(); it != _listeners.end(); ++it)
		it->second->flush();
}
