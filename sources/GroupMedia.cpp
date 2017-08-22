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

#include "GroupMedia.h"
#include "NetGroup.h"
#include "GroupStream.h"
#include "librtmfp.h"
#include "Base/Util.h"

using namespace Base;
using namespace std;

UInt32	GroupMedia::GroupMediaCounter = 0;

GroupMedia::GroupMedia(const string& name, const string& key, std::shared_ptr<RTMFPGroupConfig> parameters, bool audioReliable, bool videoReliable) : _fragmentCounter(0), _firstPushMode(true), _currentPushMask(0),
	_currentPullFragment(0), _itPullPeer(_mapPeers.end()), _itPushPeer(_mapPeers.end()), _itFragmentsPeer(_mapPeers.end()), _lastFragmentMapId(0), _firstPullReceived(false),
	_stream(name), _streamKey(key), groupParameters(parameters), id(++GroupMediaCounter), _endFragment(0), _pullPaused(false), _audioReliable(audioReliable), _videoReliable(videoReliable) {

	_onPeerClose = [this](const string& peerId, UInt8 mask) {
		// unset push masks
		if (mask) {
			for (UInt8 i = 0; i < 8; i++) {
				if (mask & (1 << i)) {
					auto itPush = _mapPushMasks.find(1 << i);
					if (itPush != _mapPushMasks.end() && itPush->second.first == peerId)
						_mapPushMasks.erase(itPush);
				}
			}
		}
		removePeer(peerId);
	};
	_onPlayPull = [this](PeerMedia* pPeer, UInt64 index) {
		auto itFragment = _fragments.find(index);
		if (itFragment == _fragments.end()) {
			DEBUG("GroupMedia ", id, " - Peer is asking for an unknown Fragment (", index, "), possibly deleted")
			return;
		}

		// Send fragment to peer (pull mode)
		pPeer->sendMedia(*itFragment->second, true, true); // pull fragment is always reliable
	};
	_onFragmentsMap = [this](UInt64 counter) {
		if (groupParameters->isPublisher)
			return false; // ignore the request

		// Record the identifier for future pull requests
		if (_lastFragmentMapId < counter) {
			_mapPullTime2Fragment.emplace(Time::Now(), counter);
			_lastFragmentMapId = counter;

			// If the state was in pause, we restart the pull requests
			if (_pullPaused) {
				DEBUG("GroupMedia ", id, " - Fragments map received, we restart the pull")
				_pullPaused = false;
			}
		}

		// Start push mode if not started
		if (_firstPushMode) {
			sendPushRequests();
			_firstPushMode = false;
		}
		return true;
	};
	onMedia = [this](bool reliable, AMF::Type type, UInt32 time, const Packet& packet) {
		BinaryReader reader(packet.data(), packet.size());
		if (!reader.size())
			return;
		UInt8 splitCounter = reader.size() / NETGROUP_MAX_PACKET_SIZE - ((reader.size() % NETGROUP_MAX_PACKET_SIZE) == 0);
		UInt8 marker = GroupStream::GROUP_MEDIA_DATA ;
		TRACE("GroupMedia ", id, " - Creating fragments ", _fragmentCounter + 1, " to ", _fragmentCounter + 1 + splitCounter, " - time : ", time)
		auto itFragment = _fragments.end();
		do {
			if (reader.size() > NETGROUP_MAX_PACKET_SIZE)
				marker = splitCounter == 0 ? GroupStream::GROUP_MEDIA_END : ((reader.current() == reader.data()) ? GroupStream::GROUP_MEDIA_START : GroupStream::GROUP_MEDIA_NEXT);

			// Add the fragment to the map
			UInt32 fragmentSize = ((splitCounter > 0) ? NETGROUP_MAX_PACKET_SIZE : reader.available());
			addFragment(itFragment, reliable, NULL, marker, ++_fragmentCounter, splitCounter, type, time, Packet(packet, reader.current(), fragmentSize));
			reader.next(fragmentSize);
		} while (splitCounter-- > 0);

	};
	_onFragment = [this](PeerMedia* pPeer, const string& peerId, UInt8 marker, UInt64 fragmentId, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, const Packet& packet, double lostRate) {
		_lastFragment.update(); // save the last fragment reception time for timeout calculation

		// Pull fragment?
		auto itWaiting = _mapWaitingFragments.find(fragmentId);
		if (itWaiting != _mapWaitingFragments.end()) {
			TRACE("GroupMedia ", id, " - Waiting fragment ", fragmentId, " is arrived")
			_mapWaitingFragments.erase(itWaiting);
			if (!_firstPullReceived)
				_firstPullReceived = true;
		}
		// Push fragment
		else {
			UInt8 mask = 1 << (fragmentId % 8);
			if (pPeer->pushInMode & mask) {
				TRACE("GroupMedia ", id, " - Push In fragment received from ", peerId, " : ", fragmentId, " ; mask : ", String::Format<UInt8>("%.2x", mask))

				auto itPushMask = _mapPushMasks.lower_bound(mask);
				// first push with this mask?
				if (itPushMask == _mapPushMasks.end() || itPushMask->first != mask)
					_mapPushMasks.emplace_hint(itPushMask, piecewise_construct, forward_as_tuple(mask), forward_as_tuple(peerId.c_str(), fragmentId));
				else {
					if (itPushMask->second.first != peerId) {
						// Peer is faster?
						if (itPushMask->second.second < fragmentId) {
							TRACE("GroupMedia ", id, " - Push In - Updating the pusher, last peer was ", itPushMask->second.first)
							auto itOldPeer = _mapPeers.find(itPushMask->second.first);
							if (itOldPeer != _mapPeers.end())
								itOldPeer->second->sendPushMode(itOldPeer->second->pushInMode - mask);
							itPushMask->second.first = peerId.c_str();
						}
						else {
							TRACE("GroupMedia ", id, " - Push In - Tested pusher is slower than current one, resetting mask...")
							pPeer->sendPushMode(pPeer->pushInMode - mask);
						}
					}
					if (itPushMask->second.second < fragmentId)
						itPushMask->second.second = fragmentId; // update the last id received for this mask
				}
			}
			else
				DEBUG("GroupMedia ", id, " - Unexpected fragment received from ", peerId, " : ", fragmentId, " ; mask : ", String::Format<UInt8>("%.2x", mask))
		}

		auto itFragment = _fragments.lower_bound(fragmentId);
		if (itFragment != _fragments.end() && itFragment->first == fragmentId) {
			TRACE("GroupMedia ", id, " - Fragment ", fragmentId, " already received, ignored")
			return;
		}

		// We must ignore fragments too old
		if (_mapTime2Fragment.size() > 2) {
			auto itBegin = _mapTime2Fragment.begin();
			auto itEnd = _mapTime2Fragment.rbegin();
			if (((itEnd->first - itBegin->first) > groupParameters->windowDuration) && itBegin->second > fragmentId) {
				TRACE("GroupMedia ", id, " - Fragment ", fragmentId, " too old (min : ", itBegin->second, "), ignored") // TODO: see if we must close the session in this case
				return;
			}
		}

		// Add the fragment to the map
		// TODO: see if we need to keep the codec infos reliable here
		addFragment(itFragment, (mediaType==AMF::TYPE_AUDIO)? _audioReliable : ((mediaType== AMF::TYPE_VIDEO)? _videoReliable : true), pPeer, marker, fragmentId, splitedNumber, mediaType, time, packet);

		// Push the fragment to the output file (if ordered)
		processFragments(itFragment);
	};
}

