#include "NetGroup.h"
#include "P2PConnection.h"
#include "GroupStream.h"
#include <set>

using namespace Mona;
using namespace std;

#if defined(_WIN32)
	#define sscanf sscanf_s
#endif

class GroupNode : public virtual Object {
public:
	GroupNode(const string& rawPeerId, const string& groupId, const SocketAddress& addr) : rawId(rawPeerId), groupAddress(groupId), address(addr) {}

	string rawId;
	string groupAddress;
	SocketAddress address;
};

// Fragment instance
class MediaPacket : public virtual Object {
public:
	MediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 totalSize, Mona::UInt32 time, AMF::ContentType mediaType,
		UInt64 fragmentId, UInt8 groupMarker, UInt8 splitId) : splittedId(splitId), type(mediaType), marker(groupMarker), time(time), pBuffer(poolBuffers, totalSize) {
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

	UInt32 payloadSize() { return pBuffer.size() - (payload - pBuffer.data()); }

	PoolBuffer			pBuffer;
	UInt32				time;
	AMF::ContentType	type;
	const UInt8*		payload; // Payload position
	UInt8				marker;
	UInt8				splittedId;
};

const string& NetGroup::GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress) {
	
	UInt8 tmp[PEER_ID_SIZE];
	EVP_Digest(rawId, PEER_ID_SIZE+2, tmp, NULL, EVP_sha256(), NULL);
	INFO("Group address : ", Util::FormatHex(tmp, PEER_ID_SIZE, groupAddress))
	return groupAddress;
}

double NetGroup::estimatedPeersCount() {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	if (_mapGroupAddress.size() < 4)
		return _mapGroupAddress.size();

	// First get the neighbors N-2 and N+2
	auto itFirst = _mapGroupAddress.lower_bound(_myGroupAddress);
	auto itLast = itFirst;
	if (itFirst == _mapGroupAddress.end()) {
		itFirst = --(--(_mapGroupAddress.end()));
		itLast = ++(_mapGroupAddress.begin());
	}
	else {

		if (itFirst->first > _myGroupAddress) {  // Current == N+1?
			if (--itFirst == _mapGroupAddress.end())
				itFirst = --(_mapGroupAddress.end());
		} else if (++itLast == _mapGroupAddress.end())  // Current == N-1
			itLast = _mapGroupAddress.begin();

		if (--itFirst == _mapGroupAddress.end())
			itFirst = --(_mapGroupAddress.end());
		if (++itLast == _mapGroupAddress.end()) 
			itLast = _mapGroupAddress.begin();
	}
	
	DEBUG("First peer (N-2) = ", itFirst->first)
	DEBUG("Last peer (N+2) = ", itLast->first)

	UInt64 valFirst = 0, valLast = 0;
	sscanf(itFirst->first.substr(0, 16).c_str(), "%llx", &valFirst);
	sscanf(itLast->first.substr(0, 16).c_str(), "%llx", &valLast);

	// Then calculate the total	
	if (valLast > valFirst)
		return (MAX_PEER_COUNT / (double(valLast - valFirst) / 4)) + 1;
	else
		return (MAX_PEER_COUNT / (double(valLast - valFirst + MAX_PEER_COUNT) / 4)) + 1;
}

UInt32 NetGroup::targetNeighborsCount() {
	double memberCount = estimatedPeersCount();
	UInt32 targetNeighbor = (UInt32)(2 * log2(memberCount)) + 13;
	INFO("estimatedMemberCount : ", memberCount, " ; targetNeighbor : ", targetNeighbor)

	return targetNeighbor;
}

