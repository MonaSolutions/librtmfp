#include "Mona/Mona.h"
#include "Mona/SocketManager.h"
#include "Mona/PoolThreads.h"
#include "Mona/PoolBuffers.h"

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

	const Mona::SocketManager	sockets;
	Mona::PoolThreads			poolThreads;
	const Mona::PoolBuffers		poolBuffers;
};
