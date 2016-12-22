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

using namespace Mona;
using namespace std;

#if defined(_WIN32)
	#define sscanf sscanf_s
#endif

// Peer instance in the heard list
class GroupNode : public virtual Object {
public:
	GroupNode(const char* rawPeerId, const string& groupId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const SocketAddress& host, UInt64 timeElapsed) :
		rawId(rawPeerId, PEER_ID_SIZE + 2), groupAddress(groupId), addresses(listAddresses), hostAddress(host), lastGroupReport(((UInt64)Time::Now()) - timeElapsed) {}

	// Return the size of peer addresses for Group Report 
	UInt32	addressesSize() {
		UInt32 size = hostAddress.host().size() + 4; // +4 for 0A, address type and port
		for (auto itAddress : addresses)
			if (itAddress.second != RTMFP::ADDRESS_LOCAL)
				size += itAddress.first.host().size() + 3; // +3 for address type and port
		return size;
	}

	string rawId;
	string groupAddress;
	PEER_LIST_ADDRESS_TYPE addresses;
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
		if (splitId > 0)
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

NetGroup::NetGroup(const string& groupId, const string& groupTxt, const string& streamName, RTMFPSession& conn, RTMFPGroupConfig* parameters) : groupParameters(parameters),
	idHex(groupId), idTxt(groupTxt), stream(streamName), _conn(conn), _fragmentCounter(0), _firstPushMode(true), _pListener(NULL), _currentPushMask(0), _currentPullFragment(0),
	_itPullPeer(_mapPeers.end()), _itPushPeer(_mapPeers.end()), _itFragmentsPeer(_mapPeers.end()), _lastFragmentMapId(0), _firstPullReceived(false) {
	onMedia = [this](bool reliable, AMF::ContentType type, Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		const UInt8* pos = data;
		const UInt8* end = data + size;
		UInt8 splitCounter = size / NETGROUP_MAX_PACKET_SIZE - ((size % NETGROUP_MAX_PACKET_SIZE) == 0);
		UInt8 marker = GroupStream::GROUP_MEDIA_DATA ;
		TRACE("Creating fragments ", _fragmentCounter + 1, " to ", _fragmentCounter + splitCounter, " - time : ", time)
		do {
			if (size > NETGROUP_MAX_PACKET_SIZE)
				marker = splitCounter == 0 ? GroupStream::GROUP_MEDIA_END : (pos == data ? GroupStream::GROUP_MEDIA_START : GroupStream::GROUP_MEDIA_NEXT);

			// Add the fragment to the map
			UInt32 fragmentSize = ((splitCounter > 0) ? NETGROUP_MAX_PACKET_SIZE : (end - pos));
			UInt32 bufferSize = fragmentSize + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitCounter > 0) + Util::Get7BitValueSize(_fragmentCounter);
			auto itFragment = _fragments.emplace(piecewise_construct, forward_as_tuple(++_fragmentCounter), forward_as_tuple(_conn.poolBuffers(), pos, fragmentSize, bufferSize, time, type,
				_fragmentCounter, marker, splitCounter)).first;

			if (marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START)
				_mapTime2Fragment[time] = _fragmentCounter;

			// Send fragment to peers (push mode)
			UInt8 nbPush = groupParameters->pushLimit + 1;
			for (auto it : _mapPeers) {
				// Send Group media Subscription if not already sent
				if (it.second->groupFirstReportSent && !it.second->mediaSubscriptionSent)
					sendGroupMedia(it.second);

				if (it.second->mediaSubscriptionReceived && it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), _fragmentCounter) && (--nbPush == 0)) {
					TRACE("Push limit (", groupParameters->pushLimit + 1, ") reached for fragment ", _fragmentCounter, " (mask=", Format<UInt8>("%.2x", 1 << (_fragmentCounter % 8)), ")")
					break;
				}
			}

