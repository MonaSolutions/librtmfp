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

#include "Mona/Mona.h"
#include "P2PSession.h"
#include "GroupListener.h"

/**********************************************
GroupMedia is the class that manage a stream
from a NetGroup connection
*/
struct GroupMedia : virtual Mona::Object {
	typedef Mona::Event<void(Mona::UInt32 time, const Mona::Packet& packet, double lostRate, AMF::Type type)> ON(GroupPacket); // called when receiving a new packet
	
	GroupMedia(const std::string& name, const std::string& key, std::shared_ptr<RTMFPGroupConfig> parameters);
	virtual ~GroupMedia();

	// Regularly called to send the fragments maps, the pull requests and the push requests
	void						manage();

	// Add the peer to map of peer, return false if the peer is already known
	void						addPeer(const std::string& peerId, std::shared_ptr<PeerMedia>& pPeer);
	
	// Send the Group Media infos to peer
	void						sendGroupMedia(std::shared_ptr<PeerMedia>& pPeer);

	// Return true if we have at least one fragment
	bool						hasFragments() { return !_fragments.empty(); }

	// Create a new fragment that will call a function
	void						callFunction(const char* function, int nbArgs, const char** args);

	GroupListener::OnMedia						onMedia; // Create a new fragment from a media packet

	Mona::UInt32								id; // id of the GroupMedia (incremental)
	std::shared_ptr<RTMFPGroupConfig>			groupParameters; // group parameters for this Group Media stream
	
private:
	#define MAP_PEERS_INFO_TYPE std::map<std::string, std::shared_ptr<PeerMedia>>
	#define MAP_PEERS_INFO_ITERATOR_TYPE std::map<std::string, std::shared_ptr<PeerMedia>>::iterator
	#define MAP_FRAGMENTS_ITERATOR std::map<Mona::UInt64, GroupFragment>::iterator

	// Add a new fragment to the map _fragments
	void						addFragment(MAP_FRAGMENTS_ITERATOR& itFragment, PeerMedia* pPeer, Mona::UInt8 marker, Mona::UInt64 id, Mona::UInt8 splitedNumber, Mona::UInt8 mediaType, Mona::UInt32 time, const Mona::Packet& packet);

	// Push an arriving fragment to the peers and write it into the output file (recursive function)
	bool						pushFragment(std::map<Mona::UInt64, GroupFragment>::iterator& itFragment);

	// Update the fragment map
	// Return 0 if there is no fragments, otherwise the last fragment number
	Mona::UInt64				updateFragmentMap();

	// Erase old fragments (called before generating the fragments map)
	void						eraseOldFragments();

	// Calculate the push play mode balance and send the requests if needed
	void						sendPushRequests();

	// Send the Pull requests if needed
	void						sendPullRequests();

	// Go to the next peer for pull or push
	// idFragment : if > 0 it will test the availability of the fragment
	// ascending : order of the research
	bool						getNextPeer(MAP_PEERS_INFO_ITERATOR_TYPE& itPeer, bool ascending, Mona::UInt64 idFragment, Mona::UInt8 mask);

	// Send the fragment pull request to the next available peer
	bool						sendPullToNextPeer(Mona::UInt64 idFragment);

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

	Mona::Time													_lastPushUpdate; // last Play Push calculation
	Mona::Time													_lastPullUpdate; // last Play Pull calculation
	Mona::Time													_lastFragmentsMap; // last Fragments Map Message calculation

	std::map<Mona::UInt64, GroupFragment>						_fragments;
	std::map<Mona::UInt32, Mona::UInt64>						_mapTime2Fragment; // Map of time to fragment (only START and DATA fragments are referenced)
	Mona::UInt64												_fragmentCounter; // Current fragment counter of writed fragments (fragments sent to application)

	Mona::Buffer												_fragmentsMapBuffer; // General buffer for fragments map
	static Mona::UInt32											GroupMediaCounter; // static counter of GroupMedia for id assignment

	MAP_PEERS_INFO_TYPE											_mapPeers; // map of peers subscribed to this media stream

	// map of peers iterators
	MAP_PEERS_INFO_ITERATOR_TYPE								_itFragmentsPeer; // Current peer for fragments map requests
	MAP_PEERS_INFO_ITERATOR_TYPE								_itPushPeer; // Current peer for push request
	MAP_PEERS_INFO_ITERATOR_TYPE								_itPullPeer; // Current peer for pull request

	// Pushers calculation
	bool														_firstPushMode; // True if no play push mode have been send for now
	Mona::UInt8													_currentPushMask; // current mask analyzed
	std::map<Mona::UInt8, std::pair<std::string, Mona::UInt64>>	_mapPushMasks; // Map of push mask to a pair of peerId/fragmentId

	 // Pull calculation TODO: convert PullRequest to a pair<peerId, time>
	struct PullRequest : public Object {
		PullRequest(std::string id) : peerId(id) {}

		std::string peerId; // Id of the peer to which we have send the pull request
		Mona::Time time; // Time when the request have been done
	};
	std::map<Mona::UInt64, PullRequest>							_mapWaitingFragments; // Map of waiting fragments in Pull requests to peer Id
	std::map<Mona::Int64, Mona::UInt64>							_mapPullTime2Fragment; // Map of reception time to fragments map id (used for pull requests)
	Mona::UInt64												_lastFragmentMapId; // Last Fragments map Id received (used for pull requests)
	Mona::UInt64												_currentPullFragment; // Current pull fragment index
	bool														_firstPullReceived; // True if we have received the first pull fragment => we can start writing
};
