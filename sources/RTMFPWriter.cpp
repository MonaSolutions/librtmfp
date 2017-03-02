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
#include "Mona/Util.h"
#include "Mona/Logs.h"
#include "GroupStream.h"
#include "librtmfp.h"
#include "PeerMedia.h"

using namespace std;
using namespace Mona;

RTMFPWriter::RTMFPWriter(State state,const Packet& signature, FlowManager& band, shared_ptr<RTMFPWriter>& pThis, UInt64 idFlow) : FlashWriter(state), id(0), _band(band),
	_stage(0), _stageAck(0), flowId(idFlow), signature(move(signature)), _repeatable(0), _lostCount(0), _ackCount(0) {

}

RTMFPWriter::RTMFPWriter(State state,const Packet& signature, FlowManager& band, UInt64 idFlow) : FlashWriter(state), id(0), _band(band),
	_stage(0), _stageAck(0), flowId(idFlow), signature(move(signature)), _repeatable(0), _lostCount(0), _ackCount(0) {

}

RTMFPWriter::RTMFPWriter(RTMFPWriter& writer) : FlashWriter(writer), _band(writer._band),
	_repeatable(writer._repeatable), _stage(writer._stage), _stageAck(writer._stageAck),
	_ackCount(writer._ackCount), _lostCount(writer._lostCount), flowId(writer.flowId), signature(move(writer.signature)), id(writer.id) {
	reliable = true;
	close(false);
}

RTMFPWriter::~RTMFPWriter() {

	// delete messages
	clear();
	for (RTMFPMessage* pMessage : _messagesSent)
		delete pMessage;
	_messagesSent.clear();
}

void RTMFPWriter::clear() {

	for (RTMFPMessage* pMessage : _messages)
		delete pMessage;
	_messages.clear();
	FlashWriter::clear();
}

void RTMFPWriter::close(bool abrupt) {
	if(_state==CLOSED)
		return;
	if(_stage>0 || _messages.size()>0)
		createMessage(); // Send a MESSAGE_END just in the case where the receiver has been created (or will be created)

	if (_state < NEAR_CLOSED)
		_closeTime.update();
	_state = abrupt ? CLOSED : NEAR_CLOSED; // before flush to get MESSAGE_END!
	flush();
}