			pos += splitCounter > 0 ? NETGROUP_MAX_PACKET_SIZE : (end - pos);
		} while (splitCounter-- > 0);
		
	};
	onFragment = [this](const string& peerId, UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		auto itPeer = _mapPeers.find(peerId);
		if (itPeer == _mapPeers.end())
			return;
		
		// Pull fragment?
		auto itWaiting = _mapWaitingFragments.find(id);
		if (itWaiting != _mapWaitingFragments.end()) {
			TRACE("Waiting fragment ", id, " is arrived")
			_mapWaitingFragments.erase(itWaiting);
			if (!_firstPullReceived)
				_firstPullReceived = true;
		}
		// Push fragment
		else {
			UInt8 mask = 1 << (id % 8);
			if (itPeer->second->pushInMode & mask) {
				TRACE("Push In fragment received from ", peerId, " : ", id, " ; mask : ", Format<UInt8>("%.2x", mask))

				auto itPushMask = _mapPushMasks.lower_bound(mask);
				// first push with this mask?
				if (itPushMask == _mapPushMasks.end() || itPushMask->first != mask)
					_mapPushMasks.emplace_hint(itPushMask, piecewise_construct, forward_as_tuple(mask), forward_as_tuple(peerId.c_str(), id));
				else {
					if (itPushMask->second.first != peerId) {
						// Peer is faster?
						if (itPushMask->second.second < id) {
							TRACE("Push In Analysis - Updating the pusher, last peer was ", itPushMask->second.first)
							auto itOldPeer = _mapPeers.find(itPushMask->second.first);
							if (itOldPeer != _mapPeers.end())
								itOldPeer->second->sendPushMode(itOldPeer->second->pushInMode - mask);
							itPushMask->second.first = peerId.c_str();
						}
						else {
							TRACE("Push In Analysis - Tested pusher is slower than current one, resetting mask...")
							itPeer->second->sendPushMode(itPeer->second->pushInMode - mask);
						}
					}
					if (itPushMask->second.second < id)
						itPushMask->second.second = id; // update the last id received for this mask
				}
			}
			else
				DEBUG("Unexpected fragment received from ", peerId, " : ", id, " ; mask : ", Format<UInt8>("%.2x", mask))
		}

		auto itFragment = _fragments.lower_bound(id);
		if (itFragment != _fragments.end() && itFragment->first == id) {
			TRACE("Fragment ", id, " already received, ignored")
			return;
		}

		// Add the fragment to the map
		UInt32 bufferSize = packet.available() + 1 + 5 * (marker == GroupStream::GROUP_MEDIA_START || marker == GroupStream::GROUP_MEDIA_DATA) + (splitedNumber > 0) + Util::Get7BitValueSize(id);
		itFragment = _fragments.emplace_hint(itFragment, piecewise_construct, forward_as_tuple(id), forward_as_tuple(_conn.poolBuffers(), packet.current(), packet.available(), bufferSize, time, (AMF::ContentType)mediaType,
			id, marker, splitedNumber));

		// Send fragment to peers (push mode)
		UInt8 nbPush = groupParameters->pushLimit + 1;
		for (auto it : _mapPeers) {
			if (it != *itPeer && it.second->mediaSubscriptionReceived && it.second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), id) && (--nbPush == 0)) {
				TRACE("Push limit (", groupParameters->pushLimit + 1, ") reached for fragment ", id, " (mask=", Format<UInt8>("%.2x", 1 << (id % 8)), ")")
				break;
			}
		}

		if ((marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START) && (_mapTime2Fragment.empty() || time > _mapTime2Fragment.rbegin()->first))
			_mapTime2Fragment[time] = id;

		// Push the fragment to the output file (if ordered)
		pushFragment(itFragment);
	};
	onGroupMedia = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		string streamName;
		const UInt8* posStart = packet.current() - 1; // Record the whole packet for sending back

		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end()) {
			ERROR("Unable to find the peer ", peerId)
			return;
		}

		// We do not accept peer if they are not in the best list
		if (!_bestList.empty() && _bestList.find(peerId) == _bestList.end()) {
			DEBUG("Best Peer management - peer ", peerId," connection rejected, not in the Best List")
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
		it->second->mediaSubscriptionReceived = true;

		Buffer farCode;
		packet.read(0x22, farCode);
		if (groupParameters->isPublisher) {
			// Another stream code => we must accept and send our stream code
			if (memcmp(_pStreamCode->data(), farCode.data(), 0x22) != 0) {
				writer.writeRaw(posStart, packet.size() - (posStart - packet.data())); // Return the request to accept
				if (!it->second->mediaSubscriptionSent)
					sendGroupMedia(it->second);
			}
		}
		else {
			// Update the NetGroup stream properties
			UInt8 size = 0, id = 0;
			unsigned int value = 0;
			char availabilitySendToAll = 0;
			while (packet.available()) {
				if ((size = packet.read8()) == 0)
					continue;
				id = packet.read8();
				value = (size > 1)? (unsigned int)packet.read7BitLongValue() : 0;
				switch (id) {
					case NETGROUP_UNKNWON_PARAMETER:
						break;
					case NETGROUP_WINDOW_DURATION:
						if (value != groupParameters->windowDuration)
							DEBUG("Updating the Window Duration : ", (groupParameters->windowDuration = value), "ms");
						break;
					case NETGROUP_OBJECT_ENCODING:
						if (value != 300000) {
							ERROR("Unexpected object encoding value : ", value) // TODO: not sure it is object encoding!
							return;
						}
						break;
					case NETGROUP_UPDATE_PERIOD:
						if (value != groupParameters->availabilityUpdatePeriod)
							DEBUG("Updating the Avaibility Update period : ", (groupParameters->availabilityUpdatePeriod = value), "ms"); 
						break;
					case NETGROUP_SEND_TO_ALL:
						availabilitySendToAll = 1;
						return;
					case NETROUP_FETCH_PERIOD:
						if (value != groupParameters->fetchPeriod)
							DEBUG("Updating the Fetch period : ", (groupParameters->fetchPeriod = value), "ms"); break;
						break;
				}
				if (groupParameters->availabilitySendToAll != availabilitySendToAll)
					DEBUG("Updating the Availability Send to All : ", ((groupParameters->availabilitySendToAll = availabilitySendToAll) != 0) ? "ON" : "OFF");
			}

			if (!it->second->mediaSubscriptionSent) {
				INFO("Starting to listen to publication ", streamName, " on connection ", peerId)

				// Save the key if first time received
				if (!_pStreamCode) {
					_pStreamCode.reset(new Buffer(0x22));
					DEBUG("Saving the key ", Util::FormatHex(BIN farCode.data(), 0x22, LOG_BUFFER))
					memcpy(_pStreamCode->data(), farCode.data(), 0x22);
				}
				sendGroupMedia(it->second);
			}
		}
	};
	onGroupReport = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		auto itPeer = _mapPeers.find(peerId);
		if (itPeer == _mapPeers.end())
			return;

		string key1, key2, tmp, newPeerId, rawId;

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
		bool manageBestList = false;
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
				packet.next(size); // ignore the addresses if peerId not set
			else
				TRACE("Empty parameter...")

			UInt64 time = packet.read7BitLongValue();
			TRACE("Group Report - Time elapsed : ", time)

			size = packet.read8(); // Address size

			// New peer, read its addresses
			if (size >= 0x08 && newPeerId != _conn.peerId() && _mapHeardList.find(newPeerId) == _mapHeardList.end() && *packet.current() == 0x0A) {

				BinaryReader addressReader(packet.current()+1, size-1); // +1 to ignore 0A
				PEER_LIST_ADDRESS_TYPE listAddresses;
				SocketAddress hostAddress(_conn.serverAddress());
				if (RTMFP::ReadAddresses(addressReader, listAddresses, hostAddress)) {
					manageBestList = true;
					addPeer2HeardList(newPeerId.c_str(), rawId.data(), listAddresses, hostAddress, time);  // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
				}
			}
			packet.next(size);
		}
		
		auto itNode = _mapHeardList.find(peerId);
		if (itNode != _mapHeardList.end())
			itNode->second.lastGroupReport = Time::Now(); // Record the time of last Group Report received to build our Group Report

		// First Viewer = > create listener
		if (groupParameters->isPublisher && !_pListener) {
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

		if (!itPeer->second->groupReportInitiator) {
			sendGroupReport(itPeer);
			_lastReport.update();
		} else
			itPeer->second->groupReportInitiator = false;

		// Send the Group Media Subscription if not already sent
		if (!itPeer->second->mediaSubscriptionSent && (_bestList.empty() || _bestList.find(peerId) != _bestList.end()))
			sendGroupMedia(itPeer->second);

		// If there are new peers : manage the best list
		if (manageBestList)
			updateBestList();
	};
	onGroupPlayPush = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		DEBUG("Group Push Out mode received from peer ", peerId, " : ", Format<UInt8>("%.2x", *packet.current()))
		auto it = _mapPeers.find(peerId);
		if (it != _mapPeers.end())
			it->second->setPushMode(packet.read8());
	};
	onGroupPlayPull = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);
		UInt64 fragment = packet.read7BitLongValue();
		auto itFragment = _fragments.find(fragment);
		auto it = _mapPeers.find(peerId);
		if (it == _mapPeers.end())
			return;

		// Send fragment to peer (pull mode)
		TRACE("Group Pull message received from peer ", peerId, " - fragment : ", fragment)
		if (itFragment == _fragments.end()) {
			DEBUG("Peer ", peerId, " is asking for an unknown Fragment (", fragment, "), possibly deleted")
			return;
		}

		it->second->sendMedia(itFragment->second.pBuffer.data(), itFragment->second.pBuffer.size(), itFragment->first, true);	
	};
	onFragmentsMap = [this](const string& peerId, PacketReader& packet, FlashWriter& writer) {
		UInt64 counter = packet.read7BitLongValue();
		DEBUG("Group Fragments map (type 22) received from ", peerId, " : ", counter)

		if (!groupParameters->isPublisher) {
			lock_guard<recursive_mutex> lock(_fragmentMutex);
			auto it = _mapPeers.find(peerId);
			if (it == _mapPeers.end())
				return;

			// Record the idenfier for future pull requests
			if (_lastFragmentMapId < counter) {
				_mapPullTime2Fragment.emplace(Time::Now(), counter);
				_lastFragmentMapId = counter;
			}
			it->second->updateFragmentsMap(counter, packet.current(), packet.available());

			// Start push mode if not started
			if (_firstPushMode) {
				sendPushRequests();
				_lastPushUpdate.update();
				_firstPushMode = false;
			}
		}
		packet.next(packet.available());
	};
	onGroupBegin = [this](const string& peerId, FlashWriter& writer) {
		lock_guard<recursive_mutex> lock(_fragmentMutex);

		 // When we receive the 0E NetGroup message type we must send the group report if not already sent
		auto it = _mapPeers.find(peerId);
		auto itNode = _mapHeardList.find(peerId);
		if (it == _mapPeers.end() || itNode == _mapHeardList.end() || it->second->groupFirstReportSent)
			return;

		it->second->groupReportInitiator = true;
		sendGroupReport(it);
		_lastReport.update();
	};
	onPeerClose = [this](const string& peerId, UInt8 mask, bool full) {
		if (mask) {
			lock_guard<recursive_mutex> lock(_fragmentMutex);
			// unset push masks
			for (UInt8 i = 0; i < 8; i++) {
				if (mask & (1 << i)) {
					auto itPush = _mapPushMasks.find(1 << i);
					if (itPush != _mapPushMasks.end() && itPush->second.first == peerId)
						_mapPushMasks.erase(itPush);
				}
			}
		}
	};

	GetGroupAddressFromPeerId(STR _conn.rawId(), _myGroupAddress);

	// If Publisher we generate the stream key
	if (groupParameters->isPublisher) {
		_pStreamCode.reset(new Buffer(0x22));
		_pStreamCode->data()[0] = 0x21; _pStreamCode->data()[1] = 0x01;
		Util::Random((UInt8*)_pStreamCode->data() + 2, 0x20); // random serie of 32 bytes
	}
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

	map<string, shared_ptr<P2PSession>>::iterator itPeer = _mapPeers.begin();
	while (itPeer != _mapPeers.end())
		removePeer(itPeer);
}

