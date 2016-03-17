#include "FlashStream.h"
#include "ParameterWriter.h"
#include "Mona/MapParameters.h"
#include "RTMFP.h"

using namespace std;
using namespace Mona;

FlashStream::FlashStream(UInt16 id/*, Invoker& invoker, Peer& peer*/) : id(id), /*invoker(invoker), peer(peer), _pPublication(NULL), _pListener(NULL), */ _bufferTime(0), _message3Sent(false) {
	DEBUG("FlashStream ",id," created")
}

FlashStream::~FlashStream() {
	disengage();
	DEBUG("FlashStream ",id," deleted")
}

void FlashStream::disengage(FlashWriter* pWriter) {
	// Stop the current  job
	/*if(_pPublication) {
		const string& name(_pPublication->name());
		if(pWriter)
			pWriter->writeAMFStatus("NetStream.Unpublish.Success",name + " is now unpublished");
		 // do after writeAMFStatus because can delete the publication, so corrupt name reference
		invoker.unpublish(peer,name);
		_pPublication = NULL;
	}
	if(_pListener) {
		const string& name(_pListener->publication.name());
		if (pWriter) {
			pWriter->writeAMFStatus("NetStream.Play.Stop", "Stopped playing " + name);
			OnStop::raise(id, *pWriter); // stream end
		}
		 // do after writeAMFStatus because can delete the publication, so corrupt publication name reference
		invoker.unsubscribe(peer,name);
		_pListener = NULL;
	}*/
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
			INFO("NetGroup data message type - ", value)
			break;
		}
		case AMF::GROUP_NKNOWN2:
			INFO("NetGroup 0E message type")

			// When we receive the 0E NetGroup message type we must send the group message 3
			writer.writeGroupMessage3(_targetID);
			_message3Sent = 3;
			break;
		case AMF::GROUP_NKNOWN3:
		{
			INFO("NetGroup 0A message type")
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

		case AMF::GROUP_MEDIA:
		{
			UInt8 sizeName = packet.read8();
			if (sizeName > 1) {
				string streamName, data;
				packet.next(); // 00
				packet.read(sizeName-1, streamName);
				packet.read(packet.available(), data);
				if (OnGroupMedia::raise<false>(streamName, data))
					writer.writeGroupMedia(streamName, data);
			}
			break;
		}
		case AMF::GROUP_NKNOWN4:
		{
			DEBUG("Group message 22 : ", Util::FormatHex(BIN packet.current(), packet.available(), LOG_BUFFER))
			packet.next(packet.available());
			break;
		}

		default:
			ERROR("Unpacking type '",Format<UInt8>("%02x",(UInt8)type),"' unknown")
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

	/*** Publisher part ***/
	if (name == "play") {
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
