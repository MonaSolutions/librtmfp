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

namespace FlashEvents {
	struct OnStreamCreated: Mona::Event<void(Mona::UInt16 idStream)> {};
};

/**************************************************************
FlashConnection is linked to an as3 NetConnection
It creates FlashStream (NetStream) and handle messages on the
connection
*/
class FlashConnection : public FlashStream, public virtual Mona::Object,
	public FlashEvents::OnStreamCreated {
public:
	FlashConnection(/*Invoker& invoker,Peer& peer*/);
	virtual ~FlashConnection();

	void	disengage(FlashWriter* pWriter = NULL);

	// Add a new stream to the Main stream with an incremental id
	FlashStream* addStream(std::shared_ptr<FlashStream>& pStream, bool group=false);

	// Add a new stream to the Main stream
	FlashStream* addStream(Mona::UInt16 id, std::shared_ptr<FlashStream>& pStream, bool group=false);

	FlashStream* getStream(Mona::UInt16 id, std::shared_ptr<FlashStream>& pStream);

	void flush() {for(auto& it : _streams) it.second->flush(); }

	// Send the connect request to the RTMFP server
	void connect(FlashWriter& writer, const std::string& url);

	// Send the stream creation request (before play or publish)
	void createStream(FlashWriter& writer);

	// Send the setPeerInfo command to server
	void sendPeerInfo(FlashWriter& writer, Mona::UInt16 port);

	// Call a function on the server/peer
	void callFunction(FlashWriter& writer, const char* function, int nbArgs, const char** args);
	
private:
	void	messageHandler(const std::string& name, AMFReader& message, FlashWriter& writer);
	void	rawHandler(Mona::UInt16 type, Mona::PacketReader& packet, FlashWriter& writer);

	std::map<Mona::UInt16,std::shared_ptr<FlashStream>>	_streams;
	std::string											_buffer;

	bool			_creatingStream; // If we are waiting for a stream to be created
	std::string		_streamToPlay;
};
