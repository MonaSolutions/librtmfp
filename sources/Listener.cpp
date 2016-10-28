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

#include "Listener.h"
#include "Publisher.h"
#include "RTMFP.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;

Listener::Listener(Publisher& publication, const string& identifier) : publication(publication), identifier(identifier),
	_publicationNamePacket((const UInt8*)publication.name().c_str(), publication.name().size()) {

}

FlashListener::FlashListener(Publisher& publication, const string& identifier, FlashWriter& writer) : Listener(publication, identifier),
	_writer(writer), receiveAudio(true), receiveVideo(true), _firstTime(true), _seekTime(0), _pAudioWriter(NULL), _pVideoWriter(NULL),
	_dataInitialized(false), _reliable(true), _startTime(0), _lastTime(0), _codecInfosSent(false) {

}

FlashListener::~FlashListener() {
	closeWriters();
}

void FlashListener::closeWriters() {
	// -1 indicate that it come of the FlashListener class
	if (_pAudioWriter)
		_pAudioWriter->close(-1);
	if (_pVideoWriter)
		_pVideoWriter->close(-1);
	_pVideoWriter = _pAudioWriter = NULL;
	_dataInitialized = false;
}

bool FlashListener::initWriters() {
	bool firstTime(false);

	if (_pVideoWriter || _pAudioWriter || _dataInitialized) {
		closeWriters();
		WARN("Reinitialisation of the publication");
	}
	else
		firstTime = true;

	_dataInitialized = true;
	_pAudioWriter = &_writer.newWriter();
	_pVideoWriter = &_writer.newWriter();

	if (firstTime && publication.running()) {
		startPublishing();
	}

	return true;
}

void FlashListener::startPublishing() {

	if (!_pVideoWriter || !_pAudioWriter || !_dataInitialized) {
		initWriters(); // call already recursivly startPublishing()!
		return;
	}

	if (!writeReliableMedia(_writer, FlashWriter::START, FlashWriter::DATA, publicationNamePacket()))// unsubscribe can be done here!
		return;
	if (!writeReliableMedia(*_pAudioWriter, FlashWriter::START, FlashWriter::AUDIO, publicationNamePacket()))
		return; // Here consider that the FlashListener have to be closed by the caller
	if (!writeReliableMedia(*_pVideoWriter, FlashWriter::START, FlashWriter::VIDEO, publicationNamePacket()))
		return; // Here consider that the FlashListener have to be closed by the caller
}

void FlashListener::stopPublishing() {

	if (firstTime())
		return;

	if (!_pVideoWriter || !_pAudioWriter || !_dataInitialized) {
		if (!initWriters())
			return;
	}

	if (!writeReliableMedia(_writer, FlashWriter::STOP, FlashWriter::DATA, publicationNamePacket()))// unsubscribe can be done here!
		return;
	if (!writeReliableMedia(*_pAudioWriter, FlashWriter::STOP, FlashWriter::AUDIO, publicationNamePacket()))
		return; // Here consider that the FlashListener have to be closed by the caller
	if (!writeReliableMedia(*_pVideoWriter, FlashWriter::STOP, FlashWriter::VIDEO, publicationNamePacket()))
		return; // Here consider that the FlashListener have to be closed by the caller

	_seekTime = _lastTime;
	_firstTime = true;
	_codecInfosSent = false;
	_startTime = 0;
}


void FlashListener::pushVideo(UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) {
	if (!receiveVideo && !RTMFP::IsH264CodecInfos(data,size))
		return;

	if (!_codecInfosSent) {
		if (RTMFP::IsKeyFrame(data, size)) {
			_codecInfosSent = true;
			if (!publication.videoCodecBuffer().empty() && !RTMFP::IsH264CodecInfos(data, size)) {
				INFO("H264 codec infos sent to one FlashListener of ", publication.name(), " publication")
				pushVideo(time, publication.videoCodecBuffer()->data(), publication.videoCodecBuffer()->size());
			}
		}
		else {
			DEBUG("Video frame dropped to wait first key frame");
			return;
		}
	}

	if (!_pVideoWriter && !initWriters())
		return;

	if (_firstTime) {
		_startTime = time;
		_firstTime = false;

		// for audio sync (audio is usually the reference track)
		if (pushAudioInfos(time))
			pushAudio(time, NULL, 0); // push a empty audio packet to avoid a video which waits audio tracks!
	}
	time -= _startTime;

	//TRACE("Video time(+seekTime) => ", time, "(+", _seekTime, "), size : ", size);

	if (!writeMedia(*_pVideoWriter, RTMFP::IsKeyFrame(data, size) || _reliable, FlashWriter::VIDEO, _lastTime = (time + _seekTime), data, size))
		initWriters();
}


void FlashListener::pushAudio(UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) {
	if (!receiveAudio && !RTMFP::IsAACCodecInfos(data, size))
		return;

	if (!_pAudioWriter && !initWriters())
		return;

	if (_firstTime) {
		_firstTime = false;
		_startTime = time;
		pushAudioInfos(time);
	}
	time -= _startTime;

	//TRACE("Audio time(+seekTime) => ", time, "(+", _seekTime, ")");

	if (!writeMedia(*_pAudioWriter, RTMFP::IsAACCodecInfos(data, size) || _reliable, FlashWriter::AUDIO, _lastTime = (time + _seekTime), data, size))
		initWriters();
}

bool FlashListener::pushAudioInfos(UInt32 time) {
	if (publication.audioCodecBuffer().empty())
		return false;
	INFO("AAC codec infos sent to one FlashListener of ", publication.name(), " publication")
	pushAudio(time, publication.audioCodecBuffer()->data(), publication.audioCodecBuffer()->size());
	return true;
}

void FlashListener::flush() {
	// in first data channel
	_writer.flush();
	// now media channel
	if (_pAudioWriter) // keep in first, because audio track is sometimes the time reference track
		_pAudioWriter->flush();
	if (_pVideoWriter)
		_pVideoWriter->flush();
}

bool FlashListener::writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, UInt32 time, const UInt8* data, UInt32 size) {
	bool wasReliable(writer.reliable);
	writer.reliable = reliable;
	bool success(writer.writeMedia(type, time, data, size));
	writer.reliable = wasReliable;
	return success;
}
