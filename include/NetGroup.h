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
#include "RTMFPSession.h"
#include "GroupListener.h"
#include "GroupMedia.h"
#include <set>

#define NETGROUP_MAX_PACKET_SIZE		959
#define MAX_PEER_COUNT					0xFFFFFFFFFFFFFFFF
#define NETGROUP_BEST_LIST_DELAY		10000	// delay between each best list calculation (in msec)
#define NETGROUP_REPORT_DELAY			10000	// delay between each NetGroup Report (in msec)
#define NETGROUP_PUSH_DELAY				2000	// delay between each push request (in msec)
#define NETGROUP_PULL_DELAY				100		// delay between each pull request (in msec)
#define NETGROUP_PEER_TIMEOUT			300000	// number of seconds since the last report known before we delete a peer from the heard list

class GroupNode;
/**************************************
NetGroup is the class that manage
a NetGroup connection related to an
RTMFPSession.
It is composed of GroupMedia objects
*/
class NetGroup : public virtual Mona::Object {
public:
	enum MULTICAST_PARAMETERS {
		UNKNWON_PARAMETER = 2,
		WINDOW_DURATION = 3,
		OBJECT_ENCODING = 4,
		UPDATE_PERIOD = 5,
		SEND_TO_ALL = 6,
		FETCH_PERIOD = 7
	};

	NetGroup(const std::string& groupId, const std::string& groupTxt, const std::string& streamName, RTMFPSession& conn, RTMFPGroupConfig* parameters);

	virtual ~NetGroup();

	void			close();

	// Add a peer to the Heard List
	// param update : if set to True we will recalculate the best list after
	void			addPeer2HeardList(const std::string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const Mona::SocketAddress& hostAddress, Mona::UInt64 timeElapsed=0);

	// Add a peer to the NetGroup map
	bool			addPeer(const std::string& peerId, std::shared_ptr<P2PSession> pPeer);

	// Remove a peer from the NetGroup map
	void			removePeer(const std::string& peerId);

	// Return True if the peer doesn't already exists
	bool			checkPeer(const std::string& peerId);

	// Manage the netgroup peers and send the recurrent requests
	void			manage();

	// Call a function on the peer side
	// return 0 if it fails, 1 otherwise
	unsigned int	callFunction(const char* function, int nbArgs, const char** args);

	// Stop listening if we are publisher
	void			stopListener();
	
	const std::string					idHex;	// Group ID in hex format
	const std::string					idTxt;	// Group ID in plain text (without final zeroes)
	const std::string					stream;	// Stream name
	RTMFPGroupConfig*					groupParameters; // NetGroup parameters

private:
	#define MAP_PEERS_TYPE std::map<std::string, std::shared_ptr<P2PSession>>
	#define MAP_PEERS_ITERATOR_TYPE std::map<std::string, std::shared_ptr<P2PSession>>::iterator

	// Static function to read group config parameters sent in a Media Subscription message
	static void					ReadGroupConfig(std::shared_ptr<RTMFPGroupConfig>& parameters, Mona::PacketReader& packet);

	// Return the Group Address calculated from a Peer ID
	static const std::string&	GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress);

	// Calculate the estimation of the number of peers (this is the same as Flash NetGroup.estimatedMemberCount)
	double						estimatedPeersCount();

	// Calculate the number of neighbors we must connect to (2*log2(N)+13)
	Mona::UInt32				targetNeighborsCount();

	void						removePeer(MAP_PEERS_ITERATOR_TYPE itPeer);

	// Build the Group Report for the peer in parameter
	// Return false if the peer is not found
	void						sendGroupReport(P2PSession* pPeer, bool initiator);

	// Update our NetGroup Best List
	void						updateBestList();

	// Calculate the Best list from a group address
	void						buildBestList(const std::string& groupAddress, std::set<std::string>& bestList);

	// Connect and disconnect peers to fit the best list
	void						manageBestConnections();

	P2PEvents::OnPeerGroupBegin::Type						onGroupBegin;
	P2PEvents::OnPeerGroupReport::Type						onGroupReport;
	P2PEvents::OnNewMedia::Type								onNewMedia;
	P2PEvents::OnPeerClose::Type							onPeerClose;
	GroupMediaEvents::OnGroupPacket::Type					onGroupPacket;

	std::string												_myGroupAddress; // Our Group Address (peer identifier into the NetGroup)

	std::map<std::string, GroupNode>						_mapHeardList; // Map of peer ID to Group address
	std::map<std::string,std::string>						_mapGroupAddress; // Map of Group Address to peer ID (same as heard list)
	std::set<std::string>									_bestList; // Last best list calculated
	MAP_PEERS_TYPE											_mapPeers; // Map of peers ID to p2p connections
	GroupListener*											_pListener; // Listener of the main publication (only one by intance)
	RTMFPSession&											_conn; // RTMFPSession related to
	Mona::Time												_lastBestCalculation; // last Best list calculation
	Mona::Time												_lastReport; // last Report Message calculation
	Mona::Buffer											_reportBuffer; // Buffer for reporting messages

	std::map<std::string, GroupMedia>						_mapGroupMedias; // map of stream key to GroupMedia
	std::map<std::string, GroupMedia>::iterator				_groupMediaPublisher; // iterator to the GroupMedia publisher
};
