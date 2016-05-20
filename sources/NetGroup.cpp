#include "NetGroup.h"
#include "P2PConnection.h"
#include "GroupStream.h"

using namespace Mona;
using namespace std;

NetGroup::MediaPacket::MediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 totalSize, Mona::UInt32 time, AMF::ContentType mediaType,
	UInt64 fragmentId, UInt8 groupMarker, UInt8 splitId) : splittedId(splitId), type(mediaType), marker(groupMarker), /*fragmentSize(0),*/ time(time), pBuffer(poolBuffers, totalSize) {
	BinaryWriter writer(pBuffer->data(), totalSize);

	// AMF Group marker
	writer.write8(marker);
	// Fragment Id
	writer.write7BitLongValue(fragmentId);
	// Splitted sequence number
	if (splitId > 1)
		writer.write8(splitId);

	// Type and time, only for the first fragment
	if (marker != GroupStream::GROUP_MEDIA_NEXT && marker != GroupStream::GROUP_MEDIA_END) {
		// Media type
		writer.write8(type);
		// Time on 4 bytes
		writer.write32(time);
	}
	// Payload
	payload = writer.data() + writer.size(); // TODO: check if it is the correct pos
	writer.write(data, size);
}

NetGroup::NetGroup(const string& groupId, const string& groupTxt, const string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, UInt16 windowDuration) :
	idHex(groupId), idTxt(groupTxt), stream(streamName), isPublisher(publisher), _conn(conn), _updatePeriod(updatePeriod*1000), _fragmentCounter(0),
	_lastSent(0), _pListener(NULL), _windowDuration(windowDuration*1000), _streamCode(0x22) {
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

			// Add the fragment to the map
			UInt32 fragmentSize = ((splitCounter > 1) ? NETGROUP_MAX_PACKET_SIZE : (end - pos));
			UInt32 bufferSize = fragmentSize + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitCounter > 1) + Util::Get7BitValueSize(_fragmentCounter);
			auto itFragment = _fragments.emplace(piecewise_construct, forward_as_tuple(++_fragmentCounter), forward_as_tuple(_conn.poolBuffers(), pos, fragmentSize, bufferSize, time, type,
				_fragmentCounter, marker, splitCounter)).first;
			_mapTime2Fragment[time] = _fragmentCounter;

			// Send fragment to peers (push mode)
			for (auto it : _mapPeers)
				it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), _fragmentCounter);

			pos += splitCounter > 1 ? NETGROUP_MAX_PACKET_SIZE : (end - pos);
			splitCounter--;
		}
		
	};
	onFragment = [this](const string& peerId, UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);

		// Add the fragment to the map
		UInt32 bufferSize = packet.available() + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitedNumber > 1) + Util::Get7BitValueSize(id);
		auto itFragment = _fragments.emplace(piecewise_construct, forward_as_tuple(id), forward_as_tuple(_conn.poolBuffers(), packet.current(), packet.available(), bufferSize, time, (AMF::ContentType)mediaType,
			id, marker, splitedNumber)).first;
		_mapTime2Fragment[time] = id;

		// If it is an ordered fragment
		pushFragment(itFragment);
	};
	onGroupMedia = [this](const string& peerId, const string& streamName, const string& data, FlashWriter& writer) {
		if (streamName == stream) {
			if (isPublisher) {
				BinaryReader reader(BIN data.data(), data.size());
				string farCode;
				reader.read<string>(0x22, farCode);

				// Another stream code => we must accept and send our stream code
				if (memcmp(_streamCode.data(), farCode.data(), 0x22) != 0) {
					writer.writeGroupMedia(streamName, BIN data.data(), 0x22);
					auto it = _mapPeers.find(peerId);
					if (it == _mapPeers.end())
						ERROR("Unable to find the peer ", peerId)
					else if (!it->second->publicationInfosSent)
						it->second->sendGroupMedia(stream, _streamCode.data(), _streamCode.size());
				}
			}
			else {
				NOTE("Starting to listen to publication ", streamName)
				writer.writeGroupMedia(streamName, BIN data.data(), 0x22);
			}
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
			it->second->lastGroupReport = Time::Now();
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
		lock_guard<recursive_mutex> lock(_fragmentMutex);
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
				INFO("First viewer play request, starting to play Stream ", stream)
				// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
				_pListener->OnMedia::subscribe(onMedia);
				_conn.publishReady = true;
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
			it->second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), itFragment->first, true);
	};
	onFragmentsMap = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		UInt64 counter = packet.read7BitLongValue();
		DEBUG("Group Fragments map (type 22) received : ", counter, " ; ", Util::FormatHex(BIN packet.current(), packet.available(), LOG_BUFFER))
		packet.next(packet.available());

		// Player? => update play mode if needed
		if (!isPublisher) {
			auto it = _mapPeers.find(peerId);
			if (it == _mapPeers.end())
				ERROR("Unable to find the peer ", peerId)
			else
				it->second->sendPushMode(0xFF);
		}
	};
	onGroupBegin = [this](const string& peerId, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex); // TODO: not sure it is needed
		
		if (buildGroupReport(peerId)) {
			// When we receive the 0E NetGroup message type we must send the group report
			INFO("Sending the group report to ", peerId)
			auto it = _mapPeers.find(peerId);
			it->second->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
		}
	};

	BinaryWriter writer(_streamCode.data(), _streamCode.size());
	writer.write16(0x2101);
	Util::Random((UInt8*)_streamCode.data()+2, 0x20); // random serie of 32 bytes
}

