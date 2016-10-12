
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
	enum MULTICAST_PARAMETERS {
		NETGROUP_UNKNWON_PARAMETER = 2,
		NETGROUP_WINDOW_DURATION = 3,
		NETGROUP_OBJECT_ENCODING = 4,
		NETGROUP_UPDATE_PERIOD = 5,
		NETGROUP_SEND_TO_ALL = 6,
		NETROUP_FETCH_PERIOD = 7
	};

	NetGroup(const std::string& groupId, const std::string& groupTxt, const std::string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, Mona::UInt16 windowDuration);
	virtual ~NetGroup();

	void close();

	// Add a peer to the Heard List
	void addPeer2HeardList(const std::string& peerId, const char* rawId, const Mona::SocketAddress& address, RTMFP::AddressType addressType, const Mona::SocketAddress& hostAddress);

	// Add a peer to the NetGroup map
	bool addPeer(const std::string& peerId, std::shared_ptr<P2PConnection> pPeer);

	// Remove a peer from the NetGroup map
	void removePeer(const std::string& peerId, bool full);

	// Called by peer when far peer close the Group Report writer
	void peerIsClosingNetgroup(const std::string& peerId);

	// Return True if the peer doesn't already exists and if the group ID match our group ID
	bool checkPeer(const std::string& groupId, const std::string& peerId);

	// Send report requests (messages 0A, 22)
	void manage();

	// Call a function on the peer side
	// return 0 if it fails, 1 otherwise
	unsigned int callFunction(const char* function, int nbArgs, const char** args);

	const std::string idHex;	// Group ID in hex format
	const std::string idTxt;	// Group ID in plain text (without final zeroes)
	const std::string stream;	// Stream name
	const bool isPublisher;

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

	std::map<Mona::UInt64, MediaPacket>						_fragments;
	std::map<Mona::UInt32, Mona::UInt64>					_mapTime2Fragment; // Map of time to fragment (only START and DATA fragments are referenced)
	std::set<Mona::UInt64>									_waitingFragments; // List of waiting fragments in Pull requests
	Mona::UInt64											_fragmentCounter;
	std::recursive_mutex									_fragmentMutex;

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
	void	updatePushMode();

	// Send the Pull requests if needed
	void	sendPullRequests();

	// Calculate the Best list from a group address
	void	buildBestList(const std::string& groupAddress, std::set<std::string>& bestList);

	// Connect and disconnect peers to fit the best list
	void	manageBestConnections(std::set<std::string>& bestList);

	// Read a pair of addresses and add peer to lists if neaded
	void	readAddress(Mona::PacketReader& packet, Mona::UInt16 size, Mona::UInt32 targetCount, const std::string& newPeerId, const std::string& rawId, bool noPeerID);

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
	std::unique_ptr<Mona::Buffer>							_pStreamCode; // 2101 + Random key on 32 bytes identifying the publication (for group media info message)

	std::map<std::string, GroupNode>						_mapHeardList; // Map of peer ID to Group address
	std::map<std::string,std::string>						_mapGroupAddress; // Map of Group Address to peer ID
	MAP_PEERS_TYPE											_mapPeers; // Map of peers ID to p2p connections
	GroupListener*											_pListener; // Listener of the main publication (only one by intance)
	RTMFPConnection&										_conn; // RTMFPConnection related to
	Mona::Time												_lastPlayUpdate; // last Play Pull & Push calculation
	Mona::Time												_lastBestCalculation; // last Best list calculation
	Mona::Time												_lastReport; // last Report Message time
	Mona::Time												_lastFragmentsMap; // last Fragments Map Message time
	Mona::Buffer											_reportBuffer; // Buffer for reporting messages

	bool													_firstPushMode; // True if no play push mode have been send for now

	// Pushers calculation
	Mona::UInt8												_currentPushMask; // current mask analyzed
	std::string												_currentPushPeer; // current pusher analyzed
	bool													_currentPushIsBad; // True if the pusher analyzed asn't send any fragment for now
};
