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
#include "RTMFP.h"
#include "Mona/Socket.h"
#include "Mona/Runner.h"

struct RTMFPSender : Mona::Runner, Mona::BinaryWriter, virtual Mona::Object {
	RTMFPSender(const std::shared_ptr<Mona::Socket>& pSocket, const std::shared_ptr<RTMFPEngine>& pEncoder): _pSocket(pSocket), _pEncoder(pEncoder), Mona::Runner("RTMFPSender"), farId(0), Mona::BinaryWriter(*new Mona::Buffer(RTMFP_HEADER_SIZE)) {
		_pBuffer.reset(&buffer());
	}
	
	Mona::UInt32					farId;
	Mona::SocketAddress				address;

private:	
	bool			run(Mona::Exception& ex);
	std::shared_ptr<RTMFPEngine>	_pEncoder;
	std::shared_ptr<Mona::Socket>	_pSocket;
	std::shared_ptr<Mona::Buffer>	_pBuffer;
};
