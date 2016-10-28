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
#include "Mona/UDPSender.h"
#include "Mona/PacketWriter.h"
#include "RTMFP.h"

class RTMFPSender : public Mona::UDPSender, public virtual Mona::Object {
public:
	RTMFPSender(const Mona::PoolBuffers& poolBuffers,const std::shared_ptr<RTMFPEngine>& pEncoder): _pEncoder(pEncoder),Mona::UDPSender("RTMFPSender"),packet(poolBuffers),farId(0) {
		packet.next(RTMFP_HEADER_SIZE);
	}
	
	Mona::UInt32		farId;
	Mona::PacketWriter	packet;

private:
	const Mona::UInt8*	data() const { return packet.size() < RTMFP_MIN_PACKET_SIZE ? NULL : packet.data(); }
	Mona::UInt32		size() const { return packet.size(); }
	
	bool			run(Mona::Exception& ex);

	const std::shared_ptr<RTMFPEngine>	_pEncoder;
};
