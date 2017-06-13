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

#include "NetGroup.h"
#include "P2PSession.h"
#include "GroupStream.h"
#include "librtmfp.h"
#include "Base/Util.h"

using namespace Base;
using namespace std;

#if defined(_WIN32)
	#define sscanf sscanf_s
#endif
#if !defined(_INC_MATH) // On Android gnu_shared library does not include math.h
	#define log2(VARIABLE) (log(VARIABLE) / log(2))
#endif

UInt32 NetGroup::GroupNode::addressesSize() {
	UInt32 size = 1; // 1 for 0A header
	if (hostAddress.host())
		size += hostAddress.host().size() + 3; // +3 for address type and port
	for (auto itAddress : addresses)
		size += itAddress.first.host().size() + 3; // +3 for address type and port
	return size;
}

const string& NetGroup::GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress) {
	
	static UInt8 tmp[PEER_ID_SIZE];
	EVP_Digest(rawId, PEER_ID_SIZE+2, tmp, NULL, EVP_sha256(), NULL);
	String::Assign(groupAddress, String::Hex(tmp, PEER_ID_SIZE));
	TRACE("Group address : ", groupAddress)
	return groupAddress;
}

double NetGroup::estimatedPeersCount() {

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

		if (itFirst->first > _myGroupAddress)  // Current == N+1?
			RTMFP::GetPreviousIt(_mapGroupAddress, itFirst);
		else
			RTMFP::GetNextIt(_mapGroupAddress, itLast); // Current == N-1

		RTMFP::GetPreviousIt(_mapGroupAddress, itFirst);
		RTMFP::GetNextIt(_mapGroupAddress, itLast);
	}
	
	TRACE("First peer (N-2) = ", itFirst->first)
	TRACE("Last peer (N+2) = ", itLast->first)

	long long valFirst = 0, valLast = 0;
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

