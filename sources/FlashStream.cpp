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

#include "FlashStream.h"
#include "ParameterWriter.h"
#include "Mona/MapParameters.h"
#include "RTMFP.h"

using namespace std;
using namespace Mona;

FlashStream::FlashStream(UInt16 id) : id(id), _bufferTime(0) {
	DEBUG("FlashStream ", id, " created")
}

FlashStream::~FlashStream() {
	DEBUG("FlashStream ",id," deleted")
}

bool FlashStream::process(PacketReader& packet, UInt64 flowId, UInt64 writerId, double lostRate) {

	UInt32 time(0);
	AMF::ContentType type = (AMF::ContentType)packet.read8();
	switch (type) {
		case AMF::AUDIO:
		case AMF::VIDEO:
			time = packet.read32();
			break;
		default:
			packet.next(4);
			break;
	}

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {
		case AMF::ABORT: // TODO: check what it is (in a NetGroup communication)
			INFO("Unknown 02 packet type : ", Util::FormatHex(packet.current(), packet.available(), LOG_BUFFER))
			break;

		case AMF::AUDIO:
			return audioHandler(time, packet, lostRate);
		case AMF::VIDEO:
			return videoHandler(time, packet, lostRate);

		case AMF::DATA_AMF3:
			packet.next();
		case AMF::DATA: {
			AMFReader reader(packet);
			return dataHandler(reader, lostRate);
		}

		case AMF::EMPTY:
			break;

		case AMF::INVOCATION_AMF3:
			packet.next();
		case AMF::INVOCATION: {
			string name;
			AMFReader reader(packet);
			reader.readString(name);
			double number(0);
			reader.readNumber(number);
			reader.readNull();
			return messageHandler(name, reader, flowId, writerId, number);
		}

		case AMF::RAW:
			return rawHandler(packet.read16(), packet);

		default:
			ERROR("Unpacking type '",Format<UInt8>("%02X",(UInt8)type),"' unknown")
	}

	return false;
}


UInt32 FlashStream::bufferTime(UInt32 ms) {
	_bufferTime = ms;
	INFO("setBufferTime ", ms, "ms on stream ",id)
	return _bufferTime;
}

bool FlashStream::messageHandler(const string& name, AMFReader& message, UInt64 flowId, UInt64 writerId, double callbackHandler) {
	
	if(name == "onStatus") {
		double callback;
		message.readNumber(callback);
		message.readNull();

		if(message.nextType() != AMFReader::OBJECT) {
			ERROR("Unexpected onStatus value type : ",message.nextType())
			return false;
		}

		MapParameters params;
		ParameterWriter paramWriter(params);
		message.read(AMFReader::OBJECT, paramWriter);

		string level;
		params.getString("level",level);
		if(!level.empty()) {
			string code, description;
			params.getString("code",code);
			params.getString("description", description);
			if (level == "status" || level == "error")
				return OnStatus::raise<true>(code, description, id, flowId, callbackHandler);
			else {
				ERROR("Unknown level message type : ", level)
				return false;
			}
		}
		ERROR("Unknown onStatus event, level is not set")
		return false;
	}
	/*** P2P Publisher part ***/
	else if (name == "play") {

		string publication;
		message.readString(publication);
		
		if (OnPlay::raise<false>(publication, id, flowId, callbackHandler))
			_streamName = publication;

		return true;
	}

	ERROR("Message '",name,"' unknown on stream ",id);
	return false;
}

bool FlashStream::dataHandler(DataReader& data, double lostRate) {
	
	AMFReader reader(data.packet);
	string func, params, value;
	if (reader.nextType() == AMFReader::STRING) {
		reader.readString(func);

		UInt8 type(AMFReader::END);
		double number(0);
		bool first = true, boolean;
		while ((type = reader.nextType()) != AMFReader::END) {
			switch (type) {
				case AMFReader::STRING:
					reader.readString(value); 
					String::Append(params, (!first) ? ", " : "", value); break;
				case AMFReader::NUMBER:
					reader.readNumber(number);
					String::Append(params, (!first) ? ", " : "", number); break;
				case AMFReader::BOOLEAN:
					reader.readBoolean(boolean);
					String::Append(params, (!first) ? ", " : "", boolean); break;
				default:
					reader.next(); break;
			}
			first = false;
		}
		TRACE("Function ", func, " received with parameters : ", params)
		// TODO: make a callback function
	}
	return true;
}

bool FlashStream::rawHandler(UInt16 type, PacketReader& packet) {
	switch (type) {
		case 0x0000:
			INFO("Stream begin message on NetStream ", id, " (value : ", packet.read32(), ")")
			break;
		case 0x0001:
			INFO("Stream stop message on NetStream ", id, " (value : ", packet.read32(), ")")
			break;
		case 0x001f: // unknown for now
		case 0x0020: // unknown for now
			break;
		case 0x0022: // TODO: useless to support it?
			//INFO("Sync ",id," : (syncId=",packet.read32(),", count=",packet.read32(),")")
			break;
		default:
			ERROR("Raw message ", Format<UInt16>("%.4x", type), " unknown on stream ", id);
			return false;
	}
	return true;
}

bool FlashStream::audioHandler(UInt32 time,PacketReader& packet, double lostRate) {

	OnMedia::raise(_streamName, time, packet, lostRate, true);
	return true;
}

bool FlashStream::videoHandler(UInt32 time,PacketReader& packet, double lostRate) {

	OnMedia::raise(_streamName, time, packet, lostRate, false);
	return true;
}
