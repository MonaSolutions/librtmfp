#pragma once
#include "FlashWriter.h"

class Publisher;
class Listener : virtual Mona::Object {
public:
	Listener(Publisher& publication, const std::string& identifier, FlashWriter& writer);
	virtual ~Listener();

	void startPublishing();
	void stopPublishing();

	void pushAudio(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	void pushVideo(Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);
	//void pushData(DataReader& packet);
	//void pushProperties(DataReader& packet);

	void flush();

	bool receiveAudio;
	bool receiveVideo;

	const Publisher&	publication;
	const std::string&	identifier;

private:

	bool writeReliableMedia(FlashWriter& writer, FlashWriter::MediaType type, Mona::UInt32 time, Mona::PacketReader& packet) { return writeMedia(writer, true, type, time, packet.data(), packet.size()); }
	bool writeMedia(FlashWriter& writer, FlashWriter::MediaType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) { return writeMedia(writer, _reliable, type, time, data, size); }
	bool writeMedia(FlashWriter& writer, bool reliable, FlashWriter::MediaType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);

	bool	initWriters();
	bool	firstTime() { return !_pVideoWriter && !_pAudioWriter && !_dataInitialized; }
	void	closeWriters();

	bool	pushAudioInfos(Mona::UInt32 time);

	Mona::PacketReader& publicationNamePacket() { _publicationNamePacket.reset(); return _publicationNamePacket; }

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
	Mona::PacketReader		_publicationNamePacket;
};