GroupMedia::~GroupMedia() {

	DEBUG("Destruction of the GroupMedia ", id)
	MAP_PEERS_INFO_ITERATOR_TYPE itPeer = _mapPeers.begin();
	while (itPeer != _mapPeers.end()) {
		itPeer->second->onPeerClose = nullptr; // to avoid callback
		itPeer->second->close(false); // Close the writers
		removePeer(itPeer++);
	}
}

void GroupMedia::close(UInt64 lastFragment) {
	
	DEBUG("Closing the GroupMedia ", id, " (last fragment : ", lastFragment, ")")
	_endFragment = lastFragment;
}

void GroupMedia::closePublisher() {
	if (_endFragment) // already closed
		return;

	UInt32 currentTime = (_fragments.empty()) ? 0 : _fragments.rbegin()->second->time; // get time from last fragment
	string tmp;
	shared_ptr<Buffer> pBuffer(new Buffer());
	AMFWriter writer(*pBuffer);

	// UnpublishNotify event
	RTMFP::WriteInvocation(writer, "onStatus", 0, true);
	RTMFP::WriteAMFState(writer, "onStatus", "NetStream.Play.UnpublishNotify", String::Append(tmp, _stream, " is now unpublished"), false);
	onMedia(true, AMF::TYPE_INVOCATION_AMF3, currentTime, Packet(pBuffer));

	// closeStream event
	pBuffer.reset(new Buffer());
	AMFWriter writerClose(*pBuffer);
	RTMFP::WriteInvocation(writerClose, "closeStream", 0, true);
	onMedia(true, AMF::TYPE_INVOCATION_AMF3, currentTime, Packet(pBuffer));

	// Send GroupMedia end message
	++_fragmentCounter;
	for (auto itPeer : _mapPeers)
		itPeer.second->sendEndMedia(_fragmentCounter);

	close(_fragmentCounter);
}

