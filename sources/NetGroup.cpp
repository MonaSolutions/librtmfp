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

UInt32 NetGroup::AddressesSize(const SocketAddress& host, const PEER_LIST_ADDRESS_TYPE& addresses) {
	UInt32 size = 1; // 1 for 0A header
	if (host.host())
		size += host.host().size() + 3; // +3 for address type and port
	for (auto& itAddress : addresses)
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

UInt32 NetGroup::TargetNeighborsCount(double peersCount) {

	UInt32 targetNeighbor = (UInt32)(log2(peersCount)) + 13;
	TRACE("estimatedMemberCount : ", peersCount, " ; targetNeighbor : ", targetNeighbor)

	return targetNeighbor;
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

		if (itFirst->first > _myGroupAddress)  // Current == N+1
			RTMFP::GetPreviousIt(_mapGroupAddress, itFirst);
		else
			RTMFP::GetNextIt(_mapGroupAddress, itLast); // Current == N-1 (TODO: can it happen?)

		RTMFP::GetPreviousIt(_mapGroupAddress, itFirst);
		RTMFP::GetNextIt(_mapGroupAddress, itLast);
	}
	
	TRACE("First peer (N-2) = ", itFirst->first)
	TRACE("Last peer (N+2) = ", itLast->first)

	unsigned long long valFirst = 0, valLast = 0;
	sscanf(itFirst->first.substr(0, 16).c_str(), "%llx", &valFirst);
	sscanf(itLast->first.substr(0, 16).c_str(), "%llx", &valLast);

	// Then calculate the total	
	if (valLast > valFirst)
		return (MAX_PEER_COUNT / (double(valLast - valFirst) / 4)) + 1;
	else
		return (MAX_PEER_COUNT / (double(valLast - valFirst + MAX_PEER_COUNT) / 4)) + 1;
}

