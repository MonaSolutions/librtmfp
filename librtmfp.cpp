#include "librtmfp.h"
#include "Mona/Exceptions.h"
#include "RTMFPConnection.h"
#include "Mona/Logs.h"
#include "Mona/String.h"
#include "Invoker.h"
#include "RTMFPLogger.h"

using namespace Mona;
using namespace std;

extern "C" {

static std::shared_ptr<Invoker>		GlobalInvoker; // manage threads, sockets and connection
static std::shared_ptr<RTMFPLogger> GlobalLogger; // handle log messages

static int		(*GlobalInterruptCb)(void*) = NULL;
static void*	GlobalInterruptArg = NULL;

unsigned int RTMFP_Connect(const char* url, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent,	OnMediaEvent pOnMedia, int blocking) {
	if (!pOnSocketError || !pOnStatusEvent) {
		ERROR("Callbacks onSocketError and onStatusEvent must be not null")
			return 0;
	}

	// Start Socket Manager if needed
	if (!GlobalInvoker) {
		GlobalInvoker.reset(new Invoker(0));
		if (!GlobalInvoker->start()) {
			return 0;
		}
	}

	// Get hostname, port and publication name
	string host, publication, query;
	Util::UnpackUrl(url, host, publication, query);

	Exception ex;
	shared_ptr<RTMFPConnection> pConn(new RTMFPConnection(GlobalInvoker.get(), pOnSocketError, pOnStatusEvent, pOnMedia));
	unsigned int index = GlobalInvoker->addConnection(pConn);
	if (!pConn->connect(ex, url, host.c_str())) {
		ERROR("Error in connect : ", ex.error())
		GlobalInvoker->removeConnection(index);
		return 0;
	}

	if (blocking) {
		while (!pConn->connected) {
			pConn->connectSignal.wait(200);
			if (GlobalInterruptCb(GlobalInterruptArg) == 1) {
				GlobalInvoker->removeConnection(index);
				return 0;
			}
		}
	}

	return index;
}

void RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId) {

	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (pConn)
		pConn->connect2Peer(peerId);
}

int RTMFP_Play(unsigned int RTMFPcontext, const char* streamName) {

	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn)
		return pConn->sendCommand(RTMFPConnection::CommandType::NETSTREAM_PLAY, streamName);

	return 0;
}

int RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable) {

	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn)
		return pConn->sendCommand(RTMFPConnection::CommandType::NETSTREAM_PUBLISH, streamName, audioReliable>0, videoReliable>0);

	return 0;
}

void RTMFP_Close(unsigned int RTMFPcontext) {
	if (!GlobalInvoker) {
		ERROR("Invoker is not ready, you must establish the connection first")
		return;
	}

	GlobalInvoker->removeConnection(RTMFPcontext);
	if (GlobalInvoker->empty()) // delete if no more connections
		GlobalInvoker.reset();
}

int RTMFP_Read(unsigned int RTMFPcontext,char *buf,unsigned int size) {
	if (!GlobalInvoker) {
		ERROR("Invoker is not ready, you must establish the connection first")
		return -1;
	}

	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (pConn) {
		UInt32 total = 0;
		int nbRead = 0;
		bool running = true;
		while (running && nbRead==0 /*size > 0*/) {
			running = pConn->read((UInt8*)buf, size, nbRead);
			if (nbRead < 0)
				return nbRead;
			if (nbRead > 0) {
				size -= nbRead;
				total += nbRead;
			}
		}
		return total;
	}
	
	return -1;
}

int RTMFP_Write(unsigned int RTMFPcontext,const char *buf,int size) {
	if (!GlobalInvoker) {
		ERROR("Invoker is not ready, you must establish the connection first")
		return -1;
	}

	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (pConn) {
		int pos = 0;
		if (!pConn->write((const UInt8*)buf, size, pos))
			return -1;
		return pos;
	}
	
	return -1;
}

void RTMFP_LogSetCallback(void(* onLog)(int,const char*)) {
	GlobalLogger.reset(new RTMFPLogger(onLog));
	Logs::SetLogger(*GlobalLogger);
}

void RTMFP_LogSetLevel(int level) {
	Logs::SetLevel(level);
	//TODO: temporary
	//Logs::SetDump("RTMFP");
}

void RTMFP_InterruptSetCallback(int(*interruptCb)(void*), void* argument) {
	GlobalInterruptCb = interruptCb;
	GlobalInterruptArg = argument;
}


void RTMFP_GetPublicationAndUrlFromUri(char* uri, char** publication) {
	char* pos = (char*)strrchr(uri, '\\');
	char* pos2 = (char*)strrchr(uri, '/');

	if (pos && pos2) {
		*publication = (char*)((pos > pos2)? pos+1 : pos2+1);
		(pos > pos2) ? *pos = '\0' : *pos2 = '\0';
	}
	else if (pos) {
		*publication = (char*)pos + 1;
		*pos = '\0';
	}
	else if (pos2) {
		*publication = (char*)pos2 + 1;
		*pos2 = '\0';
	}
}

}
