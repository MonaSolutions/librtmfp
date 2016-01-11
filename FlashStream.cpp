
#include "FlashStream.h"
#include "Mona/Logs.h"

#include "ParameterWriter.h"
#include "Mona/MapParameters.h"

using namespace std;
using namespace Mona;

FlashStream::FlashStream(UInt16 id/*, Invoker& invoker, Peer& peer*/) : id(id), /*invoker(invoker), peer(peer), _pPublication(NULL), _pListener(NULL), */ _bufferTime(0) {
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

		default:
			ERROR("Unpacking type '",Format<UInt8>("%02x",(UInt8)type),"' unknown");
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
		if(level == "status") {
			string code, description;
			params.getString("code",code);
			params.getString("description", description);
			OnStatus::raise(code, description, writer);
			return;
		}
	}

	/*** Publisher part ***/
	if (name == "play") {
		//disengage(&writer);

		string publication;
		message.readString(publication);
		
		if (OnPlay::raise<false>(publication, writer)) {
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
	if (type==0x0000) { // Stream Begin
		UInt32 idReceived = packet.readNumber<UInt32>();
		INFO("Stream begin message on NetStream ",id," (value : ",idReceived,")")
			return;
	}
	if (type==0x0001) { // Stream Stop
		UInt32 idReceived = packet.readNumber<UInt32>();
		INFO("Stream stop message on NetStream ",id," (value : ",idReceived,")")
		return;
	}
	if(type==0x0022) { // TODO Here we receive RTMFP flow sync signal, useless to support it?
		INFO("Sync ",id," : ",packet.read32(),"/",packet.read32())
		return;
	}
	ERROR("Raw message ",Format<UInt16>("%.4x",type)," unknown on stream ",id);
}

void FlashStream::audioHandler(UInt32 time,PacketReader& packet, double lostRate) {
	/*if(!_pPublication) {
		WARN("an audio packet has been received on a no publishing stream ",id,", certainly a publication currently closing");
		return;
	}
	_pPublication->pushAudio(time,packet,peer.ping(),lostRate);*/
	OnMedia::raise(time, packet, lostRate, true);
}

void FlashStream::videoHandler(UInt32 time,PacketReader& packet, double lostRate) {
	/*if(!_pPublication) {
		WARN("a video packet has been received on a no publishing stream ",id,", certainly a publication currently closing");
		return;
	}
	_pPublication->pushVideo(time,packet,peer.ping(),lostRate);*/
	OnMedia::raise(time, packet, lostRate, false);
}

void FlashStream::connect(FlashWriter& writer,const string& url) {
	ERROR("Connection request sent from a FlashStream (only main stream can send connect)")
}

void FlashStream::createStream(FlashWriter& writer) {
	ERROR("createStream request can only be sent by Main stream")
}

void FlashStream::play(FlashWriter& writer,const string& name, bool amf3) {
	AMFWriter& amfWriter = writer.writeInvocation("play", amf3);
	amfWriter.amf0 = true;
	amfWriter.writeString(name.c_str(), name.size());
	writer.flush();
}

void FlashStream::publish(FlashWriter& writer,const string& name) {
	AMFWriter& amfWriter = writer.writeInvocation("publish");
	amfWriter.writeString(name.c_str(), name.size());
	writer.flush();
}

void FlashStream::sendPeerInfo(FlashWriter& writer,UInt16 port) {
	ERROR("sendPeerInfo request can only be sent by Main stream")
}