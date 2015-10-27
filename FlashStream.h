
#pragma once

#include "Mona/Mona.h"
#include "AMF.h"
#include "AMFReader.h"
#include "FlashWriter.h"
//#include "Mona/Invoker.h"

namespace FlashEvents {
	struct OnStatus : Mona::Event<void(const std::string& code, const std::string& description)> {};
	struct OnMedia: Mona::Event<void(Mona::UInt32 time,Mona::PacketReader& packet,double lostRate,bool audio)> {};
};

class FlashStream : public virtual Mona::Object,
	public FlashEvents::OnStatus,
	public FlashEvents::OnMedia {
public:
	enum PlayStreamType {
		PLAYSTREAM_STOPPED,
		PLAYSTREAM_CREATING,
		PLAYSTREAM_CREATED,
		PLAYSTREAM_PLAYING
	};

	FlashStream(Mona::UInt16 id/*, Invoker& invoker,Peer& peer*/);
	virtual ~FlashStream();

	const Mona::UInt16	id;

	Mona::UInt32	bufferTime(Mona::UInt32 ms);
	Mona::UInt32	bufferTime() const { return _bufferTime; }

	void	disengage(FlashWriter* pWriter=NULL);

	// return flase if writer is closed!
	bool	process(AMF::ContentType type,Mona::UInt32 time,Mona::PacketReader& packet,FlashWriter& writer,double lostRate=0);

	virtual void	flush() { /*if(_pPublication) _pPublication->flush();*/ }

	// Send the connect request to the RTMFP server
	virtual void connect(FlashWriter& writer,const std::string& url,Mona::UInt16 port);

	// Send the createStream request to the RTMFP server
	virtual void createStream(FlashWriter& writer);

	// Send the play request to the RTMFP server
	virtual void play(FlashWriter& writer, const std::string& name);

	// Send the publish request to the RTMFP server
	virtual void publish(FlashWriter& writer, const std::string& name);

	// Write a media packet
	virtual void writeMedia(Mona::UInt8 type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);

/*protected:

	Invoker&		invoker;
	Peer&			peer;*/

private:

	virtual void	messageHandler(const std::string& name, AMFReader& message, FlashWriter& writer);
	virtual void	rawHandler(Mona::UInt16 type, Mona::PacketReader& data, FlashWriter& writer);
	virtual void	dataHandler(DataReader& data, double lostRate);
	virtual void	audioHandler(Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);
	virtual void	videoHandler(Mona::UInt32 time,Mona::PacketReader& packet, double lostRate);

	/*Publication*	_pPublication;
	Listener*		_pListener;*/
	Mona::UInt32	_bufferTime;

	PlayStreamType	_playStreamStep;
	std::string		_streamName;
};
