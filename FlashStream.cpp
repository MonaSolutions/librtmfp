#include "FlashStream.h"
#include "ParameterWriter.h"
#include "Mona/MapParameters.h"
#include "RTMFP.h"

using namespace std;
using namespace Mona;

FlashStream::FlashStream(UInt16 id) : id(id), _bufferTime(0), _message3Sent(false), _playing(false), _videoCodecSent(false), _splittedTime(0), _splittedLostRate(0.0), _splittedMediaType(0) {
	DEBUG("FlashStream ", id, " created")
}

FlashStream::~FlashStream() {
	disengage();
	DEBUG("FlashStream ",id," deleted")
}

void FlashStream::disengage(FlashWriter* pWriter) {

}


bool FlashStream::process(AMF::ContentType type,UInt32 time,PacketReader& packet,FlashWriter& writer, double lostRate) {

	// if exception, it closes the connection, and print an ERROR message
	switch(type) {

		case AMF::AUDIO:
			audioHandler(time,packet, lostRate);
			break;
		case AMF::VIDEO:
			videoHandler(time,packet, lostRate);
			break;

		case AMF::DATA_AMF3:
			packet.next();
		case AMF::DATA: {
			AMFReader reader(packet);
			dataHandler(reader, lostRate);
			break;
		}

		/*case AMF::EMPTY:
			break;*/

		case AMF::INVOCATION_AMF3:
			packet.next();
		case AMF::INVOCATION: {
			string name;
			AMFReader reader(packet);
			reader.readString(name);
			double number(0);
			reader.readNumber(number);
			writer.setCallbackHandle(number);
			reader.readNull();
			messageHandler(name,reader,writer);
			break;
		}

		case AMF::RAW:
			rawHandler(packet.read16(), packet, writer);
			break;

		case AMF::MEMBER:
		{
			string member;
			packet.read(PEER_ID_SIZE, member);
			memberHandler(member);
			break;
		}
		case AMF::CHUNKSIZE:
		{
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
		case AMF::ABORT:
		{
			string value;
			if (packet.available()) {
				UInt16 size = packet.read16();
				packet.read(size, value);
			}
			INFO("FlashStream ", id, " - NetGroup data message type : ", value)
			break;
		}
		case AMF::GROUP_NKNOWN2:
			INFO("FlashStream ", id, " - NetGroup 0E message type")

			// When we receive the 0E NetGroup message type we must send the group message 3
			writer.writeGroupMessage3(_targetID);
			_message3Sent = true;
			break;
		case AMF::GROUP_REPORT:
		{
			INFO("FlashStream ", id, " - NetGroup Report (type 0A)")
			UInt8 size = packet.read8();
			if (size == 1) {
				packet.next();
				size = packet.read8();
			}
			if (size != 8) {
				ERROR("Unexpected 1st parameter size in group message 3")
				break;
			}
			string value, peerId;
			DEBUG("Group message 0A - 1st parameter : ", Util::FormatHex(BIN packet.read(8, value).data(), 8, LOG_BUFFER))
			size = packet.read8();
			DEBUG("Group message 0A - 2nd parameter : ", Util::FormatHex(BIN packet.read(size, value).data(), 8, LOG_BUFFER))
			
			// Loop on each peer of the NetGroup
			while (packet.available() > 4) {
				if (packet.read32() != 0x0022210F) {
					ERROR("Unexpected format for peer infos in the group message 3")
					break;
				}
				packet.read(PEER_ID_SIZE, peerId);
				DEBUG("Group message 0A - Peer ID : ", Util::FormatHex(BIN peerId.data(), PEER_ID_SIZE, LOG_BUFFER))
				DEBUG("Group message 0A - Time elapsed : ", packet.read7BitLongValue())
				size = packet.read8();
				DEBUG("Group message 0A - infos : ", Util::FormatHex(BIN packet.read(size, value).data(), size, LOG_BUFFER))
			}
			if (!_message3Sent) {
				writer.writeGroupMessage3(_targetID);
				_message3Sent = true;
			}
			break;
		}

		case AMF::GROUP_INFOS: // contain the stream name of an eventual publication
		{
			string streamName, data;
			UInt8 sizeName = packet.read8();
			if (sizeName > 1) {
				packet.next(); // 00
				packet.read(sizeName-1, streamName);
				packet.read(packet.available(), data);
				if (OnGroupMedia::raise<false>(streamName, data))
					writer.writeGroupMedia(streamName, data);
			}
			DEBUG("FlashStream ", id, " - Group Media Infos (type 21) : ", streamName)
			break;
		}
		case AMF::GROUP_FRAGMENTS_MAP:
		{
			UInt64 counter = packet.read7BitLongValue();
			DEBUG("FlashStream ", id, " - Group Fragments map (type 22) : ", counter, " ; ", Util::FormatHex(BIN packet.current(), packet.available(), LOG_BUFFER))
			packet.next(packet.available());
			if (!_playing) {
				writer.writeGroupPlay();
				_playing = true;
			}
			break;
		}
		case AMF::GROUP_MEDIA_DATA:
		{
			UInt64 counter = packet.read7BitLongValue();
			AMF::ContentType mediaType = (AMF::ContentType)packet.read8();
			time = packet.read32();

			DEBUG("FlashStream ", id, " - Group ", (mediaType ==AMF::AUDIO?"Audio" : (mediaType ==AMF::VIDEO?"Video":"Unknown"))," media message 20 : counter=", counter, ", time=", time)
			if (mediaType == AMF::AUDIO)
				audioHandler(time, packet, lostRate);
			else if (mediaType == AMF::VIDEO) {
				if (!_videoCodecSent) {
					if (!RTMFP::IsKeyFrame(packet.current(), packet.available())) {
						DEBUG("Video frame dropped to wait first key frame");
						break;
					}
					_videoCodecSent = true;
				}
				videoHandler(time, packet, lostRate);
			}
			else if (mediaType == AMF::INVOCATION || mediaType == AMF::INVOCATION_AMF3)
				return process(mediaType, time, packet, writer, lostRate);
			else
				ERROR("Media type ", Format<UInt8>("%02X", mediaType), " not supported (or data decoding error)")
			break;
		}
		case AMF::GROUP_MEDIA_START: // Start a splitted media sequence
		{
			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			_splittedMediaType = packet.read8();
			time = packet.read32();

			DEBUG("FlashStream ", id, " - Group ", (_splittedMediaType == AMF::AUDIO ? "Audio" : (_splittedMediaType == AMF::VIDEO ? "Video" : "Unknown")), " Start Splitted media : counter=", counter, ", time=", time, ", splitNumber=", splitNumber)
			if (_splittedMediaType == AMF::AUDIO || _splittedMediaType == AMF::VIDEO) {
				_splittedTime = time;
				_splittedLostRate = lostRate;
				_splittedContent.clear();
				Buffer::Append(_splittedContent, packet.current(), packet.available());
			}
			else
				ERROR("Media type ", Format<UInt8>("%02X", _splittedMediaType), " not supported (or data decoding error)")
			break;
		}
		case AMF::GROUP_MEDIA_NEXT: // Continue a splitted media sequence
		{
			if (_splittedMediaType != AMF::AUDIO && _splittedMediaType != AMF::VIDEO)
				break;

			UInt64 counter = packet.read7BitLongValue();
			UInt8 splitNumber = packet.read8(); // counter of the splitted sequence
			DEBUG("FlashStream ", id, " - Group next Splitted media : counter=", counter, ", splitNumber=", splitNumber)
			Buffer::Append(_splittedContent, packet.current(), packet.available());
			break;
		}
		case AMF::EMPTY: // End of a splitted media sequence (TODO: differentiate empty and end of split)
		{
			if (_splittedMediaType != AMF::AUDIO && _splittedMediaType != AMF::VIDEO)
				break;

			UInt64 counter = packet.read7BitLongValue();
			Buffer::Append(_splittedContent, packet.current(), packet.available());
			PacketReader content(_splittedContent.data(), _splittedContent.size());

			DEBUG("FlashStream ", id, " - Group ", (_splittedMediaType == AMF::AUDIO ? "Audio" : (_splittedMediaType == AMF::VIDEO ? "Video" : "Unknown")), " End splitted media : counter=", counter)
			if (_splittedMediaType == AMF::AUDIO)
				audioHandler(_splittedTime, content, lostRate);
			else if (_splittedMediaType == AMF::VIDEO) {
				if (!_videoCodecSent) {
					if (!RTMFP::IsKeyFrame(content.current(), content.available())) {
						DEBUG("Video frame dropped to wait first key frame");
						break;
					}
					_videoCodecSent = true;
				}
				videoHandler(_splittedTime, content, lostRate);
			}
			break;
		}

		default:
			ERROR("Unpacking type '",Format<UInt8>("%02X",(UInt8)type),"' unknown")
	}

	writer.setCallbackHandle(0);
	return writer.state()!=FlashWriter::CLOSED;
}


UInt32 FlashStream::bufferTime(UInt32 ms) {
	_bufferTime = ms;
	INFO("setBufferTime ", ms, "ms on stream ",id)
	/*if (_pListener)
		_pListener->setNumber("bufferTime", ms);*/
	return _bufferTime;
}

void FlashStream::messageHandler(const string& name, AMFReader& message, FlashWriter& writer) {
	/*** Player ***/
	if(name == "onStatus") {
		double callback;
		message.readNumber(callback);
		message.readNull();

		if(message.nextType() != AMFReader::OBJECT) {
			ERROR("Unexpected onStatus value type : ",message.nextType())
			return;
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
				OnStatus::raise(code, description, writer);
			else
				ERROR("Unknown level message type : ", level)
			return;
		}
	}
	else if (name == "closeStream") {
		INFO("Stream ", id, " is closing...")
		// TODO: implement close method
		return;
	}
	/*** Publisher part ***/
	else if (name == "play") {
		//disengage(&writer);

		string publication;
		message.readString(publication);
		
		if (OnPlay::raise<false>(publication, writer)) {
			_streamName = publication;

			writer.writeRaw().write16(0).write32(2000000 + id); // stream begin
			writer.writeAMFStatus("NetStream.Play.Reset", "Playing and resetting " + publication); // for entiere playlist
			writer.writeAMFStatus("NetStream.Play.Start", "Started playing " + publication); // for item
			AMFWriter& amf(writer.writeAMFData("|RtmpSampleAccess"));

			// TODO: determinate if video and audio are available
			amf.writeBoolean(true); // audioSampleAccess
			amf.writeBoolean(true); // videoSampleAccess
		}

		writer.flush();
		return;
	}

	ERROR("Message '",name,"' unknown on stream ",id);
}

void FlashStream::dataHandler(DataReader& data, double lostRate) {
	
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
	}
}

