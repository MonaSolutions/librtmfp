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

#include "RTMFP.h"
#include "FlashStream.h"
#include "Base/Parameters.h"
#include "MapWriter.h"

using namespace std;
using namespace Base;

FlashStream::FlashStream(UInt16 id) : FlashHandler(id) {
	DEBUG("FlashStream ", streamId, " created")
}

FlashStream::~FlashStream() {
	DEBUG("FlashStream ", streamId," deleted")
}

bool FlashStream::process(const Packet& packet, UInt64 flowId, UInt64 writerId, double lostRate, bool lastFragment) {
	if (!packet)
		return true; // Flow is closing

	BinaryReader reader(packet.data(), packet.size());

	AMF::Type type = (AMF::Type)reader.read8();
	UInt32 time = reader.read32();
	return FlashHandler::process(type, time, Packet(packet, reader.current(), reader.available()), flowId, writerId, lostRate, lastFragment);
}

bool FlashStream::messageHandler(const string& name, AMFReader& message, UInt64 flowId, UInt64 writerId, double callbackHandler) {
	/*** P2P Publisher part ***/
	if (name == "play") {

		string publication;
		message.readString(publication);

		onPlay(publication, streamId, flowId, callbackHandler);
		return true;
	}
	else
		return FlashHandler::messageHandler(name, message, flowId, writerId, callbackHandler);
}

bool FlashHandler::process(AMF::Type type, UInt32 time, const Packet& packet, UInt64 flowId, UInt64 writerId, double lostRate, bool lastFragment) {

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {

		case AMF::TYPE_AUDIO:
			onMedia(_mediaId, time, packet, lostRate, AMF::TYPE_AUDIO);
			return true;
		case AMF::TYPE_VIDEO:
			onMedia(_mediaId, time, packet, lostRate, AMF::TYPE_VIDEO);
			return true;

		case AMF::TYPE_DATA_AMF3:
			return dataHandler(packet + 1, lostRate);
		case AMF::TYPE_DATA:
			return dataHandler(packet, lostRate);

		case AMF::TYPE_EMPTY:
			break;

		case AMF::TYPE_INVOCATION_AMF3:
		case AMF::TYPE_INVOCATION: {
			string name;
			AMFReader amfReader(packet.data() + (type & 1), packet.size() - (type & 1));
			amfReader.readString(name);
			double number(0);
			amfReader.readNumber(number);
			amfReader.readNull();
			return messageHandler(name, amfReader, flowId, writerId, number);
		}

		case AMF::TYPE_RAW:
			return rawHandler(BinaryReader(packet.data(), packet.size()).read16(), packet+2);

		default:
			ERROR("Unpacking type '", String::Format<int>("%02X", type), "' unknown");
	}

	return false;
}

bool FlashHandler::messageHandler(const string& name, AMFReader& message, UInt64 flowId, UInt64 writerId, double callbackHandler) {

	if (name == "onStatus") {
		double callback;
		message.readNumber(callback);
		message.readNull();

		if (message.nextType() != AMFReader::OBJECT) {
			ERROR("Unexpected onStatus value type : ", message.nextType())
			return false;
		}

		Parameters params;
		MapWriter<Parameters> paramWriter(params);
		message.read(AMFReader::OBJECT, paramWriter);

		string level;
		params.getString("level", level);
		if (!level.empty()) {
			string code, description;
			params.getString("code", code);
			params.getString("description", description);
			if (level == "status" || level == "error")
				return onStatus(code, description, streamId, flowId, callbackHandler);
			else {
				ERROR("Unknown level message type : ", level)
				return false;
			}
		}
		ERROR("Unknown onStatus event, level is not set")
		return false;
	}

	ERROR("Message '", name, "' unknown on stream ", streamId);
	return false;
}

bool FlashHandler::dataHandler(const Packet& packet, double lostRate) {
	
	AMFReader reader(packet.data(), packet.size());
	string func, params, value;
	UInt8 type = reader.nextType();
	if (type == AMFReader::STRING) {
		reader.readString(func);

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
		DEBUG("Function ", func, " received with parameters : ", params)
		// TODO: make a callback function
	} else
		DEBUG("Data with type ", type, " received but not handled, type : ")
	onMedia(_mediaId, 0, packet, lostRate, AMF::TYPE_DATA);
	return true;
}

bool FlashHandler::rawHandler(UInt16 type, const Packet& packet) {
	BinaryReader reader(packet.data(), packet.size());
	switch (type) {
		case 0x0000:
			INFO("Stream begin message on NetStream ", streamId, " (value : ", reader.read32(), ")")
			break;
		case 0x0001:
			INFO("Stream stop message on NetStream ", streamId, " (value : ", reader.read32(), ")")
			break;
		case 0x001f: // unknown for now
		case 0x0020: // unknown for now
			break;
		case 0x0022: // TODO: useless to support it?
			//INFO("Sync ",id," : (syncId=",packet.read32(),", count=",packet.read32(),")")
			break;
		default:
			ERROR("Raw message ", String::Format<UInt16>("%.4x", type), " unknown on NetStream ", streamId);
			return false;
	}
	return true;
}