void NetGroup::addPeer2HeardList(const string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const SocketAddress& hostAddress, UInt64 timeElapsed) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	auto it = _mapHeardList.lower_bound(peerId);
	if (it != _mapHeardList.end() && it->first == peerId) {
		DEBUG("The peer ", peerId, " is already known")
		return;
	}

	string groupAddress;
	_mapGroupAddress.emplace(GetGroupAddressFromPeerId(rawId, groupAddress), peerId);
	it = _mapHeardList.emplace_hint(it, piecewise_construct, forward_as_tuple(peerId.c_str()), forward_as_tuple(rawId, groupAddress, listAddresses, hostAddress, timeElapsed));
	DEBUG("Peer ", it->first, " added to heard list")
}

bool NetGroup::addPeer(const string& peerId, shared_ptr<P2PSession> pPeer) {
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
	DEBUG("Adding the peer ", peerId, " to the Best List")
	_mapPeers.emplace_hint(it, peerId, pPeer);

	pPeer->OnGroupMedia::subscribe(onGroupMedia);
	pPeer->OnGroupReport::subscribe(onGroupReport);
	pPeer->OnGroupPlayPush::subscribe(onGroupPlayPush);
	pPeer->OnGroupPlayPull::subscribe(onGroupPlayPull);
	pPeer->OnFragmentsMap::subscribe(onFragmentsMap);
	pPeer->OnGroupBegin::subscribe(onGroupBegin);
	pPeer->OnFragment::subscribe(onFragment);
	pPeer->OnPeerClose::subscribe(onPeerClose);

	buildBestList(_myGroupAddress, _bestList); // rebuild the best list to know if the peer is in it
	return true;
}

