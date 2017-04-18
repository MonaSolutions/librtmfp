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

#include "RTMFP.h"
#include "FlowManager.h"
#include "FlashWriter.h"
#include "RTMFPSender.h"
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
struct RTMFPWriter : FlashWriter, virtual Mona::Object {
	RTMFPWriter(Mona::UInt8 marker, Mona::UInt64 id, Mona::UInt64 flowId, const Mona::Binary& signature, RTMFP::Output& output);

	Mona::UInt64		queueing() const { return _output.queueing(); }
	void		acquit(Mona::UInt64 stageAck, Mona::UInt32 lostCount);
	bool		consumed() { return _writers.empty() && closed() && !_pSender && _pQueue.unique() && _pQueue->empty() && _closeTime.isElapsed(130000); } // Wait 130s before closing the writer definetly

	template <typename ...Args>
	void fail(Args&&... args) {
		if (closed())
			return;
		WARN("Writer ", _pQueue->id, " has failed, ", std::forward<Args>(args)...);
		fail();
	}

	void				clear() { _pSender.reset(); }
	void				flush();

	/*!
	Close the writer, override closing(Int32 code) to execute closing code */
	void				close(Mona::Int32 error = 0, const char* reason = NULL);
	bool				closed() const { return _state >= NEAR_CLOSED; }

	Mona::BinaryWriter&	writeRaw() { return *write(AMF::TYPE_RAW); }
	void				writePing() { write(AMF::TYPE_RAW)->write16(0x0006).write32((Mona::UInt32)Mona::Time::Now()); }
	void				writePong(Mona::UInt32 pingTime) { write(AMF::TYPE_RAW)->write16(0x0007).write32(pingTime); }

	// Write raw data
	virtual void		writeRaw(const Mona::UInt8* data, Mona::UInt32 size);
	// Ask the server to connect to group, netGroup must be in binary format (32 bytes)
	virtual void		writeGroupConnect(const std::string& netGroup);
	// Init the group session with a peer, netGroup must be in hexa format (64 bytes)
	virtual void		writePeerGroup(const std::string& netGroup, const Mona::UInt8* key, const Mona::Binary& rawId);
	// Send the Group begin message (02 + 0E)
	virtual void		writeGroupBegin();
	// Send the Group Media subscription
	virtual void		writeGroupMedia(const std::string& streamName, const Mona::UInt8* data, Mona::UInt32 size, RTMFPGroupConfig* groupConfig);
	// Send the Group Media end
	virtual void		writeGroupEndMedia(Mona::UInt64 lastFragment);
	// Start to play the group stream
	virtual void		writeGroupPlay(Mona::UInt8 mode);
	// Send a pull request to a peer (message 2B)
	virtual void		writeGroupPull(Mona::UInt64 index);
	// Send a fragment
	virtual void		writeGroupFragment(const GroupFragment& fragment);

	const Mona::UInt64	id;
	const Mona::UInt64	flowId;
	const Mona::Packet	signature;

private:

	void				repeatMessages(Mona::UInt32 lostCount = 0); // fragments=0 means all possible!
	AMFWriter&			newMessage(bool reliable, const Mona::Packet& packet = Mona::Packet::Null());
	AMFWriter&			write(AMF::Type type, Mona::UInt32 time = 0, RTMFP::DataType packetType = RTMFP::TYPE_AMF, const Mona::Packet& packet = Mona::Packet::Null(), bool reliable = true);


	RTMFP::Output&							_output;
	std::shared_ptr<RTMFPSender>			_pSender;
	std::shared_ptr<RTMFPSender::Queue>		_pQueue;
	Mona::UInt64							_stageAck;
	Mona::UInt32							_lostCount;
	Mona::UInt32							_repeatDelay;
	Mona::Time								_repeatTime;
	std::set<std::shared_ptr<RTMFPWriter>>	_writers;

private:

	Mona::Time					_closeTime; // Time when the writer has been closed
	Mona::UInt8					_marker;

};
