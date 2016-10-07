#include "NetGroup.h"
#include "P2PConnection.h"
#include "GroupStream.h"

using namespace Mona;
using namespace std;

#if defined(_WIN32)
	#define sscanf sscanf_s
#endif

class GroupNode : public virtual Object {
public:
	GroupNode(const char* rawPeerId, const string& groupId, const SocketAddress& addr, RTMFP::AddressType peerType, const SocketAddress& host) :
		rawId(rawPeerId, PEER_ID_SIZE + 2), groupAddress(groupId), address(addr), addressType(peerType), hostAddress(host), lastGroupReport(0) {
	}

	string rawId;
	string groupAddress;
	SocketAddress address;
	RTMFP::AddressType addressType;
	SocketAddress hostAddress;
	UInt64 lastGroupReport; // Time in msec of last Group report received
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
	Util::FormatHex(tmp, PEER_ID_SIZE, groupAddress);
	TRACE("Group address : ", groupAddress)
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
	
	TRACE("First peer (N-2) = ", itFirst->first)
	TRACE("Last peer (N+2) = ", itLast->first)

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
	TRACE("estimatedMemberCount : ", memberCount, " ; targetNeighbor : ", targetNeighbor)

	return targetNeighbor;
}

NetGroup::NetGroup(const string& groupId, const string& groupTxt, const string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, UInt16 windowDuration) :
	idHex(groupId), idTxt(groupTxt), stream(streamName), isPublisher(publisher), _conn(conn), _updatePeriod((UInt64)(updatePeriod*1000)), _fragmentCounter(0),
	_firstPushMode(true), _pListener(NULL), _windowDuration(windowDuration*1000), _streamCode(0x22), _currentPushMask(0), _currentPushIsBad(true) {
	onMedia = [this](bool reliable, AMF::ContentType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		const UInt8* pos = data;
		const UInt8* end = data + size;
		UInt8 splitCounter = size / NETGROUP_MAX_PACKET_SIZE + ((size % NETGROUP_MAX_PACKET_SIZE) > 1);
		UInt8 marker = GroupStream::GROUP_MEDIA_DATA ;
		TRACE("Creating fragments ", _fragmentCounter + 1, " to ", _fragmentCounter + splitCounter)
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
				// Send Group media infos if not already sent
				if (!it.second->publicationInfosSent) {
					it.second->sendGroupMedia(stream, _streamCode.data(), _streamCode.size(), _updatePeriod, _windowDuration);
					it.second->publicationInfosSent = true;
				}

				if (it.second->connected)
					it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), _fragmentCounter);
			}

			pos += splitCounter > 1 ? NETGROUP_MAX_PACKET_SIZE : (end - pos);
			splitCounter--;
		}
		
	};
	onFragment = [this](const string& peerId, UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);

		auto itFragment = _fragments.lower_bound(id);
		if (itFragment != _fragments.end() && itFragment->first == id) {
			DEBUG("Fragment ", id, " already received, ignored")
			return;
		}
		
		// Pull fragment?
		auto itWaiting = _waitingFragments.find(id);
		if (itWaiting != _waitingFragments.end()) {
			DEBUG("Waiting fragment ", id, " is arrived")
			_waitingFragments.erase(itWaiting);
		}
		// Push fragment
		else {
			// Fragments too old? TODO: check if necessary
			if (!_fragments.empty() && id < (_fragments.begin()->first - 8)) {
				DEBUG("Fragment ", id, " received is too old, ignored")
				return;
			}

			// Push analyzing?
			if (_currentPushMask && _currentPushIsBad && ((_currentPushMask & (1 << (id % 8))) > 0)) {
				auto itPeer = _mapPeers.find(peerId);
				if (itPeer != _mapPeers.end()) {
					DEBUG("Push In Analysis - We have received a packet")
					_currentPushIsBad = false;
				}
			}
		}

		// Add the fragment to the map
		UInt32 bufferSize = packet.available() + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitedNumber > 1) + Util::Get7BitValueSize(id);
		itFragment = _fragments.emplace_hint(itFragment, piecewise_construct, forward_as_tuple(id), forward_as_tuple(_conn.poolBuffers(), packet.current(), packet.available(), bufferSize, time, (AMF::ContentType)mediaType,
			id, marker, splitedNumber));

		// Send fragment to peers (push mode)
		for (auto it : _mapPeers) {
			if (it.second->connected)
				it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), id);
		}

		if (marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START)
			_mapTime2Fragment[time] = id;

		// Push the fragment to the output file (if ordered)
		pushFragment(itFragment);
	};
	onGroupMedia = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		string streamName;
		const UInt8* posStart = packet.current() - 1; // Record the whole packet for sending back

		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end()) {
			ERROR("Unable to find the peer ", peerId)
			return;
		}

		// Read the name
		UInt8 sizeName = packet.read8();
		if (sizeName <= 1) {
			WARN("New stream available without name")
			return;
		}
		packet.next(); // 00
		packet.read(sizeName - 1, streamName);
		if (streamName != stream) {
			INFO("New stream available in the group but not registered : ", streamName)
			return;
		}

		Buffer farCode;
		packet.read(0x22, farCode);
		if (isPublisher) {
			// Another stream code => we must accept and send our stream code
			if (memcmp(_streamCode.data(), farCode.data(), 0x22) != 0) {
				writer.writeRaw(posStart, packet.size() - (posStart - packet.data())); // Return the request to accept
				if (!it->second->publicationInfosSent) {
					it->second->sendGroupMedia(stream, _streamCode.data(), _streamCode.size(), _updatePeriod, _windowDuration);
					it->second->publicationInfosSent = true;
				}
			}
		}
		else {
			NOTE("Starting to listen to publication ", streamName)

			// Save the key if first time received
			if (_streamCode.data()[0] == 0) {
				DEBUG("Saving the key ", Util::FormatHex(BIN farCode.data(), 0x22, LOG_BUFFER))
				memcpy(_streamCode.data(), farCode.data(), 0x22);
			}
			it->second->sendGroupMedia(stream, _streamCode.data(), _streamCode.size(), _updatePeriod, _windowDuration);
			it->second->publicationInfosSent = true;
		}

		// Properties of the NetGroup stream
		UInt8 size = 0, id = 0;
		while (packet.available()) {
			if ((size = packet.read8()) == 0)
				continue;
			id = packet.read8();
			switch (id) {
				case NETGROUP_UNKNWON_PARAMETER:
					break;
				case NETGROUP_WINDOW_DURATION:
					TRACE("Window Duration : ", packet.read7BitLongValue(), "ms"); break;
				case NETGROUP_OBJECT_ENCODING:
					TRACE("Object Encoding : ", packet.read7BitLongValue()); break;
				case NETGROUP_UPDATE_PERIOD:
					TRACE("Avaibility Update period : ", packet.read7BitLongValue(), "ms");	break;
				case NETGROUP_SEND_TO_ALL:
					TRACE("Availability Send To All : ON"); break;
				case NETROUP_FETCH_PERIOD:
					TRACE("Avaibility Fetch period : ", packet.read7BitLongValue(), "ms"); break;
			}
		}
	};
	onGroupReport = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		string key1, key2, tmp, newPeerId, rawId;
		UInt32 targetCount = targetNeighborsCount();

		UInt8 size1st = packet.read8();
		while (size1st == 1) { // TODO: check what this means
			packet.next();
			size1st = packet.read8();
		}
		if (size1st != 8) {
			ERROR("Unexpected 1st parameter size in Group Report : ", size1st) // I think it happens when a peers send wrong informations in the Group Report
			return;
		}

		packet.read(8, key1);
		UInt16 size = packet.read8();
		packet.read(size, key2);
		
		TRACE("Group Report - Our address : ", Util::FormatHex(BIN key1.data(), key1.size(), LOG_BUFFER))
		TRACE("Group Report - Far peer addresses : ", Util::FormatHex(BIN key2.data(), key2.size(), LOG_BUFFER))

		// Loop on each peer of the NetGroup
		while (packet.available() > 4) {
			UInt8 zeroMarker = packet.read8();
			if (zeroMarker != 00) {
				ERROR("Unexpected marker : ", Format<UInt8>("%.2x", zeroMarker), " - Expected 00")
				break;
			}
			size = packet.read8();
			if (size == 0x22) {
				packet.read(size, rawId);
				Util::FormatHex(BIN rawId.data() + 2, PEER_ID_SIZE, newPeerId);
				if (String::ICompare(rawId, "\x21\x0F", 2) != 0) {
					ERROR("Unexpected parameter : ", newPeerId, " - Expected Peer Id")
					break;
				}
				TRACE("Group Report - Peer ID : ", newPeerId)
			}
			else if (size > 7)
				readAddress(packet, size, targetCount, newPeerId, rawId, true);
			else
				TRACE("Empty parameter...")

			UInt64 time = packet.read7BitLongValue();
			TRACE("Group Report - Time elapsed : ", time)

			size = packet.read8(); // Address size

			if (size < 0x08 || newPeerId == _conn.peerId() || _mapHeardList.find(newPeerId) != _mapHeardList.end())
				packet.next(size);
			else // New peer, get the address and try to connect to him
				readAddress(packet, size, targetCount, newPeerId, rawId, false);
		}

		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end()) {
			ERROR("Unable to find the peer ", peerId)
			return;
		}
		
		auto itNode = _mapHeardList.find(peerId);
		if (itNode != _mapHeardList.end())
			itNode->second.lastGroupReport = Time::Now(); // Record the time of last Group Report received to build our Group Report

		// First Viewer = > create listener
		if (isPublisher && !_pListener) {
			Exception ex;
			if (!(_pListener = _conn.startListening<GroupListener>(ex, stream, idTxt))) {
				WARN(ex.error()) // TODO : See if we can send a specific answer
				return;
			}
			INFO("First viewer play request, starting to play Stream ", stream)
			_pListener->OnMedia::subscribe(onMedia);
			// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
			_conn.publishReady = true;
		}

		if (!it->second->groupReportInitiator) {
			sendGroupReport(it);
			_lastReport.update();
		} else
			it->second->groupReportInitiator = false;
	};
	onGroupPlayPush = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end())
			ERROR("Unable to find the peer ", peerId)
		else
			it->second->setPushMode(packet.read8());
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
		DEBUG("Group Fragments map (type 22) received from ", peerId, " : ", counter)

		// Player? => update play mode if needed
		if (!isPublisher) {
			lock_guard<recursive_mutex> lock(_fragmentMutex);

			auto it = _mapPeers.find(peerId);
			if (it == _mapPeers.end())
				ERROR("Unable to find the peer ", peerId)
			else {
				it->second->updateFragmentsMap(counter, packet.current(), packet.available());
				if (_firstPushMode) {
					updatePushMode();
					_firstPushMode = false;
				}
			}
		}
		packet.next(packet.available());
	};
	onGroupBegin = [this](const string& peerId, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex); // TODO: not sure it is needed

		 // When we receive the 0E NetGroup message type we must send the group report
		auto it = _mapPeers.find(peerId);
		if (it != _mapPeers.end()) {
			it->second->groupReportInitiator = true;
			sendGroupReport(it);
			_lastReport.update();
		}
	};

	GetGroupAddressFromPeerId(STR _conn.rawId(), _myGroupAddress);

	// If Publisher we generate the stream key
	if (isPublisher) {
		BinaryWriter writer(_streamCode.data(), _streamCode.size());
		writer.write16(0x2101);
		Util::Random((UInt8*)_streamCode.data() + 2, 0x20); // random serie of 32 bytes
	} else 
		memset(_streamCode.data(), 0, _streamCode.size());
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