NetGroup::NetGroup(const Base::Timer& timer, UInt16 mediaId, const string& groupId, const string& groupTxt, const string& streamName, RTMFPSession& conn, RTMFPGroupConfig* parameters, 
	bool audioReliable, bool videoReliable) : groupParameters(parameters), _p2pAble(false), idHex(groupId), idTxt(groupTxt), stream(streamName), _conn(conn), _pListener(NULL), _timer(timer),
	_groupMediaPublisher(_mapGroupMedias.end()), _countP2P(0), _countP2PSuccess(0), _audioReliable(audioReliable), _videoReliable(videoReliable), _reportBuffer(NETGROUP_MAX_REPORT_SIZE), FlashHandler(0, mediaId) {
	_onNewMedia = [this](const string& peerId, shared_ptr<PeerMedia>& pPeerMedia, const string& streamName, const string& streamKey, BinaryReader& packet) {

		if (streamName != stream) {
			INFO("New stream available in the group but not registered : ", streamName)
			return false;
		}

		shared_ptr<RTMFPGroupConfig> pParameters(new RTMFPGroupConfig());
		memcpy(pParameters.get(), groupParameters, sizeof(RTMFPGroupConfig)); // TODO: make a initializer
		ReadGroupConfig(pParameters, packet);  // TODO: check groupParameters

		// Create the Group Media if it does not exists
		auto itGroupMedia = _mapGroupMedias.lower_bound(streamKey);
		if (itGroupMedia == _mapGroupMedias.end() || itGroupMedia->first != streamKey) {
			if (groupParameters->isPublisher) {
				DEBUG("New GroupMedia ignored, we are the publisher")
				return false;
			}
			itGroupMedia = _mapGroupMedias.emplace_hint(itGroupMedia, piecewise_construct, forward_as_tuple(streamKey), forward_as_tuple(_timer, stream, streamKey, pParameters, _audioReliable, _videoReliable));
			itGroupMedia->second.onNewFragment = _onNewFragment;
			itGroupMedia->second.onRemovedFragments = _onRemovedFragments;
			itGroupMedia->second.onStartProcessing = _onStartProcessing;
			DEBUG("Creation of GroupMedia ", itGroupMedia->second.id, " for the stream ", stream, " :\n", String::Hex(BIN streamKey.data(), streamKey.size()))

			// Send the group media infos to each other peers
			for (auto& itPeer : _mapPeers) {
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
		// (GroupMedia close message is not received by all peers)
		auto itGroupMedia = _mapGroupMedias.find(streamKey);
		if (itGroupMedia != _mapGroupMedias.end()) {
			DEBUG("GroupMedia ", itGroupMedia->second.id, " is closing (last fragment : ", lastFragment, ")")
			itGroupMedia->second.close(lastFragment);
		}
	};
	_onGroupReport = [this](P2PSession* pPeer, BinaryReader& packet, bool sendMediaSubscription) {
		
		auto itNode = _mapHeardList.find(pPeer->peerId);
		if (itNode == _mapHeardList.end()) {
			WARN("Group report received from unknown peer ", pPeer->peerId) // should not happen
			return;
		}

		// Read the Group Report & try to update the Best List if new peers are found
		if (readGroupReport(itNode, packet) && !_bestList.size()) { 
			updateBestList(); // Note: if we don't receive any group report we don't update the best list
			_timer.set(_onBestList, NETGROUP_BEST_LIST_DELAY);
		}

		// First Viewer = > create listener
		if (_groupMediaPublisher != _mapGroupMedias.end() && !_pListener) {
			Exception ex;
			if (!(_pListener = _conn.startListening<GroupListener>(ex, stream, idTxt))) {
				WARN(ex) // TODO : See if we can send a specific answer
				return;
			}
			INFO("First viewer play request, starting to play Stream ", stream)
			_pListener->onMedia = _groupMediaPublisher->second.onMedia;
			_pListener->onFlush = _groupMediaPublisher->second.onFlush;
			_conn.onConnected2Group(); // A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
		}

		// Group Report response?
		if (!pPeer->groupReportInitiator) {
			sendGroupReport(pPeer, false);
			_lastReport.update();
		}
		else
			pPeer->groupReportInitiator = false;

		// Send the Group Media Subscriptions if not already sent
		if (sendMediaSubscription) {
			DEBUG("Sending GroupMedia subscription (", _mapGroupMedias.size(), ") to peer ", pPeer->name(), " (publisher: ", groupParameters->isPublisher>0, ")")
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
	_onPeerClose = [this](const string& peerId) {

		removePeer(peerId);
	};
	_onGroupAskClose = [this](const string& peerId) {

		// We refuse all disconnection until reaching the best list size, then only disconnect peers not in the best list
		return !_bestList.empty() && (_mapPeers.size() > _bestList.size()) && (_bestList.find(peerId) == _bestList.end());
	};
	_onCleanHeardList = [this](UInt32 count) {
		cleanHeardList();
		return NETGROUP_CLEAN_DELAY;
	};
	_onBestList = [this](UInt32 count) {
		updateBestList();
		return NETGROUP_BEST_LIST_DELAY;
	};
	_timer.set(_onCleanHeardList, NETGROUP_CLEAN_DELAY);

	GetGroupAddressFromPeerId(_conn.rawId().c_str(), _myGroupAddress);

	// If Publisher create a new GroupMedia
	if (groupParameters->isPublisher) {

		// Generate the stream key
		string streamKey("\x21\x01");
		streamKey.resize(0x22);
		Util::Random(BIN streamKey.data() + 2, 0x20); // random serie of 32 bytes

		shared_ptr<RTMFPGroupConfig> pParameters(new RTMFPGroupConfig());
		memcpy(pParameters.get(), groupParameters, sizeof(RTMFPGroupConfig)); // TODO: make a initializer
		_groupMediaPublisher = _mapGroupMedias.emplace(piecewise_construct, forward_as_tuple(streamKey), forward_as_tuple(_timer, stream, streamKey, pParameters, _audioReliable, _videoReliable)).first;
	}
	// Else it's a player, create the fragment controler
	else {
		_pGroupBuffer.reset(new GroupBuffer());
		_pGroupBuffer->onNextPacket = [this](GroupBuffer::Result& result) { // Executed in the GroupBuffer Thread
			// Use Flash handler to process the packets (TODO: delete the group if return false?)
			for (RTMFP::MediaPacket& mediaPacket : result)
				FlashHandler::process(mediaPacket.type, mediaPacket.time, mediaPacket, 0, 0, 0, false);
		};

		_onNewFragment = [this](UInt32 groupMediaId, const shared_ptr<GroupFragment>& pFragment) {
			Exception ex;
			AUTO_ERROR(_pGroupBuffer->add(ex, groupMediaId, pFragment), "GroupBuffer ", groupMediaId, " add fragment")
		};
		_onRemovedFragments = [this](UInt32 groupMediaId, UInt64 fragmentId) {
			Exception ex;
			AUTO_ERROR(_pGroupBuffer->removeFragments(ex, groupMediaId, fragmentId), "GroupBuffer ", groupMediaId, " remove fragments")
		};
		_onStartProcessing = [this](UInt32 groupMediaId) {
			Exception ex;
			AUTO_ERROR(_pGroupBuffer->startProcessing(ex, groupMediaId), " GroupBuffer ", groupMediaId, " start processing")
		};
	}
}

NetGroup::~NetGroup() {
	close();
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
		_groupMediaPublisher->second.onFlush = nullptr;
	}
	_groupMediaPublisher = _mapGroupMedias.end();
	_pListener->onMedia = nullptr;
	_conn.stopListening(idTxt);
	_pListener = NULL;
}

void NetGroup::close() {

	DEBUG("Closing group ", idTxt, "...")

	_timer.set(_onCleanHeardList, 0);
	_timer.set(_onBestList, 0);

	if (_pGroupBuffer) {
		_pGroupBuffer->onNextPacket = nullptr;
		_pGroupBuffer.reset();
	}

	stopListener();

	for (auto& itGroupMedia : _mapGroupMedias) {
		itGroupMedia.second.onNewFragment = nullptr;
		itGroupMedia.second.onRemovedFragments = nullptr;
		itGroupMedia.second.onStartProcessing = nullptr;
	}
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

void NetGroup::handlePeerDisconnection(const string& peerId) {

	auto itHeardList = _mapHeardList.find(peerId);
	if (itHeardList == _mapHeardList.end() || itHeardList->second.died)
		return;

	itHeardList->second.died = true; // we don't delete the peer, just not send connection and group report with it anymore
	--_countP2P; // this attempt was not a fail
}

bool NetGroup::addPeer(const string& peerId, shared_ptr<P2PSession> pPeer) {

	auto itHeardList = _mapHeardList.find(peerId);
	if (itHeardList == _mapHeardList.end()) {
		ERROR("Unknown peer to add : ", peerId) // implementation error
		return false;
	}

	auto it = _mapPeers.lower_bound(peerId);
	if (it != _mapPeers.end() && it->first == peerId) {
		ERROR("Unable to add the peer ", peerId, ", it already exists")
		return false;
	}
	DEBUG("Adding the peer ", peerId, " to the Best List")
	if (!_p2pAble)
		_p2pAble = true;

	if (pPeer->initiator())
		++_countP2PSuccess;

	// Update the heard list addresses
	itHeardList->second.hostAddress = pPeer->hostAddress;
	for (auto& itAddress : pPeer->addresses())
		if (itAddress.second & RTMFP::ADDRESS_PUBLIC) // In Netgroup report we just save the public addresses
			itHeardList->second.addresses.emplace(itAddress.first, itAddress.second);

	_mapPeers.emplace_hint(it, peerId, pPeer);

	pPeer->onNewMedia = _onNewMedia;
	pPeer->onClosedMedia = _onClosedMedia;
	pPeer->onPeerGroupReport = _onGroupReport;
	pPeer->onPeerGroupBegin = _onGroupBegin;
	pPeer->onPeerClose = _onPeerClose;
	pPeer->onPeerGroupAskClose = _onGroupAskClose;

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

void NetGroup::manage() {

	// P2P unable, we reset the connection
	if (!groupParameters->isPublisher && !_p2pAble && _p2pEntities.size() >= NETGROUP_MIN_PEERS_TIMEOUT && _p2pAbleTime.isElapsed(NETGROUP_TIMEOUT_P2PABLE)) {
		ERROR(NETGROUP_TIMEOUT_P2PABLE, "ms without p2p establishment, we close the session...")
		_conn.handleNetGroupException(RTMFP::P2P_ESTABLISHMENT);
		return;
	}
	
	// P2P rate too low, we reset the connection
	if (!groupParameters->isPublisher && !groupParameters->disableRateControl && _p2pRateTime.isElapsed(NETGROUP_TIMEOUT_P2PRATE)) {
		// Count > 10 to be sure that we have sufficient tries
		if (_countP2P > 10 && ((_countP2PSuccess*100) / _countP2P) < NETGROUP_RATE_MIN) {
			ERROR("P2p connection rate is inferior to ", NETGROUP_RATE_MIN, ", we close the session...")
			_conn.handleNetGroupException(RTMFP::P2P_RATE);
			return;
		}
		_p2pRateTime.update();
	}

	// Send the Group Report message (0A) to a random connected peer
	if (_lastReport.isElapsed(NETGROUP_REPORT_DELAY)) {

		// Send the Group Report
		auto itRandom = _mapPeers.begin();
		if (RTMFP::GetRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itRandom, [](const MAP_PEERS_ITERATOR_TYPE it) { 
			return it->second->status == RTMFP::CONNECTED && !it->second->groupReportInitiator; })) // Important to check that we are not already the group report initiator
			sendGroupReport(itRandom->second.get(), true);

		_lastReport.update();
	}

	// Manage all group medias
	auto itGroupMedia = _mapGroupMedias.begin();
	while (itGroupMedia != _mapGroupMedias.end()) {
		if (!itGroupMedia->second.manage()) {
			DEBUG("Deletion of GroupMedia ", itGroupMedia->second.id, " for the stream ", stream)
			if (_groupMediaPublisher == itGroupMedia)
				_groupMediaPublisher = _mapGroupMedias.end();
			if (_pGroupBuffer) {
				Exception ex;
				AUTO_ERROR(_pGroupBuffer->removeBuffer(ex, itGroupMedia->second.id), "GroupBuffer remove buffer", itGroupMedia->second.id)
			}
			_mapGroupMedias.erase(itGroupMedia++);
		}
		else
			++itGroupMedia;
	}

	// Print statistics
	if (_lastStats.isElapsed(NETGROUP_STATS_DELAY)) {
		double peersCount = estimatedPeersCount();
		INFO("Peers connected to stream ", stream, " : ", _mapPeers.size(), "/", _mapGroupAddress.size(), " ; target count : ", _bestList.size(), "/", TargetNeighborsCount(peersCount), "/", (UInt64)peersCount,
			" ; P2P success : ", _countP2PSuccess, "/", _countP2P, " ; GroupMedia count : ", _mapGroupMedias.size())
			for (auto& itGroup : _mapGroupMedias)
				itGroup.second.printStats();

		_lastStats.update();
	}
}

void NetGroup::updateBestList() {

	// Calculate the Best List
	std::set<std::string> oldList(move(_bestList));
	buildBestList(_myGroupAddress, _conn.peerId(), _bestList);

	// Send new connection requests and close the old ones
	manageBestConnections(oldList);
}

void NetGroup::buildBestList(const string& groupAddress, const string& peerId, set<string>& bestList) {
	bestList.clear();

	// Find the 6 closest peers
	if (_mapGroupAddress.size() <= 6) {
		for (auto& it : _mapGroupAddress)
			bestList.emplace(it.second);
	}
	else { // More than 6 peers, in this part redundant peers are accepted to limit the size of the Best List
		UInt16 count(0);

		// First we search the first of the 6 closest peers
		auto itFirst = _mapGroupAddress.lower_bound(groupAddress);
		if (itFirst == _mapGroupAddress.end())
			itFirst = --(_mapGroupAddress.end());
		for (int i = 0; i < 2; i++)
			RTMFP::GetPreviousIt(_mapGroupAddress, itFirst);

		// Then we add the 6 peers
		for (int j = 0; j < 6; j++) {
			if (itFirst->first == groupAddress)
				--j; // to avoid adding our own address
			else {
				if (bestList.emplace(itFirst->second).second)
					++count;
			}
			RTMFP::GetNextIt(_mapGroupAddress, itFirst);
		}

		// Find the 6 lowest latency
		deque<shared_ptr<P2PSession>> queueLatency;
		if (!_mapPeers.empty()) {
			for (auto& it : _mapPeers) { // First, order the peers by latency (we know only latency from connected peers here)
				UInt16 latency = it.second->latency();
				auto it2 = queueLatency.begin();
				while (it2 != queueLatency.end() && (*it2)->latency() < latency)
					++it2;
				queueLatency.emplace(it2, it.second);
			}
			auto itLatency = queueLatency.begin();
			for (int i = 0; ++itLatency != queueLatency.end() && i < 6; i++) {
				if ((*itLatency)->peerId == peerId)
					--i; // to avoid adding our own address
				else {
					if (bestList.emplace((*itLatency)->peerId).second)
						++count;
				}
			}
		}

		// Add one random peer
		auto itRandom = _mapGroupAddress.begin();
		if (RTMFP::GetRandomIt<map<string, string>, map<string, string>::iterator>(_mapGroupAddress, itRandom, [bestList, groupAddress](const map<string, string>::iterator& it) { 
					return it->first != groupAddress && bestList.find(it->second) == bestList.end();
				})) {
			bestList.emplace(itRandom->second);
			++count;
		}

		// Find 2 log(N) peers with location + 1/2, 1/4, 1/8 ...
		int targetCount = min((int)TargetNeighborsCount(estimatedPeersCount()), (int)(_mapGroupAddress.size() - (peerId != _conn.peerId())));
		auto itTarget = _mapGroupAddress.lower_bound(groupAddress);
		if (itTarget == _mapGroupAddress.end())
			itTarget = _mapGroupAddress.begin();
		for (int missing = targetCount - count; missing > 0; --missing) {
			auto& itNode = itTarget;

			// Advance from x + 1/2^i
			UInt32 step = _mapGroupAddress.size() / (UInt32)pow(2, missing);
			for (UInt32 i = 0; i < step; ++i)
				RTMFP::GetNextIt(_mapGroupAddress, itNode);

			while (itNode->first == groupAddress || !bestList.emplace(itNode->second).second) // If not added go to next
				RTMFP::GetNextIt(_mapGroupAddress, itNode);
		}
	}
}

void NetGroup::sendGroupReport(P2PSession* pPeer, bool initiator) {
	TRACE("Preparing the Group Report message (type 0A) for peer ", pPeer->peerId)

	auto itNode = _mapHeardList.find(pPeer->peerId);
	if (itNode == _mapHeardList.end()) {
		ERROR("Unable to find the peer ", pPeer->peerId, " in the Heard list") // implementation error
		return;
	}

	// Build the Best list for far peer
	set<string> bestList;
	buildBestList(itNode->second.groupAddress, itNode->first, bestList);

	const SocketAddress& hostAddress(_conn.address()), peerAddress(pPeer->address());

	// Calculate the total size to allocate sufficient memory (TODO: see if allocating progressively is better)
	UInt32 sizeTotal = (UInt32)(peerAddress.host().size() + 8 + AddressesSize(hostAddress, _myAddresses));
	Int64 timeNow(Time::Now());
	for (auto& it1 : bestList) {
		itNode = _mapHeardList.find(it1);
		if (itNode != _mapHeardList.end() && !itNode->second.died)
			sizeTotal += AddressesSize(itNode->second.hostAddress, itNode->second.addresses) + PEER_ID_SIZE + 5 + ((itNode->second.lastGroupReport > 0) ? Binary::Get7BitValueSize((UInt32)((timeNow - itNode->second.lastGroupReport) / 1000)) : 1);
	}
	_reportBuffer.resize(sizeTotal, false);

	BinaryWriter writer(_reportBuffer.data(), _reportBuffer.size());

	// Group far address
	writer.write8(0x0A);
	writer.write8(peerAddress.host().size() + 4);
	writer.write8(0x0D);
	RTMFP::WriteAddress(writer, peerAddress, RTMFP::ADDRESS_PUBLIC); // current address of far peer
	TRACE("Group far peer address : ", peerAddress)

	// My addresses
	writer.write8(AddressesSize(hostAddress, _myAddresses));
	writer.write8(0x0A);
	RTMFP::WriteAddress(writer, hostAddress, RTMFP::ADDRESS_REDIRECTION); // my host address
	for (auto& itAddress : _myAddresses)
		RTMFP::WriteAddress(writer, itAddress.first, itAddress.second); // my addresses
	writer.write8(0);

	// Peers ID, addresses and time
	for (auto& it2 : bestList) {
		itNode = _mapHeardList.find(it2);
		if (itNode != _mapHeardList.end() && !itNode->second.died) {

			UInt64 timeElapsed = (UInt64)((itNode->second.lastGroupReport > 0) ? ((timeNow - itNode->second.lastGroupReport) / 1000) : 0);
			TRACE("Group 0A argument - Peer ", itNode->first, " - elapsed : ", timeElapsed)
			writer.write8(0x22).write(itNode->second.rawId.data(), PEER_ID_SIZE+2);
			writer.write7BitLongValue(timeElapsed);
			writer.write8(AddressesSize(itNode->second.hostAddress, itNode->second.addresses));
			writer.write8(0x0A);
			if (itNode->second.hostAddress)
				RTMFP::WriteAddress(writer, itNode->second.hostAddress, RTMFP::ADDRESS_REDIRECTION);
			for (auto& itAddress : itNode->second.addresses)
				RTMFP::WriteAddress(writer, itAddress.first, itAddress.second);
			writer.write8(0);
		}
	}

	DEBUG("Sending the group report to ", pPeer->peerId)
	pPeer->groupReportInitiator = initiator;
	pPeer->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
}

void NetGroup::manageBestConnections(const set<string>& oldList) {

	// Close the old connections & handshakes
	int nbDisconnect = _mapPeers.size() - _bestList.size(); // trick to keep the target count of peers
	for (auto& peerId : oldList) {
		if (_bestList.find(peerId) == _bestList.end()) {
			const auto& it2Close = _mapPeers.find(peerId);
			if (it2Close != _mapPeers.end()) {
				if (nbDisconnect > 0 && it2Close->second->askPeer2Disconnect()) // ask far peer to disconnect (if we are not in its Best List)
					--nbDisconnect;
			} else
				_conn.removePeer(peerId); // Ask parent to close the session and handshake
		}
	}

	// Connect to new peers
	int nbConnect = _bestList.size() - _mapPeers.size(); // trick to keep the target count of peers
	for (auto it2Connect = _bestList.begin(); nbConnect > 0 && it2Connect != _bestList.end(); ++it2Connect) {
		
		// if peer is not connected we try to connect to it
		if (_mapPeers.find(*it2Connect) == _mapPeers.end()) {
			auto itNode = _mapHeardList.find(*it2Connect);
			if (itNode == _mapHeardList.end())
				WARN("Unable to find the peer ", *it2Connect, " to start connecting") // implementation error, should not happen
			else if (!itNode->second.died && _conn.connect2Peer(it2Connect->c_str(), stream.c_str(), itNode->second.addresses, itNode->second.hostAddress, true)) { // rendezvous service delayed of 5s
				if (++_countP2P == ULLONG_MAX) { // reset p2p count
					_countP2PSuccess = _countP2P = 0;
					_p2pRateTime.update();
				}
				--nbConnect;
			}
		}
	}
}

void NetGroup::cleanHeardList() {

	Int64 now = Time::Now();
	auto itHeardList = _mapHeardList.begin();
	while (itHeardList != _mapHeardList.end()) {

		// No Group Report since 5min?
		if (now > itHeardList->second.lastGroupReport && ((now - itHeardList->second.lastGroupReport) > NETGROUP_PEER_TIMEOUT)) {
			DEBUG("Peer ", itHeardList->first, " timeout (", NETGROUP_PEER_TIMEOUT, "ms elapsed) - deleting from the Heard List...")

			// Close the peer if we are connected to it
			_conn.removePeer(itHeardList->first);

			// Delete from the Heard List
			auto itGroupAddress = _mapGroupAddress.find(itHeardList->second.groupAddress);
			FATAL_CHECK(itGroupAddress != _mapGroupAddress.end()) // implementation error
			_mapGroupAddress.erase(itGroupAddress);
			_mapHeardList.erase(itHeardList++);
			continue;
		}
		++itHeardList;
	}
}

unsigned int NetGroup::callFunction(const string& function, queue<string>& arguments) {

	for (auto& itGroupMedia : _mapGroupMedias)
		itGroupMedia.second.callFunction(function, arguments);

	return 1;
}

bool NetGroup::p2pNewPeer(const string& peerId) {

	// Control if we have reached the first 6 peers without any connection
	p2PAddressExchange(peerId);

	return _mapPeers.find(peerId) == _mapPeers.end();
}


void NetGroup::p2PAddressExchange(const std::string& tag) {

	// Control if we have reached the first 6 peers without any connection
	if (!_p2pAble && (_p2pEntities.size() < NETGROUP_MIN_PEERS_TIMEOUT)) {
		auto itPeer = _p2pEntities.lower_bound(tag);

		// New tag?
		if (itPeer == _p2pEntities.end() || *itPeer != tag) {
			_p2pEntities.emplace_hint(itPeer, tag);
			if (_p2pEntities.size() == NETGROUP_MIN_PEERS_TIMEOUT) {
				DEBUG(NETGROUP_MIN_PEERS_TIMEOUT, " p2p tries without connection, waiting ", NETGROUP_TIMEOUT_P2PABLE, "ms before disconnection...")
				_p2pAbleTime.update(); // start the timer for p2p capability
			}
		}
	}
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

bool NetGroup::readGroupReport(const map<string, GroupNode>::iterator& itNode, BinaryReader& packet) {
	string tmp, newPeerId, rawId;
	SocketAddress myAddress, serverAddress;
	RTMFP::AddressType addressType;
	PEER_LIST_ADDRESS_TYPE listAddresses;
	Int64 now = Time::Now();

	// Record the time of last Group Report received to build our Group Report
	itNode->second.lastGroupReport = now;

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
	TRACE("Group Report - My address : ", myAddress) // Address seen by the far peer
	auto itAddress = _myAddresses.lower_bound(myAddress);
	if (itAddress == _myAddresses.end() || itAddress->first != myAddress)
		_myAddresses.emplace_hint(itAddress, myAddress, addressType); // New address => save it
	
	size = packet.read8();
	tmpMarker = packet.read8();
	if (tmpMarker != 0x0A) {
		ERROR("Unexpected marker : ", String::Format<UInt8>("%.2x", tmpMarker), " - Expected 0A")
		return false;
	}
	// Update the far peer addresses
	BinaryReader peerAddressReader(packet.current(), size - 1);
	RTMFP::ReadAddresses(peerAddressReader, itNode->second.addresses, itNode->second.hostAddress, [](const SocketAddress&, RTMFP::AddressType) {});
	packet.next(size - 1);

	// Loop on each peer of the NetGroup
	bool newPeers = false;
	while (packet.available() > 4) {
		if ((tmpMarker = packet.read8()) != 00) {
			ERROR("Unexpected marker : ", String::Format<UInt8>("%.2x", tmpMarker), " - Expected 00")
			Logs::Dump("TEST", packet.data(), packet.size(), "Position : ", packet.position());
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

		// New peer, read its addresses if no timeout reached
		if (newPeerId != _conn.peerId() && *packet.current() == 0x0A && (time < (NETGROUP_PEER_TIMEOUT/1000))) {

			BinaryReader addressReader(packet.current() + 1, size - 1); // +1 to ignore 0A
			auto itHeardList = _mapHeardList.find(newPeerId);
			// New peer? => add it to heard list
			if (itHeardList == _mapHeardList.end()) {
				SocketAddress hostAddress;
				listAddresses.clear();
				RTMFP::ReadAddresses(addressReader, listAddresses, hostAddress, [](const SocketAddress&, RTMFP::AddressType) {});
				newPeers = true;
				addPeer2HeardList(newPeerId.c_str(), rawId.data(), listAddresses, hostAddress, time);  // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
			}
			// Else update time & addresses
			else {
				Int64 nodeTime = now - (time * 1000);
				if (nodeTime > itHeardList->second.lastGroupReport)
					itHeardList->second.lastGroupReport = nodeTime;

				RTMFP::ReadAddresses(addressReader, itHeardList->second.addresses, itHeardList->second.hostAddress, [this, newPeerId](const SocketAddress& address, RTMFP::AddressType type) {
					_conn.updatePeerAddress(newPeerId, address, type);
				});
			}
				
		}
		packet.next(size);
	}

	return newPeers;
}

void NetGroup::newGroupPeer(const std::string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const Base::SocketAddress& hostAddress) {

	++_countP2P; // new p2p connection
	addPeer2HeardList(peerId, rawId, listAddresses, hostAddress);
}