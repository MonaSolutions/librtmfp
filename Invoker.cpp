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

Invoker::Invoker(UInt16 threads) : Startable("Invoker"), poolThreads(threads), sockets(poolBuffers, poolThreads), _manager(*this), _lastIndex(0), _onManage(NULL) {
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

	if(!sockets.running()) {
		Exception ex;
		if (!((Mona::SocketManager&)sockets).start(ex) || ex || !sockets.running())
			return false;
	}

	Exception ex;
	bool result;
	EXCEPTION_TO_LOG(result = Startable::start(ex, Startable::PRIORITY_HIGH), "Invoker");
	if (result)
		TaskHandler::start();
	return result;
}

unsigned int Invoker::addConnection(std::shared_ptr<RTMFPConnection> pConn) {
	_mapConnections[++_lastIndex] = pConn;
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

unsigned int Invoker::empty() {
	return _mapConnections.empty();
}

void Invoker::manage() {
	auto it = _mapConnections.begin();
	while(it != _mapConnections.end()) {
		it->second->manage();

		if (it->second->died) {
			int id = it->first;
			it++;
			removeConnection(id);
			continue;
		}
		it++;
	}
	if (_onManage)
		_onManage();
}

void Invoker::run(Exception& exc) {
	Exception exWarn, ex;
	if (!_manager.start(exWarn, Startable::PRIORITY_LOW))
		ex.set(exWarn);
	else if (exWarn)
		WARN(exWarn.error());
	while (!ex && sleep() != STOP)
		giveHandle(ex);
	if (ex)
		FATAL("Server, ", ex.error());

	_manager.stop();

	if (sockets.running())
		((Mona::SocketManager&)sockets).stop();

	poolThreads.join();

	// release memory
	((Mona::PoolBuffers&)poolBuffers).clear();
}