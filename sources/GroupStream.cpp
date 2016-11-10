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
#include "ParameterWriter.h"
#include "RTMFP.h"

using namespace std;
using namespace Mona;

GroupStream::GroupStream(UInt16 id) : FlashStream(id) {
	DEBUG("GroupStream ", id, " created")
}

GroupStream::~GroupStream() {
	disengage();
	DEBUG("GroupStream ",id," deleted")
}

bool GroupStream::process(PacketReader& packet,FlashWriter& writer, double lostRate) {

	UInt32 time(0);
	GroupStream::ContentType type = (GroupStream::ContentType)packet.read8();

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {

		case GroupStream::GROUP_MEMBER: {
			string member, id;
			packet.read(PEER_ID_SIZE, member);
			DEBUG("NetGroup Peer ID added : ", Util::FormatHex(BIN member.data(), member.size(), id))
			OnNewPeer::raise(id);
			break;
		}
		case GroupStream::GROUP_INIT: {
			DEBUG("GroupStream ", id, " - NetGroup Peer Connection (type 01)")
			if (packet.read16() != 0x4100) {
				ERROR("Unexpected format for NetGroup ID header")
				break;
			}
			string netGroupId, encryptKey, peerId;
			packet.read(0x40, netGroupId);
			if (packet.read16() != 0x2101) {
				ERROR("Unexpected format for NetGroup ID header")
				break;
			}
			packet.read(0x20, encryptKey);
			if (packet.read32() != 0x2303210F) {
				ERROR("Unexpected format for Peer ID header")
				break;
			}
			packet.read(PEER_ID_SIZE, peerId); 
			OnGroupHandshake::raise(netGroupId, encryptKey, peerId);
			break;
		}
		case GroupStream::GROUP_DATA: {
			string value;
			if (packet.available()) {
				UInt16 size = packet.read16();
				packet.read(size, value);
			}
			INFO("GroupStream ", id, " - NetGroup data message type : ", value)
			break;
		}
		case GroupStream::GROUP_NKNOWN:
			DEBUG("GroupStream ", id, " - Unknown NetGroup 0C message type") // TODO: manage this (do we have to disconnect?)
			break;
		case GroupStream::GROUP_BEGIN_NEAREST:
			DEBUG("GroupStream ", id, " - NetGroup 0F (Begin nearest) message type")
			OnGroupBegin::raise(_peerId, writer);
			break;
		case GroupStream::GROUP_BEGIN:
			DEBUG("GroupStream ", id, " - NetGroup Begin received from ", _peerId)
			OnGroupBegin::raise(_peerId, writer);
			break;
		case GroupStream::GROUP_REPORT: {
			DEBUG("GroupStream ", id, " - NetGroup Report received from ", _peerId)
			OnGroupReport::raise(_peerId, packet, writer);
			break;
		}
		case GroupStream::GROUP_PLAY_PUSH:
			OnGroupPlayPush::raise(_peerId, packet, writer);
			break;
		case GroupStream::GROUP_PLAY_PULL:
			OnGroupPlayPull::raise(_peerId, packet, writer);
			break;
		case GroupStream::GROUP_INFOS: { // contain the stream name of an eventual publication
			INFO("GroupStream ", id, " - Group Media Subscription received from ", _peerId)
			OnGroupMedia::raise(_peerId, packet, writer);
			break;
		}
		case GroupStream::GROUP_FRAGMENTS_MAP:
			OnFragmentsMap::raise(_peerId, packet, writer);
			break;
		case GroupStream::GROUP_MEDIA_DATA: {

			UInt64 counter = packet.read7BitLongValue();
			DEBUG("GroupStream ", id, " - Group media message 20 : counter=", counter)
			
			UInt8 mediaType = packet.read8();
			time = packet.read32();
			OnFragment::raise(_peerId, type, counter, 0, mediaType, time, packet, lostRate);
			if (mediaType != AMF::AUDIO && mediaType != AMF::VIDEO)
				return FlashStream::process(packet, writer, lostRate); // recursive call, can be invocation or other
			break;
		} case GroupStream::GROUP_MEDIA_START: { // Start a splitted media sequence

			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			UInt8 mediaType = splitNumber; // In rare case the splitNumber is not written so it is the media type
			if ((*packet.current()) == AMF::AUDIO || (*packet.current()) == AMF::VIDEO)
				mediaType = packet.read8();
			time = packet.read32();

			DEBUG("GroupStream ", id, " - Group ", (mediaType == AMF::AUDIO ? "Audio" : (mediaType == AMF::VIDEO ? "Video" : "Unknown")), " Start Splitted media : counter=", counter, ", time=", time, ", splitNumber=", splitNumber)
			if (mediaType == AMF::AUDIO || mediaType == AMF::VIDEO)
				OnFragment::raise(_peerId, type, counter, splitNumber, mediaType, time, packet, lostRate);
			else // TODO: Support other types (functions) with splitted fragments
				ERROR("Media type ", Format<UInt8>("%02X", mediaType), " not supported (or data decoding error)")
			break;
		}
		case GroupStream::GROUP_MEDIA_NEXT: { // Continue a splitted media sequence

			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			DEBUG("GroupStream ", id, " - Group next Splitted media : counter=", counter, ", splitNumber=", splitNumber)
			OnFragment::raise(_peerId, type, counter, splitNumber, 0, 0, packet, lostRate);
			break;
		}
		case GroupStream::GROUP_MEDIA_END: { // End of a splitted media sequence

			UInt64 counter = packet.read7BitLongValue();
			DEBUG("GroupStream ", id, " - Group End splitted media : counter=", counter)
			OnFragment::raise(_peerId, type, counter, 1, 0, 0, packet, lostRate);
			break;
		}

		default:
			ERROR("GroupStream ", id, ", Unpacking type '",Format<UInt8>("%02X",(UInt8)type),"' unknown")
	}

	writer.setCallbackHandle(0);
	return writer.state()!=FlashWriter::CLOSED;
}


void GroupStream::messageHandler(const string& name, AMFReader& message, FlashWriter& writer) {
	/*** Player ***/
	if (name == "closeStream") {
		INFO("Stream ", id, " is closing...")
		// TODO: implement close method
		return;
	}
	
	FlashStream::messageHandler(name, message, writer);
}

