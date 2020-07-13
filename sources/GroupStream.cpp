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

#include "GroupStream.h"
#include "RTMFP.h"
#include "Base/Util.h"

using namespace std;
using namespace Base;

GroupStream::GroupStream(UInt16 id) : FlashStream(id) {
	DEBUG("GroupStream ", streamId, " created")
}

GroupStream::~GroupStream() {
	DEBUG("GroupStream ", streamId," deleted")
}

bool GroupStream::process(const Packet& packet, UInt64 flowId, UInt64 writerId, double lostRate, bool lastFragment) {
	if (!packet)
		return true; // Flow is closing

	UInt32 time(0);
	BinaryReader reader(packet.data(), packet.size());
	GroupStream::ContentType type = (GroupStream::ContentType)reader.read8();

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {

		case GroupStream::GROUP_MEMBER: { // RTMFPSession event (TODO: see if it must be moved in FlashStream)
			string rawId(PEER_ID_SIZE + 2, '\0'), id;
			BinaryWriter writer(BIN rawId.data(), rawId.size());
			writer.write("\x21\x0F");
			reader.read(PEER_ID_SIZE, STR (rawId.data() + 2));
			String::Append(id, String::Hex(BIN rawId.data() + 2, PEER_ID_SIZE));
			onNewPeer(rawId, id);
			return true;
		}
		case GroupStream::GROUP_INIT: {
			DEBUG("GroupStream ", streamId, " - NetGroup Peer Connection (type 01)")
			if (reader.read16() != 0x4100) {
				ERROR("Unexpected format for NetGroup ID header")
				break;
			}
			string netGroupId, encryptKey, rawId;
			reader.read(0x40, netGroupId);
			if (reader.read16() != 0x2101) {
				ERROR("Unexpected format for NetGroup ID header")
				break;
			}
			reader.read(0x20, encryptKey);
			if (reader.read16() != 0x2303) {
				ERROR("Unexpected format for Peer ID header")
				break;
			}
			reader.read(PEER_ID_SIZE + 2, rawId);
			return onGroupHandshake(netGroupId, encryptKey, rawId);
		}
		case GroupStream::GROUP_DATA: {
			string value;
			if (reader.available())
				reader.readString(value);
			DEBUG("GroupStream ", streamId, " - Group data message received : ", value);
			return true;
		}
		case GroupStream::GROUP_ASK_CLOSE:
			return onGroupAskClose(streamId, flowId, writerId);
		case GroupStream::GROUP_BEGIN_NEAREST:
			onGroupBegin(streamId, flowId, writerId);
			return true;
		case GroupStream::GROUP_BEGIN:
			onGroupBegin(streamId, flowId, writerId);
			return true;
		case GroupStream::GROUP_REPORT:
			onGroupReport(reader, streamId, flowId, writerId);
			return true;
		case GroupStream::GROUP_PLAY_PUSH:
			onGroupPlayPush(reader, streamId, flowId, writerId);
			return true;
		case GroupStream::GROUP_PLAY_PULL:
			onGroupPlayPull(reader, streamId, flowId, writerId, lastFragment);
			return true;
		case GroupStream::GROUP_MEDIA_INFOS:
			return onGroupMedia(reader, streamId, flowId, writerId);
		case GroupStream::GROUP_FRAGMENTS_MAP:
			onFragmentsMap(reader, streamId, flowId, writerId);
			return true;
		case GroupStream::GROUP_MEDIA_DATA: {

			UInt64 counter = reader.read7Bit<UInt64>();
			
			UInt8 mediaType = reader.read8();
			time = reader.read32();
			DEBUG("GroupStream ", streamId, " - Group media normal : counter=", counter, ", time=", time, ", type=", (mediaType == AMF::TYPE_AUDIO ? "Audio" : (mediaType == AMF::TYPE_VIDEO ? "Video" : "Unknown")))
			onFragment(type, counter, 0, mediaType, time, Packet(packet, reader.current(), reader.available()), lostRate, streamId, flowId, writerId);
			return true;
		} case GroupStream::GROUP_MEDIA_START: { // Start a splitted media sequence

			UInt64 counter = reader.read7Bit<UInt64>();
			UInt8 splitNumber = reader.read8(); // counter of the splitted sequence
			UInt8 mediaType = splitNumber; // In rare case the splitNumber is not written so it is the media type
			if ((*reader.current()) == AMF::TYPE_AUDIO || (*reader.current()) == AMF::TYPE_VIDEO)
				mediaType = reader.read8();
			time = reader.read32();

			DEBUG("GroupStream ", streamId, " - Group media start : counter=", counter, ", time=", time, ", splitNumber=", splitNumber, ", type=", (mediaType == AMF::TYPE_AUDIO ? "Audio" : (mediaType == AMF::TYPE_VIDEO ? "Video" : "Unknown")))
			onFragment(type, counter, splitNumber, mediaType, time, Packet(packet, reader.current(), reader.available()), lostRate, streamId, flowId, writerId);
			return true;
		}
		case GroupStream::GROUP_MEDIA_NEXT: { // Continue a splitted media sequence

			UInt64 counter = reader.read7Bit<UInt64>();
			UInt8 splitNumber = reader.read8(); // counter of the splitted sequence
			DEBUG("GroupStream ", streamId, " - Group media next : counter=", counter, ", splitNumber=", splitNumber)
			onFragment(type, counter, splitNumber, 0, 0, Packet(packet, reader.current(), reader.available()), lostRate, streamId, flowId, writerId);
			return true;
		}
		case GroupStream::GROUP_MEDIA_END: { // End of a splitted media sequence

			UInt64 counter = reader.read7Bit<UInt64>();
			DEBUG("GroupStream ", streamId, " - Group media end : counter=", counter)
			onFragment(type, counter, 0, 0, 0, Packet(packet, reader.current(), reader.available()), lostRate, streamId, flowId, writerId);
			return true;
		}

		default:
			ERROR("GroupStream ", streamId, ", Unpacking type '", String::Format<UInt8>("%02X", (UInt8)type), "' unknown");
	}

	return false;
}

GroupPostStream::GroupPostStream(UInt16 id) : FlashStream(id) {
	DEBUG("GroupPostStream ", streamId, " created")
}

GroupPostStream::~GroupPostStream() {
	DEBUG("GroupPostStream ", streamId, " deleted")
}

bool GroupPostStream::process(const Base::Packet& packet, Base::UInt64 flowId, Base::UInt64 writerId, double lostRate, bool lastFragment) {
	if (!packet)
		return true; // Flow is closing

	if (*packet.data() == 0x30 || *packet.data() == 0x3A) {
		BinaryReader reader(packet.data(), packet.size());
		UInt8 type = reader.read8();
		string key;
		onGroupPostKey(type, reader.read(8, key));
		return true;
	}

	AMFReader reader(packet);

	switch (reader.nextType()) {
	case DataReader::NUMBER: {
			string value;
			if (reader.available()) {
				double number;
				if (reader.readNumber(number))
					value = std::to_string(number);
			}
			onGroupPost(value);
			return true;
		}
		case DataReader::STRING: {
			string value;
			if (reader.available())
				reader.readString(value);
			onGroupPost(value);
			return true;
		}
		case DataReader::OTHER: {
			// TODO
			return true;
		}
		default:
			ERROR("GroupPostStream ", streamId, ", Unpacking type '", String::Format<UInt8>("%02X", reader.nextType()), "' unknown");
	}

	return false;
}