void GroupMedia::addFragment(MAP_FRAGMENTS_ITERATOR& itFragment, bool reliable, PeerMedia* pPeer, UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, const Packet& packet) {
	itFragment = _fragments.emplace_hint(itFragment, piecewise_construct, forward_as_tuple(id), forward_as_tuple(new GroupFragment(packet, time, (AMF::Type)mediaType, id, marker, splitedNumber)));

	// Send fragment to peers (push mode)
	UInt8 nbPush = groupParameters->pushLimit + 1;
	for (auto it : _mapPeers) {
		if (it.second.get() != pPeer && it.second->sendMedia(*itFragment->second, false, reliable) && (--nbPush == 0)) {
			TRACE("GroupMedia ", id, " - Push limit (", groupParameters->pushLimit + 1, ") reached for fragment ", id, " (mask=", String::Format<UInt8>("%.2x", 1 << (id % 8)), ")")
			break;
		}
	}

	if ((marker == GroupStream::GROUP_MEDIA_DATA || marker == GroupStream::GROUP_MEDIA_START) && (_mapTime2Fragment.empty() || id > _mapTime2Fragment.rbegin()->second))
		_mapTime2Fragment[Time::Now()] = id;
}

bool GroupMedia::manage() {
	if (!groupParameters->isPublisher && _lastFragment.isElapsed(NETGROUP_MEDIA_TIMEOUT)) // to delete the GroupMedia after 5min without reception
		return false;

	if (_mapPeers.empty())
		return true;

	// Send the Fragments Map message
	UInt64 lastFragment(0);
	if (_lastFragmentsMap.isElapsed(groupParameters->availabilityUpdatePeriod) && (lastFragment = updateFragmentMap())) {

		// Send to all neighbors
		if (groupParameters->availabilitySendToAll) {
			for (auto it : _mapPeers) {
				it.second->sendFragmentsMap(lastFragment, _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size());
			}
		} // Or just one peer at random
		else {	
			if ((_itFragmentsPeer == _mapPeers.end() && RTMFP::GetRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, _itFragmentsPeer, [](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return true; })) 
					|| getNextPeer(_itFragmentsPeer, false, 0, 0))
				_itFragmentsPeer->second->sendFragmentsMap(lastFragment, _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size());
		}
		_lastFragmentsMap.update();
	}

	// Send the Push requests
	if (!groupParameters->isPublisher && _lastPushUpdate.isElapsed(NETGROUP_PUSH_DELAY))
		sendPushRequests();

	// Send the Pull requests
	if (!groupParameters->isPublisher && _lastPullUpdate.isElapsed(NETGROUP_PULL_DELAY)) {

		sendPullRequests();
		_lastPullUpdate.update();
	}

	// Try to process again the last fragments
	if (!groupParameters->isPublisher && _lastProcessFragment.isElapsed(NETGROUP_PROCESS_FGMT_TIMEOUT)) {
		auto itLast = _fragments.find(_fragmentCounter + 1);
		if (itLast != _fragments.end())
			processFragments(itLast);
		_lastProcessFragment.update();
	}

	return true;
}

void GroupMedia::addPeer(const string& peerId, shared_ptr<PeerMedia>& pPeer) {
	auto itPeer = _mapPeers.lower_bound(peerId);
	if (itPeer != _mapPeers.end() && itPeer->first == peerId)
		return;

	_mapPeers.emplace(peerId, pPeer);
	pPeer->onPeerClose = _onPeerClose;
	pPeer->onPlayPull = _onPlayPull;
	pPeer->onFragmentsMap = _onFragmentsMap;
	pPeer->onFragment = _onFragment;
	DEBUG("GroupMedia ", id, " - Adding peer ", pPeer->id, " from ", peerId, " (", _mapPeers.size(), " peers)")

	// Send the group media & fragments map if not already sent
	sendGroupMedia(pPeer);
}

