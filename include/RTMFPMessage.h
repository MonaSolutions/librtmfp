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
	RTMFPMessage(AMF::ContentType type, Mona::UInt32 time, bool repeatable) :   _frontSize(type==AMF::EMPTY ? 0 : (type==AMF::DATA_AMF3 ? 6 : 5)), repeatable(repeatable) {
		if (type == AMF::EMPTY)
			return;
		_front[0] = type;
		Mona::BinaryWriter(&_front[1], 4).write32(time);
		if (type == AMF::DATA_AMF3)
			_front[5] = 0;
	}

	const Mona::UInt8*	front()  const { return _front;  }
	Mona::UInt8			frontSize() const { return _frontSize; }

	virtual const Mona::UInt8*	body()  const = 0;
	virtual Mona::UInt32			bodySize() const = 0;

	Mona::UInt32					size() const { return frontSize()+bodySize(); }

	std::map<Mona::UInt32,Mona::UInt64>	fragments;
	const bool				repeatable;
private:
	Mona::UInt8					_front[6];
	Mona::UInt8					_frontSize;
};


class RTMFPMessageUnbuffered : public RTMFPMessage, public virtual Mona::Object {
public:
	RTMFPMessageUnbuffered(const Mona::UInt8* data, Mona::UInt32 size) : _data(data), _size(size),RTMFPMessage(false) {}
	RTMFPMessageUnbuffered(AMF::ContentType type, Mona::UInt32 time,const Mona::UInt8* data, Mona::UInt32 size) : _data(data), _size(size),RTMFPMessage(type,time,false) {}

private:
	const Mona::UInt8*	body() const { return _data; }
	Mona::UInt32			bodySize() const { return _size; }

	Mona::UInt32			_size;
	const Mona::UInt8*	_data;
};



class RTMFPMessageBuffered: public RTMFPMessage, virtual public Mona::NullableObject {
public:
	RTMFPMessageBuffered(const Mona::PoolBuffers& poolBuffers,bool repeatable) : _pWriter(new AMFWriter(poolBuffers)),RTMFPMessage(repeatable) {}
	RTMFPMessageBuffered() : _pWriter(&AMFWriter::Null),RTMFPMessage(false) {}
	
	virtual ~RTMFPMessageBuffered() { if (*_pWriter) delete _pWriter; }

	AMFWriter&		writer() { return *_pWriter; }

	operator bool() const { return *_pWriter; }

private:

	const Mona::UInt8*	body() const { return _pWriter->packet.data(); }
	Mona::UInt32			bodySize() const { return _pWriter->packet.size(); }

	AMFWriter*		_pWriter;

};

