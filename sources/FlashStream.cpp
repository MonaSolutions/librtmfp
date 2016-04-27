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
	disengage();
	DEBUG("FlashStream ",id," deleted")
}

void FlashStream::disengage(FlashWriter* pWriter) {

}


bool FlashStream::process(PacketReader& packet,FlashWriter& writer, double lostRate) {

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

void FlashStream::connect(FlashWriter& writer,const string& url) {
	ERROR("Connection request sent from a FlashStream (only main stream can send connect)")
}

void FlashStream::createStream(FlashWriter& writer) {
	ERROR("createStream request can only be sent by Main stream")
}

void FlashStream::play(FlashWriter& writer,const string& name) {
	_streamName = name;
	AMFWriter& amfWriter = writer.writeInvocation("play", true);
	amfWriter.amf0 = true; // Important for p2p unicast play
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

void FlashStream::sendGroupPeerConnect(FlashWriter& writer, const string& netGroup, const UInt8* key, const string& peerId/*, bool initiator*/) {
	// Record target peer ID in binary format
	_targetID = peerId;
	Util::UnformatHex(_targetID);

	writer.writePeerGroup(netGroup, key, peerId/*, initiator*/);
	writer.flush();
}

void FlashStream::sendGroupBegin(FlashWriter& writer) {
	writer.writeGroupBegin();
	writer.flush();
}

void FlashStream::sendGroupMediaInfos(FlashWriter& writer, const string& stream, const UInt8* data, UInt32 size) {
	writer.writeGroupMedia(stream, data, size);
	writer.flush();
}

void FlashStream::sendGroupReport(FlashWriter& writer, const std::string& peerId) {
	writer.writeGroupReport(peerId);
	writer.flush();
}

void FlashStream::sendRaw(FlashWriter& writer, const UInt8* data, UInt32 size) {
	writer.writeRaw(data, size);
	writer.flush();
}

void FlashStream::sendGroupPlay(FlashWriter& writer, UInt8 mode) {
	writer.writeGroupPlay(mode);
}
