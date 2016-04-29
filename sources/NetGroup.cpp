#include "NetGroup.h"
#include "P2PConnection.h"
#include "GroupStream.h"

using namespace Mona;
using namespace std;

NetGroup::MediaPacket::MediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 time, AMF::ContentType type,
	UInt64 fragmentId, UInt8 marker, UInt8 splitId) : fragmentSize(0), time(time), pBuffer(poolBuffers, NETGROUP_MAX_PACKET_SIZE) {
	BinaryWriter writer(pBuffer->data(), size + 6 + (splitId > 1) + Util::Get7BitValueSize(fragmentId));

	// AMF Group marker
	writer.write8(marker);
	// Fragment Id
	writer.write7BitLongValue(fragmentId);
	// Splitted sequence number
	if (splitId > 1)
		writer.write8(splitId);
	// Media type
	writer.write8(type);
	// Time on 4 bytes
	writer.write32(time);
	// Payload
	writer.write(data, size);
	
	fragmentSize = writer.size();
}

NetGroup::NetGroup(const string& groupId, const string& groupTxt, const string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, UInt16 windowDuration) :
	idHex(groupId), idTxt(groupTxt), stream(streamName), isPublisher(publisher), _conn(conn), _updatePeriod(updatePeriod*1000), _fragmentCounter(0),
	_lastSent(0), _pListener(NULL), _windowDuration(windowDuration*1000), _streamCode(0x22) { // TODO: change the 1024
	onMedia = [this](bool reliable, AMF::ContentType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		const UInt8* pos = data;
		const UInt8* end = data + size;
		UInt8 splitCounter = size / NETGROUP_MAX_PACKET_SIZE + ((size % NETGROUP_MAX_PACKET_SIZE) > 1);
		UInt8 marker = GroupStream::GROUP_MEDIA_DATA ;
		//DEBUG("Creating fragments ", _fragmentCounter + 1, " to ", _fragmentCounter + splitCounter)
		while (splitCounter > 0) {
			if (size > NETGROUP_MAX_PACKET_SIZE)
				marker = splitCounter == 1 ? GroupStream::GROUP_MEDIA_END : (pos == data ? GroupStream::GROUP_MEDIA_START : GroupStream::GROUP_MEDIA_NEXT);

			auto itFragment = _fragments.emplace(piecewise_construct, forward_as_tuple(++_fragmentCounter), forward_as_tuple(_conn.poolBuffers(), pos, splitCounter > 1 ? NETGROUP_MAX_PACKET_SIZE : (end - pos), time, type,
				_fragmentCounter, marker, splitCounter)).first;
			_mapTime2Fragment[time] = _fragmentCounter;

			// Send fragment to peers (push mode)
			/*for (auto it : _mapPeers)
				it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.fragmentSize, _fragmentCounter);*/

			pos += splitCounter > 1 ? NETGROUP_MAX_PACKET_SIZE : (end - pos);
			splitCounter--;
		}
		
	};
	onGroupMedia = [this](const string& peerId, const string& streamName, const string& data, FlashWriter& writer) {
		if (isPublisher) {
			/*// First Viewer => create listener
			if (!_pListener) {
				Exception ex;
				if (!(_pListener = _conn.startListening<GroupListener>(ex, streamName, idTxt))) {
					WARN(ex.error()) // TODO : See if we can send a specific answer
					return;
				}
				INFO("Stream ", streamName, " found, sending start answer")
				// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
				_conn.setP2pPublisherReady();
				_pListener->OnMedia::subscribe(onMedia);
			}*/
			if (streamName != stream)
				ERROR("Stream ", streamName, " not found, ignoring the peer request")
		}
		else if (String::ICompare(streamName, stream) == 0) {
			NOTE("Starting to listen to publication ", streamName)
			writer.writeGroupMedia(streamName, BIN data.data(), data.size());
		}
		else {
			INFO("New stream available in the group but not registered : ", streamName)
			return;
		}
	};
	onGroupReport = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		string key1, key2, keyPeer, tmpId;
		DEBUG("Group message 0A - 1st parameter : ", Util::FormatHex(BIN packet.read(8, key1).data(), 8, LOG_BUFFER))
		UInt8 size = packet.read8();
		DEBUG("Group message 0A - 2nd parameter : ", Util::FormatHex(BIN packet.read(size, key2).data(), 8, LOG_BUFFER))

		// Loop on each peer of the NetGroup
		while (packet.available() > 4) {
			if (packet.read32() != 0x0022210F) {
				ERROR("Unexpected format for peer infos in the group message 0A")
				break;
			}
			packet.read(PEER_ID_SIZE, tmpId);
			DEBUG("Group message 0A - Peer ID : ", Util::FormatHex(BIN tmpId.data(), PEER_ID_SIZE, LOG_BUFFER))
			DEBUG("Group message 0A - Time elapsed : ", packet.read7BitLongValue())
			size = packet.read8();
			DEBUG("Group message 0A - infos : ", Util::FormatHex(BIN packet.read(size, keyPeer).data(), size, LOG_BUFFER))
		}

		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end())
			ERROR("Unable to find the peer ", peerId)
		else {
			INFO("Is publisher : ", isPublisher, " ; infos sent : ", it->second->publicationInfosSent)
			if (!isPublisher)
				it->second->sendGroupBegin();
			// Send the publication infos if not already sent
			else if (!it->second->publicationInfosSent)
				it->second->sendGroupMedia(stream, _streamCode.data(), _streamCode.size());
		}
		// Answer with our group report if we are a player
		/*if (!isPublisher)
			writer.writeGroupReport(peerId);*/
	};
	onGroupPlayPush = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end())
			ERROR("Unable to find the peer ", peerId)
		else {
			it->second->setPushMode(packet.read8());

			// First Viewer = > create listener
			if (!_pListener) {
				Exception ex;
				if (!(_pListener = _conn.startListening<GroupListener>(ex, stream, idTxt))) {
					WARN(ex.error()) // TODO : See if we can send a specific answer
					return;
				}
				INFO("First push request, starting to play Stream ", stream)
				// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
				_pListener->OnMedia::subscribe(onMedia);
				_conn.setP2pPublisherReady();
			}
		}
	};
	onGroupPlayPull = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		DEBUG("Group Pull message (2B) recevied")
		auto itFragment = _fragments.find(packet.read7BitLongValue());
		auto it = _mapPeers.find(peerId);

		// Send fragment to peer (pull mode)
		if (itFragment != _fragments.end() && it != _mapPeers.end())
			it->second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.fragmentSize, itFragment->first, true);
	};
	onFragmentsMap = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		UInt64 counter = packet.read7BitLongValue();
		DEBUG("Group Fragments map (type 22) received : ", counter, " ; ", Util::FormatHex(BIN packet.current(), packet.available(), LOG_BUFFER))
		packet.next(packet.available());

		// Player? => update play mode if needed
		if (!_pListener) {
			auto it = _mapPeers.find(peerId);
			if (it == _mapPeers.end())
				ERROR("Unable to find the peer ", peerId)
			else
				it->second->updatePlayMode(0xFF);
		}
	};
	onGroupBegin = [this](const string& peerId, FlashWriter& writer) {
		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end())
			ERROR("Unable to find the peer ", peerId)
		else {
			// When we receive the 0E NetGroup message type we must send the group report
			INFO("Sending the group report for ", peerId)
			it->second->sendGroupReport(peerId);
			it->second->sendGroupBegin();
		}
	};

	BinaryWriter writer(_streamCode.data(), _streamCode.size());
	writer.write16(0x2101);
	Util::Random((UInt8*)_streamCode.data()+2, 0x20); // random serie of 32 bytes
}

