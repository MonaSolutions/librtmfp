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

using namespace Base;

struct RTMFPDecoder : Runner, virtual Object{
	struct Decoded : Packet {
		Decoded(int idConnection, UInt32 idSession, const SocketAddress& address, Base::shared<Buffer>& pBuffer) : address(address), Packet(pBuffer), idConnection(idConnection), idSession(idSession) {}
		const SocketAddress		address;
		int								idConnection;
		UInt32					idSession;
	};
	typedef Event<void(Decoded& decoded)> ON(Decoded);

	RTMFPDecoder(int idConnection, UInt32 idSession, const SocketAddress& address, const Base::shared<RTMFP::Engine>& pDecoder, Base::shared<Buffer>& pBuffer, const Handler& handler) :
		_handler(handler), _idConnection(idConnection), _idSession(idSession), _address(address), _pBuffer(std::move(pBuffer)), Runner("RTMFPDecoder"), _pDecoder(pDecoder) {}

private:
	bool run(Exception& ex) {
		bool decoded;
		if ((decoded = _pDecoder->decode(ex, *_pBuffer, _address)))
			_handler.queue(onDecoded, _idConnection, _idSession, _address, _pBuffer);
		return decoded;
	}
	Base::shared<RTMFP::Engine>	_pDecoder;
	Base::shared<Buffer>	_pBuffer;
	SocketAddress				_address;
	const Handler&			_handler;
	int								_idConnection;
	UInt32					_idSession;
};