NetGroup::NetGroup(UInt16 mediaId, const string& groupId, const string& groupTxt, const string& streamName, RTMFPSession& conn, RTMFPGroupConfig* parameters) : groupParameters(parameters),
	idHex(groupId), idTxt(groupTxt), stream(streamName), _conn(conn), _pListener(NULL), _groupMediaPublisher(_mapGroupMedias.end()), FlashHandler(0, mediaId) {
	_onNewMedia = [this](const string& peerId, shared_ptr<PeerMedia>& pPeerMedia, const string& streamName, const string& streamKey, BinaryReader& packet) {

		shared_ptr<RTMFPGroupConfig> pParameters(new RTMFPGroupConfig());
		memcpy(pParameters.get(), groupParameters, sizeof(RTMFPGroupConfig)); // TODO: make a initializer
		ReadGroupConfig(pParameters, packet);  // TODO: check groupParameters

		if (streamName != stream) {
			INFO("New stream available in the group but not registered : ", streamName)
			return false;
		}

		// Create the Group Media if it does not exists
		auto itGroupMedia = _mapGroupMedias.lower_bound(streamKey);
		if (itGroupMedia == _mapGroupMedias.end() || itGroupMedia->first != streamKey) {
			itGroupMedia = _mapGroupMedias.emplace_hint(itGroupMedia, piecewise_construct, forward_as_tuple(streamKey), forward_as_tuple(stream, streamKey, pParameters));
			itGroupMedia->second.onGroupPacket = _onGroupPacket;
			DEBUG("Creation of GroupMedia ", itGroupMedia->second.id, " for the stream ", stream, " :\n", String::Hex(BIN streamKey.data(), streamKey.size()))

			// Send the group media infos to each other peers
			for (auto itPeer : _mapPeers) {
				if (itPeer.first == peerId)
					continue;
				auto pPeerMedia = itPeer.second->getPeerMedia(itGroupMedia->first);
				itGroupMedia->second.sendGroupMedia(pPeerMedia);
			}
		}
		
		// And finally try to add the peer and send the GroupMedia subscription
		itGroupMedia->second.addPeer(peerId, pPeerMedia);
		return true;
	};
	_onClosedMedia = [this](const string& streamKey, UInt64 lastFragment) {
		// TODO: not sure we need to do something here (GroupMedia close message is not received by all peers)
		auto itGroupMedia = _mapGroupMedias.find(streamKey);
		if (itGroupMedia != _mapGroupMedias.end())
			DEBUG("GroupMedia ", itGroupMedia->second.id, " is closing (last fragment : ", lastFragment, ")")
	};
	_onGroupReport = [this](P2PSession* pPeer, BinaryReader& packet, bool sendMediaSubscription) {
		
		auto itNode = _mapHeardList.find(pPeer->peerId);
		if (itNode != _mapHeardList.end())
			itNode->second.lastGroupReport = Time::Now(); // Record the time of last Group Report received to build our Group Report

		// If there are new peers : manage the best list
		if (readGroupReport(packet))
			updateBestList();

		// First Viewer = > create listener
		if (_groupMediaPublisher != _mapGroupMedias.end() && !_pListener) {
			Exception ex;
			if (!(_pListener = _conn.startListening<GroupListener>(ex, stream, idTxt))) {
				WARN(ex) // TODO : See if we can send a specific answer
				return;
			}
			INFO("First viewer play request, starting to play Stream ", stream)
			_pListener->onMedia = _groupMediaPublisher->second.onMedia;
			_conn.publishReady = true; // A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
		}

		if (!pPeer->groupReportInitiator) {
			sendGroupReport(pPeer, false);
			_lastReport.update();
		}
		else
			pPeer->groupReportInitiator = false;

		// Send the Group Media Subscriptions if not already sent
		if (sendMediaSubscription && (_bestList.empty() || _bestList.find(pPeer->peerId) != _bestList.end())) {
			for (auto& itGroupMedia : _mapGroupMedias) {
				if (itGroupMedia.second.groupParameters->isPublisher || itGroupMedia.second.hasFragments()) {
					auto pPeerMedia = pPeer->getPeerMedia(itGroupMedia.first);
					itGroupMedia.second.sendGroupMedia(pPeerMedia);
				}
			}
		}
	};
	_onGroupBegin = [this](P2PSession* pPeer) {

		 // When we receive the 0E NetGroup message type we must send the group report if not already sent
		auto itNode = _mapHeardList.find(pPeer->peerId);
		if (itNode == _mapHeardList.end() || pPeer->groupFirstReportSent)
			return;

		sendGroupReport(pPeer, true);
		_lastReport.update();
	};
	_onGroupPacket = [this](UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {
		// Go back to Flash handler
		return FlashHandler::process(type, time, packet, 0, 0, lostRate);
	};
	_onPeerClose = [this](const string& peerId) {
		removePeer(peerId);
	};
	_onGroupAskClose = [this](const string& peerId) {
		if (_bestList.empty())
			return true; // do not disconnect peer if we have not calculated the best list (can it happen?)

		return _bestList.find(peerId) != _bestList.end(); // if peer is not in the Best list return False tu close the main flow, otherwise keep connection open
	};

	GetGroupAddressFromPeerId(_conn.rawId().c_str(), _myGroupAddress);

	// If Publisher create a new GroupMedia
	if (groupParameters->isPublisher) {

		// Generate the stream key
		string streamKey("\x21\x01");
		streamKey.resize(0x22);
		Util::Random(BIN streamKey.data() + 2, 0x20); // random serie of 32 bytes

		shared_ptr<RTMFPGroupConfig> pParameters(new RTMFPGroupConfig());
		memcpy(pParameters.get(), groupParameters, sizeof(RTMFPGroupConfig)); // TODO: make a initializer
		_groupMediaPublisher = _mapGroupMedias.emplace(piecewise_construct, forward_as_tuple(streamKey), forward_as_tuple(stream, streamKey, pParameters)).first;
		_groupMediaPublisher->second.onGroupPacket = nullptr; // we do not need to follow the packet
	}
}

bool NetGroup::messageHandler(const string& name, AMFReader& message, UInt64 flowId, UInt64 writerId, double callbackHandler) {

	/*** NetGroup Player ***/
	if (name == "closeStream") {
		INFO("Stream ", streamId, " is closing...")
		return false;
	}
	return FlashHandler::messageHandler(name, message, flowId, writerId, callbackHandler);
}

void NetGroup::stopListener() {
	if (!_pListener)
		return;

	// Terminate the GroupMedia properly
	if (_groupMediaPublisher != _mapGroupMedias.end()) {
		_groupMediaPublisher->second.closePublisher();
		_groupMediaPublisher->second.onMedia = nullptr;
	}
	_groupMediaPublisher = _mapGroupMedias.end();
	_pListener->onMedia = nullptr;
	_conn.stopListening(idTxt);
	_pListener = NULL;
}

void NetGroup::close() {

	DEBUG("Closing group ", idTxt, "...")

	stopListener();

	for (auto& itGroupMedia : _mapGroupMedias)
		itGroupMedia.second.onGroupPacket = nullptr;
	_mapGroupMedias.clear();

	MAP_PEERS_ITERATOR_TYPE itPeer = _mapPeers.begin();
	while (itPeer != _mapPeers.end())
		removePeer(itPeer++); // (doesn't delete peer from the heard list but we don't care)
}

void NetGroup::addPeer2HeardList(const string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const SocketAddress& hostAddress, UInt64 timeElapsed) {

	auto it = _mapHeardList.lower_bound(peerId);
	if (it != _mapHeardList.end() && it->first == peerId) {
		DEBUG("The peer ", peerId, " is already known")
		return;
	}

	string groupAddress;
	_mapGroupAddress.emplace(piecewise_construct, forward_as_tuple(GetGroupAddressFromPeerId(rawId, groupAddress).c_str()), forward_as_tuple(peerId.c_str()));
	it = _mapHeardList.emplace_hint(it, piecewise_construct, forward_as_tuple(peerId.c_str()), forward_as_tuple(rawId, groupAddress, listAddresses, hostAddress, timeElapsed));
	DEBUG("Peer ", it->first, " added to heard list")
}

bool NetGroup::addPeer(const string& peerId, shared_ptr<P2PSession> pPeer) {

	auto itHeardList = _mapHeardList.find(peerId);
	if (itHeardList == _mapHeardList.end()) {
		ERROR("Unknown peer to add : ", peerId)
		return false;
	}

	auto it = _mapPeers.lower_bound(peerId);
	if (it != _mapPeers.end() && it->first == peerId) {
		ERROR("Unable to add the peer ", peerId, ", it already exists")
		return false;
	}
	DEBUG("Adding the peer ", peerId, " to the Best List")

	// Update the heard list addresses
	itHeardList->second.hostAddress = pPeer->hostAddress;
	for (auto itAddress : pPeer->addresses())
		if (itAddress.second & RTMFP::ADDRESS_LOCAL)
			itHeardList->second.addresses.emplace(itAddress.first, itAddress.second);

	_mapPeers.emplace_hint(it, peerId, pPeer);

	pPeer->onNewMedia = _onNewMedia;
	pPeer->onClosedMedia = _onClosedMedia;
	pPeer->onPeerGroupReport = _onGroupReport;
	pPeer->onPeerGroupBegin = _onGroupBegin;
	pPeer->onPeerClose = _onPeerClose;
	pPeer->onPeerGroupAskClose = _onGroupAskClose;

	buildBestList(_myGroupAddress, _bestList); // rebuild the best list to know if the peer is in it
	return true;
}

void NetGroup::removePeer(const string& peerId) {

	auto itPeer = _mapPeers.find(peerId);
	if (itPeer == _mapPeers.end())
		DEBUG("The peer ", peerId, " is already removed from the Best list")
	else
		removePeer(itPeer);
}

void NetGroup::removePeer(MAP_PEERS_ITERATOR_TYPE itPeer) {
	DEBUG("Deleting peer ", itPeer->first, " from the NetGroup Best List")

	itPeer->second->onNewMedia = nullptr;
	itPeer->second->onClosedMedia = nullptr;
	itPeer->second->onPeerGroupReport = nullptr;
	itPeer->second->onPeerGroupBegin = nullptr;
	itPeer->second->onPeerClose = nullptr;
	itPeer->second->onPeerGroupAskClose = nullptr;
	_mapPeers.erase(itPeer);
}

bool NetGroup::checkPeer(const string& peerId) {

	return _mapPeers.find(peerId) == _mapPeers.end();
}

void NetGroup::manage() {

	// Manage the Best list
	if (_lastBestCalculation.isElapsed(NETGROUP_BEST_LIST_DELAY))
		updateBestList();

	// Send the Group Report message (0A) to a random connected peer
	if (_lastReport.isElapsed(NETGROUP_REPORT_DELAY)) {

		auto itRandom = _mapPeers.begin();
		if (RTMFP::GetRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itRandom, [](const MAP_PEERS_ITERATOR_TYPE it) { return it->second->status == RTMFP::CONNECTED; }))
			sendGroupReport(itRandom->second.get(), true);

		// Clean the Heard List from old peers
		Int64 now = Time::Now();
		auto itHeardList = _mapHeardList.begin();
		while (itHeardList != _mapHeardList.end()) {
			if ((_mapPeers.find(itHeardList->first) == _mapPeers.end()) && now > itHeardList->second.lastGroupReport && ((now - itHeardList->second.lastGroupReport) > NETGROUP_PEER_TIMEOUT)) {
				DEBUG("Peer ", itHeardList->first, " timeout (", NETGROUP_PEER_TIMEOUT, "ms elapsed) - deleting from the heard list...")
				auto itGroupAddress = _mapGroupAddress.find(itHeardList->second.groupAddress);
				if (itGroupAddress == _mapGroupAddress.end())
					WARN("Unable to find peer ", itHeardList->first, " in the map of Group Addresses") // should not happen
				else
					_mapGroupAddress.erase(itGroupAddress);
				_mapHeardList.erase(itHeardList++);
				continue;
			}
			++itHeardList;
		}

		_lastReport.update();
	}

	// Manage all group medias
	auto itGroupMedia = _mapGroupMedias.begin();
	while (itGroupMedia != _mapGroupMedias.end()) {
		if (!itGroupMedia->second.manage()) {
			DEBUG("Deletion of GroupMedia ", itGroupMedia->second.id, " for the stream ", stream)
			if (_groupMediaPublisher == itGroupMedia)
				_groupMediaPublisher = _mapGroupMedias.end();
			_mapGroupMedias.erase(itGroupMedia++);
		}
		else
			++itGroupMedia;
	}
}