NetGroup::~NetGroup() {
	DEBUG("Deletion of NetGroup ", idTxt)

	if (_pListener) {
		_pListener->OnMedia::unsubscribe(onMedia);
		_conn.stopListening(idTxt);
		_pListener = NULL;
	}

	for (auto it : _mapPeers) {
		it.second->OnGroupMedia::unsubscribe(onGroupMedia);
		it.second->OnGroupReport::unsubscribe(onGroupReport);
		it.second->OnGroupPlayPush::unsubscribe(onGroupPlayPush);
		it.second->OnGroupPlayPull::unsubscribe(onGroupPlayPull);
		it.second->OnFragmentsMap::unsubscribe(onFragmentsMap);
		it.second->OnGroupBegin::unsubscribe(onGroupBegin);
		it.second->resetGroup();
	}
	_mapPeers.clear();

	{ // TODO: delete fragments properly
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		_fragments.clear();
	}
}

void NetGroup::addPeer(string peerId, shared_ptr<P2PConnection> pPeer) {
	_mapPeers.emplace(peerId, pPeer);
	pPeer->OnGroupMedia::subscribe(onGroupMedia);
	pPeer->OnGroupReport::subscribe(onGroupReport);
	pPeer->OnGroupPlayPush::subscribe(onGroupPlayPush);
	pPeer->OnGroupPlayPull::subscribe(onGroupPlayPull);
	pPeer->OnFragmentsMap::subscribe(onFragmentsMap);
	pPeer->OnGroupBegin::subscribe(onGroupBegin);
}