bool RTMFPWriter::acknowledgment(Exception& ex, BinaryReader& reader) {

	UInt64 bufferSize = reader.read7BitLongValue(); // TODO use this value in reliability mechanism?
	
	if(bufferSize==0) {
		// In fact here, we should send a 0x18 message (with id flow),
		// but it can create a loop... We prefer the following behavior
		WARN("Closing writer ", id, ", negative acknowledgment");
		close(false);
		return !ex;
	}

	UInt64 stageAckPrec = _stageAck;
	UInt64 stageReaden = reader.read7BitLongValue();
	UInt64 stage = _stageAck+1;

	if(stageReaden>_stage) {
		ERROR("Acknowledgment received ",stageReaden," superior than the current sending stage ",_stage," on writer ",id);
		_stageAck = _stage;
	} else if(stageReaden<=_stageAck) {
		// already acked
		if(reader.available()==0)
			DEBUG("Acknowledgment ",stageReaden," obsolete on writer ",id);
	} else
		_stageAck = stageReaden;

	UInt64 maxStageRecv = stageReaden;
	UInt32 pos=reader.position();

	while(reader.available()>0)
		maxStageRecv += reader.read7BitLongValue()+reader.read7BitLongValue()+2;
	if(pos != reader.position()) {
		// TRACE(stageReaden,"..x"Util::FormatHex(reader.current(),reader.available()));
		reader.reset(pos);
	}

	UInt64 lostCount = 0;
	UInt64 lostStage = 0;
	bool repeated = false;
	bool header = true;
	bool stop=false;

	auto it=_messagesSent.begin();
	while(!stop && it!=_messagesSent.end()) {
		RTMFPMessage& message(**it);

		if(message.fragments.empty()) {
			CRITIC("RTMFPMessage ",(stage+1)," is bad formatted on flowWriter ",id);
			++it;
			continue;
		}

		map<UInt32,UInt64>::iterator itFrag=message.fragments.begin();
		while(message.fragments.end()!=itFrag) {
			
			// ACK
			if(_stageAck>=stage) {
				message.fragments.erase(message.fragments.begin());
				itFrag=message.fragments.begin();
				++_ackCount;
				++stage;
				continue;
			}

			// Read lost informations
			while(!stop) {
				if(lostCount==0) {
					if(reader.available()>0) {
						lostCount = reader.read7BitLongValue()+1;
						lostStage = stageReaden+1;
						stageReaden = lostStage+lostCount+reader.read7BitLongValue();
					} else {
						stop=true;
						break;
					}
				}
				// check the range
				if(lostStage>_stage) {
					// Not yet sent
					ERROR("Lost information received ",lostStage," have not been yet sent on writer ",id);
					stop=true;
				} else if(lostStage<=_stageAck) {
					// already acked
					--lostCount;
					++lostStage;
					continue;
				}
				break;
			}
			if(stop)
				break;
			
			// lostStage > 0 and lostCount > 0

			if(lostStage!=stage) {
				if(repeated) {
					++stage;
					++itFrag;
					header=true;
				} else // No repeated, it means that past lost packet was not repeatable, we can ack this intermediate received sequence
					_stageAck = stage;
				continue;
			}

			/// Repeat message asked!
			if(!message.repeatable) {
				if(repeated) {
					++itFrag;
					++stage;
					header=true;
				} else {
					INFO("RTMFPWriter ",id," : message ",stage," lost");
					--_ackCount;
					++_lostCount;
					_stageAck = stage;
				}
				--lostCount;
				++lostStage;
				continue;
			}

			repeated = true;
			// Don't repeat before that the receiver receives the itFrag->second sending stage
			if(itFrag->second >= maxStageRecv) {
				++stage;
				header=true;
				--lostCount;
				++lostStage;
				++itFrag;
				continue;
			}

			// Repeat message

			DEBUG("RTMFPWriter ",id," : stage ",stage," repeated");
			UInt32 fragment(itFrag->first);
			itFrag->second = _stage; // Save actual stage sending to wait that the receiver gets it before to retry
			UInt32 contentSize = message.size() - fragment; // available
			++itFrag;

			// Compute flags
			UInt8 flags = 0;
			if(fragment>0)
				flags |= RTMFP::MESSAGE_WITH_BEFOREPART; // fragmented
			if(itFrag!=message.fragments.end()) {
				flags |= RTMFP::MESSAGE_WITH_AFTERPART;
				contentSize = itFrag->first - fragment;
			}

			UInt32 size = contentSize+4;
			UInt32 availableToWrite(_band.availableToWrite());
			if(!header && size>availableToWrite) {
				_band.flush();
				header=true;
			}

			if(header)
				size+=headerSize(stage);

			if(size>availableToWrite)
				_band.flush();

			// Write packet
			size-=3;  // type + timestamp removed, before the "writeMessage"
			packMessage(_band.writeMessage(header ? 0x10 : 0x11,(UInt16)size),stage,flags,header,message,fragment,contentSize);
			header=false;
			--lostCount;
			++lostStage;
			++stage;
		}

		if(message.fragments.empty()) {
			if(message.repeatable)
				--_repeatable;
			if(_ackCount || _lostCount) {
				//TODO : _qos.add(_lostCount / (_lostCount + _ackCount));
				_ackCount=_lostCount=0;
			}
			
			delete *it;
			it=_messagesSent.erase(it);
		} else
			++it;
	
	}

	if(lostCount>0 && reader.available()>0)
		ERROR("Some lost information received have not been yet sent on writer ",id);


	// rest messages repeatable?
	if(_repeatable==0)
		_trigger.stop();
	else if(_stageAck>stageAckPrec || repeated)
		_trigger.reset();
	return true;
}

bool RTMFPWriter::manage(Exception& ex) {
	if(_state < NEAR_CLOSED && !_band.failed()) {
		
		// if some acknowlegment has not been received we send the messages back (8 times, with progressive occurences)
		if (_trigger.raise(ex)) {
			TRACE("Sending back repeatable messages (cycle : ", _trigger.cycle(), ")")
			raiseMessage();
		}
		// When the peer/server doesn't send acknowledgment since a while we close the writer
		else if (ex) {
			ex.set<Ex::Net::Protocol>("Congestion issue, writer ", id, " can't deliver its data");
			return false;
		}
	}
	flush();
	return true;
}

UInt32 RTMFPWriter::headerSize(UInt64 stage) { // max size header = 50
	UInt32 size= Binary::Get7BitValueSize(id);
	size+= Binary::Get7BitValueSize(stage);
	if(_stageAck>stage)
		CRITIC("stageAck ",_stageAck," superior to stage ",stage," on writer ",id);
	size+= Binary::Get7BitValueSize(stage-_stageAck);
	size+= _stageAck>0 ? 0 : (signature.size()+((flowId==0 || id<=2)?2:(4+ Binary::Get7BitValueSize(flowId)))); // TODO: check the condition id<=2 (see next TODO below)
	return size;
}


