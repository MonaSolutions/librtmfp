#include "Publisher.h"
#include "RTMFP.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"
/*#include "Mona/Publication.h"
#include "Mona/MediaCodec.h"
#include "Mona/MIME.h"*/

using namespace Mona;
using namespace std;

Publisher::Publisher(FlashWriter& writer, bool audioReliable, bool videoReliable) : _writer(writer), publishAudio(true), publishVideo(true), //_firstTime(true),
	/*_seekTime(0),*/ _pAudioWriter(NULL), _pVideoWriter(NULL), /*_publicationNamePacket((const UInt8*)publication.name().c_str(), publication.name().size()),*/
	_dataInitialized(false), _audioReliable(audioReliable), _videoReliable(videoReliable)/*, _startTime(0), _lastTime(0), _codecInfosSent(false)*/ {

	INFO("Initialization of the publisher (audioReliable : ",audioReliable," - videoReliable : ", videoReliable,")")
	initWriters();
}

Publisher::~Publisher() {
	closeWriters();
}

void Publisher::closeWriters() {
	// -1 indicate that it come of the listener class
	if (_pAudioWriter)
		_pAudioWriter->close(-1);
	if (_pVideoWriter)
		_pVideoWriter->close(-1);
	_pVideoWriter = _pAudioWriter = NULL;
	_dataInitialized = false;
}

bool Publisher::initWriters() {
	// if start return false, the subscriber must unsubcribe the listener (closed by the caller)

	bool firstTime(false);

	if (_pVideoWriter || _pAudioWriter || _dataInitialized) {
		closeWriters();
		WARN("Reinitialisation of the publication");
	}
	else
		firstTime = true;

	_dataInitialized = true;
	/*if (!writeReliableMedia(_writer, FlashWriter::INIT, FlashWriter::DATA, publicationNamePacket(), *this))// unsubscribe can be done here!
		return false; // Here consider that the listener have to be closed by the caller*/

	_pAudioWriter = &_writer.newWriter();
	/*if (!writeReliableMedia(*_pAudioWriter, FlashWriter::INIT, FlashWriter::AUDIO, publicationNamePacket(), *this)) {
		closeWriters();
		return false; // Here consider that the listener have to be closed by the caller
	}*/
	_pVideoWriter = &_writer.newWriter();
	/*if (!writeReliableMedia(*_pVideoWriter, FlashWriter::INIT, FlashWriter::VIDEO, publicationNamePacket(), *this)) {
		closeWriters();
		return false; // Here consider that the listener have to be closed by the caller
	}*/

	/*if (firstTime && publication.running()) {
		startPublishing();
		// send publication properties (metadata)
		publication.requestProperties(*this);
	}*/

	return true;
}

/*void Publisher::stopPublishing() {

	_seekTime = _lastTime;
	_firstTime = true;
	_codecInfosSent = false;
	_startTime = 0;
}

void Publisher::seek(UInt32 time) {
	// To force the time to be as requested, but the stream continue normally (not reseted _codecInfosSent and _firstMedia)
	_firstTime = true;
	_startTime = 0;
	_lastTime = _seekTime = time;
	NOTE("NEW SEEK_TIME = ", _seekTime);
}

void Publisher::writeData(DataReader& reader, FlashWriter::DataType type) {
	if (!_dataInitialized && !initWriters())
		return;

	if (!reader) {
		ERROR("Impossible to stream ", typeid(reader).name(), " null DataReader");
		return;
	}

	if (!writeMedia(_writer, type == FlashWriter::DATA_INFO || _reliable, FlashWriter::DATA, MIME::DataType(reader) << 8 | type, reader.packet, *this))
		initWriters();
}*/

void Publisher::pushVideo(UInt32 time, const UInt8* data, UInt32 size) {
	if (!publishVideo /*&& !MediaCodec::H264::IsCodecInfos(packet)*/)
		return;

	/*if (!_codecInfosSent) {
		if (MediaCodec::IsKeyFrame(packet)) {
			_codecInfosSent = true;
			if (!publication.videoCodecBuffer().empty() && !MediaCodec::H264::IsCodecInfos(packet)) {
				PacketReader videoCodecPacket(publication.videoCodecBuffer()->data(), publication.videoCodecBuffer()->size());
				INFO("H264 codec infos sent to one listener of ", publication.name(), " publication")
					pushVideo(time, videoCodecPacket);
			}
		}
		else {
			DEBUG("Video frame dropped to wait first key frame");
			return;
		}
	}*/

	if (!_pVideoWriter && !initWriters())
		return;

	/*if (_firstTime) {
		_startTime = time;
		_firstTime = false;

		// for audio sync (audio is usually the reference track)
		if (pushAudioInfos(time))
			pushAudio(time, PacketReader::Null); // push a empty audio packet to avoid a video which waits audio tracks!
	}
	time -= _startTime;

	TRACE("Video time(+seekTime) => ", time, "(+", _seekTime, ") ", Util::FormatHex(packet.current(), 5, LOG_BUFFER));*/

	if (!writeMedia(*_pVideoWriter, RTMFP::IsKeyFrame(data, size) || _videoReliable, FlashWriter::VIDEO, time, data, size/*_lastTime = (time + _seekTime), packet, *this*/))
		initWriters();
}


void Publisher::pushAudio(UInt32 time, const UInt8* data, UInt32 size) {
	if (!publishAudio /*&& !MediaCodec::AAC::IsCodecInfos(packet)*/)
		return;

	if (!_pAudioWriter && !initWriters())
		return;

	/*if (_firstTime) {
		_firstTime = false;
		_startTime = time;
		pushAudioInfos(time);
	}
	time -= _startTime;

	TRACE("Audio time(+seekTime) => ", time, "(+", _seekTime, ")");*/

	if (!writeMedia(*_pAudioWriter, RTMFP::IsAACCodecInfos(data, size) || _audioReliable, FlashWriter::AUDIO, time, data, size/*_lastTime = (time + _seekTime), packet, *this*/))
		initWriters();
}

/*void Publisher::pushProperties(DataReader& packet) {
	INFO("Properties sent to one listener of ", publication.name(), " publication")
		writeData(packet, FlashWriter::DATA_INFO);
}

bool Publisher::pushAudioInfos(UInt32 time) {
	if (publication.audioCodecBuffer().empty())
		return false;
	PacketReader audioCodecPacket(publication.audioCodecBuffer()->data(), publication.audioCodecBuffer()->size());
	INFO("AAC codec infos sent to one listener of ", publication.name(), " publication")
		pushAudio(time, audioCodecPacket);
	return true;
}*/

void Publisher::flush() {
	// in first data channel
	_writer.flush();
	// now media channel
	if (_pAudioWriter) // keep in first, because audio track is sometimes the time reference track
		_pAudioWriter->flush();
	if (_pVideoWriter)
		_pVideoWriter->flush();
}

bool Publisher::writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, UInt32 time, const UInt8* data, UInt32 size) {
	bool wasReliable(writer.reliable);
	writer.reliable = reliable;
	bool success(writer.writeMedia(type, time, data, size));
	writer.reliable = wasReliable;
	return success;
}