void NetGroup::manage() {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	// Send the last fragments (TODO: see if we can send them directly)
	auto itFragment = (_lastSent==0)? _fragments.begin() : _fragments.find(_lastSent);
	if (_lastSent != 0)
		itFragment++;
	for (itFragment; itFragment != _fragments.end(); itFragment++) {
		for (auto it : _mapPeers) {
			//DEBUG("Sending fragment ", itFragment->first, " to one peer")
			it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.fragmentSize, itFragment->first);
		}
		_lastSent = itFragment->first;
	}

	// Send the report message
	if (_lastReport.isElapsed((Int64)_updatePeriod) && updateFragmentMap()) {

		// Send to all neighbors
		for (auto it : _mapPeers) {
			INFO("Sending Report message (type 22) - counter : ", _fragmentCounter)
			it.second->sendFragmentsMap(_reportBuffer.data(), _reportBuffer.size());
		}
		_lastReport.update();
	}
}

bool NetGroup::updateFragmentMap() {
	if (_fragments.empty())
		return false;

	// Erase old fragments
	auto it = _fragments.find(_fragmentCounter);
	if (it != _fragments.end()) {
		UInt32 end = it->second.time;
		auto itTime = _mapTime2Fragment.lower_bound(end - _windowDuration);
		if (itTime != _mapTime2Fragment.end()) {
			_fragments.erase(_fragments.begin(), _fragments.find(itTime->second));
			DEBUG("Deletion of fragments ", _mapTime2Fragment.begin()->second, " (", _mapTime2Fragment.begin()->first, ") to ", itTime->second, " (", itTime->first,')')
			_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);
		}
	}

	// Generate the report message
	if (_pListener) { // Publisher : We have all fragments
		UInt64 firstFragment = _fragments.begin()->first;
		UInt64 nbFragments = _fragmentCounter - firstFragment;

		_reportBuffer.resize((UInt32)((nbFragments / 8) + 1) + Util::Get7BitValueSize(_fragmentCounter) + 1, false);
		BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
		writer.write8(GroupStream::GROUP_FRAGMENTS_MAP).write7BitLongValue(_fragmentCounter);
		while (nbFragments > 8) {
			writer.write8(0xFF);
			nbFragments -= 8;
		}
		UInt8 lastByte = 1;
		while (nbFragments > 1) {
			lastByte = (lastByte << 1) + 1;
			--nbFragments;
		}
		writer.write8(lastByte);
	}
	else {
		//TODO
	}

	return true;
}
