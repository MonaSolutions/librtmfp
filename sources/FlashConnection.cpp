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

#include "FlashConnection.h"
#include "GroupStream.h"
#include "ParameterWriter.h"
#include "Mona/IPAddress.h"
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
		it.second->OnFragmentsMap::unsubscribe((OnFragmentsMap&)*this);
		it.second->OnGroupBegin::unsubscribe((OnGroupBegin&)*this);
		it.second->OnFragment::unsubscribe((OnFragment&)*this);
		it.second->OnGroupAskClose::unsubscribe((OnGroupAskClose&)*this);
	}
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

//TODO: make a template function instead of boolean
FlashStream* FlashConnection::addStream(UInt16 id, shared_ptr<FlashStream>& pStream, bool group) {

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
	pStream->OnFragmentsMap::subscribe((OnFragmentsMap&)*this);
	pStream->OnGroupBegin::subscribe((OnGroupBegin&)*this);
	pStream->OnFragment::subscribe((OnFragment&)*this);
	pStream->OnGroupAskClose::subscribe((OnGroupAskClose&)*this);

	return pStream.get();
}

bool FlashConnection::messageHandler(const string& name, AMFReader& message, UInt64 flowId, Mona::UInt64 writerId, double callbackHandler) {
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

					return OnStatus::raise<true>(code, description, id, flowId, callbackHandler);
				} // TODO: else
			}
			else if (type == AMFReader::NUMBER && _creatingStream) {
				double idStream(0);
				if (!message.readNumber(idStream)) {
					ERROR("Unable to read id stream")
					return false;
				}
				_creatingStream = false;
				shared_ptr<FlashStream> pStream;
				addStream((UInt16)idStream, pStream);
				return OnStreamCreated::raise<true>((UInt16)idStream);
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
					return OnStatus::raise<true>(code, description, id, flowId, callbackHandler);
				}
			}
		}
		else {
			message.next();
			ERROR("Unhandled message ", name, " (type : ", type, ")")
			return false;
		}

		type = message.nextType();
	}
	return true;
}

bool FlashConnection::rawHandler(UInt16 type,PacketReader& packet) {

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
	return true;
}

void FlashConnection::createStream() {
	_creatingStream = true;
}
