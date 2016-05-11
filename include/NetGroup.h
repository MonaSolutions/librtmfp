
#pragma once

#include "Mona/Mona.h"
#include "FlashStream.h"
#include "RTMFPConnection.h"
#include "GroupListener.h"

#define NETGROUP_MAX_PACKET_SIZE		959

class P2PConnection;
class NetGroup : public virtual Mona::Object {
public:
	NetGroup(const std::string& groupId, const std::string& groupTxt, const std::string& streamName, bool publisher, RTMFPConnection& conn, double updatePeriod, Mona::UInt16 windowDuration);
	virtual ~NetGroup();

	void close();

	// Add a peer to the NetGroup map
	void addPeer(std::string peerId, std::shared_ptr<P2PConnection> pPeer);

	// Send report requests (messages 0A, 22)
	void manage();

	const std::string idHex;	// Group ID in hex format
	const std::string idTxt;	// Group ID in plain text (without final zeroes)
	const std::string stream;	// Stream name
	const bool isPublisher;

private:

	// Update the fragment map
	// Return False if there is no fragments, otherwise true
	bool	updateFragmentMap();

	// Build the Group Report for the peer in parameter
	// Return false if the peer is not found
	bool	buildGroupReport(const std::string& peerId);

	// Fragments 
	struct MediaPacket {

		MediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 time, AMF::ContentType type,
			Mona::UInt64 fragmentId, Mona::UInt8 marker, Mona::UInt8 splitId);

		Mona::PoolBuffer	pBuffer;
		Mona::UInt32		time;
		Mona::UInt16		fragmentSize;
	};
	std::map<Mona::UInt64, MediaPacket>						_fragments;
	std::map<Mona::UInt32, Mona::UInt64>					_mapTime2Fragment;
	Mona::UInt64											_fragmentCounter;
	std::recursive_mutex									_fragmentMutex;

	double													_updatePeriod; // NetStream.multicastAvailabilityUpdatePeriod equivalent in msec
	Mona::UInt16											_windowDuration; // NetStream.multicastWindowDuration equivalent in msec

	FlashEvents::OnGroupMedia::Type							onGroupMedia;
	FlashEvents::OnGroupReport::Type						onGroupReport;
	FlashEvents::OnGroupPlayPush::Type						onGroupPlayPush;
	FlashEvents::OnGroupPlayPull::Type						onGroupPlayPull;
	FlashEvents::OnFragmentsMap::Type						onFragmentsMap;
	FlashEvents::OnGroupBegin::Type							onGroupBegin;
	GroupEvents::OnMedia::Type								onMedia;

	Mona::Buffer											_streamCode; // 2101 + Random key on 32 bytes to be send in the publication infos packet

	std::map<std::string, std::shared_ptr<P2PConnection>>	_mapPeers; // Map of peers ID to p2p connections
	std::map<std::string, FlashWriter*>						_mapId2Writer; // Map of peers ID to report writers
	GroupListener*											_pListener; // Listener of the main publication (only one by intance)
	RTMFPConnection&										_conn; // RTMFPConnection related to
	Mona::Time												_lastReport; // last Report Message time
	Mona::Time												_lastFragmentsMap; // last Fragments Map Message time
	Mona::Buffer											_reportBuffer; // Buffer for reporting messages

	Mona::UInt64											_lastSent; // Last fragment id sent
};
