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

#include "Base/IOSocket.h"
#include "Base/Timer.h"

#define DELAY_CONNECTIONS_MANAGER	50 // Delay between each onManage (in msec)

class RTMFPSession;
class RTMFPLogger;
struct Invoker : private Base::Thread {

	// Create the Invoker
	// createLogger : if True it will associate a logger instance to the static log class, otherwise it will let the default logger
	Invoker(bool createLogger=true);
	virtual ~Invoker();

	// Start the socket manager if not started
	bool			start();

	unsigned int	addConnection(std::shared_ptr<RTMFPSession>& pConn);

	bool			getConnection(unsigned int index, std::shared_ptr<RTMFPSession>& pConn);

	void			removeConnection(unsigned int index);

	bool			isInterrupted();

	unsigned int	empty();

	/*** Callback functions ***/
	void			setLogCallback(void(*onLog)(unsigned int, const char*, long, const char*));

	void			setDumpCallback(void(*onDump)(const char*, const void*, unsigned int));

	void			setInterruptCallback(int(*interruptCb)(void*), void* argument);

private:
	Base::Handler						_handler; // keep in first (must be build before sockets)
public:
	Base::ThreadPool					threadPool; // keep in first (must be build before sockets)
	Base::IOSocket						sockets;
	const Base::Timer&					timer; 
	const Base::Handler&				handler;
private:
	virtual void		manage();
	bool				run(Base::Exception& exc, const volatile bool& stopping);

	void				removeConnection(std::map<int, std::shared_ptr<RTMFPSession>>::iterator it);
	Base::Timer										_timer;
	int												_lastIndex; // last index of connection
	std::mutex										_mutexConnections;
	std::map<int, std::shared_ptr<RTMFPSession>>	_mapConnections;
	std::unique_ptr<RTMFPLogger>					_logger; // global logger for librtmfp
	int												(*_interruptCb)(void*); // global interrupt callback function (NULL by default)
	void*											_interruptArg; // global interrup callback argument for interrupt function
};
