#include "Mona/Mona.h"
#include "Mona/SocketManager.h"
#include "Mona/PoolThreads.h"
#include "Mona/PoolBuffers.h"
#include "RTMFPConnection.h"

class Invoker : public virtual Mona::Object {
public:

	Invoker(Mona::UInt16 threads) : poolThreads(threads), sockets(poolBuffers, poolThreads) {}
	virtual ~Invoker() {}

	// Start the socket manager if not started
	bool start(Mona::Exception& ex) {
		if (!sockets.running())
			return ((Mona::SocketManager&)sockets).start(ex) && !ex && sockets.running();
		return true;
	}

	// Stop the socket manager if running
	void stop() {
		if (sockets.running())
			((Mona::SocketManager&)sockets).stop();
	}

	unsigned int addConnection(std::shared_ptr<RTMFPConnection> pConn) {
		_mapConnections.push_back(pConn);
		return _mapConnections.size(); // Index of a connection is the position in the vector + 1 (0 is reserved for errors)
	}

	std::shared_ptr<RTMFPConnection>	getConnection(unsigned int index) {
		return _mapConnections.at(index-1); // Index of a connection is the position in the vector + 1 (0 is reserved for errors)
	}

	const Mona::SocketManager				sockets;
	Mona::PoolThreads						poolThreads;
	const Mona::PoolBuffers					poolBuffers;
private:
	std::vector<std::shared_ptr<RTMFPConnection>>	_mapConnections;
};
