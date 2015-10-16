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

unsigned int RTMFP_Connect(const char* host, int port, const char* url, void (* onSocketError)(const char*), 
						   void (* onStatusEvent)(const char*, const char*), void (* onMedia)(unsigned int, const char*, unsigned int, int)) {
	if(!onSocketError || !onStatusEvent) {
		ERROR("Callbacks onSocketError and onStatusEvent must be not null")
		return 0;
	}

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
	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn)
		pConn->playStream(ex, streamName);
}

void RTMFP_Close(unsigned int RTMFPcontext) {
	GlobalInvoker->removeConnection(RTMFPcontext);
	if (GlobalInvoker->empty()) // delete if no more connections
		GlobalInvoker.reset();
}

int RTMFP_Read(unsigned int RTMFPcontext,char *buf,unsigned int size) {
	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn)
		return pConn->read((UInt8*)buf, size);
	else
		return -1;
}

int RTMFP_Write(unsigned int RTMFPcontext,const char *buf,int size) {
	ERROR("not implemented yet")
		return -1;
}

}
