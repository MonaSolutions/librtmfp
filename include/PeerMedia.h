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
#include "Base/Event.h"
#include "Base/Packet.h"
#include "AMF.h"
#include <set>

#define MAX_FRAGMENT_MAP_SIZE			1024 // TODO: check this

struct P2PSession;
struct RTMFPWriter;
struct RTMFPGroupConfig;

// Fragment instance
struct GroupFragment : Base::Packet, virtual Base::Object {
	GroupFragment(const Base::Packet& packet, Base::UInt32 time, AMF::Type mediaType, Base::UInt64 fragmentId, Base::UInt8 groupMarker, Base::UInt8 splitId) :
		id(fragmentId), splittedId(splitId), type(mediaType), marker(groupMarker), time(time), Packet(std::move(packet)) {}

	Base::UInt32		time;
	AMF::Type			type;
	Base::UInt8			marker;
	Base::UInt64		id;
	Base::UInt8			splittedId;
};

/***************************************************
Class used to save group media infos for
a peer in a NetGroup stream and send media and report
media messages to the peer
*/
struct PeerMedia : public virtual Base::Object {
	typedef Base::Event<void(const std::string& peerId, Base::UInt8 mask)>	ON(PeerClose); // notify parent that the peer is closing (update the NetGroup push flags)
	typedef Base::Event<void(PeerMedia*, Base::UInt64, bool)>				ON(PlayPull); // called when we receive a pull request
	typedef Base::Event<bool(Base::UInt64)>									ON(FragmentsMap); // called when we receive a fragments map, must return false if we want to ignore the request (if publisher)
	typedef Base::Event<void(PeerMedia*, const std::string&, Base::UInt8, Base::UInt64, Base::UInt8, Base::UInt8, Base::UInt32, const Base::Packet&, double)> ON(Fragment); // called when receiving a fragment

	PeerMedia(P2PSession* pSession, std::shared_ptr<RTMFPWriter>& pMediaReportWriter);
	virtual ~PeerMedia();

	// Close the PeerMedia object
	void close(bool abrupt);

	// Called by P2PSession to close the media writer
	void closeMediaWriter(bool abrupt);

	// Flush the media report writer
	void flushReportWriter();

	// Called by P2PSession when receiving a fragments map
	void handleFragmentsMap(Base::UInt64 id, const Base::UInt8* data, Base::UInt32 size);

	// Called by P2PSession when receiving a fragment
	void handleFragment(Base::UInt8 marker, Base::UInt64 id, Base::UInt8 splitedNumber, Base::UInt8 mediaType, Base::UInt32 time, const Base::Packet& packet, double lostRate);

	// Return True if bit number is available in the fragments map (for push out mode)
	bool checkMask(Base::UInt8 bitNumber);

	// Return True if the fragment is available
	bool hasFragment(Base::UInt64 index);

	// Write the Group publication infos
	void sendGroupMedia(const std::string& stream, const std::string& streamKey, RTMFPGroupConfig* groupConfig);

	// Write the Group publication end message
	void sendEndMedia(Base::UInt64 lastFragment);

	// Create the flow if necessary and send media
	// The fragment is sent if pull is true or if this is a pushable fragment
	bool sendMedia(const GroupFragment& fragment, bool pull, bool reliable);

	// Send the Fragments map message
	// param lastFragment : latest fragment in the message
	// return : true if the fragments map has been sent
	bool sendFragmentsMap(Base::UInt64 lastFragment, const Base::UInt8* data, Base::UInt32 size);

	// Set the Group Publish Push mode (after a message 23)
	void setPushMode(Base::UInt8 mode);

	// Update the Group Play Push mode
	void sendPushMode(Base::UInt8 mode);

	// Send a pull request (2B)
	void sendPull(Base::UInt64 index);

	// Handle a pull request
	void handlePlayPull(Base::UInt64 index, bool flush);

	// Flush the media Writer
	void flush();

	Base::UInt64					id; // id of the PeerMedia, it is also the id of the report writer
	Base::UInt64					idFlow; // id of the Media Report RTMFPFlow linked to, used to create the Media Writer
	Base::UInt64					idFlowMedia; // id of the Media RTMFPFlow (the one who send fragments)
	const std::string*				pStreamKey; // pointer to the streamKey index in the map P2PSession::_mapStream2PeerMedia
	Base::UInt8						pushInMode; // Group Play Push mode
	bool							groupMediaSent; // True if the Group Media infos have been sent

private:
	// Return true if the new fragment is pushable (according to the Group push mode)
	bool							isPushable(Base::UInt8 rest);

	P2PSession*						_pParent; // P2P session related to
	bool							_closed; // closed state

	Base::UInt8						_pushOutMode; // Group Publish Push mode
	Base::Buffer					_fragmentsMap; // Last Fragments Map received
	Base::UInt64					_idFragmentsMapIn; // Last ID received from the Fragments Map
	Base::UInt64					_idFragmentsMapOut; // Last ID sent in the Fragments map
	std::shared_ptr<RTMFPWriter>	_pMediaReportWriter; // Media Report writer used to send report messages from the current media
	std::shared_ptr<RTMFPWriter>	_pMediaWriter; // Writer for media packets
};
