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
#include "AMF.h"
#include "AMFReader.h"
#include "FlashWriter.h"

namespace FlashEvents {
	struct OnStatus : Mona::Event<void(const std::string& code, const std::string& description, FlashWriter& writer)> {};
	struct OnMedia : Mona::Event<void(const std::string& peerId, const std::string& stream, Mona::UInt32 time, Mona::PacketReader& packet, double lostRate, bool audio)> {};
	struct OnPlay: Mona::Event<bool(const std::string& streamName, FlashWriter& writer)> {};
	struct OnNewPeer : Mona::Event<void(const std::string& groupId, const std::string& peerId)> {};
	struct OnGroupHandshake : Mona::Event<void(const std::string& groupId, const std::string& key, const std::string& peerId)> {};
	struct OnGroupMedia : Mona::Event<void(const std::string& peerId, Mona::PacketReader& packet, FlashWriter& writer)> {};
	struct OnGroupReport : Mona::Event<void(const std::string& peerId, Mona::PacketReader& packet, FlashWriter& writer)> {};
	struct OnGroupPlayPush: Mona::Event<void(const std::string& peerId, Mona::PacketReader& packet, FlashWriter& writer)>{};
	struct OnGroupPlayPull : Mona::Event<void(const std::string& peerId, Mona::PacketReader& packet, FlashWriter& writer)> {};
	struct OnFragmentsMap : Mona::Event<void(const std::string& peerId, Mona::PacketReader& packet, FlashWriter& writer)> {};
	struct OnGroupBegin : Mona::Event<void(const std::string& peerId, FlashWriter& writer)> {};
	struct OnFragment : Mona::Event<void(const std::string& peerId, Mona::UInt8 type, Mona::UInt64 id, Mona::UInt8 splitNumber, Mona::UInt8 mediaType, Mona::UInt32 time, Mona::PacketReader& packet, double lostRate)> {};
};

/**************************************************************
FlashStream is linked to an as3 NetStream
*/
class FlashStream : public virtual Mona::Object,
	public FlashEvents::OnStatus,
	public FlashEvents::OnMedia,
	public FlashEvents::OnPlay,
	public FlashEvents::OnNewPeer,
	public FlashEvents::OnGroupHandshake,
	public FlashEvents::OnGroupMedia,
	public FlashEvents::OnGroupReport,
	public FlashEvents::OnGroupPlayPush,
	public FlashEvents::OnGroupPlayPull,
	public FlashEvents::OnFragmentsMap,
	public FlashEvents::OnGroupBegin,
	public FlashEvents::OnFragment {
public:

	FlashStream(Mona::UInt16 id);
	virtual ~FlashStream();

	const Mona::UInt16	id;

	Mona::UInt32	bufferTime(Mona::UInt32 ms);
	Mona::UInt32	bufferTime() const { return _bufferTime; }

	void	disengage(FlashWriter* pWriter=NULL);

	// return flase if writer is closed!
	virtual bool	process(Mona::PacketReader& packet,FlashWriter& writer, double lostRate=0);

	virtual void	flush() { }

	// Send the connect request to the RTMFP server
	virtual void connect(FlashWriter& writer,const std::string& url);

	// Send the createStream request to the RTMFP server
	virtual void createStream(FlashWriter& writer);

	// Send the play request to the RTMFP server
	virtual void play(FlashWriter& writer, const std::string& name);

	// Send the publish request to the RTMFP server
	virtual void publish(FlashWriter& writer, const std::string& name);

	// Send the setPeerInfo request to the RTMFP server
	virtual void sendPeerInfo(FlashWriter& writer, Mona::UInt16 port);

	// Call a function on the far side
	void call(FlashWriter& writer, const char* function, int nbArgs, const char** args);

	// Send the group connection request to the server
	void sendGroupConnect(FlashWriter& writer, const std::string& groupId);

	// Send the group connection request to the peer
	void sendGroupPeerConnect(FlashWriter& writer, const std::string& netGroup, const Mona::UInt8* key, const char* rawId /*, bool initiator*/);

	// Send the group begin message (02 + 0E)
	void sendGroupBegin(FlashWriter& writer);

	// Send the group publication infos
	void sendGroupMediaInfos(FlashWriter& writer, const std::string& stream, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt64 updatePeriod, Mona::UInt16 windowDuration, Mona::UInt16 fetchPeriod);

	// Send the media
	void sendRaw(FlashWriter& writer, const Mona::UInt8* data, Mona::UInt32 size, bool flush);

	// Send the group play message (type 23)
	void sendGroupPlay(FlashWriter& writer, Mona::UInt8 mode);

	// Send a group pull request (type 2B)
	void sendGroupPull(FlashWriter& writer, Mona::UInt64 index) { writer.writeGroupPull(index); }

	// Record target peer ID for identifying media source (play mode)
	virtual void setPeerId(const std::string& peerId) { _peerId = peerId; }

protected:
	virtual void	messageHandler(const std::string& name, AMFReader& message, FlashWriter& writer);
	virtual void	audioHandler(Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);
	virtual void	videoHandler(Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);

	std::string		_peerId; // Peer ID of the target in plain text
	std::string		_groupId; // Group ID (only for NetGroup stream)

private:
	virtual void	rawHandler(Mona::UInt16 type, Mona::PacketReader& data, FlashWriter& writer);
	virtual void	dataHandler(DataReader& data, double lostRate);

	Mona::UInt32	_bufferTime;
	std::string		_streamName;
};