void NetGroup::addPeer2HeardList(const string& peerId, const char* rawId, const SocketAddress& address, RTMFP::AddressType addressType, const SocketAddress& hostAddress) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	auto it = _mapHeardList.lower_bound(peerId);
	if (it != _mapHeardList.end() && it->first == peerId) {
		DEBUG("The peer ", peerId, " is already known")
		return;
	}

	string groupAddress;
	_mapGroupAddress.emplace(GetGroupAddressFromPeerId(rawId, groupAddress), peerId);
	it = _mapHeardList.emplace_hint(it, piecewise_construct, forward_as_tuple(peerId.c_str()), forward_as_tuple(rawId, groupAddress, address, addressType, hostAddress));
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

void NetGroup::removePeer(const string& peerId, bool full) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	auto it = _mapPeers.find(peerId);
	if (it == _mapPeers.end())
		DEBUG("The peer ", peerId, " is already removed from the best list")
	else
		removePeer(it);

	if (!full)
		return;

	// The peer is not reachable anymore, we remove it from the heard list
	auto it2 = _mapHeardList.find(peerId);
	if (it2 != _mapHeardList.end()) {
		_mapGroupAddress.erase(it2->second.groupAddress);
		_mapHeardList.erase(it2);
		INFO("Peer ", peerId, " deleted from the heard list")
	}
}

