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

#include "Mona/SocketManager.h"
#include "Mona/TerminateSignal.h"
#include "RTMFPSession.h"

#define DELAY_CONNECTIONS_MANAGER	50 // Delay between each onManage (in msec)

class Invoker;
// Thread class that call manage() function of the connections each second to flush the writers and send ping requests
class ConnectionsManager : private Mona::Task, public Mona::Startable, public virtual Mona::Object {
public:
	ConnectionsManager(Invoker& invoker);
	virtual ~ConnectionsManager() {}
private:
	void run(Mona::Exception& ex);
	void handle(Mona::Exception& ex);
	Invoker& _invoker;
};

class Invoker : public Mona::TaskHandler, private Mona::Startable {
friend class ConnectionsManager;
public:

	Invoker(Mona::UInt16 threads);
	virtual ~Invoker();

	// Start the socket manager if not started
	bool			start();

	unsigned int	addConnection(std::shared_ptr<RTMFPSession>& pConn);

	bool			getConnection(unsigned int index, std::shared_ptr<RTMFPSession>& pConn);

	void			removeConnection(unsigned int index);

	unsigned int	empty();

	void			terminate();

	const Mona::SocketManager				sockets;
	Mona::PoolThreads						poolThreads;
	const Mona::PoolBuffers					poolBuffers;
private:
	virtual void		manage();
	void				requestHandle() { wakeUp(); }
	void				run(Mona::Exception& exc);

	bool											_init; // True if at least a connection has been added
	ConnectionsManager								_manager;
	int												_lastIndex; // last index of connection

	std::recursive_mutex							_mutexConnections;
	std::map<int, std::shared_ptr<RTMFPSession>>	_mapConnections;
};
