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

#include "Mona/Mona.h"
#include "FlashWriter.h"
//#include "Mona/QualityOfService.h"
#include "DataReader.h"
#include "Mona/Task.h"
#include <deque>

class Invoker;
class Listener;
class Publisher : public Mona::Task, public virtual Mona::Object {
public:

	Publisher(const std::string& name, Invoker& invoker, bool audioReliable, bool videoReliable, bool p2p);
	virtual ~Publisher();

	// Add packets to the waiting queue
	bool publish(const Mona::UInt8* data, Mona::UInt32 size, int& pos);

	const std::string&		name() const { return _name; }

	void					start();
	bool					running() const { return _running; }
	void					stop();
	Mona::UInt32			count() const { return _listeners.size(); }

	template <typename ListenerType, typename... Args>
	ListenerType*				addListener(Mona::Exception& ex, const std::string& identifier, Args... args) {
		auto it = _listeners.lower_bound(identifier);
		if (it != _listeners.end() && it->first == identifier) {
			ex.set(Mona::Exception::APPLICATION, "Already subscribed to ", _name);
			return NULL;
		}
		if (it != _listeners.begin())
			--it;
		ListenerType* pListener = new ListenerType(*this, identifier, args...);
		_listeners.emplace_hint(it, identifier, pListener);
		return pListener;
	}
	void					removeListener(const std::string& identifier);

	const Mona::PoolBuffer&		audioCodecBuffer() const { return _audioCodecBuffer; }
	const Mona::PoolBuffer&		videoCodecBuffer() const { return _videoCodecBuffer; }

	bool	isP2P; // If true it is a p2p publisher
private:

	void pushAudio(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	void pushVideo(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	/*void pushData(DataReader& packet);
	void pushProperties(DataReader& packet);*/

	// Task handle : running function to send packets
	virtual void handle(Mona::Exception& ex);

	void flush();

	bool publishAudio;
	bool publishVideo;

	/*const QualityOfService&	videoQOS() const { return _pVideoWriter ? _pVideoWriter->qos() : QualityOfService::Null; }
	const QualityOfService&	audioQOS() const { return _pAudioWriter ? _pAudioWriter->qos() : QualityOfService::Null; }
	const QualityOfService&	dataQOS() const { return _writer.qos(); }*/

	Invoker&							_invoker;
	bool								_running; // If the publication is running
	std::map<std::string, Listener*>	_listeners; // list of listeners to this publication
	const std::string					_name; // name of the publication


	//void    writeData(DataReader& reader, FlashWriter::DataType type);

	bool									_videoReliable;
	bool									_audioReliable;

	Mona::PoolBuffer						_audioCodecBuffer;
	Mona::PoolBuffer						_videoCodecBuffer;

	bool									_new; // True if there is at list a packet to send

	std::unique_ptr<Mona::PacketReader>		_reader; // Current reader of input data
	int										_pos; // Current position of input data
};
