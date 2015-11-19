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

static unsigned int Connect(const char* url, unsigned short isPublisher, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMedia, 
	unsigned short audioReliable, unsigned short videoReliable, const char* peerId=NULL) {

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
	string	host, publication, query, app = url;
	size_t	filePos = Util::UnpackUrl(url, host, publication, query);
	if (!peerId && filePos == string::npos) {
		ERROR("No publication name found in url")
			GlobalInvoker.reset();
		return 0;
	}
	else if (!peerId) {
		app.erase(app.size() - publication.size() + 1);
		publication.erase(0, filePos);
	}

	Exception ex;
	shared_ptr<RTMFPConnection> pConn(new RTMFPConnection(pOnSocketError, pOnStatusEvent, pOnMedia, audioReliable>0, videoReliable>0));
	unsigned int index = GlobalInvoker->addConnection(pConn);
	if (!pConn->connect(ex, GlobalInvoker.get(), peerId? peerId : app.c_str(), host.c_str(), publication.c_str(), isPublisher > 0, peerId ? RTMFPConnection::P2P_HANDSHAKE : RTMFPConnection::BASE_HANDSHAKE)) {
		ERROR("Error in connect : ", ex.error())
			GlobalInvoker->removeConnection(index);
		return 0;
	}

	return index;
}

unsigned int RTMFP_Connect2Peer(const char* host, const char* peerId, unsigned short isPublisher, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMedia, unsigned short audioReliable, unsigned short videoReliable) {
	return Connect(host, isPublisher, pOnSocketError, pOnStatusEvent, pOnMedia, audioReliable, videoReliable, peerId);
}

unsigned int RTMFP_Connect(const char* url, unsigned short isPublisher, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent,	OnMediaEvent pOnMedia, unsigned short audioReliable, unsigned short videoReliable) {
	return Connect(url, isPublisher, pOnSocketError, pOnStatusEvent, pOnMedia, audioReliable, videoReliable);
}

/*void RTMFP_Play(unsigned int RTMFPcontext, const char* streamName) {
	Exception ex;
	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn)
		pConn->sendCommand(RTMFPConnection::CommandType::NETSTREAM_PLAY, streamName);
}

void RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName) {
	Exception ex;
	shared_ptr<RTMFPConnection> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn)
		pConn->sendCommand(RTMFPConnection::CommandType::NETSTREAM_PUBLISH, streamName);
}*/

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
}

}
