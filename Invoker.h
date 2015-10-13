#include "Mona/Mona.h"
#include "Mona/Startable.h"
#include "Mona/SocketManager.h"
#include "Mona/PoolThreads.h"
#include "Mona/PoolBuffers.h"
#include "RTMFPConnection.h"

#define DELAY_CONNECTIONS_MANAGER	1000 // 1s

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

	Invoker(Mona::UInt16 threads) : Mona::Startable("Invoker"), poolThreads(threads), sockets(poolBuffers, poolThreads), _manager(*this) {}
	virtual ~Invoker();

	// Start the socket manager if not started
	bool start();

	unsigned int addConnection(std::shared_ptr<RTMFPConnection> pConn);

	std::shared_ptr<RTMFPConnection>	getConnection(unsigned int index);

	void removeConnection(unsigned int index);

	unsigned int count();

	const Mona::SocketManager				sockets;
	Mona::PoolThreads						poolThreads;
	const Mona::PoolBuffers					poolBuffers;
private:
	virtual void		manage();
	void				requestHandle() { wakeUp(); }
	void				run(Mona::Exception& exc);

	ConnectionsManager								_manager;
	std::vector<std::shared_ptr<RTMFPConnection>>	_connections;
};