NetGroup::~NetGroup() {
	
}

void NetGroup::close() {
	DEBUG("Closing the NetGroup ", idTxt)

	{ // TODO: delete fragments properly
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		_fragments.clear();
	}
	/*auto it = _mapTime2Fragment.rbegin();
	UInt32 lastTime = (it != _mapTime2Fragment.rend()) ? it->first : 0;*/
	_mapTime2Fragment.clear();

	if (_pListener) {
		_pListener->OnMedia::unsubscribe(onMedia);
		_conn.stopListening(idTxt);
		_pListener = NULL;

		// Send the close and UnpublishNotify messages
		/*for (auto it : _mapPeers) {
		if (it.second->connected) {
		it.second->closeGroupStream(GroupStream::GROUP_MEDIA_START, _fragmentCounter, lastTime);
		}
		}*/
	}

	for (auto it : _mapPeers) {
		it.second->OnGroupMedia::unsubscribe(onGroupMedia);
		it.second->OnGroupReport::unsubscribe(onGroupReport);
		it.second->OnGroupPlayPush::unsubscribe(onGroupPlayPush);
		it.second->OnGroupPlayPull::unsubscribe(onGroupPlayPull);
		it.second->OnFragmentsMap::unsubscribe(onFragmentsMap);
		it.second->OnGroupBegin::unsubscribe(onGroupBegin);
		it.second->OnFragment::unsubscribe(onFragment);
		it.second->resetGroup();
	}
	_mapPeers.clear();
}

void NetGroup::addPeer(string peerId, shared_ptr<P2PConnection> pPeer) {
	_mapPeers.emplace(peerId, pPeer);
	pPeer->OnGroupMedia::subscribe(onGroupMedia);
	pPeer->OnGroupReport::subscribe(onGroupReport);
	pPeer->OnGroupPlayPush::subscribe(onGroupPlayPush);
	pPeer->OnGroupPlayPull::subscribe(onGroupPlayPull);
	pPeer->OnFragmentsMap::subscribe(onFragmentsMap);
	pPeer->OnGroupBegin::subscribe(onGroupBegin);
	pPeer->OnFragment::subscribe(onFragment);
}

void NetGroup::manage() {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	// Send the Fragments Map message
	if (_lastFragmentsMap.isElapsed((Int64)_updatePeriod)) {
		if (updateFragmentMap()) {

			// Send to all neighbors
			for (auto it : _mapPeers) {
				if (it.second->connected)
					it.second->sendFragmentsMap(_reportBuffer.data(), _reportBuffer.size());
			}
			_lastFragmentsMap.update();
		}
	}

	// Send the Group Report message (0A)
	if (_lastReport.isElapsed(3000)) { // TODO: add to configuration
		
		for (auto it : _mapPeers) {
			if (buildGroupReport(it.first)) {
				it.second->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
			}
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

		// TODO: See if we need to delete only on fragments map 'key' values
		auto itTime = _mapTime2Fragment.lower_bound(end - _windowDuration);
		if (itTime != _mapTime2Fragment.end() && (end - itTime->first) > _windowDuration)
			itTime--; // To not delete more than the window duration
		if (itTime != _mapTime2Fragment.end() && itTime != _mapTime2Fragment.begin()) {
			_fragments.erase(_fragments.begin(), _fragments.find(itTime->second));
			DEBUG("Deletion of fragments ", _mapTime2Fragment.begin()->second, " (", _mapTime2Fragment.begin()->first, ") to ", 
				itTime->second, " (", itTime->first, ") - current time : ", end)
			_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);
		}
	}

	// Generate the report message
	UInt64 firstFragment = _fragments.begin()->first;
	UInt64 nbFragments = _fragmentCounter - firstFragment;
	_reportBuffer.resize((UInt32)((nbFragments / 8) + 1) + Util::Get7BitValueSize(_fragmentCounter) + 1, false);
	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(GroupStream::GROUP_FRAGMENTS_MAP).write7BitLongValue(_fragmentCounter);

	if (isPublisher) { // Publisher : We have all fragments
		
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
		// Loop on each byte
		UInt64 index = _fragmentCounter-1;
		while (index >= firstFragment) {

			UInt8 currentByte = 0;
			for (UInt8 fragment = 0; fragment < 8 && (index-fragment)>0; fragment++) {
				if (_fragments.find(index - fragment) != _fragments.end())
					currentByte += (1 << fragment);
			}
			index -= 8;
		}
	}

	return true;
}

