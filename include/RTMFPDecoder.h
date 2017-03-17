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

#include "Mona/Runner.h"
#include "Mona/Event.h"
#include "Mona/Handler.h"
#include "Mona/Packet.h"
#include "RTMFP.h"

struct RTMFPDecoder : Mona::Runner, virtual Mona::Object{
	struct Decoded : Mona::Packet {
		Decoded(Mona::UInt32 id, const Mona::SocketAddress& address, std::shared_ptr<Mona::Buffer>& pBuffer) : address(address), Packet(pBuffer), idSession(id) {}
		const Mona::SocketAddress		address;
		Mona::UInt32					idSession;
	};
	typedef Mona::Event<void(Decoded& decoded)> ON(Decoded);

	RTMFPDecoder(Mona::UInt32 id, const Mona::SocketAddress& address, const std::shared_ptr<RTMFP::Engine>& pDecoder, std::shared_ptr<Mona::Buffer>& pBuffer, const Mona::Handler& handler) :
		_handler(handler), _idSession(id), _address(address), _pBuffer(std::move(pBuffer)), Mona::Runner("RTMFPDecoder"), _pDecoder(pDecoder) {}

private:
	bool run(Mona::Exception& ex) {
		bool decoded;
		if ((decoded = _pDecoder->decode(ex, *_pBuffer, _address)))
			_handler.queue(onDecoded, _idSession, _address, _pBuffer);
		return decoded;
	}
	std::shared_ptr<RTMFP::Engine>	_pDecoder;
	std::shared_ptr<Mona::Buffer>	_pBuffer;
	Mona::SocketAddress				_address;
	const Mona::Handler&			_handler;
	Mona::UInt32					_idSession;
};
