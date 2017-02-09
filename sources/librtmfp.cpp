/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "librtmfp.h"
#include "Mona/Exceptions.h"
#include "RTMFPSession.h"
#include "Mona/Logs.h"
#include "Mona/String.h"
#include "Invoker.h"

using namespace Mona;
using namespace std;

extern "C" {

static std::shared_ptr<Invoker>		GlobalInvoker; // manage threads, sockets and connection

void RTMFP_Init(RTMFPConfig* config, RTMFPGroupConfig* groupConfig) {
	if (!config) {
		ERROR("config parameter must be not null")
		return;
	}

	// Init global invoker (+logger)
	if (!GlobalInvoker) {
		GlobalInvoker.reset(new Invoker(0));
		if (!GlobalInvoker->start()) {
			GlobalInvoker.reset();
			return;
		}
	}

	memset(config, 0, sizeof(RTMFPConfig));

	if (!groupConfig)
		return; // ignore groupConfig if not set

	memset(groupConfig, 0, sizeof(RTMFPGroupConfig));
	groupConfig->availabilityUpdatePeriod = 100;
	groupConfig->relayMargin = 2000;
	groupConfig->fetchPeriod = 2500;
	groupConfig->windowDuration = 8000;
	groupConfig->pushLimit = 4;
}

int RTMFP_LibVersion() {
	return RTMFP_LIB_VERSION;
}

unsigned int RTMFP_Connect(const char* url, RTMFPConfig* parameters) {
	if (!parameters->pOnSocketError || !parameters->pOnStatusEvent) {
		ERROR("Callbacks onSocketError and onStatusEvent must be not null")
		return 0;
	}
	else if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it before trying to connect")
		return 0;
	}

	// Get hostname, port and publication name
	string host, publication, query;
	Util::UnpackUrl(url, host, publication, query);

	Exception ex;
	shared_ptr<RTMFPSession> pConn(new RTMFPSession(GlobalInvoker.get(), parameters->pOnSocketError, parameters->pOnStatusEvent, parameters->pOnMedia));
	unsigned int index = GlobalInvoker->addConnection(pConn);
	if (!pConn->connect(ex, url, host.c_str())) {
		ERROR("Error in connect : ", ex.error())
		GlobalInvoker->removeConnection(index);
		return 0;
	}

	if (parameters->isBlocking) {
		while (!pConn->connectReady) {
			pConn->connectSignal.wait(200);
			if (GlobalInvoker->isInterrupted()) {
				GlobalInvoker->removeConnection(index);
				return 0;
			}
		}
	}

	return index;
}

int RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName, int blocking) {

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (pConn)
		pConn->connect2Peer(peerId, streamName);

	if (blocking) {
		while (!pConn->p2pPlayReady) {
			pConn->p2pPlaySignal.wait(200);
			if (GlobalInvoker->isInterrupted())
				return 0;
		}
	}

	return 1;
}

int RTMFP_Connect2Group(unsigned int RTMFPcontext, const char* streamName, RTMFPGroupConfig* parameters) {

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (pConn)
		pConn->connect2Group(streamName, parameters);

	if (parameters->isPublisher > 0)
		pConn->addCommand(RTMFPSession::CommandType::NETSTREAM_PUBLISH_P2P, streamName, true, true);

	if (parameters->isBlocking && parameters->isPublisher) {
		while (!pConn->publishReady) {
			pConn->publishSignal.wait(200);
			if (GlobalInvoker->isInterrupted())
				return -1;
		}
	}

	return 1;
}

int RTMFP_Play(unsigned int RTMFPcontext, const char* streamName) {

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (pConn) {
		pConn->addCommand(RTMFPSession::CommandType::NETSTREAM_PLAY, streamName);
		return 1;
	}

	return 0;
}

int RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (!pConn)
		return 0;
	
	pConn->addCommand(RTMFPSession::CommandType::NETSTREAM_PUBLISH, streamName, audioReliable>0, videoReliable>0);

	if (blocking) {
		while (!pConn->publishReady) {
			pConn->publishSignal.wait(200);
			if (GlobalInvoker->isInterrupted())
				return -1;
		}
	}

	return 1;
}

int RTMFP_PublishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (!pConn)
		return 0;

	pConn->addCommand(RTMFPSession::CommandType::NETSTREAM_PUBLISH_P2P, streamName, audioReliable > 0, videoReliable > 0);

	if (blocking) {
		while (!pConn->p2pPublishReady) {
			pConn->p2pPublishSignal.wait(200);
			if (GlobalInvoker->isInterrupted())
				return -1;
		}
	}

	return 1;
}

int RTMFP_ClosePublication(unsigned int RTMFPcontext,const char* streamName) {

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(pConn) {
		pConn->addCommand(RTMFPSession::CommandType::NETSTREAM_CLOSE, streamName);
		return 1;
	}

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

int RTMFP_Read(const char* peerId, unsigned int RTMFPcontext,char *buf,unsigned int size) {
	if (!GlobalInvoker) {
		ERROR("Invoker is not ready, you must establish the connection first")
		return -1;
	}

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (pConn) {
		UInt32 total = 0;
		int nbRead = 0;
		while (nbRead==0 && !GlobalInvoker->isInterrupted()) {
			if (!pConn->read(peerId, (UInt8*)buf, size, nbRead)) {
				WARN("Connection is not established, cannot read data")
				return -1;
			}
			if (nbRead < 0)
				return nbRead;
			else if (nbRead > 0) {
				size -= nbRead;
				total += nbRead;
			}
			else { // Nothing read, wait for data
				while (!pConn->dataAvailable) {
					DEBUG("Nothing available, sleeping...")
					pConn->readSignal.wait(100);
					if (GlobalInvoker->isInterrupted())
						return -1;
				}
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

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (pConn) {
		int pos = 0;
		if (!pConn->write((const UInt8*)buf, size, pos))
			return -1;
		return pos;
	}
	
	return -1;
}

unsigned int RTMFP_CallFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId) {
	if (!GlobalInvoker) {
		ERROR("Invoker is not ready, you must establish the connection first")
			return -1;
	}

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (!pConn)
		return -1;
	
	return pConn->callFunction(function, nbArgs, args, peerId);
}

void RTMFP_LogSetCallback(void(* onLog)(unsigned int, int, const char*, long, const char*)) {
	if (!GlobalInvoker)
		ERROR("RTMFP_Init() has not been called, please call it first")

	GlobalInvoker->setLogCallback(onLog);
}

void RTMFP_LogSetLevel(int level) {
	Logs::SetLevel(level);
}

void RTMFP_DumpSetCallback(void(*onDump)(const char*, const void*, unsigned int)) {
	if (!GlobalInvoker)
		ERROR("RTMFP_Init() has not been called, please call it first")
	GlobalInvoker->setDumpCallback(onDump);
}

void RTMFP_InterruptSetCallback(int(*interruptCb)(void*), void* argument) {
	if (!GlobalInvoker)
		ERROR("RTMFP_Init() has not been called, please call it first")
	GlobalInvoker->setInterruptCallback(interruptCb, argument);
}


void RTMFP_GetPublicationAndUrlFromUri(const char* uri, char** publication) {
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

void RTMFP_ActiveDump() {
	if (!GlobalInvoker)
		ERROR("RTMFP_Init() has not been called, please call it first")
	Logs::SetDump("RTMFP");
}

}
