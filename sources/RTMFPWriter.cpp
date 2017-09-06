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

#include "RTMFPWriter.h"
#include "Base/Util.h"
#include "Base/Logs.h"
#include "GroupStream.h"
#include "librtmfp.h"
#include "PeerMedia.h"

using namespace std;
using namespace Base;

RTMFPWriter::RTMFPWriter(UInt8 marker, UInt64 id, UInt64 flowId, const Binary& signature, RTMFP::Output& output) :
	_marker(marker), _repeatDelay(0), _output(output), _stageAck(0), _lostCount(0), id(id), flowId(flowId), signature(signature) {
	_pQueue.reset(new RTMFPSender::Queue(id, flowId, signature));
}

void RTMFPWriter::close(Int32 error, const char* reason) {
	
	if (_state < NEAR_CLOSED) {
		if (error < 0) // if impossible to send (code<0, See Session::DEATH_...) clear message queue impossible to send!
			clear();
		if (error >= 0 && (_stageAck || _repeatDelay)) {
			Packet emptyPacket;
			newMessage(true, emptyPacket); // Send a MESSAGE_END just in the case where the receiver has been created
		}
		if (error >= 0)
			flush();
		_closeTime.update();
	}
		
	_state = (_state>=NEAR_CLOSED) ? CLOSED : NEAR_CLOSED; // before flush to get MESSAGE_END!
}

void RTMFPWriter::acquit(UInt64 stageAck, UInt32 lostCount) {
	TRACE("Ack ", stageAck, " on writer ", _pQueue->id, " (lostCount=", lostCount, ")");
	// have to continue to become consumed even if writer closed!
	if (stageAck > _stageAck) {
		// progress!
		_stageAck = stageAck;
		_lostCount = 0;
		// reset repeat time on progression!
		_repeatDelay = _output.rto();
		_repeatTime.update();
		// continue sending
		_output.send(make_shared<RTMFPAcquiter>(_marker, _pQueue, _stageAck));
		return;
	}
	if (!lostCount) {
		DEBUG("Ack ", stageAck, " obsolete on writer ", _pQueue->id);
		return;
	}
	if (lostCount > _lostCount) {
		/// emulate ERTO-timeout=ping if lost infos =>
		// repeating is caused by a gap in ack-range, it can be a packet lost or an non-ordering transfer
		// to avoid a self-sustaining congestion repeated just the first missing packet if lost infos are present
		// and just once time (raising=true, to emulate first RTMFP ERTO=ping), let do the trigger after
		repeatMessages(_lostCount = lostCount);
	}
}

void RTMFPWriter::repeatMessages(UInt32 lostCount) {
	if (lostCount) {
		// means that there is something lost! We can start a repeation without wait end of current sending
		_output.send(make_shared<RTMFPRepeater>(_marker, _pQueue, lostCount>0xFF ? 0xFF : lostCount));
		return;
	}
	if (!_pQueue.unique())
		return; // wait next! is sending, wait before to repeat packets
				// REPEAT!
	if (_pQueue->empty()) {
		// nothing to repeat, stop repeat
		_repeatDelay = 0;
		return;
	}
	if (!_repeatTime.isElapsed(_repeatDelay))
		return;
	_repeatTime.update();
	if (_repeatDelay<7072)
		_repeatDelay = (UInt32)(_repeatDelay*1.4142);
	else
		_repeatDelay = 10000;
	_output.send(make_shared<RTMFPRepeater>(_marker, _pQueue));
}

void RTMFPWriter::flush() {

	repeatMessages();
	if (!_pSender)
		return;
	if (!_repeatDelay) {
		// start repeat messages
		_repeatDelay = _output.rto();
		_repeatTime.update();
	}
	_output.send(_pSender);
	_pSender.reset();
}

AMFWriter& RTMFPWriter::newMessage(bool reliable, const Packet& packet) {
	if (closed())
		return AMFWriter::Null();
	if (!_pSender)
		_pSender.reset(new RTMFPMessenger(_marker, _pQueue));
	return ((RTMFPMessenger&)*_pSender).newMessage(reliable, packet);
}

