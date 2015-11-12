
#pragma once

#include "Mona/Mona.h"
#include "RTMFP.h"
#include "Mona/Trigger.h"
#include "BandWriter.h"
#include "RTMFPMessage.h"
#include "FlashWriter.h"
#include "Mona/Logs.h"

#include <deque>


#define MESSAGE_HEADER			0x80
#define MESSAGE_WITH_AFTERPART  0x10 
#define MESSAGE_WITH_BEFOREPART	0x20
#define MESSAGE_ABANDONMENT		0x02
#define MESSAGE_END				0x01

class RTMFPWriter : public FlashWriter, public virtual Mona::Object {
public:
	RTMFPWriter(State state,const std::string& signature, BandWriter& band);
	RTMFPWriter(State state,const std::string& signature, BandWriter& band,std::shared_ptr<RTMFPWriter>& pThis);
	virtual ~RTMFPWriter();

	const Mona::UInt64	id;
	const bool			critical;
	const Mona::UInt64	flowId;
	const std::string	signature;

	virtual FlashWriter&		newWriter() { return *(new RTMFPWriter(state(),signature, _band)); }

	bool				flush() { return flush(true); }

	void				acknowledgment(Mona::PacketReader& packet);
	void				manage(Mona::Exception& ex);

	template <typename ...Args>
	void fail(Args&&... args) {
		if (state() == CLOSED)
			return;
		WARN("RTMFPWriter ", id, " has failed, ", args ...);
		abort();
		_stage = _stageAck = _lostCount = 0;
		 _ackCount = 0;
        //std::shared_ptr<RTMFPWriter> pThis = _band.changeWriter(*new RTMFPWriter(*this));
        //_band.initWriter(pThis);
		//_qos.reset();
		_resetStream = true;
	}

	void				clear();
	void				abort();
	void				close(Mona::Int32 code=0);
	bool				consumed() { return _messages.empty() && state() == CLOSED; }

	Mona::UInt64		stage() { return _stage; }

	//bool				writeMedia(MediaType type,Mona::UInt32 time,Mona::PacketReader& packet,const Mona::Parameters& properties);
	void				writeRaw(const Mona::UInt8* data,Mona::UInt32 size);
	//bool				writeMember(const Client& client);

private:
	RTMFPWriter(RTMFPWriter& writer);
	
	Mona::UInt32			headerSize(Mona::UInt64 stage);
	
	// Complete the message with the final container (header, flags, body and front) and write it
	void					packMessage(Mona::BinaryWriter& writer,Mona::UInt64 stage,Mona::UInt8 flags,bool header, const RTMFPMessage& message, Mona::UInt32 offset, Mona::UInt16 size);
	// Write unbuffered data if not null and flush all messages
	bool					flush(bool full);
	// Write again repeatable messages
	void					raiseMessage();
	RTMFPMessageBuffered&	createMessage();
	AMFWriter&				write(AMF::ContentType type,Mona::UInt32 time=0,const Mona::UInt8* data=NULL, Mona::UInt32 size=0);

	Mona::Trigger				_trigger;

	std::deque<RTMFPMessage*>	_messages;
	Mona::UInt64				_stage;
	std::deque<RTMFPMessage*>	_messagesSent;
	Mona::UInt64				_stageAck;
	Mona::UInt32				_lostCount;
	double						_ackCount;
	Mona::UInt32				_repeatable;
	BandWriter&					_band;
	bool						_resetStream;

};