NetGroup::NetGroup(const string& groupId, const string& groupTxt, const string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, UInt16 windowDuration) :
	idHex(groupId), idTxt(groupTxt), stream(streamName), isPublisher(publisher), _conn(conn), _updatePeriod((Int64)updatePeriod*1000), _fragmentCounter(0),
	_firstPushMode(true), _pListener(NULL), _windowDuration(windowDuration*1000), _streamCode(0x22) {
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

			if (marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START)
				_mapTime2Fragment[time] = _fragmentCounter;

			// Send fragment to peers (push mode)
			for (auto it : _mapPeers) {
				if (it.second->connected)
					it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), _fragmentCounter);
			}

			pos += splitCounter > 1 ? NETGROUP_MAX_PACKET_SIZE : (end - pos);
			splitCounter--;
		}
		
	};
	onFragment = [this](const string& peerId, UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);

		// TODO: check the fragment with pull & push modes and return if it is wrong

		auto itFragment = _fragments.lower_bound(id);
		if (itFragment != _fragments.end() && itFragment->first == id) {
			WARN("Fragment ", id, " already received, ignored")
			return;
		}

		bool newFragment = _fragments.empty() || id > _fragments.rbegin()->first;

		// Add the fragment to the map
		UInt32 bufferSize = packet.available() + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitedNumber > 1) + Util::Get7BitValueSize(id);
		itFragment = _fragments.emplace_hint(itFragment, piecewise_construct, forward_as_tuple(id), forward_as_tuple(_conn.poolBuffers(), packet.current(), packet.available(), bufferSize, time, (AMF::ContentType)mediaType,
			id, marker, splitedNumber));

		// Send fragment to peers (push mode)
		if (newFragment) {
			for (auto it : _mapPeers) {
				if (it.second->connected)
					it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), id);
			}
		}

		if (marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START)
			_mapTime2Fragment[time] = id;

		// Push the fragment to the output file (if ordered)
		pushFragment(itFragment);
	};
	onGroupMedia = [this](const string& peerId, PacketReader& packet, const string& streamName, FlashWriter& writer) {
		if (streamName == stream) {
			if (isPublisher) {
				Buffer farCode;
				packet.read(0x22, farCode);

				// Another stream code => we must accept and send our stream code
				if (memcmp(_streamCode.data(), farCode.data(), 0x22) != 0) {
					writer.writeGroupMedia(streamName, farCode.data(), 0x22);
					auto it = _mapPeers.find(peerId);
					if (it == _mapPeers.end())
						ERROR("Unable to find the peer ", peerId)
					else if (!it->second->publicationInfosSent)
						it->second->sendGroupMedia(stream, _streamCode.data(), _streamCode.size());
				}
			}
			else {
				NOTE("Starting to listen to publication ", streamName)
				packet.read(0x22, _streamCode);
				writer.writeGroupMedia(streamName, _streamCode.data(), 0x22);
			}

			// The meaning of the rest is unknown for now
			string data;
			UInt8 size = packet.read8();
			if (packet.read(size, data) != "\x02")
				WARN("Unexpected 1st argument value in Group publication : ", Util::FormatHex(BIN data.data(), data.size(), LOG_BUFFER))
			size = packet.read8();
			if (packet.read(size, data) != "\x03\xBE\x40")
				WARN("Unexpected 2nd argument value in Group publication : ", Util::FormatHex(BIN data.data(), data.size(), LOG_BUFFER))
			size = packet.read8();
			if (packet.read(size, data) != "\x04\x92\xA7\x60")
				WARN("Unexpected 3rd argument value in Group publication : ", Util::FormatHex(BIN data.data(), data.size(), LOG_BUFFER))
			size = packet.read8();
			if (packet.read(size, data) != "\x05\x64")
				WARN("Unexpected 4th argument value in Group publication : ", Util::FormatHex(BIN data.data(), data.size(), LOG_BUFFER))
			size = packet.read8();
			if (packet.read(size, data) != "\x07\x93\x44")
				WARN("Unexpected 5th argument value in Group publication : ", Util::FormatHex(BIN data.data(), data.size(), LOG_BUFFER))
		}
		else {
			INFO("New stream available in the group but not registered : ", streamName)
			return;
		}
	};
	onGroupReport = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		string key1, key2, tmp, newPeerId, rawId;
		UInt32 targetCount = targetNeighborsCount();
		SocketAddress address;

		packet.read(8, key1);
		UInt16 size = packet.read8();
		packet.read(size, key2);
		
		DEBUG("Group message 0A - 1st parameter : ", Util::FormatHex(BIN key1.data(), key2.size(), LOG_BUFFER))
		DEBUG("Group message 0A - 2nd parameter : ", Util::FormatHex(BIN key2.data(), key2.size(), LOG_BUFFER))

		// Loop on each peer of the NetGroup
		while (packet.available() > 4) {
			size = packet.read16();
			if (size < 2)
				continue;
			else if (size > packet.available())
				size = packet.available();
			packet.read(size, rawId);
			bool unknown = String::ICompare(rawId, "\x21\x0F" , 2)!=0;
			if (unknown)
				DEBUG("Group message 0A - Unknown parameter : ", Util::FormatHex(BIN rawId.data(), size, LOG_BUFFER))
			else
				DEBUG("Group message 0A - Peer ID : ", Util::FormatHex(BIN rawId.data()+2, PEER_ID_SIZE, newPeerId))
			UInt64 time = packet.read7BitLongValue();
			DEBUG("Group message 0A - Time elapsed : ", time)

			size = packet.read8();
			if (unknown || newPeerId == _conn.peerId() || _mapHeardList.find(newPeerId) != _mapHeardList.end()) {
				packet.next(size);
				continue;
			}

			// New peer, get the address and try to connect to him
			UInt8 addressMarker = packet.read8();
			--size;
			if (addressMarker != 0x0A) {
				WARN("Unexpected address marker : ", Format<UInt8>("%.2x", addressMarker))
				packet.next(size);
				break;
			}
			while (size > 0) {
				
				UInt8 addressType = packet.read8();
				RTMFP::ReadAddress(packet, address, addressType);
				if ((addressType&0x0F) == RTMFP::ADDRESS_PUBLIC && address.family() == IPAddress::IPv4) {// TODO: Handle ivp6
					DEBUG("Group message 0A - IP Address : ", address.toString())

					addPeer2HeardList(newPeerId.c_str(), rawId.c_str(), address);  // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
					if (_mapHeardList.size() < targetCount)
						_conn.connect2Peer(newPeerId.c_str(), stream.c_str(), rawId, address);
				}
				size -= 3 + ((address.family() == IPAddress::IPv6) ? sizeof(in6_addr) : sizeof(in_addr));
			}
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
	};
	onGroupPlayPush = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end())
			ERROR("Unable to find the peer ", peerId)
		else {
			it->second->setPushMode(packet.read8());

			// First Viewer = > create listener
			if (isPublisher && !_pListener) {
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
		DEBUG("Group Fragments map (type 22) received : ", counter)

		// Player? => update play mode if needed
		if (!isPublisher) {
			lock_guard<recursive_mutex> lock(_fragmentMutex);

			auto it = _mapPeers.find(peerId);
			if (it == _mapPeers.end())
				ERROR("Unable to find the peer ", peerId)
			else {
				it->second->updateFragmentsMap(counter, packet.current(), packet.available());
				if (_firstPushMode) {
					updatePlayMode();
					_firstPushMode = false;
				}
			}
		}
		packet.next(packet.available());
	};
	onGroupBegin = [this](const string& peerId, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex); // TODO: not sure it is needed
		
		if (buildGroupReport(peerId)) {
			// When we receive the 0E NetGroup message type we must send the group report
			INFO("Sending the group report to ", peerId)
			auto it = _mapPeers.find(peerId);
			if (it != _mapPeers.end())
				it->second->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
		}
	};

	GetGroupAddressFromPeerId(STR _conn.rawId(), _myGroupAddress);
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
		_mapTime2Fragment.clear();
	}
	/*auto it = _mapTime2Fragment.rbegin();
	UInt32 lastTime = (it != _mapTime2Fragment.rend()) ? it->first : 0;*/

	if (_pListener) {
		_pListener->OnMedia::unsubscribe(onMedia);
		_conn.stopListening(idTxt);
		_pListener = NULL;

		// TODO: Send the close and UnpublishNotify messages
		/*for (auto it : _mapPeers) {
		if (it.second->connected) {
		it.second->closeGroupStream(GroupStream::GROUP_MEDIA_START, _fragmentCounter, lastTime);
		}
		}*/
	}

	map<string, shared_ptr<P2PConnection>>::iterator itPeer = _mapPeers.begin();
	while (itPeer != _mapPeers.end())
		removePeer(itPeer);
}