void NetGroup::removePeer(const string& peerId) {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	auto it = _mapPeers.find(peerId);
	if (it == _mapPeers.end())
		DEBUG("The peer ", peerId, " is already removed from the best list")
	else
		removePeer(it);
}

void NetGroup::removePeer(MAP_PEERS_ITERATOR_TYPE& itPeer) {
	DEBUG("Deleting peer ", itPeer->first, " from the NetGroup Best List")

	itPeer->second->OnGroupMedia::unsubscribe(onGroupMedia);
	itPeer->second->OnGroupReport::unsubscribe(onGroupReport);
	itPeer->second->OnGroupPlayPush::unsubscribe(onGroupPlayPush);
	itPeer->second->OnGroupPlayPull::unsubscribe(onGroupPlayPull);
	itPeer->second->OnFragmentsMap::unsubscribe(onFragmentsMap);
	itPeer->second->OnGroupBegin::unsubscribe(onGroupBegin);
	itPeer->second->OnFragment::unsubscribe(onFragment);
	itPeer->second->OnPeerClose::unsubscribe(onPeerClose);

	// If it is a current peer => increment
	if (itPeer == _itPullPeer && getNextPeer(_itPullPeer, true, 0, 0) && itPeer == _itPullPeer)
		_itPullPeer = _mapPeers.end(); // to avoid bad pointer
	if (itPeer == _itPushPeer && getNextPeer(_itPushPeer, false, 0, 0) && itPeer == _itPushPeer)
		_itPushPeer = _mapPeers.end(); // to avoid bad pointer
	if (itPeer == _itFragmentsPeer && getNextPeer(_itFragmentsPeer, false, 0, 0) && itPeer == _itFragmentsPeer)
		_itFragmentsPeer = _mapPeers.end(); // to avoid bad pointer
	_mapPeers.erase(itPeer++);
}