void NetGroup::peerIsClosingNetgroup(const string& peerId) {
	auto it = _mapPeers.find(peerId);
	if (it == _mapPeers.end())
		DEBUG("The peer ", peerId, " is already removed from the best list")
	else {
		it->second->close(false);
		removePeer(it);
	}

}

void NetGroup::removePeer(MAP_PEERS_ITERATOR_TYPE& itPeer) {
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

		set<string> bestList;
		buildBestList(_myGroupAddress, bestList);
		DEBUG("Manage Best list ; Peers connected : ", _mapPeers.size(), " ; new count : ", bestList.size(), " ; Peers known : ", _mapGroupAddress.size())
		manageBestConnections(bestList);
		_lastBestCalculation.update();
	}

	// Send the Fragments Map message
	UInt64 lastFragment(0);
	if (_lastFragmentsMap.isElapsed(_updatePeriod)) {
		if (lastFragment = updateFragmentMap()) {

			// Send to all neighbors
			for (auto it : _mapPeers) {
				if (it.second->connected)
					it.second->sendFragmentsMap(lastFragment, _reportBuffer.data(), _reportBuffer.size());
			}
			_lastFragmentsMap.update();
		}
	}

	// Send the Group Report message (0A) to a random connected peer
	if (_lastReport.isElapsed(10000)) { // TODO: add to configuration
		
		auto itRandom = _mapPeers.begin();
		if (RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itRandom, [](const MAP_PEERS_ITERATOR_TYPE it) { return it->second->connected; })) {
			itRandom->second->groupReportInitiator = true;
			sendGroupReport(itRandom);
			_lastReport.update();
		}
	}

	// Send the Push & Pull requests
	if (!isPublisher && _lastPlayUpdate.isElapsed(2000)) { // TODO: add to configuration (is this the fetch period?)

		sendPullRequests();
		updatePushMode();
		_lastPlayUpdate.update();
	}
}