void NetGroup::addPeer2HeardList(const string& peerId, const char* rawId, const SocketAddress& address) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	auto it = _mapHeardList.lower_bound(peerId);
	if (it != _mapHeardList.end() && it->first == peerId) {
		DEBUG("The peer ", peerId, " is already known")
		return;
	}

	string groupAddress;
	_mapGroupAddress.emplace(GetGroupAddressFromPeerId(rawId, groupAddress), peerId);
	it = _mapHeardList.emplace_hint(it, piecewise_construct, forward_as_tuple(peerId.c_str()), forward_as_tuple(rawId, groupAddress, address));
	INFO("Peer ", it->first, " added to heard list")
}

bool NetGroup::addPeer(const string& peerId, shared_ptr<P2PConnection> pPeer) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	if (_mapHeardList.find(peerId) == _mapHeardList.end()) {
		ERROR("Unknown peer to add : ", peerId)
		return false;
	}

	auto it = _mapPeers.lower_bound(peerId);
	if (it != _mapPeers.end() && it->first == peerId) {
		ERROR("Unable to add the peer ", peerId, ", it already exists")
		return false;
	}
	INFO("Adding the peer ", peerId, " to group")
	_mapPeers.emplace_hint(it, peerId, pPeer);

	pPeer->OnGroupMedia::subscribe(onGroupMedia);
	pPeer->OnGroupReport::subscribe(onGroupReport);
	pPeer->OnGroupPlayPush::subscribe(onGroupPlayPush);
	pPeer->OnGroupPlayPull::subscribe(onGroupPlayPull);
	pPeer->OnFragmentsMap::subscribe(onFragmentsMap);
	pPeer->OnGroupBegin::subscribe(onGroupBegin);
	pPeer->OnFragment::subscribe(onFragment);
	return true;
}

