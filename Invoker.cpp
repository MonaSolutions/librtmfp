#include "Invoker.h"

using namespace Mona;

ConnectionsManager::ConnectionsManager(Invoker& invoker):_invoker(invoker),Task(invoker),Startable("ServerManager") {
}

void ConnectionsManager::run(Exception& ex) {
	do {
		waitHandle();
	} while (sleep(DELAY_CONNECTIONS_MANAGER) != STOP);
}

void ConnectionsManager::handle(Exception& ex) { _invoker.manage(); }

Invoker::Invoker(UInt16 threads) : Startable("Invoker"), poolThreads(threads), sockets(*this, poolBuffers, poolThreads), _manager(*this), _lastIndex(0), _init(false) {
}

Invoker::~Invoker() {
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

unsigned int Invoker::addConnection(std::shared_ptr<RTMFPConnection>& pConn) {
	_init = true;
	_mapConnections.emplace(++_lastIndex, pConn);
	return _lastIndex; // Index of a connection is the position in the vector + 1 (0 is reserved for errors)
}

bool	Invoker::getConnection(unsigned int index, std::shared_ptr<RTMFPConnection>& pConn) {
	auto it = _mapConnections.find(index);
	if(it == _mapConnections.end()) {
		ERROR("There is no connection at specified index ", index)
		return false;
	}

	pConn = it->second;
	return true;
}

void Invoker::removeConnection(unsigned int index) {
	auto it = _mapConnections.find(index);
	if(it == _mapConnections.end()) {
		ERROR("There is no connection at specified index ", index)
		return;
	}

	_mapConnections.erase(it);
}

void Invoker::terminate() {
	_mapConnections.clear();
}

unsigned int Invoker::empty() {
	return _mapConnections.empty();
}

void Invoker::manage() {
	auto it = _mapConnections.begin();
	while(it != _mapConnections.end()) {
		it->second->manage();

		if (it->second->failed()) {
			int id = it->first;
			it++;
			removeConnection(id);
			continue;
		}
		it++;
	}

	if (_init && _mapConnections.empty())
		terminate();
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