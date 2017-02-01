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
#include "AMF.h"
#include "AMFReader.h"

namespace FlashEvents {
	struct OnStatus : Mona::Event<bool(const std::string& code, const std::string& description, Mona::UInt16 streamId, Mona::UInt64 flowId, double cbHandler)> {}; // NetConnection or NetStream status event
	struct OnMedia : Mona::Event<void(const std::string& stream, Mona::UInt32 time, Mona::PacketReader& packet, double lostRate, bool audio)> {};  // Received when we receive media (audio/video) in server or p2p 1-1
	struct OnPlay: Mona::Event<bool(const std::string& streamName, Mona::UInt16 streamId, Mona::UInt64 flowId, double cbHandler)> {}; // Received when a peer is trying to play a stream
	struct OnNewPeer : Mona::Event<void(const std::string& rawId, const std::string& peerId)> {}; // Received when the server send us a peer ID (after NetGroup connection)
	struct OnGroupHandshake : Mona::Event<bool(const std::string& groupId, const std::string& key, const std::string& peerId)> {}; // Received when a connected peer send us the Group hansdhake (only for P2PSession)
	struct OnGroupMedia : Mona::Event<bool(Mona::PacketReader& packet, Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {}; // Received when a connected peer send us a peer Group Media (Subscription/Infos)
	struct OnGroupReport : Mona::Event<void(Mona::PacketReader& packet, Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {};
	struct OnGroupPlayPush: Mona::Event<void(Mona::PacketReader& packet, Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)>{};
	struct OnGroupPlayPull : Mona::Event<void(Mona::PacketReader& packet, Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {};
	struct OnFragmentsMap : Mona::Event<void(Mona::PacketReader& packet, Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {};
	struct OnGroupBegin : Mona::Event<void(Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {};
	struct OnFragment : Mona::Event<void(Mona::UInt8 type, Mona::UInt64 id, Mona::UInt8 splitNumber, Mona::UInt8 mediaType, Mona::UInt32 time, Mona::PacketReader& packet, double lostRate, 
		Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {};
	struct OnGroupAskClose : Mona::Event<bool(Mona::UInt16 streamId, Mona::UInt64 flowId, Mona::UInt64 writerId)> {}; // Receiver when the peer want us to close the connection (if we accept we must close the current flow)
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
	public FlashEvents::OnFragment,
	public FlashEvents::OnGroupAskClose {
public:

	FlashStream(Mona::UInt16 id);
	virtual ~FlashStream();

	const Mona::UInt16	id;

	Mona::UInt32	bufferTime(Mona::UInt32 ms);
	Mona::UInt32	bufferTime() const { return _bufferTime; }

	// return flase if writer is closed!
	virtual bool	process(Mona::PacketReader& packet, Mona::UInt64 flowId, Mona::UInt64 writerId, double lostRate=0);

	virtual void	flush() { }

protected:
	virtual bool	messageHandler(const std::string& name, AMFReader& message, Mona::UInt64 flowId, Mona::UInt64 writerId, double callbackHandler);
	virtual bool	audioHandler(Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);
	virtual bool	videoHandler(Mona::UInt32 time, Mona::PacketReader& packet, double lostRate);

	bool			process(AMF::ContentType type, Mona::UInt32 time, Mona::PacketReader& packet, Mona::UInt64 flowId, Mona::UInt64 writerId, double lostRate);

private:
	virtual bool	rawHandler(Mona::UInt16 type, Mona::PacketReader& data);
	virtual bool	dataHandler(DataReader& data, double lostRate);

	Mona::UInt32	_bufferTime;
	std::string		_streamName;
};
