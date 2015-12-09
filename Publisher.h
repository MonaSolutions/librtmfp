
#pragma once

#include "Mona/Mona.h"
#include "FlashWriter.h"
//#include "Mona/QualityOfService.h"
#include "DataReader.h"
#include "Mona/Task.h"
#include <deque>

class Publisher : public virtual Mona::Object, private Mona::Task {
public:

	Publisher(const Mona::PoolBuffers& poolBuffers, Mona::TaskHandler& handler, bool audioReliable, bool videoReliable);
	virtual ~Publisher();

	void setWriter(FlashWriter* pWriter) { _pWriter = pWriter; }

	bool publish(const Mona::UInt8* data, Mona::UInt32 size, int& pos);

	virtual void	handle(Mona::Exception& ex);

private:
	/*void seek(Mona::UInt32 time);

	void startPublishing();
	void stopPublishing();*/

	void pushAudio(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	void pushVideo(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	/*void pushData(DataReader& packet);
	void pushProperties(DataReader& packet);*/

	void flush();

	bool publishAudio;
	bool publishVideo;

	/*const QualityOfService&	videoQOS() const { return _pVideoWriter ? _pVideoWriter->qos() : QualityOfService::Null; }
	const QualityOfService&	audioQOS() const { return _pAudioWriter ? _pAudioWriter->qos() : QualityOfService::Null; }
	const QualityOfService&	dataQOS() const { return _writer.qos(); }*/

//private:

	//bool writeReliableMedia(FlashWriter& writer, FlashWriter::MediaType type, Mona::UInt32 time, Mona::PacketReader& packet, const Mona::Parameters& properties) { return writeMedia(writer, true, type, time, packet, properties); }
	//bool writeMedia(FlashWriter& writer, FlashWriter::MediaType type, Mona::UInt32 time, Mona::PacketReader& packet, const Mona::Parameters& properties) { return writeMedia(writer, _reliable, type, time, packet, properties); }
	bool writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);

	//void    writeData(DataReader& reader, FlashWriter::DataType type);

	bool	initWriters();
	bool	firstTime() { return !_pVideoWriter && !_pAudioWriter && !_dataInitialized; }
	void	closeWriters();

	//bool	pushAudioInfos(Mona::UInt32 time);

	//Mona::PacketReader& publicationNamePacket() { _publicationNamePacket.reset(); return _publicationNamePacket; }

	/*Mona::UInt32 			_startTime;
	Mona::UInt32			_lastTime;
	bool					_firstTime;
	Mona::UInt32			_seekTime;
	bool					_codecInfosSent;*/

	FlashWriter*				_pWriter;
	FlashWriter*				_pAudioWriter;
	FlashWriter*				_pVideoWriter;
	bool						_dataInitialized;
	bool						_videoReliable;
	bool						_audioReliable;
	const Mona::PoolBuffers&	_poolBuffers;
	//PacketReader				_publicationNamePacket;

	struct OutMediaPacket : public Mona::Object {

		OutMediaPacket(const Mona::PoolBuffers& poolBuffers, AMF::ContentType typeMedia, Mona::UInt32 timeMedia, const Mona::UInt8* data, Mona::UInt32 size) : pBuffer(poolBuffers,size), time(timeMedia), type(typeMedia) {
			if (pBuffer->size()>=size)
				memcpy(pBuffer->data(), data, size);
		}

		Mona::PoolBuffer	pBuffer;
		AMF::ContentType	type;
		Mona::UInt32		time;
	};

	std::deque<OutMediaPacket>	_mediaPackets;
};
