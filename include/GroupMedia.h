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
	typedef Base::Event<bool(Base::UInt32 time, const Base::Packet& packet, double lostRate, AMF::Type type)> ON(GroupPacket); // called when a new packet is ready (complete & ordered)
	
	GroupMedia(const std::string& name, const std::string& key, std::shared_ptr<RTMFPGroupConfig> parameters, bool audioReliable, bool videoReliable);
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
	#define MAP_PEERS_INFO_TYPE std::map<std::string, std::shared_ptr<PeerMedia>>
	#define LIST_PEERS_INFO_TYPE std::list<std::shared_ptr<PeerMedia>>
	#define MAP_PEERS_INFO_ITERATOR_TYPE std::map<std::string, std::shared_ptr<PeerMedia>>::iterator
	#define MAP_FRAGMENTS_ITERATOR std::map<Base::UInt64, std::unique_ptr<GroupFragment>>::iterator

	// Add a new fragment to the map _fragments
	void						addFragment(MAP_FRAGMENTS_ITERATOR& itFragment, bool reliable, PeerMedia* pPeer, Base::UInt8 marker, Base::UInt64 id, Base::UInt8 splitedNumber, Base::UInt8 mediaType, Base::UInt32 time, const Base::Packet& packet);

	// Try to push a fragment (to the parent) and following fragments until finding a hole or reaching timeout
	void						processFragments(MAP_FRAGMENTS_ITERATOR& itFragment);

	// Try to push the fragment to the parent
	// return true if the fragment has been processed, otherwise false
	bool						processFragment(MAP_FRAGMENTS_ITERATOR& itFragment);

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

	PeerMedia::OnPeerClose										_onPeerClose; // notify parent that the peer is closing (update the NetGroup push flags)
	PeerMedia::OnPlayPull										_onPlayPull; // called when we receive a pull request
	PeerMedia::OnFragmentsMap									_onFragmentsMap; // called when we receive a fragments map, must return false if we want to ignore the request (if publisher)
	PeerMedia::OnFragment										_onFragment;

	const std::string&											_stream; // stream name
	const std::string											_streamKey; // stream key

	Base::Time													_lastPushUpdate; // last Play Push calculation
	Base::Time													_lastPullUpdate; // last Play Pull calculation
	Base::Time													_lastFragmentsMap; // last Fragments Map Message calculation
	Base::Time													_lastFragment; // last time we received a fragment
	Base::Time													_lastProcessFragment; // last time we have tried to process fragments
	bool														_pullPaused; // True if no fragments have been received since fetch period

	std::map<Base::UInt64, std::unique_ptr<GroupFragment>>		_fragments;
	std::map<Base::Int64, Base::UInt64>							_mapTime2Fragment; // Map of time to fragment (only START and DATA fragments are referenced)
	Base::UInt64												_fragmentCounter; // Current fragment counter of writed fragments (fragments sent to application)

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