void NetGroup::removePeer(const string& peerId) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	auto it = _mapPeers.find(peerId);
	if (it == _mapPeers.end())
		DEBUG("The peer ", peerId, " is already removed from the best list")
	else
		removePeer(it);

	// The peer is disconnected, we remove it from the heard list
	auto it2 = _mapHeardList.find(peerId);
	if (it2 != _mapHeardList.end()) {
		_mapGroupAddress.erase(it2->second.groupAddress);
		_mapHeardList.erase(it2);
		INFO("Peer ", peerId, " deleted from the heard list")
	}
}

void NetGroup::removePeer(map<string, shared_ptr<P2PConnection>>::iterator& itPeer) {
	INFO("Deleting peer ", itPeer->first, " from the NetGroup map of peers")

	itPeer->second->OnGroupMedia::unsubscribe(onGroupMedia);
	itPeer->second->OnGroupReport::unsubscribe(onGroupReport);
	itPeer->second->OnGroupPlayPush::unsubscribe(onGroupPlayPush);
	itPeer->second->OnGroupPlayPull::unsubscribe(onGroupPlayPull);
	itPeer->second->OnFragmentsMap::unsubscribe(onFragmentsMap);
	itPeer->second->OnGroupBegin::unsubscribe(onGroupBegin);
	itPeer->second->OnFragment::unsubscribe(onFragment);
	itPeer->second->resetGroup();
	_mapPeers.erase(itPeer++);
}