AMFWriter& RTMFPWriter::write(AMF::Type type, UInt32 time, RTMFP::DataType packetType, const Packet& packet, bool reliable) {
	if (type < AMF::TYPE_AUDIO || type > AMF::TYPE_VIDEO)
		time = 0; // Because it can "dropped" the packet otherwise (like if the Writer was not reliable!)

	AMFWriter& writer = newMessage(reliable, packet);
	writer->write8(type).write32(time);
	if (type == AMF::TYPE_DATA_AMF3)
		writer->write8(0);
	return writer;
}

void RTMFPWriter::writeGroupConnect(const string& netGroup) {
	string tmp;
	Packet emptyPacket;
	newMessage(reliable, emptyPacket)->write8(GroupStream::GROUP_INIT).write16(0x2115).write(String::ToHex(netGroup, tmp)); // binary string
}

void RTMFPWriter::writePeerGroup(const string& netGroup, const UInt8* key, const Binary& rawId) {

	Packet emptyPacket;
	AMFWriter& writer = newMessage(reliable, emptyPacket);
	writer->write8(GroupStream::GROUP_INIT).write16(0x4100).write(netGroup); // hexa format
	writer->write16(0x2101).write(key, Crypto::SHA256_SIZE);
	writer->write16(0x2303).write(rawId); // binary format
}

void RTMFPWriter::writeGroupBegin() {
	Packet emptyPacket;
	newMessage(reliable, emptyPacket)->write8(AMF::TYPE_ABORT);
	newMessage(reliable, emptyPacket)->write8(GroupStream::GROUP_BEGIN);
}

void RTMFPWriter::writeGroupMedia(const std::string& streamName, const UInt8* data, UInt32 size, RTMFPGroupConfig* groupConfig) {

	Packet emptyPacket;
	AMFWriter& writer = newMessage(reliable, emptyPacket);
	writer->write8(GroupStream::GROUP_MEDIA_INFOS).write7BitEncoded(streamName.size() + 1).write8(0).write(streamName);
	writer->write(data, size);
	writer->write("\x01\x02");
	if (groupConfig->availabilitySendToAll)
		writer->write("\x01\x06");
	writer->write8(1 + Binary::Get7BitValueSize(UInt32(groupConfig->windowDuration))).write8('\x03').write7BitLongValue(groupConfig->windowDuration);
	writer->write("\x04\x04\x92\xA7\x60"); // Object encoding?
	writer->write8(1 + Binary::Get7BitValueSize(groupConfig->availabilityUpdatePeriod)).write8('\x05').write7BitLongValue(groupConfig->availabilityUpdatePeriod);
	writer->write8(1 + Binary::Get7BitValueSize(UInt32(groupConfig->fetchPeriod))).write8('\x07').write7BitLongValue(groupConfig->fetchPeriod);
}

void RTMFPWriter::writeGroupEndMedia(UInt64 lastFragment) {
	Packet emptyPacket;
	newMessage(reliable, emptyPacket)->write8(GroupStream::GROUP_MEDIA_INFOS).write7BitLongValue(lastFragment);
	flush();
}

void RTMFPWriter::writeGroupPlay(UInt8 mode) {
	Packet emptyPacket;
	newMessage(reliable, emptyPacket)->write8(GroupStream::GROUP_PLAY_PUSH).write8(mode);
}

void RTMFPWriter::writeGroupPull(UInt64 index) {
	Packet emptyPacket;
	newMessage(reliable, emptyPacket)->write8(GroupStream::GROUP_PLAY_PULL).write7BitLongValue(index);
}

void RTMFPWriter::writeRaw(const UInt8* data,UInt32 size) {
	Packet emptyPacket;
	newMessage(reliable, emptyPacket)->write(data, size);
	flush();
}

void RTMFPWriter::writeGroupFragment(const GroupFragment& fragment, bool fragmentReliable) {
	Packet emptyPacket;
	AMFWriter& writer = newMessage(reliable && fragmentReliable, emptyPacket);

	// AMF Group marker
	writer->write8(fragment.marker);
	// Fragment Id
	writer->write7BitLongValue(fragment.id);
	// Splitted sequence number
	if (fragment.splittedId > 0)
		writer->write8(fragment.splittedId);

	// Type and time, only for the first fragment
	if (fragment.marker != GroupStream::GROUP_MEDIA_NEXT && fragment.marker != GroupStream::GROUP_MEDIA_END) {
		// Media type
		writer->write8(fragment.type);
		// Time on 4 bytes
		writer->write32(fragment.time);
	}
	
	writer->write(fragment.data(), fragment.size());
	//flush();
}
