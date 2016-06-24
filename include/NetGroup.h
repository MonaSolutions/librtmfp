
#pragma once

#include "Mona/Mona.h"
#include "FlashStream.h"
#include "RTMFPConnection.h"
#include "GroupListener.h"
#include <set>

#define NETGROUP_MAX_PACKET_SIZE		959
#define MAX_FRAGMENT_MAP_SIZE			1024
#define MAX_PEER_COUNT					0xFFFFFFFFFFFFFFFF

class MediaPacket;
class GroupNode;
class P2PConnection;
class NetGroup : public virtual Mona::Object {
public:
	NetGroup(const std::string& groupId, const std::string& groupTxt, const std::string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, Mona::UInt16 windowDuration);
	virtual ~NetGroup();

	void close();

	// Add a peer to the Heard List
	void addPeer2HeardList(const std::string& peerId, const char* rawId, const Mona::SocketAddress& address);

	// Add a peer to the NetGroup map
	bool addPeer(const std::string& peerId, std::shared_ptr<P2PConnection> pPeer);

	// Remove a peer from the NetGroup map
	void removePeer(const std::string& peerId, bool full);

	// Return True if the peer doesn't already exists and if the group ID match our group ID
	bool checkPeer(const std::string& groupId, const std::string& peerId);

	// Send report requests (messages 0A, 22)
	void manage();

	const std::string idHex;	// Group ID in hex format
	const std::string idTxt;	// Group ID in plain text (without final zeroes)
	const std::string stream;	// Stream name
	const bool isPublisher;

private:

	// Calculate the estimation of the number of peers (this is the same as Flash NetGroup.estimatedMemberCount)
	double estimatedPeersCount();

	// Calculate the number of neighbors we must connect to (2*log2(N)+13)
	Mona::UInt32 targetNeighborsCount();

	// Return the Group Address calculated from a Peer ID
	static const std::string& GetGroupAddressFromPeerId(const char* rawId, std::string& groupAddress);

	void removePeer(std::map<std::string, std::shared_ptr<P2PConnection>>::iterator& itPeer);

	std::map<Mona::UInt64, MediaPacket>						_fragments;
	std::map<Mona::UInt32, Mona::UInt64>					_mapTime2Fragment; // Map of time to fragment (only START and DATA fragments are referenced)
	Mona::UInt64											_fragmentCounter;
	std::recursive_mutex									_fragmentMutex;

	// Erase old fragments (called before generating the fragments map)
	void	eraseOldFragments();

	// Update the fragment map
	// Return False if there is no fragments, otherwise true
	bool	updateFragmentMap();

	// Build the Group Report for the peer in parameter
	// Return false if the peer is not found
	void	sendGroupReport(std::map<std::string, std::shared_ptr<P2PConnection>>::iterator itPeer);

	// Push an arriving fragment to the peers and write it into the output file (recursive function)
	bool	pushFragment(std::map<Mona::UInt64, MediaPacket>::iterator itFragment);

	// Calculate the pull & play mode balance and send the requests if needed
	void	updatePlayMode();

	// Calculate the Best list from a group address
	void	buildBestList(const std::string& groupAddress, std::set<std::string>& bestList);

	// Connect and disconnect peers to fit the best list
	void	manageBestConnections(std::set<std::string>& bestList);

	Mona::Int64												_updatePeriod; // NetStream.multicastAvailabilityUpdatePeriod equivalent in msec
	Mona::UInt16											_windowDuration; // NetStream.multicastWindowDuration equivalent in msec

	FlashEvents::OnGroupMedia::Type							onGroupMedia;
	FlashEvents::OnGroupReport::Type						onGroupReport;
	FlashEvents::OnGroupPlayPush::Type						onGroupPlayPush;
	FlashEvents::OnGroupPlayPull::Type						onGroupPlayPull;
	FlashEvents::OnFragmentsMap::Type						onFragmentsMap;
	FlashEvents::OnGroupBegin::Type							onGroupBegin;
	FlashEvents::OnFragment::Type							onFragment;
	GroupEvents::OnMedia::Type								onMedia;

	std::string												_myGroupAddress; // Our Group Address (peer identifier into the NetGroup)
	Mona::Buffer											_streamCode; // 2101 + Random key on 32 bytes to be send in the publication infos packet

	std::map<std::string, GroupNode>						_mapHeardList; // Map of peer ID to Group address
	std::map<std::string,std::string>						_mapGroupAddress; // Map of Group Address to peer ID
	std::map<std::string, std::shared_ptr<P2PConnection>>	_mapPeers; // Map of peers ID to p2p connections
	GroupListener*											_pListener; // Listener of the main publication (only one by intance)
	RTMFPConnection&										_conn; // RTMFPConnection related to
	Mona::Time												_lastPlayUpdate; // last Play Pull & Push calculation
	Mona::Time												_lastBestCalculation; // last Best list calculation
	Mona::Time												_lastReport; // last Report Message time
	Mona::Time												_lastFragmentsMap; // last Fragments Map Message time
	Mona::Buffer											_reportBuffer; // Buffer for reporting messages

	bool													_firstPushMode; // True if no play push mode have been send for now
};
