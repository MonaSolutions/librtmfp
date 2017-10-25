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
#include "FlashStream.h"
#include "RTMFPSession.h"
#include "GroupListener.h"
#include "GroupMedia.h"
#include "GroupBuffer.h"
#include <set>

#define NETGROUP_MAX_REPORT_SIZE		20000 // max size used for NetGroup Report messages
#define NETGROUP_MAX_PACKET_SIZE		959
#define MAX_PEER_COUNT					0xFFFFFFFFFFFFFFFF
#define NETGROUP_BEST_LIST_DELAY		5000	// delay between each best list calculation (in msec)
#define NETGROUP_REPORT_DELAY			10000	// delay between each NetGroup Report (in msec)
#define NETGROUP_PUSH_DELAY				2000	// delay between each push request (in msec)
#define NETGROUP_PULL_DELAY				100		// delay between each pull request (in msec)
#define NETGROUP_PEER_TIMEOUT			300000	// number of msec since the last report known before we delete a peer from the heard list
#define NETGROUP_DISCONNECT_DELAY		90000	// delay between each try to disconnect from a peer
#define NETGROUP_MEDIA_TIMEOUT			300000	// number of msec before we delete a GroupMedia after being closed
#define NETGROUP_PROCESS_FGMT_TIMEOUT	50		// number of msec before exiting the processFragments function
#define NETGROUP_MIN_PEERS_TIMEOUT		6		// number of p2p connections tries to reach before saying that a peer is p2p unable
#define NETGROUP_TIMEOUT_P2PABLE		100000	// number of msec since the 6th connection try before closing the connection when it's p2p unable
#define NETGROUP_STATS_DELAY			5000	// delay between each print of statistics (in msec)
#define NETGROUP_CLEAN_DELAY			19000	// delay betwen each Heard List clean (in msec)
#define NETGROUP_TIMEOUT_P2PRATE		30000	// delay before disconnecting if p2p connection rate is too low (in msec)
#define NETGROUP_RATE_MIN				3		// minimum rate of p2p connection to keep connection open

/**************************************
NetGroup is the class that manage
a NetGroup connection related to an
RTMFPSession.
It is composed of GroupMedia objects
*/
struct NetGroup : FlashHandler, virtual Base::Object {
public:
	enum MULTICAST_PARAMETERS {
		UNKNWON_PARAMETER = 2,
		WINDOW_DURATION = 3,
		OBJECT_ENCODING = 4,
		UPDATE_PERIOD = 5,
		SEND_TO_ALL = 6,
		FETCH_PERIOD = 7
	};

	NetGroup(const Base::Timer& timer, Base::UInt16 mediaId, const std::string& groupId, const std::string& groupTxt, const std::string& streamName, RTMFPSession& conn, RTMFPGroupConfig* parameters, 
		bool audioReliable, bool videoReliable);
	virtual ~NetGroup();

	// Close the NetGroup
	void			close();

	// Add a peer to the Heard List
	// param update : if set to True we will recalculate the best list after
	void			addPeer2HeardList(const std::string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const Base::SocketAddress& hostAddress, Base::UInt64 timeElapsed=0);

	// Handle a peer disconnection, set the peer has died
	void			handlePeerDisconnection(const std::string& peerId);

	// Add a peer to the NetGroup map
	bool			addPeer(const std::string& peerId, std::shared_ptr<P2PSession> pPeer);

	// Remove a peer from the NetGroup map
	void			removePeer(const std::string& peerId);

	// Manage the netgroup peers and send the recurrent requests
	void			manage();

	// Call a function on the peer side
	// return 0 if it fails, 1 otherwise
	unsigned int	callFunction(const std::string& function, std::queue<std::string>& arguments);

	// Stop listening if we are publisher
	void			stopListener();

	// Called by parent when the server send us a new peer ID to connect to
	// Return True if the peer doesn't already exists
	bool			p2pNewPeer(const std::string& peerId);

	// Called by parent when a peer is trying to connect to us
	void			p2PAddressExchange(const std::string& tag);
	
	// Called by parent when a concurrent connection happen (to update P2P count)
	void			handleConcurrentSwitch() { --_countP2P; } // This is not a fail

	// Called by parent when the server send us a new Peer ID (new peer connected to the group)
	void			newGroupPeer(const std::string& peerId, const char* rawId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const Base::SocketAddress& hostAddress);
	
	const std::string					idHex;	// Group ID in hex format
	const std::string					idTxt;	// Group ID in plain text (without final zeroes)
	const std::string					stream;	// Stream name
	RTMFPGroupConfig*					groupParameters; // NetGroup parameters

protected:
	// FlashHandler messageHandler implementation
	virtual bool	messageHandler(const std::string& name, AMFReader& message, Base::UInt64 flowId, Base::UInt64 writerId, double callbackHandler);

private:
	#define MAP_PEERS_TYPE std::map<std::string, std::shared_ptr<P2PSession>>
	#define MAP_PEERS_ITERATOR_TYPE std::map<std::string, std::shared_ptr<P2PSession>>::iterator

	// Static function to read group config parameters sent in a Media Subscription message
	static void					ReadGroupConfig(std::shared_ptr<RTMFPGroupConfig>& parameters, Base::BinaryReader& packet);