void RTMFPWriter::packMessage(BinaryWriter& writer,UInt64 stage,UInt8 flags,bool header,const RTMFPMessage& message, UInt32 offset, UInt16 size) {
	if(_stageAck==0 && header)
		flags |= RTMFP::MESSAGE_OPTIONS;
	if(size==0)
		flags |= RTMFP::MESSAGE_ABANDON;
	if(_state >= NEAR_CLOSED && _messages.size()==1) // On LAST message
		flags |= RTMFP::MESSAGE_END;

	// TRACE("RTMFPWriter ",id," stage ",stage);

	writer.write8(flags);

	if(header) {
		writer.write7BitLongValue(id);
		writer.write7BitLongValue(stage);
		writer.write7BitLongValue(stage-_stageAck);

		// signature
		if(_stageAck==0) {
			writer.write8((UInt8)signature.size()).write(signature);
			// Send flowId for a new flow (not for writer 2 => AMS support)
			if(id>2) {
				writer.write8(1+Binary::Get7BitValueSize(flowId)); // following size
				writer.write8(0x0a); // Unknown!
				writer.write7BitLongValue(flowId);
			}
			writer.write8(0); // marker of end for this part
		}
	}

	if (size == 0)
		return;

	if (offset < message.frontSize()) {
		UInt8 count = message.frontSize()-offset;
		if (size<count)
			count = (UInt8)size;
		writer.write(message.front()+offset,count);
		size -= count;
		if (size == 0)
			return;
		offset += count;
	}

	writer.write(message.body()+offset-message.frontSize(), size);
}

void RTMFPWriter::raiseMessage() {
	bool header = true;
	bool stop = true;
	bool sent = false;
	UInt64 stage = _stageAck+1;

	for(RTMFPMessage* pMessage : _messagesSent) {
		RTMFPMessage& message(*pMessage);
		
		if(message.fragments.empty())
			break;

		// not repeat unbuffered messages
		if(!message.repeatable) {
			stage += message.fragments.size();
			header = true;
			continue;
		}
		
		/// HERE -> message repeatable AND already flushed one time!

		if(stop) {
			_band.flush(); // To repeat message, before we must send precedent waiting mesages
			stop = false;
		}

		map<UInt32,UInt64>::const_iterator itFrag=message.fragments.begin();
		UInt32 available = message.size()-itFrag->first;
	
		while(itFrag!=message.fragments.end()) {
			UInt32 contentSize = available;
			UInt32 fragment(itFrag->first);
			++itFrag;

			// Compute flags
			UInt8 flags = 0;
			if(fragment>0)
				flags |= RTMFP::MESSAGE_WITH_BEFOREPART; // fragmented
			if(itFrag!=message.fragments.end()) {
				flags |= RTMFP::MESSAGE_WITH_AFTERPART;
				contentSize = itFrag->first - fragment;
			}

			UInt32 size = contentSize+4;

			if(header)
				size+=headerSize(stage);

			// Actual sending packet is enough large? Here we send just one packet!
			if(size>_band.availableToWrite()) {
				if(!sent)
					ERROR("Raise messages on writer ",id," without sending!");
				DEBUG("Raise message on writer ",id," finishs on stage ",stage);
				return;
			}
			sent=true;

			// Write packet
			size-=3;  // type + timestamp removed, before the "writeMessage"
			packMessage(_band.writeMessage(header ? 0x10 : 0x11,(UInt16)size),stage++,flags,header,message,fragment,contentSize);
			available -= contentSize;
			header=false;
		}
	}

	if(stop)
		_trigger.stop();
}

