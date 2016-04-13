
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

	Publisher(const std::string& name, Invoker& invoker, bool audioReliable, bool videoReliable);
	virtual ~Publisher();

	// Add packets to the waiting queue
	bool publish(const Mona::UInt8* data, Mona::UInt32 size, int& pos);

	const std::string&		name() const { return _name; }

	void					start();
	bool					running() const { return _running; }
	void					stop();
	Mona::UInt32			count() const { return _listeners.size(); }

	Listener*				addListener(Mona::Exception& ex, const std::string& identifier, FlashWriter& writer);
	void					removeListener(const std::string& identifier);

	const Mona::PoolBuffer&		audioCodecBuffer() const { return _audioCodecBuffer; }
	const Mona::PoolBuffer&		videoCodecBuffer() const { return _videoCodecBuffer; }
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