bool NetGroup::checkPeer(const string& groupId, const string& peerId) {

	return idHex == groupId && (_mapPeers.find(peerId) == _mapPeers.end());
}

void NetGroup::manage() {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	if (_mapPeers.empty())
		return;

	// Manage the Best list
	if (_lastBestCalculation.isElapsed(1000)) {

		manageBestList();
		_lastBestCalculation.update();
	}

	// Send the push & pull requests
	if (!isPublisher && _lastPlayUpdate.isElapsed(2000)) { // TODO: add to configuration

		updatePlayMode();
		_lastPlayUpdate.update();
	}

	// Send the Fragments Map message
	if (_lastFragmentsMap.isElapsed(_updatePeriod)) {
		if (updateFragmentMap()) {

			// Send to all neighbors
			for (auto it : _mapPeers) {
				if (it.second->connected)
					it.second->sendFragmentsMap(_reportBuffer.data(), _reportBuffer.size());
			}
			_lastFragmentsMap.update();
		}
	}

	// TODO: Send Pull request(s) if needed

	// Send the Group Report message (0A) to a random connected peer
	if (_lastReport.isElapsed(5000)) { // TODO: add to configuration
		
		UInt32 index = Util::Random<UInt32>() % _mapPeers.size();
		auto itRandom = _mapPeers.begin();
		advance(itRandom, index);
		if (itRandom->second->connected && buildGroupReport(itRandom->first))
			itRandom->second->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());

		_lastReport.update();
	}
}

void NetGroup::manageBestList() {
	if (_mapPeers.empty())
		return;

	INFO("manageBestList() ; Peers connected : ", _mapPeers.size(), " ; Peers known : ", _mapGroupAddress.size())
	set<string> bestList;

	// Find the 6 closest peers
	if (_mapGroupAddress.size() <= 6) {
		for (auto it : _mapGroupAddress)
			bestList.emplace(it.second);
	}
	else { // More than 6 peers

		// First we search the first of the 6 peers
		map<string, string>::iterator itFirst = _mapGroupAddress.lower_bound(_myGroupAddress);
		if (itFirst == _mapGroupAddress.end())
			itFirst = --(_mapGroupAddress.end());
		for (int i = 0; i < 2; i++) {
			if (--itFirst == _mapGroupAddress.end()) // if we reach the first peer we restart from the end
				itFirst = --(_mapGroupAddress.end());
		}

		for (int j = 0; j < 6; j++) {
			bestList.emplace(itFirst->second);

			if (++itFirst == _mapGroupAddress.end()) // if we reach the end we restart from the beginning
				itFirst = _mapGroupAddress.begin();
		}
	}

	// Find the 6 lowest latency
	if (_mapGroupAddress.size() > 6) {
		deque<shared_ptr<P2PConnection>> queueLatency;
		for (auto it : _mapPeers) { // First, order the peers by latency
			UInt16 latency = it.second->latency();
			auto it2 = queueLatency.begin();
			while (it2 != queueLatency.end() && (*it2)->latency() < latency)
				++it2;
			queueLatency.emplace(it2, it.second);
		}
		auto itLatency = queueLatency.begin();
		int i = 0;
		do {
			if (bestList.emplace((*itLatency)->peerId).second)
				i++;
		} while (++itLatency != queueLatency.end() && i < 6);

		// Add one random peer
		if (_mapGroupAddress.size() > bestList.size()) {

			auto itRandom = _mapGroupAddress.begin();
			advance(itRandom, Util::Random<UInt32>() % _mapGroupAddress.size());
			while (itRandom != _mapGroupAddress.end() && !bestList.emplace(itRandom->second).second)
				++itRandom;
		}

		// Find 2 log(N) peers with location + 1/2, 1/4, 1/8 ...
		UInt32 bests = bestList.size();
		if (_mapGroupAddress.size() > bests) {
			UInt32 count = targetNeighborsCount() - bests;
			if (count > _mapGroupAddress.size() - bests)
				count = _mapGroupAddress.size() - bests;

			auto itNode = _mapGroupAddress.lower_bound(_myGroupAddress);
			UInt32 rest = (_mapGroupAddress.size() / 2) - 1;
			UInt32 step = rest / (2 * count);
			for (; count > 0; count--) {
				if (distance(itNode, _mapGroupAddress.end()) <= step) {
					itNode = _mapGroupAddress.begin();
				}
				advance(itNode, step);
				while (!bestList.emplace(itNode->second).second) { // If not added go to next
					if (++itNode == _mapGroupAddress.end())
						itNode = _mapGroupAddress.begin();
				}
			}
		}
	}

	// Close old peers
	auto it2Close = _mapPeers.begin();
	shared_ptr<P2PConnection> p2Close;
	while (it2Close != _mapPeers.end()) {
		if (bestList.find(it2Close->first) == bestList.end()) {
			p2Close = it2Close->second;
			removePeer(it2Close);
			p2Close->close();
		}
		else
			it2Close++;
	}

	// Connect to new peers
	for (auto it : bestList) {
		if (_mapPeers.find(it) == _mapPeers.end()) {
			auto itNode = _mapHeardList.find(it);
			if (itNode == _mapHeardList.end())
				WARN("Unable to find the peer ", it) // implementation error, should not happen
			else
				_conn.connect2Peer(it.c_str(), stream.c_str(), itNode->second.rawId, itNode->second.address);
		}
	}

	INFO("End of best list calculation, neighbor count : ", bestList.size())
}

