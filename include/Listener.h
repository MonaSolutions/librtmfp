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
#include "FlashWriter.h"

class Publisher;
class Listener : public virtual Mona::Object {
public:
	Listener(Publisher& publication, const std::string& identifier);
	virtual ~Listener() {}

	virtual void startPublishing() = 0;
	virtual void stopPublishing() = 0;

	virtual void pushAudio(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) = 0;
	virtual void pushVideo(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) = 0;
	//virtual void pushData(DataReader& packet) = 0;
	//virtual void pushProperties(DataReader& packet) = 0;

	virtual void flush() = 0;

	const Publisher&	publication;
	const std::string&	identifier;

protected:
	Mona::PacketReader& publicationNamePacket() { _publicationNamePacket.reset(); return _publicationNamePacket; }

private:
	Mona::PacketReader		_publicationNamePacket;
};


class FlashListener : public Listener {
public:
	FlashListener(Publisher& publication, const std::string& identifier, FlashWriter& writer);
	virtual ~FlashListener();

	virtual void startPublishing();
	virtual void stopPublishing();

	virtual void pushAudio(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	virtual void pushVideo(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	//virtual void pushData(DataReader& packet);
	//virtual void pushProperties(DataReader& packet);

	virtual void flush();

	bool receiveAudio;
	bool receiveVideo;

private:

	bool writeReliableMedia(FlashWriter& writer, FlashWriter::MediaType type, Mona::UInt32 time, Mona::PacketReader& packet) { return writeMedia(writer, true, type, time, packet.data(), packet.size()); }
	bool writeMedia(FlashWriter& writer, FlashWriter::MediaType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) { return writeMedia(writer, _reliable, type, time, data, size); }
	bool writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);

	bool	initWriters();
	bool	firstTime() { return !_pVideoWriter && !_pAudioWriter && !_dataInitialized; }
	void	closeWriters();

	bool	pushAudioInfos(Mona::UInt32 time);

	Mona::UInt32 			_startTime;
	Mona::UInt32			_lastTime;
	bool					_firstTime;
	Mona::UInt32			_seekTime;
	bool					_codecInfosSent;

	FlashWriter&			_writer;
	FlashWriter*			_pAudioWriter;
	FlashWriter*			_pVideoWriter;
	bool					_dataInitialized;
	bool					_reliable;
};
