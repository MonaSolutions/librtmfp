
#pragma once

#include "Mona/Mona.h"
#include "FlashWriter.h"
//#include "Mona/QualityOfService.h"
#include "DataReader.h"

class Publisher : virtual Mona::Object {
public:

	Publisher(FlashWriter& writer);
	virtual ~Publisher();

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

private:

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

	FlashWriter&			_writer;
	FlashWriter*			_pAudioWriter;
	FlashWriter*			_pVideoWriter;
	bool					_dataInitialized;
	bool					_reliable;
	//PacketReader			_publicationNamePacket;
};
