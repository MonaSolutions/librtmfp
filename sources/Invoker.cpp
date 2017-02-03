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

#include "Invoker.h"
#include "RTMFPLogger.h"

using namespace Mona;
using namespace std;

/** ConnectionsManager **/

ConnectionsManager::ConnectionsManager(Invoker& invoker) :_invoker(invoker), Task(invoker), Startable("ServerManager") {
}

void ConnectionsManager::run(Exception& ex) {
	do {
		waitHandle();
	} while (sleep(DELAY_CONNECTIONS_MANAGER) != STOP);
}

void ConnectionsManager::handle(Exception& ex) { _invoker.manage(); }

/** Invoker **/

Invoker::Invoker(UInt16 threads) : Startable("Invoker"), _interruptCb(NULL), _interruptArg(NULL), poolThreads(threads), sockets(*this, poolBuffers, poolThreads), _manager(*this), _lastIndex(0) {
	_logger.reset(new RTMFPLogger());
	Logs::SetLogger(*_logger);
}

Invoker::~Invoker() {

	TRACE("Closing global invoker...")

	// terminate the tasks (forced to do immediatly, because no more "giveHandle" is called)
	TaskHandler::stop();

	// Destroy the connections
	terminate();

	Startable::stop();
}

// Start the socket manager if not started
bool Invoker::start() {
	if(Startable::running()) {
		ERROR("Invoker is already running, call stop method before");
		return false;
	}

	Exception ex;
	if (!((Mona::SocketManager&)sockets).start(ex) || ex || !sockets.running())
		return false;
	
	bool result;
	EXCEPTION_TO_LOG(result = Startable::start(ex, Startable::PRIORITY_HIGH), "Invoker");
	if (result)
		TaskHandler::start();
	return result;
}

unsigned int Invoker::addConnection(std::shared_ptr<RTMFPSession>& pConn) {
	lock_guard<recursive_mutex>	lock(_mutexConnections);

	_mapConnections.emplace(++_lastIndex, pConn);
	return _lastIndex; // Index of a connection is the position in the vector + 1 (0 is reserved for errors)
}

bool	Invoker::getConnection(unsigned int index, std::shared_ptr<RTMFPSession>& pConn) {
	lock_guard<recursive_mutex>	lock(_mutexConnections);
	auto it = _mapConnections.find(index);
	if(it == _mapConnections.end()) {
		WARN("There is no connection at specified index ", index)
		return false;
	}

	pConn = it->second;
	return true;
}

void Invoker::removeConnection(unsigned int index) {
	lock_guard<recursive_mutex>	lock(_mutexConnections);
	auto it = _mapConnections.find(index);
	if(it == _mapConnections.end()) {
		INFO("Connection at index ", index, " as already been removed")
		return;
	}
	removeConnection(it);
}

void Invoker::removeConnection(map<int, shared_ptr<RTMFPSession>>::iterator it) {

	INFO("Deleting connection ", it->first, "...")
	it->second->close(true); // we must close here because there can be shared pointers
	_mapConnections.erase(it);
}

void Invoker::terminate() {
	lock_guard<recursive_mutex>	lock(_mutexConnections);

	Logs::SetDump("");
	auto it = _mapConnections.begin();
	while (it != _mapConnections.end())
		removeConnection(it++);
}

unsigned int Invoker::empty() {
	lock_guard<recursive_mutex>	lock(_mutexConnections);
	return _mapConnections.empty();
}

void Invoker::manage() {
	lock_guard<recursive_mutex>	lock(_mutexConnections);
	auto it = _mapConnections.begin();
	while(it != _mapConnections.end()) {
		it->second->manage();

		if (it->second->closed()) {
			int id = it->first;
			it++;
			removeConnection(id);
			continue;
		}
		it++;
	}
}

void Invoker::run(Exception& exc) {
	Exception exWarn, ex;

	if (!_manager.start(exWarn, Startable::PRIORITY_LOW))
		ex=exWarn;
	else if (exWarn)
		WARN(exWarn.error());
	while (!ex && sleep() != STOP)
		giveHandle(ex);

	// terminate the tasks (forced to do immediatly, because no more "giveHandle" is called)
	TaskHandler::stop();

	_manager.stop();

	if (sockets.running())
		((Mona::SocketManager&)sockets).stop();

	poolThreads.join();

	// release memory
	((Mona::PoolBuffers&)poolBuffers).clear();
}

void Invoker::setLogCallback(void(*onLog)(unsigned int, int, const char*, long, const char*)){
	_logger->setLogCallback(onLog);
}

void Invoker::setDumpCallback(void(*onDump)(const char*, const void*, unsigned int)) { 
	_logger->setDumpCallback(onDump);
}

void Invoker::setInterruptCallback(int(*interruptCb)(void*), void* argument) {
	_interruptCb = interruptCb;
	_interruptArg = argument;
}

bool Invoker::isInterrupted() {
	return !Startable::running() || (_interruptCb && _interruptCb(_interruptArg) == 1);
}