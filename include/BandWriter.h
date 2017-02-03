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
#include "Mona/UDPSocket.h"

class RTMFPSender;
class RTMFPEngine;
class PoolThread;
/***************************************************
BandWriter class is used to write messages
It is implemented by FlowManager & RTMFPHandshaker
*/
class BandWriter : public virtual Mona::Object {
public:
	BandWriter();
	virtual ~BandWriter();

	// Return the pool buffers object
	virtual const Mona::PoolBuffers&		poolBuffers()=0;
	
	// Return the data for writing
	virtual Mona::UInt8*					packet();

	// Return the name of the session
	virtual const std::string&				name() = 0;

	// Flush the current packet
	// marker is : 0B for handshake, 09 for raw request, 89 for AMF request
	virtual void							flush(bool echoTime, Mona::UInt8 marker);

	// Return true if the session has failed
	virtual bool							failed()=0;

	// Return the socket object
	virtual Mona::UDPSocket&				socket(Mona::IPAddress::Family family)=0;

	// Handle receiving packet
	virtual void							process(const Mona::SocketAddress& address, Mona::PoolBuffer& pBuffer)=0;

protected:
	bool									decode(const Mona::SocketAddress& address, Mona::PoolBuffer& pBuffer);

	std::shared_ptr<RTMFPSender>			_pSender; // Current sender object

	// Encryption/Decryption
	std::shared_ptr<RTMFPEngine>			_pDecoder;
	std::shared_ptr<RTMFPEngine>			_pEncoder;
	
	Mona::UInt32							_farId;
	Mona::UInt16							_timeReceived;
	Mona::Time								_lastReceptionTime;
	Mona::SocketAddress						_address;

private:
	Mona::PoolThread*						_pThread; // Thread used to send last message
};
