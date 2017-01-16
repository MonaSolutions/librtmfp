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
#include "Mona/Event.h"
#include "Mona/PacketReader.h"
#include <set>

#define MAX_FRAGMENT_MAP_SIZE			1024 // TODO: check this

class PeerMedia;
class P2PSession;
class RTMFPWriter;
struct RTMFPGroupConfig;

namespace PeerMediaEvents {
	struct OnPeerClose : Mona::Event<void(const std::string& peerId, Mona::UInt8 mask)> {}; // notify parent that the peer is closing (update the NetGroup push flags)
	struct OnPlayPull : Mona::Event<void(PeerMedia*, Mona::UInt64)> {}; // called when we receive a pull request
	struct OnFragmentsMap : Mona::Event<bool(Mona::UInt64)> {}; // called when we receive a fragments map, must return false if we want to ignore the request (if publisher)
	struct OnFragment : Mona::Event<void(PeerMedia*, const std::string&, Mona::UInt8, Mona::UInt64, Mona::UInt8, Mona::UInt8, Mona::UInt32, Mona::PacketReader&, double) > {}; // called when receiving a fragment
}

/***************************************************
Class used to save group media infos for
a peer in a NetGroup stream and send media and report
media messages to the peer
*/
class PeerMedia : public virtual Mona::Object,
	public PeerMediaEvents::OnPeerClose,
	public PeerMediaEvents::OnPlayPull,
	public PeerMediaEvents::OnFragmentsMap,
	public PeerMediaEvents::OnFragment {
public:
	PeerMedia(P2PSession* pSession, std::shared_ptr<RTMFPWriter>& pMediaReportWriter);
	virtual ~PeerMedia();

	// Close the PeerMedia object
	void close(bool abrupt);

	// Called by P2PSession to close the media writer
	void closeMediaWriter(bool abrupt);

	// Set the media writer
	void setMediaWriter(std::shared_ptr<RTMFPWriter>& pWriter);

	// Flush the media report writer
	void flushReportWriter();

	// Called by P2PSession when receiving a fragments map
	void onFragmentsMap(Mona::UInt64 id, const Mona::UInt8* data, Mona::UInt32 size);

	// Called by P2PSession when receiving a fragment
	void onFragment(Mona::UInt8 marker, Mona::UInt64 id, Mona::UInt8 splitedNumber, Mona::UInt8 mediaType, Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);

	// Return True if bit number is available in the fragments map (for push out mode)
	bool checkMask(Mona::UInt8 bitNumber);

	// Return True if the fragment is available
	bool hasFragment(Mona::UInt64 index);

	// Write the Group publication infos
	void sendGroupMedia(const std::string& stream, const std::string& streamKey, RTMFPGroupConfig* groupConfig);

	// Create the flow if necessary and send media
	// The fragment is sent if pull is true or if this is a pushable fragment
	bool sendMedia(const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt64 fragment, bool pull = false);

	// Send the Fragments map message
	// param lastFragment : latest fragment in the message
	// return : true if the fragments map has been sent
	bool sendFragmentsMap(Mona::UInt64 lastFragment, const Mona::UInt8* data, Mona::UInt32 size);

	// Set the Group Publish Push mode (after a message 23)
	void setPushMode(Mona::UInt8 mode);

	// Update the Group Play Push mode
	void sendPushMode(Mona::UInt8 mode);

	// Send a pull request (2B)
	void sendPull(Mona::UInt64 index);

	// Handle a pull request
	void onPlayPull(Mona::UInt64 index);

	// Add a fragment to the blacklist of pull to avoid a new pull request for this peer
	void addPullBlacklist(Mona::UInt64 idFragment);

	Mona::UInt64					idFlow; // id of the Media Report RTMFPFlow linked to, used to create the Media Writer
	Mona::UInt64					idFlowMedia; // id of the Media RTMFPFlow (the one who send fragments)
	const std::string*				pStreamKey; // pointer to the streamKey index in the map P2PSession::_mapStream2PeerMedia
	Mona::UInt8						pushInMode; // Group Play Push mode
	bool							groupMediaSent; // True if the Group Media infos have been sent
private:
	// Return true if the new fragment is pushable (according to the Group push mode)
	bool							isPushable(Mona::UInt8 rest);

	P2PSession*						_pParent; // P2P session related to

	Mona::UInt8						_pushOutMode; // Group Publish Push mode
	Mona::Buffer					_fragmentsMap; // Last Fragments Map received
	Mona::UInt64					_idFragmentsMapIn; // Last ID received from the Fragments Map
	Mona::UInt64					_idFragmentsMapOut; // Last ID sent in the Fragments map
	std::set<Mona::UInt64>			_blacklistPull; // set of fragments blacklisted for pull requests to this peer
	std::shared_ptr<RTMFPWriter>	_pMediaReportWriter; // Media Report writer used to send report messages from the current media
	std::shared_ptr<RTMFPWriter>	_pMediaWriter; // Writer for media packets
};
