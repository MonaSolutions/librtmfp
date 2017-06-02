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

#include "Base/Runner.h"
#include "Base/Event.h"
#include "Base/Handler.h"
#include "Base/Packet.h"
#include "RTMFP.h"

struct RTMFPDecoder : Base::Runner, virtual Base::Object{
	struct Decoded : Base::Packet {
		Decoded(Base::UInt32 id, const Base::SocketAddress& address, std::shared_ptr<Base::Buffer>& pBuffer) : address(address), Packet(pBuffer), idSession(id) {}
		const Base::SocketAddress		address;
		Base::UInt32					idSession;
	};
	typedef Base::Event<void(Decoded& decoded)> ON(Decoded);

	RTMFPDecoder(Base::UInt32 id, const Base::SocketAddress& address, const std::shared_ptr<RTMFP::Engine>& pDecoder, std::shared_ptr<Base::Buffer>& pBuffer, const Base::Handler& handler) :
		_handler(handler), _idSession(id), _address(address), _pBuffer(std::move(pBuffer)), Base::Runner("RTMFPDecoder"), _pDecoder(pDecoder) {}

private:
	bool run(Base::Exception& ex) {
		bool decoded;
		if ((decoded = _pDecoder->decode(ex, *_pBuffer, _address)))
			_handler.queue(onDecoded, _idSession, _address, _pBuffer);
		return decoded;
	}
	std::shared_ptr<RTMFP::Engine>	_pDecoder;
	std::shared_ptr<Base::Buffer>	_pBuffer;
	Base::SocketAddress				_address;
	const Base::Handler&			_handler;
	Base::UInt32					_idSession;
};
