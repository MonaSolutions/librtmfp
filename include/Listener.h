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
#include "RTMFPWriter.h"

struct Publisher;
class Listener : public virtual Base::Object {
public:
	Listener(Publisher& publication, const std::string& identifier);
	virtual ~Listener() {}

	virtual void startPublishing() = 0;
	virtual void stopPublishing() = 0;

	virtual void pushAudio(Base::UInt32 time, const Base::Packet& packet, bool reliable) = 0;
	virtual void pushVideo(Base::UInt32 time, const Base::Packet& packet, bool reliable) = 0;
	virtual void pushData(Base::UInt32 time, const Base::Packet& packet, bool reliable) = 0;
	//virtual void pushData(DataReader& packet) = 0;
	//virtual void pushProperties(DataReader& packet) = 0;

	virtual void flush() = 0;

	const Publisher&	publication;
	const std::string&	identifier;

};

class FlashListener : public Listener {
public:
	FlashListener(Publisher& publication, const std::string& identifier, const Base::shared<RTMFPWriter>& pDataWriter, const Base::shared<RTMFPWriter>& pAudioWriter, const Base::shared<RTMFPWriter>& pVideoWriter);
	virtual ~FlashListener();

	virtual void startPublishing();
	virtual void stopPublishing();

	virtual void pushAudio(Base::UInt32 time, const Base::Packet& packet, bool reliable);
	virtual void pushVideo(Base::UInt32 time, const Base::Packet& packet, bool reliable);
	virtual void pushData(Base::UInt32 time, const Base::Packet& packet, bool reliable);

	virtual void flush();

	bool receiveAudio;
	bool receiveVideo;

private:

	bool writeReliableMedia(FlashWriter& writer, FlashWriter::MediaType type, Base::UInt32 time, const Base::Packet& packet) { return writeMedia(writer, true, type, time, packet); }
	bool writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, Base::UInt32 time, const Base::Packet& packet);

	bool	initWriters();
	bool	firstTime() { return !_pVideoWriter && !_pAudioWriter && !_dataInitialized; }
	void	closeWriters();

	bool	pushAudioInfos(Base::UInt32 time);

	Base::UInt32 					_startTime;
	Base::UInt32					_lastTime;
	bool							_firstTime;
	Base::UInt32					_seekTime;
	bool							_codecInfosSent;

	Base::shared<RTMFPWriter>	_pDataWriter;
	Base::shared<RTMFPWriter>	_pAudioWriter;
	Base::shared<RTMFPWriter>	_pVideoWriter;
	bool							_dataInitialized;
};
