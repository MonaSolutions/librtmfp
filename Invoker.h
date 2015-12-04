#include "Mona/SocketManager.h"
#include "Mona/TerminateSignal.h"
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

	Invoker(Mona::UInt16 threads);
	virtual ~Invoker();

	// Start the socket manager if not started
	bool			start();

	unsigned int	addConnection(std::shared_ptr<RTMFPConnection>& pConn);

	bool			getConnection(unsigned int index, std::shared_ptr<RTMFPConnection>& pConn);

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
	std::map<int,std::shared_ptr<RTMFPConnection>>	_mapConnections;
	int												_lastIndex; // last index of connection
};
