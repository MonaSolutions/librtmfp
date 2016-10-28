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
#include "Mona/PacketWriter.h"

class RTMFPWriter;
class BandWriter : public virtual Mona::Object {
public:
	BandWriter(): connected(false) {}

	virtual const Mona::PoolBuffers&		poolBuffers() = 0;
	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter)=0;
	virtual std::shared_ptr<RTMFPWriter>	changeWriter(RTMFPWriter& writer) = 0;

	virtual bool						failed() const = 0;
	virtual bool						canWriteFollowing(RTMFPWriter& writer)=0;
	virtual Mona::UInt32				availableToWrite()=0;
	virtual Mona::BinaryWriter&			writeMessage(Mona::UInt8 type,Mona::UInt16 length,RTMFPWriter* pWriter=NULL)=0;
	virtual void						flush()=0;
	//virtual Mona::UInt16				ping() const = 0;

	bool								connected;
	
};
