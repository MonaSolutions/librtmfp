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

#include "Connection.h"

/**************************************************
DefaultConnection is used by SocketHandler :
 - handle messages from unknown addresses
 - and send handshake messages without connection
*/
class DefaultConnection : public Connection {
public:
	DefaultConnection(SocketHandler* pHandler);

	~DefaultConnection();

	// Close the connection properly
	//virtual void close();

	// Called by SocketHandler before receiving/sending a message
	void							setAddress(const Mona::SocketAddress& address) { _address = address; }

protected:

	// Handle message received
	virtual void					handleMessage(const Mona::PoolBuffer& pBuffer);

private:
	// Handle a handshake 30 received from an unknown address
	void							handleHandshake30(Mona::BinaryReader& reader);

	// Handle a handshake 70 received from an unknown address
	void							handleHandshake70(Mona::BinaryReader& reader);
};