void NetGroup::sendGroupMedia(std::shared_ptr<P2PSession> pPeer) {
	if (pPeer && pPeer->status == RTMFP::CONNECTED && pPeer->groupFirstReportSent && _pStreamCode)
		pPeer->sendGroupMedia(stream, _pStreamCode->data(), _pStreamCode->size(), groupParameters);
}

bool NetGroup::checkPeer(const string& peerId) {

	return _mapPeers.find(peerId) == _mapPeers.end();
}

void NetGroup::manage() {
	lock_guard<recursive_mutex> lock(_fragmentMutex);

	if (_mapPeers.empty())
		return;

	// Manage the Best list
	if (_lastBestCalculation.isElapsed(NETGROUP_BEST_LIST_DELAY))
		updateBestList();

	// Send the Fragments Map message
	UInt64 lastFragment(0);
	if (_lastFragmentsMap.isElapsed(groupParameters->availabilityUpdatePeriod) && (lastFragment = updateFragmentMap())) {

		// Send to all neighbors
		if (groupParameters->availabilitySendToAll) {
			for (auto it : _mapPeers) {
				if (it.second->mediaSubscriptionSent && it.second->mediaSubscriptionReceived)
					it.second->sendFragmentsMap(lastFragment, _reportBuffer.data(), _reportBuffer.size());
			}
		} // Or just one peer at random
		else {
			bool found(false);
			if (_itFragmentsPeer == _mapPeers.end())
				found = RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, _itFragmentsPeer, [](const MAP_PEERS_ITERATOR_TYPE it) { return it->second->mediaSubscriptionReceived; });
			else
				found = getNextPeer(_itFragmentsPeer, false, 0, 0);

			if (found && _itFragmentsPeer != _mapPeers.end())
				_itFragmentsPeer->second->sendFragmentsMap(lastFragment, _reportBuffer.data(), _reportBuffer.size());
		}
		_lastFragmentsMap.update();
	}

	// Send the Group Report message (0A) to a random connected peer
	if (_lastReport.isElapsed(NETGROUP_REPORT_DELAY)) {

		auto itRandom = _mapPeers.begin();
		if (RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itRandom, [](const MAP_PEERS_ITERATOR_TYPE it) { return it->second->status == RTMFP::CONNECTED; })) {
			itRandom->second->groupReportInitiator = true;
			sendGroupReport(itRandom);
		}

		// Cleaning the Heard List from old peers
		Int64 now = Time::Now();
		auto itHeardList = _mapHeardList.begin();
		while (itHeardList != _mapHeardList.end()) {
			if ((_mapPeers.find(itHeardList->first) == _mapPeers.end()) && now > itHeardList->second.lastGroupReport && ((now - itHeardList->second.lastGroupReport) > NETGROUP_PEER_TIMEOUT)) {
				DEBUG("Peer ", itHeardList->first, " timeout (", NETGROUP_PEER_TIMEOUT, "ms elapsed) - deleting from the heard list...")
				removePeer((itHeardList++)->first);
				continue;
			}
			++itHeardList;
		}

		_lastReport.update();
	}

	// Send the Push requests (if we have received at least one Media Subscription message)
	if (_pStreamCode && !groupParameters->isPublisher && _lastPushUpdate.isElapsed(NETGROUP_PUSH_DELAY)) {

		sendPushRequests();
		_lastPushUpdate.update();
	}

	// Send the Pull requests
	if (!groupParameters->isPublisher && _lastPullUpdate.isElapsed(NETGROUP_PULL_DELAY)) {

		sendPullRequests();
		_lastPullUpdate.update();
	}
}

void NetGroup::updateBestList() {

	buildBestList(_myGroupAddress, _bestList);
	manageBestConnections();
	_lastBestCalculation.update();
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
			if (RTMFP::getRandomIt<map<string, string>, map<string, string>::iterator>(_mapGroupAddress, itRandom, [bestList](const map<string, string>::iterator& it) { return bestList.find(it->second) != bestList.end(); }))
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

	if (bestList == _bestList)
		TRACE("Best Peer management - Peers connected : ", _mapPeers.size(), " ; new count : ", _bestList.size(), " ; Peers known : ", _mapGroupAddress.size())
}

