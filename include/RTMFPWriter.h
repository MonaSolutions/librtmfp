
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

/*******************************************************
RTMFPWriter is the class for sending RTMFP messages to
the server on a NetStream.
It manages acknowlegment and lost of packet sent
*/
class RTMFPWriter : public FlashWriter, public virtual Mona::Object {
public:
	RTMFPWriter(State state,const std::string& signature, BandWriter& band);
	RTMFPWriter(State state,const std::string& signature, BandWriter& band,std::shared_ptr<RTMFPWriter>& pThis);
	virtual ~RTMFPWriter();

	const Mona::UInt64	id;
	const bool			critical;
	const Mona::UInt64	flowId;
	const std::string	signature;

	virtual FlashWriter&		newWriter() { 
		if (signature.size()>6) {
			std::string tmpSignature("\x00\x54\x43\x04", 4);
			Mona::UInt32 idStream(Mona::BinaryReader((const Mona::UInt8*)signature.c_str() + 6, signature.length() - 6).read7BitValue());
			RTMFP::Write7BitValue(tmpSignature, idStream);
			return *(new RTMFPWriter(state(), tmpSignature, _band)); 
		} else
			return *(new RTMFPWriter(state(), signature, _band)); 
	}

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
        std::shared_ptr<RTMFPWriter> pThis = _band.changeWriter(*new RTMFPWriter(*this));
        _band.initWriter(pThis);
		//_qos.reset();
		//_resetStream = true;
	}

	void				clear();
	void				abort();
	void				close(Mona::Int32 code=0);
	bool				consumed() { return _messages.empty() && state() == CLOSED; }

	Mona::UInt64		stage() { return _stage; }

	//bool				writeMedia(MediaType type,Mona::UInt32 time,Mona::PacketReader& packet,const Mona::Parameters& properties);
	virtual void		writeRaw(const Mona::UInt8* data,Mona::UInt32 size);
	//bool				writeMember(const Client& client);

	// Ask the server to connect to group, netGroup must be in binary format (32 bytes)
	virtual void		writeGroup(const std::string& netGroup);
	// Init the group session with a peer, netGroup must be in hexa format (64 bytes)
	virtual void		writePeerGroup(const std::string& netGroup, const Mona::UInt8* key, const std::string& peerId/*, bool initiator*/);
	// Send the Group begin message (02 + 0E)
	virtual void		writeGroupBegin();
	// Play the stream in argument
	virtual void		writeGroupMedia(const std::string& streamName, const Mona::UInt8* data, Mona::UInt32 size);
	// Start to play the group stream
	virtual void		writeGroupPlay(Mona::UInt8 mode);
	// Send the UnpublishNotify and closeStream messages
	//virtual void		sendGroupCloseStream(Mona::UInt8 type, Mona::UInt64 fragmentCounter, Mona::UInt32 time, const std::string& streamName);

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

	Mona::Trigger				_trigger; // count the number of sended cycles for managing repeated/lost counts
	std::deque<RTMFPMessage*>	_messages; // queue of messages to send
	Mona::UInt64				_stage; // stage (index) of the last message sent
	std::deque<RTMFPMessage*>	_messagesSent; // queue of messages to send back or consider lost if delay is elapsed
	Mona::UInt64				_stageAck; // stage of the last message acknowledged by the server
	Mona::UInt32				_lostCount; // number of lost messages
	double						_ackCount; // number of acknowleged messages
	Mona::UInt32				_repeatable; // number of repeatable messages waiting for acknowledgment
	BandWriter&					_band; // RTMFP connection for sending message
	//bool						_resetStream;

};