void NetGroup::updateBestList() {

	buildBestList(_myGroupAddress, _bestList);
	manageBestConnections();
	_lastBestCalculation.update();
}

void NetGroup::buildBestList(const string& groupAddress, set<string>& bestList) {
	bestList.clear();

	// Find the 6 closest peers
	if (_mapGroupAddress.size() <= 6) {
		for (auto it : _mapGroupAddress)
			bestList.emplace(it.second);
	}
	else { // More than 6 peers

		// First we search the first of the 6 peers
		auto itFirst = _mapGroupAddress.lower_bound(groupAddress);
		if (itFirst == _mapGroupAddress.end())
			itFirst = --(_mapGroupAddress.end());
		for (int i = 0; i < 2; i++)
			RTMFP::GetPreviousIt(_mapGroupAddress, itFirst);

		// Then we add the 6 peers
		for (int j = 0; j < 6; j++) {
			bestList.emplace(itFirst->second);
			RTMFP::GetNextIt(_mapGroupAddress, itFirst);
		}
	}

	if (_mapGroupAddress.size() > 6) {

		// Find the 6 lowest latency
		deque<shared_ptr<P2PSession>> queueLatency;
		if (!_mapPeers.empty()) {
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
		}

		// Add one random peer
		if (_mapGroupAddress.size() > bestList.size()) {

			auto itRandom = _mapGroupAddress.begin();
			if (RTMFP::GetRandomIt<map<string, string>, map<string, string>::iterator>(_mapGroupAddress, itRandom, [bestList](const map<string, string>::iterator& it) { return bestList.find(it->second) != bestList.end(); }))
				bestList.emplace(itRandom->second);
		}

		// Find 2 log(N) peers with location + 1/2, 1/4, 1/8 ...
		UInt32 bests = bestList.size(), estimatedCount = targetNeighborsCount();
		if (_mapGroupAddress.size() > bests && estimatedCount > bests) {
			UInt32 count = estimatedCount - bests;
			if (count > _mapGroupAddress.size() - bests)
				count = _mapGroupAddress.size() - bests;

			auto itNode = _mapGroupAddress.lower_bound(groupAddress);
			UInt32 rest = (_mapGroupAddress.size() / 2) - 1;
			int step = rest / (2 * count);
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

	if (bestList == _bestList)
		INFO("Best Peer - Peers connected : ", _mapPeers.size(), "/", _mapGroupAddress.size(), " ; target count : ", _bestList.size(), " ; GroupMedia count : ", _mapGroupMedias.size())
}

void NetGroup::sendGroupReport(P2PSession* pPeer, bool initiator) {
	TRACE("Preparing the Group Report message (type 0A) for peer ", pPeer->peerId)

	auto itNode = _mapHeardList.find(pPeer->peerId);
	if (itNode == _mapHeardList.end()) {
		ERROR("Unable to find the peer ", pPeer->peerId, " in the Heard list") // implementation error
		return;
	}

	set<string> bestList;
	buildBestList(itNode->second.groupAddress, bestList);

	// Calculate the total size to allocate sufficient memory
	UInt32 sizeTotal = (UInt32)(pPeer->address().host().size() + _conn.address().host().size() + 12);
	Int64 timeNow(Time::Now());
	for (auto it1 : bestList) {
		itNode = _mapHeardList.find(it1);
		if (itNode != _mapHeardList.end())
			sizeTotal += itNode->second.addressesSize() + PEER_ID_SIZE + 5 + ((itNode->second.lastGroupReport > 0) ? Binary::Get7BitValueSize((UInt32)((timeNow - itNode->second.lastGroupReport) / 1000)) : 1);
	}
	_reportBuffer.resize(sizeTotal, false);

	BinaryWriter writer(_reportBuffer.data(), _reportBuffer.size());
	writer.write8(0x0A);
	writer.write8(pPeer->address().host().size() + 4);
	writer.write8(0x0D);
	RTMFP::WriteAddress(writer, pPeer->address(), RTMFP::ADDRESS_PUBLIC);
	writer.write8(_conn.address().host().size() + 4);
	writer.write8(0x0A);
	RTMFP::WriteAddress(writer, _conn.address(), RTMFP::ADDRESS_REDIRECTION);
	writer.write8(0);

	for (auto it2 : bestList) {
		itNode = _mapHeardList.find(it2);
		if (itNode != _mapHeardList.end()) {

			UInt64 timeElapsed = (UInt64)((itNode->second.lastGroupReport > 0) ? ((timeNow - itNode->second.lastGroupReport) / 1000) : 0);
			TRACE("Group 0A argument - Peer ", itNode->first, " - elapsed : ", timeElapsed) //, " (latency : ", itPeer.second->latency(), ")")
			writer.write8(0x22).write(itNode->second.rawId.data(), PEER_ID_SIZE+2);
			writer.write7BitLongValue(timeElapsed);
			writer.write8(itNode->second.addressesSize());
			writer.write8(0x0A);
			if (itNode->second.hostAddress)
				RTMFP::WriteAddress(writer, itNode->second.hostAddress, RTMFP::ADDRESS_REDIRECTION);
			for (auto itAddress : itNode->second.addresses)
				RTMFP::WriteAddress(writer, itAddress.first, itAddress.second);
			writer.write8(0);
		}
	}

	TRACE("Sending the group report to ", pPeer->peerId)
	pPeer->groupReportInitiator = initiator;
	pPeer->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
}

void NetGroup::manageBestConnections() {

	// Close old peers
	int nbDisconnect = _mapPeers.size() - _bestList.size(); // trick to keep the target count of peers
	auto it2Close = _mapPeers.begin();
	while (nbDisconnect > 0 && it2Close != _mapPeers.end()) {
		if (_bestList.find(it2Close->first) == _bestList.end() && it2Close->second->askPeer2Disconnect())
			--nbDisconnect;
		++it2Close;
	}

	// Connect to new peers
	int nbConnect = _bestList.size() - _mapPeers.size(); // trick to keep the target count of peers
	auto it2Connect = _bestList.begin();
	while (nbConnect > 0 && it2Connect != _bestList.end()) {
		if (_mapPeers.find(*it2Connect) == _mapPeers.end()) {
			auto itNode = _mapHeardList.find(*it2Connect);
			if (itNode == _mapHeardList.end())
				WARN("Unable to find the peer ", *it2Connect) // implementation error, should not happen
			else {
				DEBUG("Best Peer - Connecting to peer ", *it2Connect, "...")
				_conn.connect2Peer(it2Connect->c_str(), stream.c_str(), itNode->second.addresses, itNode->second.hostAddress);
				--nbConnect;
			}
		}
		++it2Connect;
	}
}

unsigned int NetGroup::callFunction(const char* function, int nbArgs, const char** args) {

	for (auto& itGroupMedia : _mapGroupMedias)
		itGroupMedia.second.callFunction(function, nbArgs, args);

	return 1;
}

void NetGroup::ReadGroupConfig(shared_ptr<RTMFPGroupConfig>& parameters, BinaryReader& packet) {

	// Update the NetGroup stream properties
	UInt8 size = 0, id = 0;
	unsigned int value = 0;
	parameters->availabilitySendToAll = 0;
	while (packet.available()) {
		if ((size = packet.read8()) == 0)
			continue;
		id = packet.read8();
		value = (size > 1) ? (unsigned int)packet.read7BitLongValue() : 0;
		switch (id) {
		case NetGroup::UNKNWON_PARAMETER:
			break;
		case NetGroup::WINDOW_DURATION:
			parameters->windowDuration = value;
			TRACE("Window Duration : ", parameters->windowDuration, "ms");
			break;
		case NetGroup::OBJECT_ENCODING:
			if (value != 300000)
				ERROR("Unexpected object encoding value : ", value) // TODO: not sure it is object encoding!
				break;
		case NetGroup::UPDATE_PERIOD:
			if (value != parameters->availabilityUpdatePeriod) {
				parameters->availabilityUpdatePeriod = value;
				TRACE("Avaibility Update period : ", parameters->availabilityUpdatePeriod, "ms");
			}
			break;
		case NetGroup::SEND_TO_ALL:
			parameters->availabilitySendToAll = 1;
			TRACE("Availability Send to All ON");
			return;
		case NetGroup::FETCH_PERIOD:
			parameters->fetchPeriod = value;
			TRACE("Fetch period : ", parameters->fetchPeriod, "ms"); break;
			break;
		}
	}
}

bool NetGroup::readGroupReport(BinaryReader& packet) {
	string tmp, newPeerId, rawId;
	SocketAddress myAddress, serverAddress;
	RTMFP::AddressType addressType;
	PEER_LIST_ADDRESS_TYPE listAddresses;
	SocketAddress hostAddress(_conn.address());

	UInt8 size = packet.read8();
	while (size == 1) { // TODO: check what this means
		packet.next();
		size = packet.read8();
	}

	// Read my address & the far peer addresses
	UInt8 tmpMarker = packet.read8();
	if (tmpMarker != 0x0D) {
		ERROR("Unexpected marker : ", String::Format<UInt8>("%.2x", tmpMarker), " - Expected 0D")
		return false;
	}
	RTMFP::ReadAddress(packet, myAddress, addressType);
	TRACE("Group Report - My address : ", myAddress)
	
	size = packet.read8();
	tmpMarker = packet.read8();
	if (tmpMarker != 0x0A) {
		ERROR("Unexpected marker : ", String::Format<UInt8>("%.2x", tmpMarker), " - Expected 0A")
		return false;
	}
	BinaryReader peerAddressReader(packet.current(), size - 1);
	RTMFP::ReadAddresses(peerAddressReader, listAddresses, hostAddress, [](const SocketAddress&, RTMFP::AddressType) {});
	packet.next(size - 1);

	// Loop on each peer of the NetGroup
	bool newPeers = false;
	while (packet.available() > 4) {
		if ((tmpMarker = packet.read8()) != 00) {
			ERROR("Unexpected marker : ", String::Format<UInt8>("%.2x", tmpMarker), " - Expected 00")
			break;
		}
		size = packet.read8();
		if (size == 0x22) {
			packet.read(size, rawId);
			String::Assign(newPeerId, String::Hex(BIN rawId.data() + 2, PEER_ID_SIZE));
			if (String::ICompare(rawId, "\x21\x0F", 2) != 0) {
				ERROR("Unexpected parameter : ", newPeerId, " - Expected Peer Id")
				break;
			}
			TRACE("Group Report - Peer ID : ", newPeerId)
		}
		else if (size > 7)
			packet.next(size); // ignore the addresses if peerId not set
		else
			TRACE("Empty parameter...")

		UInt64 time = packet.read7BitLongValue();
		TRACE("Group Report - Time elapsed : ", time)
		size = packet.read8(); // Addresses size

		// New peer, read its addresses
		if (newPeerId != _conn.peerId() && *packet.current() == 0x0A) {

			BinaryReader addressReader(packet.current() + 1, size - 1); // +1 to ignore 0A
			auto itHeardList = _mapHeardList.find(newPeerId);
			// New peer? => add it to heard list
			if (itHeardList == _mapHeardList.end()) {
				hostAddress.reset();
				listAddresses.clear();
				RTMFP::ReadAddresses(addressReader, listAddresses, hostAddress, [](const SocketAddress&, RTMFP::AddressType) {});
				newPeers = true;
				addPeer2HeardList(newPeerId.c_str(), rawId.data(), listAddresses, hostAddress, time);  // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
			} 
			// Else update the addresses
			else
				RTMFP::ReadAddresses(addressReader, itHeardList->second.addresses, itHeardList->second.hostAddress, [](const SocketAddress&, RTMFP::AddressType) {});
				
		}
		packet.next(size);
	}

	return newPeers;
}