void GroupMedia::sendGroupMedia(shared_ptr<PeerMedia>& pPeer) {
	if (pPeer->groupMediaSent)
		return;

	pPeer->sendGroupMedia(_stream, _streamKey, groupParameters.get());
	UInt64 lastFragment = updateFragmentMap();
	if (!lastFragment || !pPeer->sendFragmentsMap(lastFragment, _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size()))
		pPeer->flushReportWriter();
}

bool GroupMedia::getNextPeer(MAP_PEERS_INFO_ITERATOR_TYPE& itPeer, bool ascending, UInt64 idFragment, UInt8 mask) {
	if (!_mapPeers.empty()) {

		// To go faster when there is only one peer
		if (_mapPeers.size() == 1) {
			itPeer = _mapPeers.begin();
			if (itPeer != _mapPeers.end() && (!idFragment || itPeer->second->hasFragment(idFragment)) && (!mask || !(itPeer->second->pushInMode & mask)))
				return true;
		}
		else {

			auto itBegin = itPeer;
			do {
				if (ascending)
					RTMFP::GetNextIt(_mapPeers, itPeer);
				else // descending
					RTMFP::GetPreviousIt(_mapPeers, itPeer);

				// Peer match? Exiting
				if ((!idFragment || itPeer->second->hasFragment(idFragment)) && (!mask || !(itPeer->second->pushInMode & mask)))
					return true;
			}
			// loop until finding a peer available
			while (itPeer != itBegin);
		}
	}

	return false;
}

void GroupMedia::eraseOldFragments() {
	if (_fragments.empty() || _mapTime2Fragment.empty())
		return;

	Int64 timeNow = Time::Now();
	Int64 time2Keep = timeNow - (groupParameters->windowDuration + groupParameters->relayMargin);
	auto itTime = _mapTime2Fragment.lower_bound(time2Keep);

	// To not delete more than the window duration
	if (itTime != _mapTime2Fragment.end() && itTime != _mapTime2Fragment.begin() && time2Keep > itTime->first)
		--itTime;

	// Ignore if no fragment found or if it is the first reference
	if (itTime == _mapTime2Fragment.end() || itTime == _mapTime2Fragment.begin())
		return;

	// Get the first fragment before the itTime reference
	auto itFragment = _fragments.find(itTime->second);
	if (itFragment != _fragments.begin())
		--itFragment;

	if (itFragment == _fragments.end()) {
		FATAL_ERROR("Unable to find the reference fragment with time ", itTime->second)
		return;
	}

	// Delete the old fragments and the old fragments references
	DEBUG("GroupMedia ", id, " - Deletion of fragments ", _fragments.begin()->first, " to ", itFragment->first, " - current time : ", timeNow)
	_fragments.erase(_fragments.begin(), itFragment);
	_mapTime2Fragment.erase(_mapTime2Fragment.begin(), itTime);

	// Delete the old waiting fragments
	auto itWait = _mapWaitingFragments.lower_bound(itFragment->first);
	if (!_mapWaitingFragments.empty() && _mapWaitingFragments.begin()->first < itFragment->first) {
		WARN("GroupMedia ", id, " - Deletion of waiting fragments ", _mapWaitingFragments.begin()->first, " to ", (itWait == _mapWaitingFragments.end())? _mapWaitingFragments.rbegin()->first : itWait->first)
		_mapWaitingFragments.erase(_mapWaitingFragments.begin(), itWait);
	}
	if (_currentPullFragment < itFragment->first)
		_currentPullFragment = itFragment->first; // move the current pull fragment to the 1st fragment

	// Delete the old fragments map references
	auto firstFragmentMap = _mapPullTime2Fragment.lower_bound(time2Keep);
	if (firstFragmentMap != _mapPullTime2Fragment.begin() && firstFragmentMap != _mapPullTime2Fragment.end())
		_mapPullTime2Fragment.erase(_mapPullTime2Fragment.begin(), firstFragmentMap);

	// Update the current fragment id if needed
	if (_fragmentCounter < itFragment->first) {
		WARN("GroupMedia ", id, " - Deleting unread fragments to keep the window duration... (", itFragment->first - _fragmentCounter, " fragments ignored)")
		_fragmentCounter = itFragment->first;

		// Try to push again the last fragments
		auto itLast = _fragments.find(_fragmentCounter + 1);
		if (itLast != _fragments.end())
			processFragments(itLast);
	}
}

