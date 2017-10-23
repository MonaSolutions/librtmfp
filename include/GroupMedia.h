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

#pragma once

#include "Base/Mona.h"
#include "P2PSession.h"
#include "GroupListener.h"
#include <queue>
#include <list>

/**********************************************
GroupMedia is the class that manage a stream
from a NetGroup connection
*/
struct GroupMedia : virtual Base::Object {
	typedef Base::Event<void(Base::UInt32 groupMediaId, const std::shared_ptr<GroupFragment>& pFragment)>	ON(NewFragment); // called on reception of a new fragment
	typedef Base::Event<void(Base::UInt32 groupMediaId, Base::UInt64 fragmentId)>							ON(RemovedFragments); // called when removing a group of fragments
	typedef Base::Event<void(Base::UInt32 groupMediaId)>													ON(StartProcessing); // called when the first pull fragment is received, we can start processing fragments

	GroupMedia(const Base::Timer& timer, const std::string& name, const std::string& key, std::shared_ptr<RTMFPGroupConfig> parameters, bool audioReliable, bool videoReliable);
	virtual ~GroupMedia();

	void						printStats();

	// Close the Group Media (when receiving onClosedMedia notification)
	void						close(Base::UInt64 lastFragment);

	// Close the publisher Group Media (send last end fragments)
	void						closePublisher();

	// Regularly called to send the fragments maps, the pull requests and the push requests
	// return : False if the GroupMedia must be deleted (no activity since 5min), True otherwise
	bool						manage();

	// Add the peer to map of peer, return false if the peer is already known
	void						addPeer(const std::string& peerId, std::shared_ptr<PeerMedia>& pPeer);
	
	// Send the Group Media infos to peer
	void						sendGroupMedia(std::shared_ptr<PeerMedia>& pPeer);

	// Return true if we have at least one fragment
	bool						hasFragments() { return !_fragments.empty(); }

	// Create a new fragment that will call a function
	void						callFunction(const std::string& function, std::queue<std::string>& arguments);

	GroupListener::OnMedia						onMedia; // Create a new fragment from a media packet
	GroupListener::OnFlush						onFlush; // Flush the listeners

	const Base::UInt32								id; // id of the GroupMedia (incremental)
	std::shared_ptr<RTMFPGroupConfig>			groupParameters; // group parameters for this Group Media stream
	
private:
	#define LIST_PEERS_INFO_TYPE std::list<std::shared_ptr<PeerMedia>>
	#define MAP_PEERS_INFO_TYPE std::map<std::string, std::shared_ptr<PeerMedia>>
	#define MAP_PEERS_INFO_ITERATOR_TYPE std::map<std::string, std::shared_ptr<PeerMedia>>::iterator

	// Add a new fragment to the map _fragments
	void						addFragment(MAP_FRAGMENTS_ITERATOR& itFragment, bool reliable, PeerMedia* pPeer, Base::UInt8 marker, Base::UInt64 fragmentId, Base::UInt8 splitedNumber, Base::UInt8 mediaType, Base::UInt32 time, const Base::Packet& packet, bool flush);

	// Update the fragment map
	// Return 0 if there is no fragments, otherwise the last fragment number
	Base::UInt64				updateFragmentMap();

	// Erase old fragments (called before generating the fragments map)
	void						eraseOldFragments();

	// Calculate the push play mode balance and send the requests if needed
	void						sendPushRequests();

	// Send the Pull requests if needed
	void						sendPullRequests();

	// Go to the next peer for pull or push
	// idFragment : if > 0 it will test the availability of the fragment
	// ascending : order of the research
	bool						getNextPeer(MAP_PEERS_INFO_ITERATOR_TYPE& itPeer, bool ascending, Base::UInt64 idFragment, Base::UInt8 mask);

	// Send the fragment pull request to the next available peer
	bool						sendPullToNextPeer(Base::UInt64 idFragment);

	// Remove the peer from the map
	void						removePeer(const std::string& peerId);

	// Remove the peer from the map
	void						removePeer(MAP_PEERS_INFO_ITERATOR_TYPE itPeer);

	PeerMedia::OnPeerClose										_onPeerClose; // update the NetGroup push flags when a peer disconnect
	PeerMedia::OnPlayPull										_onPlayPull; // called when we receive a pull request
	PeerMedia::OnFragmentsMap									_onFragmentsMap; // called when we receive a fragments map, must return false if we want to ignore the request (if publisher)
	PeerMedia::OnFragment										_onFragment;

	Base::Timer::OnTimer										_onPullRequests;
	Base::Timer::OnTimer										_onPushRequests;
	Base::Timer::OnTimer										_onSendFragmentsMap;
	const Base::Timer&											_timer; // timer for pull & push events

	const std::string&											_stream; // stream name
	const std::string											_streamKey; // stream key

	Base::Time													_lastFragment; // last time we received a fragment
	bool														_pullPaused; // True if no fragments have been received since fetch period

	MAP_FRAGMENTS												_fragments;
	std::map<Base::Int64, Base::UInt64>							_mapTime2Fragment; // Map of time to fragment (only START and DATA fragments are referenced)
	Base::UInt64												_fragmentCounter; // Current fragment counter (only for publisher)

	Base::Buffer												_fragmentsMapBuffer; // General buffer for fragments map
	static Base::UInt32											GroupMediaCounter; // static counter of GroupMedia for id assignment

	Base::UInt64												_endFragment; // last fragment number, if > 0 the GroupMedia is closed

	bool														_audioReliable; // if False we do not send back audio packets
	bool														_videoReliable; // if False we do not send back video packets

	// map of peers & iterators
	MAP_PEERS_INFO_TYPE											_mapPeers; // map of peers subscribed to this media stream
	LIST_PEERS_INFO_TYPE										_listPeers; // list of peers in order of connection for sending fragments
	MAP_PEERS_INFO_ITERATOR_TYPE								_itFragmentsPeer; // Current peer for fragments map requests
	MAP_PEERS_INFO_ITERATOR_TYPE								_itPushPeer; // Current peer for push request
	MAP_PEERS_INFO_ITERATOR_TYPE								_itPullPeer; // Current peer for pull request

	// Pushers calculation
	Base::UInt8													_currentPushMask; // current mask analyzed
	std::map<Base::UInt8, std::pair<std::string, Base::UInt64>>	_mapPushMasks; // Map of push IN mask to a pair of peerId/fragmentId

	std::map<Base::UInt64, Base::Time>							_mapWaitingFragments; // Map of waiting fragments in Pull requests to the time of the request
	std::map<Base::Int64, Base::UInt64>							_mapPullTime2Fragment; // Map of reception time to fragments map id (used for pull requests)
	Base::UInt64												_lastFragmentMapId; // Last Fragments map Id received (used for pull requests)
	Base::UInt64												_currentPullFragment; // Current pull fragment index
	bool														_firstPullReceived; // True if we have received the first pull fragment => we can start writing
};
