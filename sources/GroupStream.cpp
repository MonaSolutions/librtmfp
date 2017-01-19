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
	DEBUG("GroupStream ",id," deleted")
}

bool GroupStream::process(PacketReader& packet, UInt64 flowId, UInt64 writerId, double lostRate) {

	UInt32 time(0);
	GroupStream::ContentType type = (GroupStream::ContentType)packet.read8();

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {

		case GroupStream::GROUP_MEMBER: { // RTMFPSession event (TODO: see if it must be moved in FlashStream)
			string member, id;
			packet.read(PEER_ID_SIZE, member);
			Util::FormatHex(BIN member.data(), member.size(), id);
			DEBUG("NetGroup Peer ID added : ", id)
			OnNewPeer::raise(id);
			return true;
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
			return true;
		}
		case GroupStream::GROUP_DATA: {
			string value;
			if (packet.available()) {
				UInt16 size = packet.read16();
				packet.read(size, value);
			}
			DEBUG("GroupStream ", id, " - NetGroup data message type : ", value)
			return true;
		}
		case GroupStream::GROUP_ASK_CLOSE:
			return OnGroupAskClose::raise<false>(id, flowId, writerId);
		case GroupStream::GROUP_BEGIN_NEAREST:
			OnGroupBegin::raise(id, flowId, writerId);
			return true;
		case GroupStream::GROUP_BEGIN:
			OnGroupBegin::raise(id, flowId, writerId); 
			return true;
		case GroupStream::GROUP_REPORT:
			OnGroupReport::raise(packet, id, flowId, writerId); 
			return true;
		case GroupStream::GROUP_PLAY_PUSH:
			OnGroupPlayPush::raise(packet, id, flowId, writerId); 
			return true;
		case GroupStream::GROUP_PLAY_PULL:
			OnGroupPlayPull::raise(packet, id, flowId, writerId);
			return true;
		case GroupStream::GROUP_MEDIA_INFOS:
			return OnGroupMedia::raise<false>(packet, id, flowId, writerId); 
		case GroupStream::GROUP_FRAGMENTS_MAP:
			OnFragmentsMap::raise(packet, id, flowId, writerId);
			return true;
		case GroupStream::GROUP_MEDIA_DATA: {

			UInt64 counter = packet.read7BitLongValue();
			
			UInt8 mediaType = packet.read8();
			time = packet.read32();
			DEBUG("GroupStream ", id, " - Group media normal : counter=", counter, ", time=", time, ", type=", (mediaType == AMF::AUDIO ? "Audio" : (mediaType == AMF::VIDEO ? "Video" : "Unknown")))
			OnFragment::raise(type, counter, 0, mediaType, time, packet, lostRate, id, flowId, writerId);
			if (mediaType != AMF::AUDIO && mediaType != AMF::VIDEO)
				return FlashStream::process((AMF::ContentType)mediaType, time, packet, id, writerId, lostRate); // recursive call, can be invocation, data etc.. (TODO: manage fragmented data)
			return true;
		} case GroupStream::GROUP_MEDIA_START: { // Start a splitted media sequence

			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			UInt8 mediaType = splitNumber; // In rare case the splitNumber is not written so it is the media type
			if ((*packet.current()) == AMF::AUDIO || (*packet.current()) == AMF::VIDEO)
				mediaType = packet.read8();
			time = packet.read32();

			DEBUG("GroupStream ", id, " - Group media start : counter=", counter, ", time=", time, ", splitNumber=", splitNumber, ", type=", (mediaType == AMF::AUDIO ? "Audio" : (mediaType == AMF::VIDEO ? "Video" : "Unknown")))
			if (mediaType == AMF::AUDIO || mediaType == AMF::VIDEO)
				OnFragment::raise(type, counter, splitNumber, mediaType, time, packet, lostRate, id, flowId, writerId);
			else // TODO: Support other types (functions) with splitted fragments
				ERROR("Media type ", Format<UInt8>("%02X", mediaType), " not supported (or data decoding error)")
			return true;
		}
		case GroupStream::GROUP_MEDIA_NEXT: { // Continue a splitted media sequence

			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			DEBUG("GroupStream ", id, " - Group media next : counter=", counter, ", splitNumber=", splitNumber)
			OnFragment::raise(type, counter, splitNumber, 0, 0, packet, lostRate, id, flowId, writerId);
			return true;
		}
		case GroupStream::GROUP_MEDIA_END: { // End of a splitted media sequence

			UInt64 counter = packet.read7BitLongValue();
			DEBUG("GroupStream ", id, " - Group media end : counter=", counter)
			OnFragment::raise(type, counter, 0, 0, 0, packet, lostRate, id, flowId, writerId);
			return true;
		}

		default:
			ERROR("GroupStream ", id, ", Unpacking type '",Format<UInt8>("%02X",(UInt8)type),"' unknown")
	}

	return false;
}


bool GroupStream::messageHandler(const string& name, AMFReader& message, UInt64 flowid, UInt64 writerId, double callbackHandler) {
	/*** Player ***/
	if (name == "closeStream") {
		INFO("Stream ", id, " is closing...")
		// TODO: implement close method
		return false;
	}
	
	return FlashStream::messageHandler(name, message, id, writerId, callbackHandler);
}