void NetGroup::eraseOldFragments() {

	auto it = _fragments.rbegin();
	if (it != _fragments.rend()) {
		UInt32 end = it->second.time;

		auto itTime = _mapTime2Fragment.lower_bound(end - _windowDuration);
		if (itTime != _mapTime2Fragment.end() && (end - itTime->first) > _windowDuration)
			itTime--; // To not delete more than the window duration
		if (itTime != _mapTime2Fragment.end() && itTime != _mapTime2Fragment.begin()) {

			// Get the first fragment before itTime->second (To be sure to begin with a reference)
			auto itFragment = _fragments.find(itTime->second);
			--itFragment;
			if (_fragmentCounter < itFragment->first) {
				_fragmentCounter = itFragment->first;
				DEBUG("New fragment reference is ", _fragmentCounter)
			}
			_fragments.erase(_fragments.begin(), itFragment);

			--itTime;
			DEBUG("Deletion of fragments ", _mapTime2Fragment.begin()->second, " (", _mapTime2Fragment.begin()->first, ") to ",
				itTime->second, " (", itTime->first, ") - current time : ", end)
				_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);

			// Try to push again the last fragments
			auto itLast = _fragments.find(_fragmentCounter + 1);
			if (itLast != _fragments.end())
				pushFragment(itLast);
		}
	}
}