void NetGroup::eraseOldFragments() {
	if (_fragments.empty())
		return;

	UInt32 end = _fragments.rbegin()->second.time;
	UInt32 time2Keep = end - (groupParameters->windowDuration + groupParameters->relayMargin);
	auto itTime = _mapTime2Fragment.lower_bound(time2Keep);

	// To not delete more than the window duration
	if (itTime != _mapTime2Fragment.end() && time2Keep > itTime->first)
		--itTime;
		
	// Ignore if no fragment found or if it is the first reference
	if (itTime == _mapTime2Fragment.end() || itTime == _mapTime2Fragment.begin())
		return;

	auto itFragment = _fragments.find(itTime->second);
	if (itFragment == _fragments.end()) {
		ERROR("Unable to find the fragment ", itTime->second, " for cleaning buffer") // implementation error
		return;
	}

	// Get the first fragment before the itTime reference
	--itFragment;
	if (_fragmentCounter < itFragment->first) {
		WARN("Deleting unread fragments to keep the window duration... (", itFragment->first - _fragmentCounter, " fragments ignored)")
		_fragmentCounter = itFragment->first;
	}

	DEBUG("Deletion of fragments ", _fragments.begin()->first, " (~", _mapTime2Fragment.begin()->first, ") to ",
		itFragment->first, " (~", itTime->first, ") - current time : ", end)
	_fragments.erase(_fragments.begin(), itFragment);
	_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);

	// Delete the old waiting fragments
	auto itWait = _mapWaitingFragments.lower_bound(itFragment->first);
	if (!_mapWaitingFragments.empty() && _mapWaitingFragments.begin()->first < itFragment->first) {
		WARN("Deletion of waiting fragments ", _mapWaitingFragments.begin()->first, " to ", (itWait == _mapWaitingFragments.end())? _mapWaitingFragments.rbegin()->first : itWait->first)
		_mapWaitingFragments.erase(_mapWaitingFragments.begin(), itWait);
	}
	if (_currentPullFragment < itFragment->first)
		_currentPullFragment = itFragment->first; // move the current pull fragment to the 1st fragment

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
		return lastFragment;

	if (groupParameters->isPublisher) { // Publisher : We have all fragments, faster treatment
		
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
		// Loop on each bit
		for (UInt64 index = lastFragment - 1; index >= firstFragment && index >= 8; index -= 8) {

			UInt8 currentByte = 0;
			for (UInt8 fragment = 0; fragment < 8 && (index-fragment) >= firstFragment; fragment++) {
				if (_fragments.find(index - fragment) != _fragments.end())
					currentByte += (1 << fragment);
			}
			writer.write8(currentByte);
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
	Int64 timeNow(Time::Now());
	for (auto it1 : bestList) {
		itNode = _mapHeardList.find(it1);
		if (itNode != _mapHeardList.end())
			sizeTotal += itNode->second.addressesSize() + PEER_ID_SIZE + 5 + ((itNode->second.lastGroupReport > 0) ? Util::Get7BitValueSize((timeNow - itNode->second.lastGroupReport) / 1000) : 1);
	}
	_reportBuffer.resize(sizeTotal);

	BinaryWriter writer(BIN _reportBuffer.data(), _reportBuffer.size());
	writer.write8(0x0A);
	writer.write8(itPeer->second->peerAddress().host().size() + 4);
	writer.write8(0x0D);
	RTMFP::WriteAddress(writer, itPeer->second->peerAddress(), RTMFP::ADDRESS_PUBLIC);
	writer.write8(_conn.serverAddress().host().size() + 4);
	writer.write8(0x0A);
	RTMFP::WriteAddress(writer, _conn.serverAddress(), RTMFP::ADDRESS_REDIRECTION);
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
			RTMFP::WriteAddress(writer, itNode->second.hostAddress, RTMFP::ADDRESS_REDIRECTION);
			for (auto itAddress : itNode->second.addresses)
				if (itAddress.second != RTMFP::ADDRESS_LOCAL)
					RTMFP::WriteAddress(writer, itAddress.first, itAddress.second);
			writer.write8(0);
		}
	}

	TRACE("Sending the group report to ", itPeer->first)
	itPeer->second->sendGroupReport(_reportBuffer.data(), _reportBuffer.size());
}

