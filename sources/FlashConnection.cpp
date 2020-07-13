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

#include "Base/IPAddress.h"
#include "FlashConnection.h"
#include "GroupStream.h"
#include "MapWriter.h"
#include "Base/Parameters.h"

using namespace std;
using namespace Base;

FlashConnection::FlashConnection() : FlashStream(0), _creatingStream(0) {
	
}

FlashConnection::~FlashConnection() {
	for (auto& it : _streams) {
		it.second->onStatus = nullptr;
		it.second->onMedia = nullptr;
		it.second->onPlay = nullptr;
		it.second->onNewPeer = nullptr;
		it.second->onGroupHandshake = nullptr;
		it.second->onGroupMedia = nullptr;
		it.second->onGroupReport = nullptr;
		it.second->onGroupPlayPush = nullptr;
		it.second->onGroupPlayPull = nullptr;
		it.second->onFragmentsMap = nullptr;
		it.second->onGroupBegin = nullptr;
		it.second->onFragment = nullptr;
		it.second->onGroupAskClose = nullptr;
		it.second->onGroupPost = nullptr;
		it.second->onGroupPostKey = nullptr;
	}
}

FlashStream* FlashConnection::getStream(UInt16 id,shared<FlashStream>& pStream) {
	const auto& it = _streams.find(id);
	if (it == _streams.end()) {
		pStream.reset();
		return NULL;
	}
	return (pStream = it->second).get();
}

template <>
void FlashConnection::addStream<FlashStream>(UInt16 id, shared<FlashStream>& pStream) {

	pStream.set(id);
	_streams[id] = pStream;
	pStream->onStatus = onStatus;
	pStream->onMedia = onMedia;
	pStream->onPlay = onPlay;

}

template <>
void FlashConnection::addStream<GroupStream>(UInt16 id, shared<FlashStream>& pStream) {

	pStream.set<GroupStream>(id);
	_streams[id] = pStream;
	pStream->onStatus = onStatus;
	pStream->onMedia = onMedia;
	pStream->onPlay = onPlay;
	pStream->onNewPeer = onNewPeer;
	pStream->onGroupHandshake = onGroupHandshake;
	pStream->onGroupMedia = onGroupMedia;
	pStream->onGroupReport = onGroupReport;
	pStream->onGroupPlayPush = onGroupPlayPush;
	pStream->onGroupPlayPull = onGroupPlayPull;
	pStream->onFragmentsMap = onFragmentsMap;
	pStream->onGroupBegin = onGroupBegin;
	pStream->onFragment = onFragment;
	pStream->onGroupAskClose = onGroupAskClose;
	
}

template <>
void FlashConnection::addStream<GroupPostStream>(UInt16 id, shared<FlashStream>& pStream) {
	pStream.set<GroupPostStream>(id);
	_streams[id] = pStream;
	pStream->onGroupPost = onGroupPost;
	pStream->onGroupPostKey = onGroupPostKey;
}

bool FlashConnection::messageHandler(const string& name, AMFReader& message, UInt64 flowId, Base::UInt64 writerId, double callbackHandler) {
	UInt8 type(AMFReader::END);
	bool result = true;
	while ((type = message.nextType()) != AMFReader::END) {
		if (name == "_result") {
			if (type == AMFReader::OBJECT || type == AMFReader::MAP) {
				Parameters params;
				MapWriter<Parameters> paramWriter(params);
				message.read(type, paramWriter);

				string level;
				params.getString("level", level);
				if (level == "status") {
					string code, description;
					params.getString("code", code);
					params.getString("description", description);

					result |= onStatus(code, description, streamId, flowId, callbackHandler);
				} // TODO: else
				continue;
			}
			else if (type == AMFReader::NUMBER && _creatingStream) {
				double idStream(0);
				UInt16 idMedia(0);
				if (!message.readNumber(idStream)) {
					ERROR("Unable to read id stream")
					return false;
				}
				_creatingStream = false;
				shared<FlashStream> pStream;
				addStream<FlashStream>((UInt16)idStream, pStream);
				if (onStreamCreated((UInt16)idStream, idMedia)) {
					pStream->setIdMedia(idMedia); // set the media Id to retrieve the player/publisher in onMedia
					continue;
				} else
					return false;
			}
		}
		else if (name == "_error" && (type == AMFReader::OBJECT)) {
			Parameters params;
			MapWriter<Parameters> paramWriter(params);
			message.read(AMFReader::OBJECT, paramWriter);

			string level;
			params.getString("level", level);
			if (level == "error") {
				string code, description;
				params.getString("code", code);
				params.getString("description", description);
				result |= onStatus(code, description, streamId, flowId, callbackHandler);
			}
			continue;
		}

		message.next();
		WARN("Unhandled message ", name, " (type : ", type, ")")
		return false;
	}
	return result;
}

bool FlashConnection::rawHandler(UInt16 type, const Packet& packet) {
	BinaryReader reader(packet.data(), packet.size());
	switch (type) {
		case 0x0022: // TODO Here we receive RTMFP flow sync signal, useless to support it?
			INFO("Sync ", streamId, " : (syncId=", reader.read32(), ", count=", reader.read32(), ")")
			break;
		case 0x0029:
			INFO("Set Keepalive timer : server period=", reader.read32(), "ms - peer period=", reader.read32(), "ms")
			break;
		default:
			ERROR("Raw message ", String::Format<UInt16>("%.4x", type), " unknown on main stream ", streamId);
			break;
	}
	return true;
}

void FlashConnection::createStream() {
	_creatingStream = true;
}
