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
#include "FlashStream.h"
#include "RTMFPConnection.h"
#include "GroupListener.h"
#include <set>

#define NETGROUP_MAX_PACKET_SIZE		959
#define MAX_FRAGMENT_MAP_SIZE			1024
#define MAX_PEER_COUNT					0xFFFFFFFFFFFFFFFF
#define NETGROUP_PUSH_DELAY				2000	// delay between each push request (in msec)
#define NETGROUP_PULL_DELAY				100		// delay between each pull request (in msec)
#define NETGROUP_PEER_TIMEOUT			300000	// number of seconds since the last report known before we delete a peer from the heard list

class MediaPacket;
class GroupNode;
class P2PConnection;
struct RTMFPGroupConfig;
class NetGroup : public virtual Mona::Object {
public:
	enum MULTICAST_PARAMETERS {
		NETGROUP_UNKNWON_PARAMETER = 2,
		NETGROUP_WINDOW_DURATION = 3,
		NETGROUP_OBJECT_ENCODING = 4,
		NETGROUP_UPDATE_PERIOD = 5,
		NETGROUP_SEND_TO_ALL = 6,
		NETROUP_FETCH_PERIOD = 7
	};

	NetGroup(const std::string& groupId, const std::string& groupTxt, const std::string& streamName, RTMFPConnection& conn, RTMFPGroupConfig* parameters);
	virtual ~NetGroup();

	void close();

	// Add a peer to the Heard List
	void addPeer2HeardList(const std::string& peerId, const char* rawId, const Mona::SocketAddress& address, RTMFP::AddressType addressType, const Mona::SocketAddress& hostAddress);

	// Add a peer to the NetGroup map
	bool addPeer(const std::string& peerId, std::shared_ptr<P2PConnection> pPeer);

	// Remove a peer from the NetGroup map
	void removePeer(const std::string& peerId);

	// Return True if the peer doesn't already exists
	bool checkPeer(const std::string& peerId);

	// Send report requests (messages 0A, 22)
	void manage();

	// If the peer is connected send the group Media message to start read/write of the stream
	void sendGroupMedia(std::shared_ptr<P2PConnection> pPeer);

	// Call a function on the peer side
	// return 0 if it fails, 1 otherwise
	unsigned int callFunction(const char* function, int nbArgs, const char** args);
	
	const std::string					idHex;	// Group ID in hex format
	const std::string					idTxt;	// Group ID in plain text (without final zeroes)
	const std::string					stream;	// Stream name
	RTMFPGroupConfig*					groupParameters; // NetGroup parameters

private:
	#define MAP_PEERS_TYPE std::map<std::string, std::shared_ptr<P2PConnection>>
	#define MAP_PEERS_ITERATOR_TYPE std::map<std::string, std::shared_ptr<P2PConnection>>::iterator

	// Calculate the estimation of the number of peers (this is the same as Flash NetGroup.estimatedMemberCount)
	double estimatedPeersCount();

	// Calculate the number of neighbors we must connect to (2*log2(N)+13)
	Mona::UInt32 targetNeighborsCount();