void NetGroup::buildBestList(const string& groupAddress, set<string>& bestList) {

	// Find the 6 closest peers
	if (_mapGroupAddress.size() <= 6) {
		for (auto it : _mapGroupAddress)
			bestList.emplace(it.second);
	}
	else { // More than 6 peers

		// First we search the first of the 6 peers
		map<string, string>::iterator itFirst = _mapGroupAddress.lower_bound(groupAddress);
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
			if (RTMFP::getRandomIt<map<string, string>, map<string, string>::iterator>(_mapGroupAddress, itRandom, [bestList](const map<string, string>::iterator& it) { return bestList.find(it->second) != bestList.end(); }))
				bestList.emplace(itRandom->second);
		}

		// Find 2 log(N) peers with location + 1/2, 1/4, 1/8 ...
		UInt32 bests = bestList.size();
		if (_mapGroupAddress.size() > bests) {
			UInt32 count = targetNeighborsCount() - bests;
			if (count > _mapGroupAddress.size() - bests)
				count = _mapGroupAddress.size() - bests;

			auto itNode = _mapGroupAddress.lower_bound(groupAddress);
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

	TRACE("End of best list calculation, neighbor count : ", bestList.size())
}

void NetGroup::eraseOldFragments() {
	if (_fragments.empty())
		return;

	UInt32 end = _fragments.rbegin()->second.time;
	UInt32 time2Keep = end - (_windowDuration + 2000); // +2s is Relay Margin
	auto itTime = _mapTime2Fragment.lower_bound(time2Keep);

	// To not delete more than the window duration
	if (itTime != _mapTime2Fragment.end() && time2Keep > itTime->first)
		--itTime;
		
	// Ignore if no fragment found or if it is the first reference
	if (itTime == _mapTime2Fragment.end() || itTime == _mapTime2Fragment.begin())
		return;

	// Get the first fragment before the itTime reference
	auto itFragment = --(_fragments.find(itTime->second));
	if (_fragmentCounter < itFragment->first) {
		WARN("Deleting unread fragments to keep the window duration... (", itFragment->first - _fragmentCounter, " fragments ignored)")
		_fragmentCounter = itFragment->first;
	}

	--itTime;
	DEBUG("Deletion of fragments ", _fragments.begin()->first, " (~", _mapTime2Fragment.begin()->first, ") to ",
		itFragment->first, " (~", itTime->first, ") - current time : ", end)
	_fragments.erase(_fragments.begin(), itFragment);
	_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);

	// TODO: delete also _waitingFragments

	// Try to push again the last fragments
	auto itLast = _fragments.find(_fragmentCounter + 1);
	if (itLast != _fragments.end())
		pushFragment(itLast);
}