void FlashStream::rawHandler(UInt16 type, PacketReader& packet, FlashWriter& writer) {
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
			break;
	}
}

void FlashStream::audioHandler(UInt32 time,PacketReader& packet, double lostRate) {

	OnMedia::raise(_peerId, _streamName, time, packet, lostRate, true);
}

void FlashStream::videoHandler(UInt32 time,PacketReader& packet, double lostRate) {

	OnMedia::raise(_peerId, _streamName, time, packet, lostRate, false);
}

void FlashStream::memberHandler(const string& peerId) {

	string id;
	INFO("NetGroup Peer ID added : ", Util::FormatHex(BIN peerId.data(), peerId.size(), id))
	OnNewPeer::raise(_groupId, id);
}

void FlashStream::connect(FlashWriter& writer,const string& url) {
	ERROR("Connection request sent from a FlashStream (only main stream can send connect)")
}

void FlashStream::createStream(FlashWriter& writer) {
	ERROR("createStream request can only be sent by Main stream")
}

void FlashStream::play(FlashWriter& writer,const string& name, bool amf3) {
	_streamName = name;
	AMFWriter& amfWriter = writer.writeInvocation("play", amf3);
	//amfWriter.amf0 = true;
	amfWriter.writeString(name.c_str(), name.size());
	writer.flush();
}

void FlashStream::publish(FlashWriter& writer,const string& name) {
	_streamName = name;
	AMFWriter& amfWriter = writer.writeInvocation("publish", true);
	amfWriter.writeString(name.c_str(), name.size());
	writer.flush();
}

void FlashStream::sendPeerInfo(FlashWriter& writer,UInt16 port) {
	ERROR("sendPeerInfo request can only be sent by Main stream")
}

void FlashStream::sendGroupConnect(FlashWriter& writer, const string& groupId) {
	_groupId = groupId; // record the group id before sending answer
	writer.writeGroup(groupId);
	writer.flush();
}

void FlashStream::sendGroupPeerConnect(FlashWriter& writer, const string& netGroup, const UInt8* key, const string& peerId, bool initiator) {
	// Record target peer ID in binary format
	_targetID = peerId;
	Util::UnformatHex(_targetID);

	writer.writePeerGroup(netGroup, key, peerId, initiator);
	writer.flush();
}
