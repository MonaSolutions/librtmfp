/*
Copyright 2014 Mona
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License received along this program for more
details (or else see http://www.gnu.org/licenses/).

This file is a part of Mona.
*/

#pragma once

#include "Mona/Mona.h"
#include "FlashConnection.h"
#include "Mona/PoolBuffers.h"
#include "RTMFPWriter.h"
#include "BandWriter.h"

class RTMFPPacket;
class RTMFPFragment;
class RTMFPFlow : public virtual Mona::Object {
public:
	RTMFPFlow(Mona::UInt64 id,const std::string& signature,/*Peer& peer,*/const Mona::PoolBuffers& poolBuffers, BandWriter& band, const std::shared_ptr<FlashConnection>& pMainStream);
	RTMFPFlow(Mona::UInt64 id,const std::string& signature,const std::shared_ptr<FlashStream>& pStream, /*Peer& peer,*/const Mona::PoolBuffers& poolBuffers, BandWriter& band);
	virtual ~RTMFPFlow();

	const Mona::UInt64		id;

	void				setId(Mona::UInt64 idFlow);

	bool critical() const { return _pWriter->critical; }

	void				receive(Mona::UInt64 stage,Mona::UInt64 deltaNAck,Mona::PacketReader& fragment,Mona::UInt8 flags);
	
	void				commit();

	void				fail(const std::string& error);

	bool				consumed() { return _completed; }

	void				sendConnect(const std::string& url, Mona::UInt16 port);

	void				sendPlay(const std::string& name);

	void				createStream(const std::string& name);
	
private:
	void				onFragment(Mona::UInt64 stage,Mona::PacketReader& fragment,Mona::UInt8 flags);

	void				complete();

	/*Peer&							_peer;
	Group*							_pGroup;*/

	bool							_completed;
	BandWriter&						_band;
	std::shared_ptr<RTMFPWriter>	_pWriter;
	const Mona::UInt64				_stage;
	std::shared_ptr<FlashStream>	_pStream;

	// Receiving
	RTMFPPacket*					_pPacket;
	std::map<Mona::UInt64,RTMFPFragment>	_fragments;
	Mona::UInt32					_numberLostFragments;
	const Mona::PoolBuffers&		_poolBuffers;
};