UInt64 NetGroup::updateFragmentMap() {
	if (_fragments.empty())
		return 0;

	// First we erase old fragments
	eraseOldFragments();

	// Generate the report message
	UInt64 firstFragment = _fragments.begin()->first;
	UInt64 lastFragment = _fragments.rbegin()->first;
	UInt64 nbFragments = lastFragment - firstFragment; // number of fragments - the first one
	_reportBuffer.resize((UInt32)((nbFragments / 8) + ((nbFragments % 8) > 0)) + Util::Get7BitValueSize(lastFragment) + 1, false);
	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(GroupStream::GROUP_FRAGMENTS_MAP).write7BitLongValue(lastFragment);

	// If there is only one fragment we just write its counter
	if (!nbFragments)
		return true;

	if (isPublisher) { // Publisher : We have all fragments, faster treatment
		
		while (nbFragments > 8) {
			writer.write8(0xFF);
			nbFragments -= 8;
		}
		UInt8 lastByte = 1;
		while (--nbFragments > 0)
			lastByte = (lastByte << 1) + 1;
		writer.write8(lastByte);
	}
	else {
		// Loop on each byte
		UInt64 index = lastFragment;
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

	return lastFragment;
}

void NetGroup::sendGroupReport(const MAP_PEERS_ITERATOR_TYPE& itPeer) {
	DEBUG("Preparing the Group Report message (type 0A) for peer ", itPeer->first)

	auto itNode = _mapHeardList.find(itPeer->first);
	if (itNode == _mapHeardList.end()) {
		ERROR("Unable to find the peer ", itPeer->first, " in the Heard list")
		return;
	}

	set<string> bestList;
	buildBestList(itNode->second.groupAddress, bestList);

	// Calculate the total size to allocate sufficient memory
	UInt32 sizeTotal = (UInt32)(itPeer->second->peerAddress().host().size() + _conn.serverAddress().host().size() + 12);
	for (auto it1 : bestList) {
		itNode = _mapHeardList.find(it1);
		if (itNode != _mapHeardList.end())
			sizeTotal += itNode->second.hostAddress.host().size();
			sizeTotal += itNode->second.address.host().size() + PEER_ID_SIZE + 12 + ((itNode->second.lastGroupReport > 0) ? Util::Get7BitValueSize((Time::Now() - itNode->second.lastGroupReport) / 1000) : 1);
	}
	_reportBuffer.resize(sizeTotal);

	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(0x0A);
	writer.write8(itPeer->second->peerAddress().host().size() + 4);
	writer.write8(0x0D);
	RTMFP::WriteAddress(writer, itPeer->second->peerAddress(), itPeer->second->peerType);
	writer.write8(_conn.serverAddress().host().size() + 4);
	writer.write8(0x0A);
	RTMFP::WriteAddress(writer, _conn.serverAddress(), RTMFP::ADDRESS_REDIRECTION);
	writer.write8(0);

	for (auto it2 : bestList) {
		itNode = _mapHeardList.find(it2);
		if (itNode != _mapHeardList.end()) {

			UInt64 timeElapsed = (UInt64)((itNode->second.lastGroupReport > 0) ? ((Time::Now() - itNode->second.lastGroupReport) / 1000) : 0);
			TRACE("Group 0A argument - Peer ", itNode->first, " - elapsed : ", timeElapsed) //, " (latency : ", itPeer.second->latency(), ")")
			writer.write8(0x22).write(itNode->second.rawId.data(), PEER_ID_SIZE+2);
			writer.write7BitLongValue(timeElapsed);
			writer.write8(itNode->second.address.host().size() + itNode->second.hostAddress.host().size() + 7);
			writer.write8(0x0A);
			RTMFP::WriteAddress(writer, itNode->second.hostAddress, RTMFP::ADDRESS_REDIRECTION);
			RTMFP::WriteAddress(writer, itNode->second.address, itNode->second.addressType);
			writer.write8(0);
		}
	}

	TRACE("Sending the group report to ", itPeer->first)
	itPeer->second->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
}

bool NetGroup::pushFragment(map<UInt64, MediaPacket>::iterator& itFragment) {
	if (itFragment == _fragments.end())
		return false;

	// Stand alone fragment
	if (itFragment->second.marker == GroupStream::GROUP_MEDIA_DATA) {
		// Is it the next fragment?
		if (_fragmentCounter == 0 || itFragment->first == _fragmentCounter + 1) {
			_fragmentCounter = itFragment->first;

			TRACE("Pushing Data Fragment ", itFragment->first)
			if (itFragment->second.type == AMF::AUDIO || itFragment->second.type == AMF::VIDEO)
				_conn.pushMedia(stream, itFragment->second.time, itFragment->second.payload, itFragment->second.payloadSize(), 0, itFragment->second.type == AMF::AUDIO);

			return pushFragment(++itFragment); // Go to next fragment
		}
	}
	// Splitted packet
	else  {
		// Delete first splitted fragments
		if (_fragmentCounter == 0 && itFragment->second.marker != GroupStream::GROUP_MEDIA_START) {
			TRACE("Ignoring splitted fragment ", itFragment->first, ", we are waiting for a starting fragment")
			_fragments.erase(itFragment);
			return false;
		}

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

			// Buffer the fragments and write to file if audio/video
			if (itStart->second.type == AMF::AUDIO || itStart->second.type == AMF::VIDEO) {
				Buffer	payload(payloadSize);
				BinaryWriter writer(payload.data(), payloadSize);
				auto	itCurrent = itStart;

				do {
					writer.write(itCurrent->second.payload, itCurrent->second.payloadSize());
				} while (itCurrent++ != itEnd);

				TRACE("Pushing splitted packet ", itStart->first, " - ", nbFragments, " fragments for a total size of ", payloadSize)
				_conn.pushMedia(stream, itStart->second.time, payload.data(), payloadSize, 0, itStart->second.type == AMF::AUDIO);
			}

			return pushFragment(++itEnd);
		}
	}

	return false;
}