bool NetGroup::pushFragment(map<UInt64, MediaPacket>::iterator& itFragment) {
	if (itFragment == _fragments.end() || !_firstPullReceived)
		return false;

	// Stand alone fragment (special case : sometime Flash send media END without splitted fragments)
	if (itFragment->second.marker == GroupStream::GROUP_MEDIA_DATA || (itFragment->second.marker == GroupStream::GROUP_MEDIA_END && itFragment->first == _fragmentCounter + 1)) {
		// Is it the next fragment?
		if (_fragmentCounter == 0 || itFragment->first == _fragmentCounter + 1) {
			_fragmentCounter = itFragment->first;

			TRACE("Pushing Media Fragment ", itFragment->first)
			if (itFragment->second.type == AMF::AUDIO || itFragment->second.type == AMF::VIDEO)
				_conn.pushMedia(stream, itFragment->second.time, itFragment->second.payload, itFragment->second.payloadSize(), 0, itFragment->second.type == AMF::AUDIO);

			return pushFragment(++itFragment); // Go to next fragment
		}
	}
	// Splitted packet
	else  {
		if (_fragmentCounter == 0) {
			// Delete first splitted fragments
			if (itFragment->second.marker != GroupStream::GROUP_MEDIA_START) {
				TRACE("Ignoring splitted fragment ", itFragment->first, ", we are waiting for a starting fragment")
				_fragments.erase(itFragment);
				return false;
			}
			else {
				TRACE("First fragment is a Start Media Fragment")
				_fragmentCounter = itFragment->first-1; // -1 to be catched by the next fragment condition 
			}
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
		if (itStart->first == _fragmentCounter + 1) {
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

void NetGroup::sendPushRequests() {
	if (_mapPeers.empty())
		return;

	// First bit mask is random, next are incremental
	_currentPushMask = (!_currentPushMask) ? 1 << (Util::Random<UInt8>() % 8) : ((_currentPushMask == 0x80)? 1 : _currentPushMask << 1);
	TRACE("Push In Analysis - Current mask is ", Format<UInt8>("%.2x", _currentPushMask))

	// Get the next peer
	bool found = false;
	if (_itPushPeer == _mapPeers.end())
		found = RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, _itPushPeer, [this](const MAP_PEERS_ITERATOR_TYPE& it) { return it->second->mediaSubscriptionReceived && !(it->second->pushInMode & _currentPushMask); });
	else
		found = getNextPeer(_itPushPeer, false, 0, _currentPushMask);

	// Send the push request
	if (found && _itPushPeer != _mapPeers.end())
		_itPushPeer->second->sendPushMode(_itPushPeer->second->pushInMode | _currentPushMask);
	else
		TRACE("Push In Analysis - No new peer available for mask ", Format<UInt8>("%.2x", _currentPushMask))
}

void NetGroup::manageBestConnections() {

	// Close old peers
	auto it2Close = _mapPeers.begin();
	while (it2Close != _mapPeers.end()) {
		if (_bestList.find(it2Close->first) == _bestList.end()) {
			DEBUG("Best Peer management - Closing the connection to peer ", it2Close->first)
			it2Close->second->close(false);
			removePeer(it2Close);
		} else
			it2Close++;
	}

	// Connect to new peers
	for (auto it : _bestList) {
		if (_mapPeers.find(it) == _mapPeers.end()) {
			auto itNode = _mapHeardList.find(it);
			if (itNode == _mapHeardList.end())
				WARN("Unable to find the peer ", it) // implementation error, should not happen
			else {
				DEBUG("Best Peer management - Connecting to peer ", it, "...")
				_conn.connect2Peer(it.c_str(), stream.c_str(), itNode->second.addresses, itNode->second.hostAddress);
			}
		}
	}
}

void NetGroup::sendPullRequests() {
	if (_mapPullTime2Fragment.empty()) // not started yet
		return;

	Int64 timeMax = Time::Now() - groupParameters->fetchPeriod;
	auto maxFragment = _mapPullTime2Fragment.lower_bound(timeMax);
	if (maxFragment == _mapPullTime2Fragment.begin() || maxFragment == _mapPullTime2Fragment.end()) {
		TRACE("sendPullRequests - Unable to find a reference older than Fetch period (current time available : ", Time::Now() - _mapPullTime2Fragment.begin()->first, "ms)")
		return;
	}
	UInt64 lastFragment = (--maxFragment)->second; // get the first fragment < the fetch period
	
	// The first pull request get the latest known fragments
	if (!_currentPullFragment) {
		_currentPullFragment = (lastFragment > 1)? lastFragment - 1 : 1;
		auto itRandom1 = _mapPeers.begin();
		_itPullPeer = _mapPeers.begin();
		if (RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, itRandom1, [this](const MAP_PEERS_ITERATOR_TYPE& it) { return it->second->mediaSubscriptionReceived && it->second->hasFragment(_currentPullFragment); })) {
			TRACE("sendPullRequests - first fragment found : ", _currentPullFragment)
			if (_fragments.find(_currentPullFragment) == _fragments.end()) { // ignoring if already received
				itRandom1->second->sendPull(_currentPullFragment);
				_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(_currentPullFragment), forward_as_tuple(itRandom1->first.c_str()));
			}
			else
				_firstPullReceived = true;
		} else
			TRACE("sendPullRequests - Unable to find the first fragment (", _currentPullFragment, ")")
		if (RTMFP::getRandomIt<MAP_PEERS_TYPE, MAP_PEERS_ITERATOR_TYPE>(_mapPeers, _itPullPeer, [this](const MAP_PEERS_ITERATOR_TYPE& it) { return it->second->mediaSubscriptionReceived && it->second->hasFragment(_currentPullFragment + 1); })) {
			TRACE("sendPullRequests - second fragment found : ", _currentPullFragment + 1)
			if (_fragments.find(++_currentPullFragment) == _fragments.end()) { // ignoring if already received
				_itPullPeer->second->sendPull(_currentPullFragment);
				_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(_currentPullFragment), forward_as_tuple(_itPullPeer->first.c_str()));
			}
			else
				_firstPullReceived = true;
			return;
		}
		TRACE("sendPullRequests - Unable to find the second fragment (", _currentPullFragment + 1, ")")
		_currentPullFragment = 0; // no pullers found
		return;
	}

	// Loop on older fragments to send back the requests
	timeMax = Time::Now() - (groupParameters->fetchPeriod * 2);
	maxFragment = _mapPullTime2Fragment.lower_bound(timeMax);
	if (maxFragment != _mapPullTime2Fragment.begin() && maxFragment != _mapPullTime2Fragment.end()) {
		UInt64 lastOldFragment = (--maxFragment)->second; // get the first fragment < the fetch period * 2
		for (auto itPull = _mapWaitingFragments.begin(); itPull != _mapWaitingFragments.end() && itPull->first <= lastOldFragment; itPull++) {

			// Fetch period elapsed? => blacklist the peer and send back the request to another peer
			if (itPull->second.time.isElapsed(groupParameters->fetchPeriod)) {

				DEBUG("sendPullRequests - ", groupParameters->fetchPeriod, "ms without receiving fragment ", itPull->first, ", blacklisting peer ", itPull->second.peerId)
				auto itPeer = _mapPeers.find(itPull->second.peerId);
				if (itPeer != _mapPeers.end())
					itPeer->second->addPullBlacklist(itPull->first);
				
				if (sendPullToNextPeer(itPull->first)) {
					itPull->second.peerId = _itPullPeer->first.c_str();
					itPull->second.time.update();
				}
			}
		}
	}

	// Find the holes and send pull requests
	for (; _currentPullFragment < lastFragment; _currentPullFragment++) {

		if (_fragments.find(_currentPullFragment + 1) == _fragments.end() && !sendPullToNextPeer(_currentPullFragment + 1))
				break; // we wait for the fragment to be available
	}

	TRACE("sendPullRequests - Pull requests done : ", _mapWaitingFragments.size(), " waiting fragments (current : ", _currentPullFragment, "; last Fragment : ", lastFragment, ")")
}