	// Return the Group Address calculated from a Peer ID
	static const std::string&	GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress);

	// Return the size of peer addresses for Group Report 
	static Base::UInt32			AddressesSize(const Base::SocketAddress& host, const PEER_LIST_ADDRESS_TYPE& addresses);

	// Calculate the number of neighbors we must connect to (log2(N)+13)
	static Base::UInt32			TargetNeighborsCount(double peersCount);

	// Calculate the estimation of the number of peers (this is the same as Flash NetGroup.estimatedMemberCount)
	double						estimatedPeersCount();

	// Remove a peer from the connected peer list
	void						removePeer(MAP_PEERS_ITERATOR_TYPE itPeer);

	// Build the Group Report for the peer in parameter
	// Return false if the peer is not found
	void						sendGroupReport(P2PSession* pPeer, bool initiator);

	// Update our NetGroup Best List
	void						updateBestList();

	// Calculate the Best list from a group address
	void						buildBestList(const std::string& groupAddress, const std::string& peerId, std::set<std::string>& bestList);

	// Connect and disconnect peers to fit the best list
	void						manageBestConnections(const std::set<std::string>& oldList);

	// Clean the Heard List
	void						cleanHeardList();

	// Peer instance in the heard list
	struct GroupNode : virtual Base::Object {
		GroupNode(const char* rawPeerId, const std::string& groupId, const PEER_LIST_ADDRESS_TYPE& listAddresses, const Base::SocketAddress& host, Base::UInt64 timeElapsed) :
			rawId(rawPeerId, PEER_ID_SIZE + 2), groupAddress(groupId), addresses(listAddresses), hostAddress(host), lastGroupReport(((Base::UInt64)Base::Time::Now()) - (timeElapsed * 1000)), died(false) {}

		bool		died; // Trick to stop connexion to old peers faster
		std::string rawId;
		std::string groupAddress;
		PEER_LIST_ADDRESS_TYPE addresses;
		Base::SocketAddress hostAddress;
		Base::Int64 lastGroupReport; // Time in msec of last Group report received
	};

	// Read the group report and return true if at least a new peer has been found
	bool						readGroupReport(const std::map<std::string, GroupNode>::iterator& itNode, Base::BinaryReader& packet);

	P2PSession::OnPeerGroupReport							_onGroupReport; // called when receiving a Group Report message from the peer
	P2PSession::OnNewMedia									_onNewMedia; // called when a new PeerMedia is called (new stream available for the peer)
	P2PSession::OnClosedMedia								_onClosedMedia; // called when the peer publisher close a Group Media
	P2PSession::OnPeerGroupBegin							_onGroupBegin; // called when receiving a Group Begin message from the peer
	P2PSession::OnPeerClose									_onPeerClose; // called when the peer is closing
	P2PSession::OnPeerGroupAskClose							_onGroupAskClose;
	GroupMedia::OnNewFragment								_onNewFragment; // called when receiving a fragment to push it to the GroupBuffer
	GroupMedia::OnRemovedFragments							_onRemovedFragments; // called when fragments have been removed
	GroupMedia::OnStartProcessing							_onStartProcessing; // called when receiving the first pull fragment, processing can start
	
	Base::Timer::OnTimer									_onCleanHeardList; // clean the Heard List from old peers
	Base::Timer::OnTimer									_onBestList; // update the Best List
	const Base::Timer&										_timer; // timer for all time events

	std::unique_ptr<GroupBuffer>							_pGroupBuffer; // Group fragments buffer, order all fragment in a thread and forward them

	std::string												_myGroupAddress; // Our Group Address (peer identifier into the NetGroup)
	PEER_LIST_ADDRESS_TYPE									_myAddresses; // Our public ip addresses for Group Report
	
	bool													_audioReliable; // if False we do not send back audio packets
	bool													_videoReliable; // if False we do not send back video packets

	std::map<std::string, GroupNode>						_mapHeardList; // Map of peer ID to Group address and info from Group Report
	std::map<std::string,std::string>						_mapGroupAddress; // Map of Group Address to peer ID (same as heard list)
	std::set<std::string>									_bestList; // Last best list calculated
	MAP_PEERS_TYPE											_mapPeers; // Map of peers ID to p2p connections
	GroupListener*											_pListener; // Listener of the main publication (only one by intance)
	RTMFPSession&											_conn; // RTMFPSession related to
	Base::Buffer											_reportBuffer; // Buffer for reporting messages

	Base::Time												_lastReport; // last Report Message calculation
	Base::Time												_lastStats; // last Statistics print

	bool													_p2pAble; // True if at least 1 connection has succeed
	Base::Time												_p2pAbleTime; // Time since p2pExchanges reaches 6 to detect a p2p unable error
	Base::UInt8												_p2pExchanges; // Count of p2p tries to control p2p ability
	std::set<std::string>									_p2pEntities; // Set of identifier (tag or peer ID)

	Base::UInt64											_countP2P; // Count of p2p attempt
	Base::UInt64											_countP2PSuccess; // Count of p2p success
	Base::Time												_p2pRateTime; // last Time we check the p2p rate

	std::map<std::string, GroupMedia>						_mapGroupMedias; // map of stream key to GroupMedia
	std::map<std::string, GroupMedia>::iterator				_groupMediaPublisher; // iterator to the GroupMedia publisher
};