void NetGroup::updatePushMode() {
	if (_mapPeers.empty())
		return;

	TRACE("Peers connected : ", _mapPeers.size(), " ; Peers known : ", _mapHeardList.size())
	auto itCurrentPusher = _mapPeers.find(_currentPushPeer);
	if (_currentPushMask && itCurrentPusher != _mapPeers.end()) {
		if (_currentPushIsBad) {
			itCurrentPusher->second->badPusher = true;
			itCurrentPusher->second->sendPushMode(0);
			WARN("Push In Analysis - Bad pusher found for ", Format<UInt8>("%.2x", _currentPushMask), " : ", _currentPushPeer)
		} else
			INFO("Push In Analysis - Peer found for ", Format<UInt8>("%.2x", _currentPushMask), " : ", _currentPushPeer)
	}

	// Calculate the current mode
	UInt16 totalMode = 0;
	for (auto it : _mapPeers) {
		if (it.second->publicationInfosSent && it.second->pushInMode)
			totalMode |= it.second->pushInMode;
	}

	if (totalMode == 0xFF) {
		_currentPushMask = 0; // reset push mask to avoid best peer calculation
		return;
	}

	// We determine the next bit to add to the mask
	UInt8 bitNumber = 7;
	if (totalMode != 0) {
		while ((totalMode & (1 << bitNumber)) == 0) // search the 1st bit to 1
			--bitNumber;

		while ((totalMode & (1 << bitNumber)) > 0)// search the next bit to 0
			bitNumber = (bitNumber == 7)? 0 : bitNumber+1;
	} else
		bitNumber = Util::Random<UInt8>() % 8; // First bit is random
	UInt8 toAdd = 1 << bitNumber;

	// Send the push request to the peer with the best latency
	_currentPushMask = toAdd;
	_currentPushPeer = "";
	_currentPushIsBad = true;
	UInt16 bestLatency = numeric_limits<UInt16>::max();
	DEBUG("Push In Analysis - Start of Group Push In mode ", Format<UInt8>("%.2x", _currentPushMask))
	auto itPusher = _mapPeers.end();
	for (auto itPeer = _mapPeers.begin(); itPeer != _mapPeers.end(); itPeer++) {
		if (itPeer->second->publicationInfosSent && !itPeer->second->badPusher && itPeer->second->latency() < bestLatency) {
			bestLatency = itPeer->second->latency();
			_currentPushPeer = itPeer->first;
			itPusher = itPeer;
		}
	}

	if (itPusher != _mapPeers.end()) {
		itPusher->second->sendPushMode(itPusher->second->pushInMode | _currentPushMask);
		if ((totalMode | _currentPushMask) == 0xFF)
			NOTE("Push In Analysis - Push mode 0xFF reached")
	} else
		WARN("Push In Analysis - No peer available for mask ", Format<UInt8>("%.2x", _currentPushMask))
}