bool NetGroup::buildGroupReport(const string& peerId) {
	auto itTarget = _mapPeers.find(peerId);
	if (itTarget == _mapPeers.end()) {
		ERROR("Unable to find the peer ", peerId)
		return false;
	}
	DEBUG("Sending Group Report message (type 0A) to peer ", peerId)

	UInt32 sizeTotal = (UInt32)(itTarget->second->peerAddress().host().size() + _conn.serverAddress().host().size() + 12);
	for (auto it1 : _mapPeers)
		sizeTotal += it1.second->peerAddress().host().size() + _conn.serverAddress().host().size() + PEER_ID_SIZE + 13;
	
	_reportBuffer.resize(sizeTotal, false);
	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(0x0A);
	writer.write8(itTarget->second->peerAddress().host().size() + 4);
	writer.write8(0x0D);
	RTMFP::WriteAddress(writer, itTarget->second->peerAddress(), RTMFP::ADDRESS_PUBLIC);
	writer.write8(_conn.serverAddress().host().size() + 4);
	writer.write8(0x0A);
	RTMFP::WriteAddress(writer, _conn.serverAddress(), RTMFP::ADDRESS_REDIRECTION);

	for (auto itPeer : _mapPeers) {
		// TODO: check if it is time since last report message
		UInt8 timeElapsed = (UInt8)((itPeer.second->lastGroupReport > 0) ? ((Time::Now() - itPeer.second->lastGroupReport) / 1000) : 0);
		DEBUG("Group 0A argument - Peer ", itPeer.first, " - elapsed : ", timeElapsed)
		string id(itPeer.first.c_str()); // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
		writer.write32(0x0022210F).write(Util::UnformatHex(id));
		writer.write8(timeElapsed);
		writer.write8(itPeer.second->peerAddress().host().size() + _conn.serverAddress().host().size() + 7);
		writer.write8(0x0A);
		RTMFP::WriteAddress(writer, _conn.serverAddress(), RTMFP::ADDRESS_REDIRECTION);
		RTMFP::WriteAddress(writer, itPeer.second->peerAddress(), RTMFP::ADDRESS_PUBLIC);
	}
	writer.write8(0);
	return true;
}

bool NetGroup::pushFragment(map<UInt64, MediaPacket>::iterator itFragment) {
	if (itFragment == _fragments.end())
		return false;

	// Stand alone fragment
	if (itFragment->second.marker == GroupStream::GROUP_MEDIA_DATA) {
		// Is it the next fragment?
		if (_fragmentCounter == 0 || itFragment->first == _fragmentCounter + 1) {
			_fragmentCounter = itFragment->first;

			DEBUG("Pushing Data Fragment ", itFragment->first)
			_conn.pushMedia(stream, itFragment->second.time, itFragment->second.payload, itFragment->second.payloadSize(), 0, itFragment->second.type == AMF::AUDIO);

			// Send fragment to peers (push mode)
			for (auto it : _mapPeers)
				it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), _fragmentCounter);

			return pushFragment(itFragment++); // Go to next fragment
		}
	}
	// Splitted packet
	else  {
		// Search the start fragment
		auto itStart = itFragment;
		while (itStart->second.marker != GroupStream::GROUP_MEDIA_START) {
			itStart = _fragments.find(itStart->first - 1);
			if (itStart == _fragments.end())
				return false; // ignore these fragments if there is a hole
		}
		
		// Check if all splitted fragments are present
		UInt8 nbFragments = itStart->second.splittedId+1;
		UInt32 payloadSize = itStart->second.payloadSize();
		auto itEnd = itStart;
		for (int i = 1; i < nbFragments; ++i) {
			itEnd = _fragments.find(itStart->first + i);
			if (itEnd == _fragments.end())
				return false; // ignore these fragments if there is a hole

			payloadSize += itEnd->second.payloadSize();
		}

		// Is it the next fragment?
		if (_fragmentCounter == 0 || itStart->first == _fragmentCounter + 1) {
			_fragmentCounter = itEnd->first;

			// Buffer the fragments and push to peers
			Buffer	payload(payloadSize); // TODO: see if we must use PoolBuffer
			BinaryWriter writer(payload.data(), payloadSize);
			auto	itCurrent = itStart;
			do {
				writer.write(itCurrent->second.payload, itCurrent->second.payloadSize());

				// Send fragment to peers (push mode)
				for (auto it : _mapPeers)
					it.second->sendMedia(itCurrent->second.pBuffer.data(), itCurrent->second.pBuffer.size(), itCurrent->first);
			} while (itCurrent++ != itEnd);

			DEBUG("Pushing splitted packet ", itStart->first, " - ", itStart->second.splittedId, "fragments for a total size of ", payloadSize)
			_conn.pushMedia(stream, itStart->second.time, payload.data(), payloadSize, 0, itStart->second.type == AMF::AUDIO);

			return pushFragment(itEnd++);
		}
	}
	return false;
}