UInt64 GroupMedia::updateFragmentMap() {
	if (_fragments.empty() && !_endFragment)
		return 0;

	// First we erase old fragments
	eraseOldFragments();

	// Generate the report message
	UInt64 firstFragment = _fragments.empty() ? _endFragment : _fragments.begin()->first;
	UInt64 lastFragment = _fragments.empty() ? _endFragment : _fragments.rbegin()->first;
	UInt64 nbFragments = lastFragment - firstFragment; // number of fragments - the first one
	_fragmentsMapBuffer.resize((UInt32)((nbFragments / 8) + ((nbFragments % 8) > 0)) + Binary::Get7BitValueSize(lastFragment) + 1, false);
	BinaryWriter writer(BIN _fragmentsMapBuffer.data(), _fragmentsMapBuffer.size());
	writer.write8(GroupStream::GROUP_FRAGMENTS_MAP).write7BitLongValue(_endFragment? _endFragment : lastFragment);

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

void GroupMedia::processFragments(MAP_FRAGMENTS_ITERATOR& itFragment) {
	Time now;
	while (processFragment(itFragment) && !now.isElapsed(NETGROUP_PROCESS_FGMT_TIMEOUT))
		++itFragment;
}

bool GroupMedia::processFragment(MAP_FRAGMENTS_ITERATOR& itFragment) {
	if (itFragment == _fragments.end() || (!_pullPaused && !_firstPullReceived))
		return false;

	DEBUG("GroupMedia ", id, " - processFragment ", itFragment->first, " ; marker : ", itFragment->second->marker)

	// Stand alone fragment (special case : sometimes Flash send media END without splitted fragments)
	if (itFragment->second->marker == GroupStream::GROUP_MEDIA_DATA || (itFragment->second->marker == GroupStream::GROUP_MEDIA_END && itFragment->first == _fragmentCounter + 1)) {
		// Is it the next fragment?
		if (_fragmentCounter == 0 || itFragment->first == _fragmentCounter + 1) {
			_fragmentCounter = itFragment->first;

			DEBUG("GroupMedia ", id, " - Pushing Media Fragment ", itFragment->first)
			if (!onGroupPacket(itFragment->second->time, *itFragment->second, 0, itFragment->second->type)) {
				close(itFragment->first + 1); // if last fragment receive we record it
				return false;
			}
			return true;
		}
	}
	// else Splitted fragment
	
	// First fragment? Search for a start fragment
	if (_fragmentCounter == 0) {
		// Delete first splitted fragments
		if (itFragment->second->marker != GroupStream::GROUP_MEDIA_START) {
			TRACE("GroupMedia ", id, " - Ignoring splitted fragment ", itFragment->first, ", we are waiting for a starting fragment")
			_fragments.erase(itFragment);
			return false;
		}
		TRACE("GroupMedia ", id, " - First fragment is a Start Media Fragment")
		_fragmentCounter = itFragment->first-1; // -1 to be catched by the next fragment condition 
	}

	// Search the start fragment
	auto itStart = itFragment;
	while (itStart->second->marker != GroupStream::GROUP_MEDIA_START) {
		itStart = _fragments.find(itStart->first - 1);
		if (itStart == _fragments.end())
			return false; // ignore these fragments if there is a hole
	}

	// Is it the next fragment?
	if (itStart->first == _fragmentCounter + 1) {

		// Check if all splitted fragments are present
		UInt8 nbFragments = itStart->second->splittedId + 1;
		UInt32 totalSize = itStart->second->size();
		auto itEnd = itStart;
		for (int i = 1; i < nbFragments; ++i) {
			if (++itEnd == _fragments.end() || itEnd->first != (itStart->first + i))
				return false; // wait fulfil if there is a hole
			totalSize += itEnd->second->size();
		}

		// update the current fragment
		_fragmentCounter = itEnd->first;
		itFragment = itEnd;

		// Buffer the fragments and forward the whole packet
		_internalBuffer.resize(totalSize, false);
		BinaryWriter writer(_internalBuffer.data(), _internalBuffer.size());
		auto itCurrent = itStart;
		do {
			writer.write(itCurrent->second->data(), itCurrent->second->size());
		} while (itCurrent++ != itEnd);

		DEBUG("GroupMedia ", id, " - Pushing splitted packet ", itStart->first, " - ", nbFragments, " fragments for a total size of ", writer.size())
		Packet packet(_internalBuffer);
		if (!onGroupPacket(itStart->second->time, packet, 0, itStart->second->type)) {
			close(itFragment->first + 1); // if last fragment receive we record it
			return false;
		}
		return true;
	}
	return false;
}

void GroupMedia::sendPushRequests() {
	if (!_mapPeers.empty()) {

		// First bit mask is random, next are incremental
		_currentPushMask = (!_currentPushMask) ? 1 << (Util::Random<UInt8>() % 8) : ((_currentPushMask == 0x80) ? 1 : _currentPushMask << 1);
		DEBUG("GroupMedia ", id, " - Push In - Current mask is ", String::Format<UInt8>("%.2x", _currentPushMask))

		// Get the next peer & send the push request
		if ((_itPushPeer == _mapPeers.end() && RTMFP::GetRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, _itPushPeer, [this](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return !(it->second->pushInMode & _currentPushMask); }))
				|| getNextPeer(_itPushPeer, false, 0, _currentPushMask))
			_itPushPeer->second->sendPushMode(_itPushPeer->second->pushInMode | _currentPushMask);
		else
			DEBUG("GroupMedia ", id, " - Push In - No new peer available for mask ", String::Format<UInt8>("%.2x", _currentPushMask))
	}

	_lastPushUpdate.update();
}