void NetGroup::manageBestConnections(set<string>& bestList) {

	// Close old peers
	auto it2Close = _mapPeers.begin();
	while (it2Close != _mapPeers.end()) {
		if (bestList.find(it2Close->first) == bestList.end()) {
			INFO("Closing the connection to peer ", it2Close->second->peerAddress().toString())
			it2Close->second->close(false);
			removePeer(it2Close);
		} else
			it2Close++;
	}

	// Connect to new peers
	for (auto it : bestList) {
		if (_mapPeers.find(it) == _mapPeers.end()) {
			auto itNode = _mapHeardList.find(it);
			if (itNode == _mapHeardList.end())
				WARN("Unable to find the peer ", it) // implementation error, should not happen
			else
				_conn.connect2Peer(it.c_str(), stream.c_str(), itNode->second.rawId, itNode->second.address, itNode->second.addressType, itNode->second.hostAddress);
		}
	}
}

void NetGroup::readAddress(PacketReader& packet, UInt16 size, UInt32 targetCount, const string& newPeerId, const string& rawId, bool noPeerID) {
	UInt8 addressMarker = packet.read8();
	--size;
	if (addressMarker != 0x0A) {
		ERROR("Unexpected address marker : ", Format<UInt8>("%.2x", addressMarker))
		packet.next(size);
		return;
	}

	// Read all addresses
	SocketAddress address, peerAddress, hostAddress(_conn.serverAddress());
	RTMFP::AddressType peerType = RTMFP::ADDRESS_UNSPECIFIED;
	while (size > 0) {

		UInt8 addressType = packet.read8();
		RTMFP::ReadAddress(packet, address, addressType);
		if (!noPeerID && address.family() == IPAddress::IPv4) { // TODO: Handle ivp6

			switch (addressType & 0x0F) {
				case RTMFP::ADDRESS_LOCAL:
				case RTMFP::ADDRESS_PUBLIC:
					peerAddress = address; peerType = (RTMFP::AddressType)addressType; break;
				case RTMFP::ADDRESS_REDIRECTION:
					hostAddress = address; break;
			}
			TRACE("Group Report - IP Address : ", address.toString(), " - type : ", addressType)
		}
		size -= 3 + ((address.family() == IPAddress::IPv6) ? sizeof(in6_addr) : sizeof(in_addr));
	}

	// New Peer ID & address not null => we add it to heard list and connect to him if possible
	if (peerAddress) {
		addPeer2HeardList(newPeerId.c_str(), rawId.data(), peerAddress, peerType, hostAddress);  // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
		if (_mapHeardList.size() < targetCount) // TODO: check that
			_conn.connect2Peer(newPeerId.c_str(), stream.c_str(), rawId, peerAddress, peerType, hostAddress);
	}
}