bool NetGroup::sendPullToNextPeer(UInt64 idFragment) {

	if (!getNextPeer(_itPullPeer, true, idFragment, 0)) {
		WARN("sendPullRequests - No peer found for fragment ", idFragment)
		return false;
	}
	
	_itPullPeer->second->sendPull(idFragment);
	_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(idFragment), forward_as_tuple(_itPullPeer->first.c_str()));
	return true;
}

bool NetGroup::getNextPeer(MAP_PEERS_ITERATOR_TYPE& itPeer, bool ascending, UInt64 idFragment, UInt8 mask) {
	if (!_mapPeers.empty()) {

		// To go faster when there is only one peer
		if (_mapPeers.size() == 1) {
			itPeer = _mapPeers.begin();
			if (itPeer != _mapPeers.end() && itPeer->second->mediaSubscriptionReceived && (!idFragment || itPeer->second->hasFragment(idFragment)) && (!mask || !(itPeer->second->pushInMode & mask)))
				return true;
		}
		else {

			auto itBegin = itPeer;
			do {
				if (ascending)
					(itPeer == _mapPeers.end()) ? itPeer = _mapPeers.begin() : ++itPeer;
				else // descending
					(itPeer == _mapPeers.begin()) ? itPeer = _mapPeers.end() : --itPeer;

				// Peer match? Exiting
				if (itPeer != _mapPeers.end() && itPeer->second->mediaSubscriptionReceived && (!idFragment || itPeer->second->hasFragment(idFragment)) && (!mask || !(itPeer->second->pushInMode & mask)))
					return true;
			}
			// loop until finding a peer available
			while (itPeer != itBegin);
		}
	}

	return false;
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

