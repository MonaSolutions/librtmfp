
#include "FlashConnection.h"
#include "ParameterWriter.h"
#include "Mona/HostEntry.h"
#include "Mona/DNS.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;

FlashConnection::FlashConnection() : FlashStream(0), _port(0), _creatingStream(0) {
	
}

FlashConnection::~FlashConnection() {
	for (auto& it : _streams) {
		it.second->OnStatus::unsubscribe((OnStatus&)*this);
		it.second->OnMedia::unsubscribe((OnMedia&)*this);
		it.second->OnPlay::unsubscribe((OnPlay&)*this);
	}
}

void FlashConnection::disengage(FlashWriter* pWriter) {
	for (auto& it : _streams)
		it.second->disengage(pWriter);
}


FlashStream* FlashConnection::getStream(UInt16 id,shared_ptr<FlashStream>& pStream) {
	const auto& it = _streams.find(id);
	if (it == _streams.end()) {
		pStream.reset();
		return NULL;
	}
	return (pStream = it->second).get();
}

FlashStream* FlashConnection::addStream(UInt16 id, shared_ptr<FlashStream>& pStream) {
	pStream.reset(new FlashStream(id));
	_streams[id] = pStream;
	pStream->OnStatus::subscribe((OnStatus&)*this);
	pStream->OnMedia::subscribe((OnMedia&)*this);
	pStream->OnPlay::subscribe((OnPlay&)*this);

	return pStream.get();
}

void FlashConnection::messageHandler(const string& name,AMFReader& message,FlashWriter& writer) {
	UInt8 type = message.nextType();
	if(name == "_result") {
		if(type == AMFReader::OBJECT) {
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
		else if (type == AMFReader::NUMBER && _creatingStream) {
			double idStream(0);
			if(!message.readNumber(idStream)) {
				ERROR("Unable to read id stream")
				return;
			}
			_creatingStream = false;
			OnStreamCreated::raise((UInt16)idStream);
			return;
		}
	} else if(name == "_error") {
		if(type == AMFReader::OBJECT) {
			MapParameters params;
			ParameterWriter paramWriter(params);
			message.read(AMFReader::OBJECT, paramWriter);

			string level;
			params.getString("level",level);
			if(level == "error") {
				string code, description;
				params.getString("code",code);
				params.getString("description", description);
				OnStatus::raise(code, description, writer);
				return;
			}
		} 
	}
	ERROR("Unhandled message ", name, " (next type : ", message.nextType(), ")")
}

void FlashConnection::sendPeerInfo(FlashWriter& writer,UInt16 port) {
	AMFWriter& amfWriter = writer.writeInvocation("setPeerInfo");

	Exception ex;
	HostEntry host;
	DNS::ThisHost(ex, host);
	string buf;
	for(auto it : host.addresses()) {
		if (it.family() == IPAddress::IPv4)
			amfWriter.writeString(String::Format(buf, it.toString(), ":", _port).c_str(), buf.size());
	}
}

void FlashConnection::rawHandler(UInt16 type,PacketReader& packet,FlashWriter& writer) {
	
	 // ping message
	/*if(type==0x0006) {
		writer.writePong(packet.read32());
		return;
	}

	 // pong message
	if(type==0x0007) {
		//TODO: peer.pong();
		return;
	}*/

	// setPeerInfo response
	if(type==0x0029) {
		UInt32 keepAliveServer = packet.read32();
		UInt32 keepAliverPeer = packet.read32();
		return;
	}

	ERROR("Raw message ",Format<UInt16>("%.4x",type)," unknown on main stream");
}

void FlashConnection::connect(FlashWriter& writer, const string& url, UInt16 port) {

	AMFWriter& amfWriter = writer.writeInvocation("connect");

	amfWriter.beginObject();
	//TODO: writer.writeStringProperty("app",)
	amfWriter.writeStringProperty("flashVer", EXPAND("WIN 19,0,0,185"));
	//TODO: writer.writeStringProperty("swfUrl")
	amfWriter.writeStringProperty("tcUrl", url);
	//TODO: writer.writeStringProperty("fpad")
	amfWriter.writeNumberProperty("capabilities",235);
	amfWriter.writeNumberProperty("audioCodecs",3575);
	amfWriter.writeNumberProperty("videoCodecs",252);
	amfWriter.writeNumberProperty("videoFunction",1);
	amfWriter.writeNumberProperty("videoCodecs",252);
	//TODO: writer.writeStringProperty("pageUrl")
	amfWriter.writeNumberProperty("objectEncoding",3);
	amfWriter.endObject();

	writer.flush();
}

void FlashConnection::createStream(FlashWriter& writer) {
	AMFWriter& amfWriter = writer.writeInvocation("createStream");
	writer.flush();
	_creatingStream = true;
}

void play(FlashWriter& writer,const std::string& name) {
	ERROR("Forbidden operation : play stream request sent to main stream")
}