	// Return the Group Address calculated from a Peer ID
	static const std::string& GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress);

	void removePeer(MAP_PEERS_ITERATOR_TYPE& itPeer);

	// Erase old fragments (called before generating the fragments map)
	void	eraseOldFragments();

	// Update the fragment map
	// Return 0 if there is no fragments, otherwise the last fragment number
	Mona::UInt64	updateFragmentMap();

	// Build the Group Report for the peer in parameter
	// Return false if the peer is not found
	void	sendGroupReport(const MAP_PEERS_ITERATOR_TYPE& itPeer);

	// Push an arriving fragment to the peers and write it into the output file (recursive function)
	bool	pushFragment(std::map<Mona::UInt64, MediaPacket>::iterator& itFragment);

	// Calculate the push play mode balance and send the requests if needed
	void	sendPushRequests();

	// Send the Pull requests if needed
	void	sendPullRequests();

	// Calculate the Best list from a group address
	void	buildBestList(const std::string& groupAddress, std::set<std::string>& bestList);

	// Connect and disconnect peers to fit the best list
	void	manageBestConnections();

	// Read addresses and add peer to heard list if needed
	// return : True if a new peer has been discovered (to redo the best list calculation)
	bool	readAddress(Mona::PacketReader& packet, Mona::UInt16 size, Mona::UInt32 targetCount, const std::string& newPeerId, const std::string& rawId);

	// Go to the next peer for pull or push
	// idFragment : if > 0 it will test the availability of the fragment
	// ascending : order of the research
	bool	getNextPeer(MAP_PEERS_ITERATOR_TYPE& itPeer, bool ascending, Mona::UInt64 idFragment, Mona::UInt8 mask);

	// Send the fragment pull request to the next available peer
	bool	sendPullToNextPeer(Mona::UInt64 idFragment);

	std::map<Mona::UInt64, MediaPacket>						_fragments;
	std::map<Mona::UInt32, Mona::UInt64>					_mapTime2Fragment; // Map of time to fragment (only START and DATA fragments are referenced)
	Mona::UInt64											_fragmentCounter; // Current fragment counter of writed fragments (fragments sent to application)
	bool													_firstPullReceived; // True if we have received the first pull fragment => we can start writing
	std::recursive_mutex									_fragmentMutex; // Global mutex for NetGroup

	FlashEvents::OnGroupMedia::Type							onGroupMedia;
	FlashEvents::OnGroupReport::Type						onGroupReport;
	FlashEvents::OnGroupPlayPush::Type						onGroupPlayPush;
	FlashEvents::OnGroupPlayPull::Type						onGroupPlayPull;
	FlashEvents::OnFragmentsMap::Type						onFragmentsMap;
	FlashEvents::OnGroupBegin::Type							onGroupBegin;
	FlashEvents::OnFragment::Type							onFragment;
	GroupEvents::OnMedia::Type								onMedia;
	P2PEvents::OnPeerClose::Type							onPeerClose; // callback when a peer close its group connection

	std::string												_myGroupAddress; // Our Group Address (peer identifier into the NetGroup)
	std::unique_ptr<Mona::Buffer>							_pStreamCode; // 2101 + Random key on 32 bytes identifying the publication (for group media Subscription message)

	std::map<std::string, GroupNode>						_mapHeardList; // Map of peer ID to Group address
	std::map<std::string,std::string>						_mapGroupAddress; // Map of Group Address to peer ID (same as heard list)
	std::set<std::string>									_bestList; // Last best list calculated
	MAP_PEERS_TYPE											_mapPeers; // Map of peers ID to p2p connections
	GroupListener*											_pListener; // Listener of the main publication (only one by intance)
	RTMFPConnection&										_conn; // RTMFPConnection related to
	Mona::Time												_lastPushUpdate; // last Play Push calculation
	Mona::Time												_lastPullUpdate; // last Play Pull calculation
	Mona::Time												_lastBestCalculation; // last Best list calculation
	Mona::Time												_lastReport; // last Report Message calculation
	Mona::Time												_lastFragmentsMap; // last Fragments Map Message calculation
	Mona::Buffer											_reportBuffer; // Buffer for reporting messages

	// Pushers calculation
	bool														_firstPushMode; // True if no play push mode have been send for now
	Mona::UInt8													_currentPushMask; // current mask analyzed
	MAP_PEERS_ITERATOR_TYPE										_itPushPeer; // Current peer for push request
	std::map<Mona::UInt8, std::pair<std::string, Mona::UInt64>>	_mapPushMasks; // Map of push mask to a pair of peerId/fragmentId

	// Pull calculation
	struct PullRequest : public Object {
		PullRequest(std::string id) : peerId(id) {}

		std::string peerId; // Id of the peer to which we have send the pull request
		Mona::Time time; // Time when the request have been done
	};
	std::map<Mona::UInt64, PullRequest>						_mapWaitingFragments; // Map of waiting fragments in Pull requests to peer Id
	std::map<Mona::Int64, Mona::UInt64>						_mapPullTime2Fragment; // Map of reception time to fragments map id (used for pull requests)

	Mona::UInt64											_lastFragmentMapId; // Last Fragments map Id received (used for pull requests)
	Mona::UInt64											_currentPullFragment; // Current pull fragment index
	MAP_PEERS_ITERATOR_TYPE									_itPullPeer; // Current peer for pull request
};
