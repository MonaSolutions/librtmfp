
#pragma once

#include "Mona/Mona.h"
#include "AMF.h"
#include "AMFReader.h"
#include "FlashWriter.h"

namespace FlashEvents {
	struct OnStatus : Mona::Event<void(const std::string& code, const std::string& description, FlashWriter& writer)> {};
	struct OnMedia: Mona::Event<void(Mona::UInt32 time,Mona::PacketReader& packet,double lostRate,bool audio)> {};
	struct OnPlay: Mona::Event<void(const std::string& streamName, FlashWriter& writer)> {};
};

/**************************************************************
FlashStream is linked to an as3 NetStream
*/
class FlashStream : public virtual Mona::Object,
	public FlashEvents::OnStatus,
	public FlashEvents::OnMedia,
	public FlashEvents::OnPlay {
public:

	FlashStream(Mona::UInt16 id);
	virtual ~FlashStream();

	const Mona::UInt16	id;

	Mona::UInt32	bufferTime(Mona::UInt32 ms);
	Mona::UInt32	bufferTime() const { return _bufferTime; }

	void	disengage(FlashWriter* pWriter=NULL);

	// return flase if writer is closed!
	bool	process(AMF::ContentType type,Mona::UInt32 time,Mona::PacketReader& packet,FlashWriter& writer, double lostRate=0);

	virtual void	flush() { }

	// Send the connect request to the RTMFP server
	virtual void connect(FlashWriter& writer,const std::string& url);

	// Send the createStream request to the RTMFP server
	virtual void createStream(FlashWriter& writer);

	// Send the play request to the RTMFP server
	virtual void play(FlashWriter& writer, const std::string& name, bool amf3=false);

	// Send the publish request to the RTMFP server
	virtual void publish(FlashWriter& writer, const std::string& name);

	// Send the setPeerInfo request to the RTMFP server
	virtual void sendPeerInfo(FlashWriter& writer, Mona::UInt16 port);

private:

	virtual void	messageHandler(const std::string& name, AMFReader& message, FlashWriter& writer);
	virtual void	rawHandler(Mona::UInt16 type, Mona::PacketReader& data, FlashWriter& writer);
	virtual void	dataHandler(DataReader& data, double lostRate);
	virtual void	audioHandler(Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);
	virtual void	videoHandler(Mona::UInt32 time,Mona::PacketReader& packet, double lostRate);

	Mona::UInt32	_bufferTime;
	std::string		_streamName;

	//Mona::UInt32	_timeFrequency; // to retrieve time
};
