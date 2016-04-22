
#include "FlashConnection.h"
#include "GroupStream.h"
#include "ParameterWriter.h"
#include "Mona/HostEntry.h"
#include "Mona/DNS.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;

FlashConnection::FlashConnection() : FlashStream(0), _creatingStream(0) {
	
}

FlashConnection::~FlashConnection() {
	for (auto& it : _streams) {
		it.second->OnStatus::unsubscribe((OnStatus&)*this);
		it.second->OnMedia::unsubscribe((OnMedia&)*this);
		it.second->OnPlay::unsubscribe((OnPlay&)*this);
		it.second->OnNewPeer::unsubscribe((OnNewPeer&)*this);
		it.second->OnGroupHandshake::unsubscribe((OnGroupHandshake&)*this);
		it.second->OnGroupMedia::unsubscribe((OnGroupMedia&)*this);
		it.second->OnGroupReport::unsubscribe((OnGroupReport&)*this);
		it.second->OnGroupPlayPush::unsubscribe((OnGroupPlayPush&)*this);
		it.second->OnGroupPlayPull::unsubscribe((OnGroupPlayPull&)*this);
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

FlashStream* FlashConnection::addStream(shared_ptr<FlashStream>& pStream, bool group) {
	return addStream((UInt16)_streams.size(), pStream, group);
}

FlashStream* FlashConnection::addStream(UInt16 id, shared_ptr<FlashStream>& pStream, bool group) {
	// TODO: check existence of an older stream with the id
	pStream.reset(group? new GroupStream(id) : new FlashStream(id));
	_streams[id] = pStream;
	pStream->OnStatus::subscribe((OnStatus&)*this);
	pStream->OnMedia::subscribe((OnMedia&)*this);
	pStream->OnPlay::subscribe((OnPlay&)*this);
	pStream->OnNewPeer::subscribe((OnNewPeer&)*this);
	pStream->OnGroupHandshake::subscribe((OnGroupHandshake&)*this);
	pStream->OnGroupMedia::subscribe((OnGroupMedia&)*this);
	pStream->OnGroupReport::subscribe((OnGroupReport&)*this);
	pStream->OnGroupPlayPush::subscribe((OnGroupPlayPush&)*this);
	pStream->OnGroupPlayPull::subscribe((OnGroupPlayPull&)*this);

	return pStream.get();
}

void FlashConnection::messageHandler(const string& name,AMFReader& message,FlashWriter& writer) {
	UInt8 type = message.nextType();
	while (type != AMFReader::END) {
		if (name == "_result") {
			if (type == AMFReader::OBJECT || type == AMFReader::MAP) {
				MapParameters params;
				ParameterWriter paramWriter(params);
				message.read(type, paramWriter);

				string level;
				params.getString("level", level);
				if (level == "status") {
					string code, description;
					params.getString("code", code);
					params.getString("description", description);

					OnStatus::raise(code, description, writer);
				} // TODO: else
			}
			else if (type == AMFReader::NUMBER && _creatingStream) {
				double idStream(0);
				if (!message.readNumber(idStream)) {
					ERROR("Unable to read id stream")
					return;
				}
				_creatingStream = false;
				OnStreamCreated::raise((UInt16)idStream);
			}
		}
		else if (name == "_error") {
			if (type == AMFReader::OBJECT) {
				MapParameters params;
				ParameterWriter paramWriter(params);
				message.read(AMFReader::OBJECT, paramWriter);

				string level;
				params.getString("level", level);
				if (level == "error") {
					string code, description;
					params.getString("code", code);
					params.getString("description", description);
					OnStatus::raise(code, description, writer);
				}
			}
		}
		else {
			message.next();
			ERROR("Unhandled message ", name, " (type : ", type, ")")
		}

		type = message.nextType();
	}
}

void FlashConnection::sendPeerInfo(FlashWriter& writer,UInt16 port) {
	AMFWriter& amfWriter = writer.writeInvocation("setPeerInfo");

	Exception ex;
	HostEntry host;
	DNS::ThisHost(ex, host);
	string buf;
	for(auto it : host.addresses()) {
		if(it.family() == IPAddress::IPv4) {
			String::Format(buf, it.toString(), ":", port);
			amfWriter.writeString(buf.c_str(), buf.size());
		}
	}
}

void FlashConnection::rawHandler(UInt16 type,PacketReader& packet,FlashWriter& writer) {

	switch (type) {
		case 0x0022: // TODO Here we receive RTMFP flow sync signal, useless to support it?
			INFO("Sync ", id, " : (syncId=", packet.read32(), ", count=", packet.read32(), ")")
			break;
		case 0x0029:
			INFO("Set Keepalive timer : server period=", packet.read32(), "ms - peer period=", packet.read32(), "ms")
			break;
		default:
			ERROR("Raw message ", Format<UInt16>("%.4x", type), " unknown on main stream ", id);
			break;
	}
}

void FlashConnection::connect(FlashWriter& writer, const string& url) {

	AMFWriter& amfWriter = writer.writeInvocation("connect");

	// TODO: add parameters to configuration
	bool amf = amfWriter.amf0;
	amfWriter.amf0 = true;
	amfWriter.beginObject();
	amfWriter.writeStringProperty("app", "live");
	amfWriter.writeStringProperty("flashVer", EXPAND("WIN 20,0,0,286"));
	amfWriter.writeStringProperty("swfUrl", "");
	amfWriter.writeStringProperty("tcUrl", url);
	amfWriter.writeBooleanProperty("fpad", false);
	amfWriter.writeNumberProperty("capabilities",235);
	amfWriter.writeNumberProperty("audioCodecs",3575);
	amfWriter.writeNumberProperty("videoCodecs",252);
	amfWriter.writeNumberProperty("videoFunction",1);
	amfWriter.writeStringProperty("pageUrl", "");
	amfWriter.writeNumberProperty("objectEncoding",3);
	amfWriter.endObject();
	amfWriter.amf0 = amf;

	writer.flush();
}

void FlashConnection::createStream(FlashWriter& writer) {
	AMFWriter& amfWriter = writer.writeInvocation("createStream");
	writer.flush();
	_creatingStream = true;
}