bool RTMFPWriter::flush(bool full) {

	if(_messagesSent.size()>100)
		TRACE("Buffering become high : _messagesSent.size()=",_messagesSent.size());

	if(_state==OPENING) {
		ERROR("Violation policy, impossible to flush data on a opening writer");
		return false;
	}

	bool hasSent(false);

	// flush
	bool header = !_band.canWriteFollowing(*this);

	while(!_messages.empty()) {
		hasSent = true;

		RTMFPMessage& message(*_messages.front());

		if(message.repeatable) {
			++_repeatable;
			_trigger.start();
		}

		UInt32 fragments= 0;
		UInt32 available = message.size();
	
		do {

			++_stage;

			// Actual sending packet is enough large?
			UInt32 contentSize = _band.availableToWrite();
			UInt32 headerSize = (header && contentSize<62) ? this->headerSize(_stage) : 0; // calculate only if need!
			if(contentSize<(headerSize+12)) { // 12 to have a size minimum of fragmentation
				_band.flush(); // send packet (and without time echo)
				header=true;
			}

			contentSize = available;
			UInt32 size = contentSize+4;
			
			if(header)
				size+= headerSize>0 ? headerSize : this->headerSize(_stage);

			// Compute flags
			UInt8 flags = 0;
			if(fragments>0)
				flags |= RTMFP::MESSAGE_WITH_BEFOREPART;

			bool head = header;
			UInt32 availableToWrite(_band.availableToWrite());
			if(size>availableToWrite) {
				// the packet will change! The message will be fragmented.
				flags |= RTMFP::MESSAGE_WITH_AFTERPART;
				contentSize = availableToWrite-(size-contentSize);
				size=availableToWrite;
				header=true;
			} else
				header=false; // the packet stays the same!

			// Write packet
			size-=3; // type + timestamp removed, before the "writeMessage"
			packMessage(_band.writeMessage(head ? 0x10 : 0x11,(UInt16)size,this),_stage,flags,head,message,fragments,contentSize);
			
			message.fragments[fragments] = _stage;
			available -= contentSize;
			fragments += contentSize;

		} while(available>0);

		//TODO : _qos.add(message.size(),_band.ping());
		_messagesSent.emplace_back(&message);
		_messages.pop_front();
	}

	if (full)
		_band.flush();
	return hasSent;
}

RTMFPMessageBuffered& RTMFPWriter::createMessage() {
	if (_state == CLOSED || _band.failed()) { // not NEAR_CLOSED because we want to send the last messages
		static RTMFPMessageBuffered MessageNull;
		return MessageNull;
	}
	RTMFPMessageBuffered* pMessage = new RTMFPMessageBuffered(reliable);
	_messages.emplace_back(pMessage);
	return *pMessage;
}

AMFWriter& RTMFPWriter::write(AMF::Type type, const Packet& packet, UInt32 time) {
	if (type < AMF::TYPE_AUDIO || type > AMF::TYPE_VIDEO)
		time = 0; // Because it can "dropped" the packet otherwise (like if the Writer was not reliable!)
	if(packet && !reliable && _state==OPENED && !_band.failed()) {
		_messages.emplace_back(new RTMFPMessageUnbuffered(type, time, packet));
		flush(false);
        return AMFWriter::Null();
	}
	AMFWriter& amf = createMessage().writer;
	amf->write8(type);
	if (type == AMF::TYPE_INVOCATION_AMF3) // Added for Play request in P2P, TODO: see if it is really needed
		amf->write8(0);
	amf->write32(time);
	if(type==AMF::TYPE_DATA_AMF3)
		amf->write8(0);
	if(packet)
		amf->write(packet);
	return amf;
}

void RTMFPWriter::writeGroupConnect(const string& netGroup) {
	string tmp;
	BinaryWriter& writer = createMessage().writer->write8(GroupStream::GROUP_INIT).write16(0x2115).write(String::ToHex(netGroup, tmp)); // binary string
}

void RTMFPWriter::writePeerGroup(const string& netGroup, const UInt8* key, const Binary& rawId) {

	AMFWriter& writer = createMessage().writer;
	writer->write8(GroupStream::GROUP_INIT).write16(0x4100).write(netGroup); // hexa format
	writer->write16(0x2101).write(key, Crypto::SHA256_SIZE);
	writer->write16(0x2303).write(rawId); // binary format
}

void RTMFPWriter::writeGroupBegin() {
	createMessage().writer->write8(AMF::TYPE_ABORT);
	createMessage().writer->write8(GroupStream::GROUP_BEGIN);
}

void RTMFPWriter::writeGroupMedia(const std::string& streamName, const UInt8* data, UInt32 size, RTMFPGroupConfig* groupConfig) {

	AMFWriter& writer = createMessage().writer;
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

void RTMFPWriter::writeGroupPlay(UInt8 mode) {
	AMFWriter& writer = createMessage().writer;
	writer->write8(GroupStream::GROUP_PLAY_PUSH).write8(mode);
}

void RTMFPWriter::writeGroupPull(UInt64 index) {
	AMFWriter& writer = createMessage().writer;
	writer->write8(GroupStream::GROUP_PLAY_PULL).write7BitLongValue(index);
}

void RTMFPWriter::writeRaw(const UInt8* data,UInt32 size) {
	if(reliable || _state==OPENING) {
		createMessage().writer->write(data,size);
		return;
	}
	if(_state >= NEAR_CLOSED || _band.failed())
		return;
	_messages.emplace_back(new RTMFPMessageUnbuffered(Packet(data, size)));
	flush(false);
}


void RTMFPWriter::writeGroupFragment(const GroupFragment& fragment) {
	AMFWriter& writer = createMessage().writer;

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
}
