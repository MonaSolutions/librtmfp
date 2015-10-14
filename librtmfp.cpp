#include "librtmfp.h"
#include "Mona/Exceptions.h"
#include "RTMFPConnection.h"
#include "Mona/Logs.h"
#include "Mona/String.h"
#include "Invoker.h"

using namespace Mona;
using namespace std;

extern "C" {

static std::shared_ptr<Invoker> GlobalInvoker;

unsigned int RTMFP_Connect(const char* host, int port, const char* url, void (__cdecl * onSocketError)(const char*), 
						   void (__cdecl * onStatusEvent)(const char*, const char*), void (__cdecl * onMedia)(unsigned int, const char*, unsigned int, int)) {
	// Start Socket Manager if needed
	if(!GlobalInvoker) {
		GlobalInvoker.reset(new Invoker(0));
		if(!GlobalInvoker->start()) {
			return 0;
		}
	}

	Exception ex;
	shared_ptr<RTMFPConnection> pConn(new RTMFPConnection(onSocketError, onStatusEvent, onMedia));
	unsigned int index = GlobalInvoker->addConnection(pConn);
	if(!pConn->connect(ex,GlobalInvoker.get(),host,port,url)) {
		ERROR(ex.error())
		return 0;
	}

	return index;
}

void RTMFP_Play(unsigned int RTMFPcontext, const char* streamName) {
	Exception ex;
	shared_ptr<RTMFPConnection> pConn(GlobalInvoker->getConnection(RTMFPcontext));
	pConn->playStream(ex, streamName);
}

void RTMFP_Close(unsigned int RTMFPcontext) {
	GlobalInvoker->removeConnection(RTMFPcontext);
	if (!GlobalInvoker->count()) // delete if no more connections
		GlobalInvoker.reset();
}

}