void NetGroup::sendPullRequests() {
	if (_fragments.empty() || _mapTime2Fragment.empty() || _mapTime2Fragment.rbegin()->first <= 1500)
		return;

	// We wait Fetch Period (1500ms)
	auto itLastReference = _mapTime2Fragment.lower_bound(_mapTime2Fragment.rbegin()->first - 1500);
	if (itLastReference == _mapTime2Fragment.begin() || itLastReference == _mapTime2Fragment.end()) {
		DEBUG("sendPullRequests - Unable to find a reference older than Fetch period (first time : ", _mapTime2Fragment.begin()->first, " ; last time : ", _mapTime2Fragment.rbegin()->first, ")")
		return;
	}
	UInt64 lastFragment = (--itLastReference)->second;
	UInt64 current = _fragmentCounter;

	// Ignore last requests, too much time is elapsed
	if (!_waitingFragments.empty())
		_waitingFragments.clear();
	TRACE("sendPullRequests - current fragment : ", current, " ; last reference : ", lastFragment, " ; last fragment : ", _fragments.rbegin()->first)

	for (auto itFragment = _fragments.find(_fragmentCounter); itFragment != _fragments.end() && itFragment->first <= lastFragment; itFragment++) {
		// Is there a hole?
		if (current + 1 < itFragment->first) {
			for (UInt64 i = current + 1; i < itFragment->first; i++) {
				auto itWait = _waitingFragments.lower_bound(i);

				// Send the Pull request to the first available peer
				auto itPeer = _mapPeers.begin();
				if (RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itPeer, [i](const MAP_PEERS_ITERATOR_TYPE& it) { return it->second->publicationInfosSent && it->second->hasFragment(i); })) {
					itPeer->second->sendPull(i);
					_waitingFragments.emplace(i);
				} else
					WARN("No peer found for fragment ", i)
			}
		}
		current = itFragment->first;
	}
	TRACE("Pull requests done : ", _waitingFragments.size(), " waiting fragments")
}

unsigned int NetGroup::callFunction(const char* function, int nbArgs, const char** args) {
	AMFWriter writer(_conn.poolBuffers());
	writer.amf0 = true;
	writer.packet.write8(0);
	writer.writeString(function, strlen(function));
	for (int i = 0; i < nbArgs; i++) {
		if (args[i])
			writer.writeString(args[i], strlen(args[i]));
	}

	UInt32 currentTime = (_fragments.empty())? 0 : _fragments.rbegin()->second.time;

	// Create and send the fragment
	TRACE("Creating fragment for function ", function, "...")
	onMedia(true, AMF::DATA_AMF3, currentTime, writer.packet.data(), writer.packet.size());

	return 1;
}

