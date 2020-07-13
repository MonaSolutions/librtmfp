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
#include "Base/Logs.h"

using namespace std;
using namespace Base;

Listener::Listener(Publisher& publication, const string& identifier) : publication(publication), identifier(identifier) {

}

FlashListener::FlashListener(Publisher& publication, const string& identifier, const shared<RTMFPWriter>& pDataWriter, const shared<RTMFPWriter>& pAudioWriter, const shared<RTMFPWriter>& pVideoWriter) : Listener(publication, identifier),
	_pDataWriter(pDataWriter), _pAudioWriter(pAudioWriter), _pVideoWriter(pVideoWriter), receiveAudio(true), receiveVideo(true), _firstTime(true), _seekTime(0),
	_dataInitialized(false), _startTime(0), _lastTime(0), _codecInfosSent(false) {

}

FlashListener::~FlashListener() {
	closeWriters();
}

void FlashListener::closeWriters() {
	// -1 indicate that it come from the FlashListener class
	if (_pDataWriter)
		_pDataWriter->close(false);
	if (_pAudioWriter)
		_pAudioWriter->close(false);
	if (_pVideoWriter)
		_pVideoWriter->close(false);
	_pDataWriter.reset();
	_pVideoWriter.reset();
	_pAudioWriter.reset();
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

	if (!writeReliableMedia(*_pDataWriter, FlashWriter::START, FlashWriter::DATA, Packet(publication.name().data(), publication.name().size())))// unsubscribe can be done here!
		return;
	if (!writeReliableMedia(*_pAudioWriter, FlashWriter::START, FlashWriter::AUDIO, Packet(publication.name().data(), publication.name().size())))
		return; // Here consider that the FlashListener have to be closed by the caller
	if (!writeReliableMedia(*_pVideoWriter, FlashWriter::START, FlashWriter::VIDEO, Packet(publication.name().data(), publication.name().size())))
		return; // Here consider that the FlashListener have to be closed by the caller
}

void FlashListener::stopPublishing() {

	if (!_pVideoWriter || !_pAudioWriter || !_dataInitialized)
		return;

	if (!writeReliableMedia(*_pDataWriter, FlashWriter::STOP, FlashWriter::DATA, Packet(publication.name().data(), publication.name().size())))// unsubscribe can be done here!
		return;
	if (!writeReliableMedia(*_pAudioWriter, FlashWriter::STOP, FlashWriter::AUDIO, Packet(publication.name().data(), publication.name().size())))
		return; // Here consider that the FlashListener have to be closed by the caller
	if (!writeReliableMedia(*_pVideoWriter, FlashWriter::STOP, FlashWriter::VIDEO, Packet(publication.name().data(), publication.name().size())))
		return; // Here consider that the FlashListener have to be closed by the caller

	_seekTime = _lastTime;
	_firstTime = true;
	_codecInfosSent = false;
	_startTime = 0;
}


void FlashListener::pushVideo(UInt32 time, const Packet& packet, bool reliable) {
	if (!receiveVideo && !RTMFP::IsVideoCodecInfos(packet.data(), packet.size()))
		return;

	if (!_codecInfosSent) {
		if (RTMFP::IsKeyFrame(packet.data(), packet.size())) {
			_codecInfosSent = true;
			if (publication.videoCodecBuffer() && !RTMFP::IsVideoCodecInfos(packet.data(), packet.size())) {
				INFO("Video codec infos sent to one FlashListener of ", publication.name(), " publication")
				pushVideo(time, publication.videoCodecBuffer(), true);
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
			pushAudio(time, Packet::Null(), true); // push a empty audio packet to avoid a video which waits audio tracks!
	}
	time -= _startTime;

	//TRACE("Video time(+seekTime) => ", time, "(+", _seekTime, "), size : ", size);

	if (!writeMedia(*_pVideoWriter, RTMFP::IsKeyFrame(packet.data(), packet.size()) || reliable, FlashWriter::VIDEO, _lastTime = (time + _seekTime), packet))
		initWriters();
}

void FlashListener::pushData(UInt32 time, const Packet& packet, bool reliable) {
	if (!_pDataWriter && !initWriters())
		return;

	if (_firstTime) {
		_startTime = time;
		_firstTime = false;

		// for audio sync (audio is usually the reference track)
		if (pushAudioInfos(time))
			pushAudio(time, Packet::Null(), true); // push a empty audio packet to avoid a video which waits audio tracks!
	}
	time -= _startTime;

	//TRACE("Data time(+seekTime) => ", time, "(+", _seekTime, "), size : ", size);

	if (!writeMedia(*_pDataWriter, RTMFP::IsKeyFrame(packet.data(), packet.size()) || reliable, FlashWriter::DATA, _lastTime = (time + _seekTime), packet))
		initWriters();
}


void FlashListener::pushAudio(UInt32 time, const Packet& packet, bool reliable) {
	if (!receiveAudio && !RTMFP::IsAACCodecInfos(packet.data(), packet.size()))
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

	if (!writeMedia(*_pAudioWriter, RTMFP::IsAACCodecInfos(packet.data(), packet.size()) || reliable, FlashWriter::AUDIO, _lastTime = (time + _seekTime), packet))
		initWriters();
}

bool FlashListener::pushAudioInfos(UInt32 time) {
	if (!publication.audioCodecBuffer())
		return false;
	INFO("AAC codec infos sent to one FlashListener of ", publication.name(), " publication")
	pushAudio(time, publication.audioCodecBuffer(), true);
	return true;
}

void FlashListener::flush() {
	// in first data channel
	if (_pDataWriter)
		_pDataWriter->flush();
	// now media channel
	if (_pAudioWriter) // keep in first, because audio track is sometimes the time reference track
		_pAudioWriter->flush();
	if (_pVideoWriter)
		_pVideoWriter->flush();
}

bool FlashListener::writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, UInt32 time, const Packet& packet) {
	bool wasReliable(writer.reliable);
	writer.reliable = reliable;
	bool success(writer.writeMedia(type, time, packet));
	writer.reliable = wasReliable;
	return success;
}
