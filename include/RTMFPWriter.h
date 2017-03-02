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

#pragma once

#include "Mona/Mona.h"
#include "RTMFP.h"
#include "RTMFPTrigger.h"
#include "FlowManager.h"
#include "RTMFPMessage.h"
#include "FlashWriter.h"
#include "Mona/Logs.h"
#include <deque>

struct GroupFragment;
struct RTMFPGroupConfig;
/*******************************************************
RTMFPWriter is the class for sending RTMFP messages to
the server on a NetStream.
It does not need an RTMFPFlow to exist but can be
related to one.
It manages acknowlegment and lost of packet sent
*/
class RTMFPWriter : public FlashWriter, public virtual Mona::Object {
public:
	RTMFPWriter(State state,const Mona::Packet& signature, FlowManager& band, Mona::UInt64 idFlow=0);
	RTMFPWriter(State state,const Mona::Packet& signature, FlowManager& band,std::shared_ptr<RTMFPWriter>& pThis, Mona::UInt64 idFlow=0);
	virtual ~RTMFPWriter();


	const Mona::UInt64	id;
	const Mona::UInt64	flowId; // ID of the flow associated to
	const Mona::Packet	signature;

	bool				flush() { return flush(true); }

	bool				acknowledgment(Mona::Exception& ex, Mona::BinaryReader& reader);
	bool				manage(Mona::Exception& ex);
	FlashWriter::State	state() { return _state; }
	void				clear();
	void				close(bool abrupt);
	bool				consumed() { return _messages.empty() && _state == CLOSED || (_state == NEAR_CLOSED && _closeTime.isElapsed(130000)); } // Wait 130s before closing the writer definetly

	Mona::UInt64		stage() { return _stage; }

	virtual void		writeRaw(const Mona::UInt8* data,Mona::UInt32 size);

	// Ask the server to connect to group, netGroup must be in binary format (32 bytes)
	virtual void		writeGroupConnect(const std::string& netGroup);
	// Init the group session with a peer, netGroup must be in hexa format (64 bytes)
	virtual void		writePeerGroup(const std::string& netGroup, const Mona::UInt8* key, const Mona::Binary& rawId);
	// Send the Group begin message (02 + 0E)
	virtual void		writeGroupBegin();
	// Play the stream in argument
	virtual void		writeGroupMedia(const std::string& streamName, const Mona::UInt8* data, Mona::UInt32 size, RTMFPGroupConfig* groupConfig);
	// Start to play the group stream
	virtual void		writeGroupPlay(Mona::UInt8 mode);
	// Send a pull request to a peer (message 2B)
	virtual void		writeGroupPull(Mona::UInt64 index);
	// Send a fragment
	virtual void		writeGroupFragment(const GroupFragment& fragment);

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
	AMFWriter&				write(AMF::Type type, const Mona::Packet& packet, Mona::UInt32 time=0);

	RTMFPTrigger				_trigger; // count the number of sended cycles for managing repeated/lost counts
	std::deque<RTMFPMessage*>	_messages; // queue of messages to send
	Mona::UInt64				_stage; // stage (index) of the last message sent
	std::deque<RTMFPMessage*>	_messagesSent; // queue of messages to send back or consider lost if delay is elapsed
	Mona::UInt64				_stageAck; // stage of the last message acknowledged by the server
	Mona::UInt32				_lostCount; // number of lost messages
	double						_ackCount; // number of acknowleged messages
	Mona::UInt32				_repeatable; // number of repeatable messages waiting for acknowledgment
	FlowManager&				_band; // RTMFP session to send messages
	Mona::Time					_closeTime; // time since writer has been closed

};