void GroupMedia::sendPullRequests() {
	if (_mapPullTime2Fragment.empty() || _pullPaused) // not started yet
		return;

	Int64 timeNow(Time::Now());
	Int64 timeMax = timeNow - groupParameters->fetchPeriod;
	auto maxFragment = _mapPullTime2Fragment.lower_bound(timeMax);
	if (maxFragment == _mapPullTime2Fragment.begin() || maxFragment == _mapPullTime2Fragment.end()) {
		if ((timeNow - _mapPullTime2Fragment.begin()->first) > groupParameters->fetchPeriod) {
			DEBUG("GroupMedia ", id, " - sendPullRequests - No Fragments map received since Fectch period (", groupParameters->fetchPeriod, "ms), pull paused")
			_pullPaused = true;
		}
		// else we are waiting for fetch period before starting pull requests
		return;
	}
	UInt64 lastFragment = (--maxFragment)->second; // get the first fragment < the fetch period
	
	// The first pull request get the latest known fragments
	if (!_currentPullFragment) {
		_currentPullFragment = (lastFragment > 1)? lastFragment - 1 : 1;
		auto itRandom1 = _mapPeers.begin();
		_itPullPeer = _mapPeers.begin();
		if (RTMFP::GetRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, itRandom1, [this](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return it->second->hasFragment(_currentPullFragment); })) {
			TRACE("GroupMedia ", id, " - sendPullRequests - first fragment found : ", _currentPullFragment)
			if (_fragments.find(_currentPullFragment) == _fragments.end()) { // ignoring if already received
				itRandom1->second->sendPull(_currentPullFragment);
				_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(_currentPullFragment), forward_as_tuple());
			}
			else
				_firstPullReceived = true;
		} else
			TRACE("GroupMedia ", id, " - sendPullRequests - Unable to find the first fragment (", _currentPullFragment, ")")
		if (RTMFP::GetRandomIt<MAP_PEERS_INFO_TYPE, MAP_PEERS_INFO_ITERATOR_TYPE>(_mapPeers, _itPullPeer, [this](const MAP_PEERS_INFO_ITERATOR_TYPE& it) { return it->second->hasFragment(_currentPullFragment + 1); })) {
			TRACE("GroupMedia ", id, " - sendPullRequests - second fragment found : ", _currentPullFragment + 1)
			if (_fragments.find(++_currentPullFragment) == _fragments.end()) { // ignoring if already received
				_itPullPeer->second->sendPull(_currentPullFragment);
				_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(_currentPullFragment), forward_as_tuple());
			}
			else
				_firstPullReceived = true;
			return;
		}
		TRACE("GroupMedia ", id, " - sendPullRequests - Unable to find the second fragment (", _currentPullFragment + 1, ")")
		_currentPullFragment = 0; // no pullers found
		return;
	}

	// Loop on older fragments to send back the requests
	timeMax -= groupParameters->fetchPeriod;
	maxFragment = _mapPullTime2Fragment.lower_bound(timeMax);
	if (maxFragment != _mapPullTime2Fragment.begin() && maxFragment != _mapPullTime2Fragment.end()) {
		UInt64 lastOldFragment = (--maxFragment)->second; // get the first fragment < the fetch period * 2
		for (auto itPull = _mapWaitingFragments.begin(); itPull != _mapWaitingFragments.end() && itPull->first <= lastOldFragment; itPull++) {

			// Fetch period elapsed? => blacklist the peer and send back the request to another peer
			if (itPull->second.isElapsed(groupParameters->fetchPeriod)) {
				DEBUG("GroupMedia ", id, " - sendPullRequests - ", groupParameters->fetchPeriod, "ms without receiving fragment ", itPull->first, " retrying...")
				
				if (sendPullToNextPeer(itPull->first))
					itPull->second.update();
			}
		}
	}

	// Find the holes and send pull requests
	for (; _currentPullFragment < lastFragment; _currentPullFragment++) {

		if (_fragments.find(_currentPullFragment + 1) == _fragments.end() && !sendPullToNextPeer(_currentPullFragment + 1))
			break; // we wait for the fragment to be available
	}

	TRACE("GroupMedia ", id, " - sendPullRequests - Pull requests done : ", _mapWaitingFragments.size(), " waiting fragments (current : ", _currentPullFragment, "; last Fragment : ", lastFragment, ")")
}

