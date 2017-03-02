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
#include "AMFWriter.h"


class RTMFPMessage : public virtual Mona::Object {
public:

	RTMFPMessage(bool repeatable) : repeatable(repeatable),_frontSize(0) {}
	RTMFPMessage(AMF::Type type, Mona::UInt32 time, bool repeatable) :   _frontSize(type==AMF::TYPE_EMPTY ? 0 : (type==AMF::TYPE_DATA_AMF3 ? 6 : 5)), repeatable(repeatable) {
		if (type == AMF::TYPE_EMPTY)
			return;
		_front[0] = type;
		Mona::BinaryWriter(&_front[1], 4).write32(time);
		if (type == AMF::TYPE_DATA_AMF3)
			_front[5] = 0;
	}

	const Mona::UInt8*	front()  const { return _front;  }
	Mona::UInt8			frontSize() const { return _frontSize; }

	virtual const Mona::UInt8*		body()  const = 0;
	virtual Mona::UInt32			bodySize() const = 0;

	Mona::UInt32					size() const { return frontSize()+bodySize(); }

	std::map<Mona::UInt32,Mona::UInt64>	fragments;
	const bool					repeatable;
private:
	Mona::UInt8					_front[6];
	Mona::UInt8					_frontSize;
};


struct RTMFPMessageUnbuffered : RTMFPMessage, virtual Mona::Object {

	RTMFPMessageUnbuffered(const Mona::Packet& packet) : _packet(std::move(packet)), RTMFPMessage(false) {}
	RTMFPMessageUnbuffered(AMF::Type type, Mona::UInt32 time, const Mona::Packet& packet) : _packet(std::move(packet)), RTMFPMessage(type, time, false) {}

private:
	virtual const Mona::UInt8*	body() const { return _packet.data(); }
	virtual Mona::UInt32		bodySize() const { return _packet.size(); }

	Mona::Packet		_packet;
};

struct RTMFPMessageBuffered: RTMFPMessage, virtual Mona::Object {
	RTMFPMessageBuffered(bool repeatable) : RTMFPMessage(repeatable), writer(*new Mona::Buffer()) { _pBuffer.reset(&writer->buffer()); }
	RTMFPMessageBuffered() : RTMFPMessage(false), writer(Mona::Buffer::Null()) {}

	AMFWriter						writer;

private:
	virtual const Mona::UInt8*	body() const { return _pBuffer->data(); }
	virtual Mona::UInt32		bodySize() const { return _pBuffer->size(); }
	
	std::shared_ptr<Mona::Buffer>	_pBuffer;

};