bool NetGroup::updateFragmentMap() {
	if (_fragments.empty())
		return false;

	// First we erase old fragments
	eraseOldFragments();

	// Generate the report message
	UInt64 firstFragment = _fragments.begin()->first;
	UInt64 lastFragment = _fragments.rbegin()->first;
	UInt64 nbFragments = lastFragment - firstFragment;
	_reportBuffer.resize((UInt32)((nbFragments / 8) + 1) + Util::Get7BitValueSize(lastFragment) + 1, false);
	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(GroupStream::GROUP_FRAGMENTS_MAP).write7BitLongValue(lastFragment);

	if (isPublisher) { // Publisher : We have all fragments, faster treatment
		
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
		UInt64 index = lastFragment-1;
		while (index >= firstFragment) {

			UInt8 currentByte = 0;
			for (UInt8 fragment = 0; fragment < 8 && (index-fragment) >= firstFragment; fragment++) {
				if (_fragments.find(index - fragment) != _fragments.end())
					currentByte += (1 << fragment);
			}
			writer.write8(currentByte);
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
	DEBUG("Preparing the Group Report message (type 0A) for peer ", peerId)

	UInt32 sizeTotal = (UInt32)(itTarget->second->peerAddress().host().size() + _conn.serverAddress().host().size() + 12);
	for (auto it1 : _mapPeers) {
		if (it1.second->connected)
			sizeTotal += it1.second->peerAddress().host().size() + _conn.serverAddress().host().size() + PEER_ID_SIZE + 13;
	}
	
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
		if (!itPeer.second->connected)
			continue;
		// TODO: check if it is time since last report message
		UInt8 timeElapsed = (UInt8)((itPeer.second->lastGroupReport > 0) ? ((Time::Now() - itPeer.second->lastGroupReport) / 1000) : 0);
		DEBUG("Group 0A argument - Peer ", itPeer.first, " - elapsed : ", timeElapsed, " (latency : ", itPeer.second->latency(),")")
		writer.write32(0x0022).write(itPeer.second->rawId);
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

			return pushFragment(++itFragment); // Go to next fragment
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

			// Buffer the fragments and write to file
			Buffer	payload(payloadSize);
			BinaryWriter writer(payload.data(), payloadSize);
			auto	itCurrent = itStart;
			do {
				writer.write(itCurrent->second.payload, itCurrent->second.payloadSize());
			} while (itCurrent++ != itEnd);

			DEBUG("Pushing splitted packet ", itStart->first, " - ", nbFragments, " fragments for a total size of ", payloadSize)
			_conn.pushMedia(stream, itStart->second.time, payload.data(), payloadSize, 0, itStart->second.type == AMF::AUDIO);

			return pushFragment(++itEnd);
		}
	}

	// TODO: there is a hole, send pull requests

	return false;
}

void NetGroup::updatePlayMode() {
	if (_mapPeers.empty())
		return;

	INFO("Peers connected : ", _mapPeers.size(), " ; Peers known : ", _mapHeardList.size())

	// Calculate the current mode
	UInt16 totalMode = 0;
	for (auto it : _mapPeers) {
		if (it.second->connected)
			totalMode += it.second->pushInMode;
	}

	if (totalMode == 0xFF)
		return;

	// We determine the next bit to add to the mask
	UInt8 bitNumber = Util::Random<UInt8>() % 8; // First bit is random
	if (totalMode != 0) {
		bitNumber = 7;
		while ((totalMode & (1 << bitNumber)) == 0) // search the 1st bit to 1
			--bitNumber;

		while ((totalMode & (1 << bitNumber)) > 0)// search the next bit to 0
			bitNumber = (bitNumber == 7)? 0 : bitNumber+1;
	}
	UInt8 toAdd = 1 << bitNumber;

	// If at least a neighbor have this mask we send the new mask to one
	// TODO: create and use the bestPeers map
	vector<shared_ptr<P2PConnection>> possiblePeers;
	for (auto itPeer : _mapPeers) {
		if (itPeer.second->connected && itPeer.second->checkMask(bitNumber))
			possiblePeers.push_back(itPeer.second);
	}
	
	if (!possiblePeers.empty()) {
		UInt8 indexPeer = (Util::Random<UInt8>() % possiblePeers.size());
		auto itRandom = possiblePeers.begin();
		while (indexPeer > 0) {
			++itRandom; --indexPeer;
		}

		(*itRandom)->sendPushMode((*itRandom)->pushInMode + toAdd);
	}
	else {
		WARN("No peer available for the mask ", Format<UInt8>("%.2x", toAdd))
		return;
	}

	if ((toAdd + totalMode) == 0xFF)
		INFO("NetGroup Push mode completed (0xFF reached)")
}