bool GroupMedia::sendPullToNextPeer(UInt64 idFragment) {

	if (!getNextPeer(_itPullPeer, true, idFragment, 0)) {
		DEBUG("GroupMedia ", id, " - sendPullRequests - No peer found for fragment ", idFragment)
		return false;
	}
	
	_itPullPeer->second->sendPull(idFragment);
	_mapWaitingFragments.emplace(piecewise_construct, forward_as_tuple(idFragment), forward_as_tuple());
	return true;
}

void GroupMedia::removePeer(const string& peerId) {
	
	auto itPeer = _mapPeers.find(peerId);
	if (itPeer != _mapPeers.end())
		removePeer(itPeer);
	else
		DEBUG("GroupMedia ", id, " - Unable to find peer ", peerId, " for closing")
}

void GroupMedia::removePeer(MAP_PEERS_INFO_ITERATOR_TYPE itPeer) {

	DEBUG("GroupMedia ", id, " - Removing peer ", itPeer->second->id, " from ", itPeer->first, " (", _mapPeers.size()," peers)")
	itPeer->second->onPeerClose = nullptr;
	itPeer->second->onPlayPull = nullptr;
	itPeer->second->onFragmentsMap = nullptr;
	itPeer->second->onFragment = nullptr;

	// If it is a current peer => increment
	if (itPeer == _itPullPeer && getNextPeer(_itPullPeer, true, 0, 0) && itPeer == _itPullPeer)
		_itPullPeer = _mapPeers.end(); // to avoid bad pointer
	if (itPeer == _itPushPeer && getNextPeer(_itPushPeer, false, 0, 0) && itPeer == _itPushPeer)
		_itPushPeer = _mapPeers.end(); // to avoid bad pointer
	if (itPeer == _itFragmentsPeer && getNextPeer(_itFragmentsPeer, false, 0, 0) && itPeer == _itFragmentsPeer)
		_itFragmentsPeer = _mapPeers.end(); // to avoid bad pointer
	_mapPeers.erase(itPeer);
}

void GroupMedia::callFunction(const char* function, int nbArgs, const char** args) {
	if (!groupParameters->isPublisher) // only publisher can create fragments
		return;

	shared_ptr<Buffer> pBuffer;
	AMFWriter writer(*pBuffer);
	writer.amf0 = true;
	writer->write8(0);
	writer.writeString(function, strlen(function));
	for (int i = 0; i < nbArgs; i++) {
		if (args[i])
			writer.writeString(args[i], strlen(args[i]));
	}

	UInt32 currentTime = (_fragments.empty())? 0 : _fragments.rbegin()->second->time;

	// Create and send the fragment
	TRACE("Creating fragment for function ", function, "...")
	onMedia(true, AMF::TYPE_DATA_AMF3, currentTime, Packet(pBuffer));
}

void GroupMedia::printStats() {
	INFO("Fragments : ", _fragments.size(), " ; Times : ", _mapTime2Fragment.size(), " ; peers : ", _mapPeers.size(), " ; masks : ", _mapPushMasks.size())
}
