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
#include "Base/Exceptions.h"
#include "RTMFPSession.h"
#include "Base/Logs.h"
#include "Base/String.h"
#include "Base/Util.h"

using namespace Base;
using namespace std;

Invoker* GlobalInvoker = NULL;

extern "C" {

void RTMFP_Init(RTMFPConfig* config, RTMFPGroupConfig* groupConfig, int createLogger) {
	if (!config) {
		ERROR("config parameter must be not null")
		return;
	}

	// Init global invoker (+logger)
	if (!GlobalInvoker) {
		GlobalInvoker = new Invoker(createLogger>0);
		if (!GlobalInvoker->start()) {
			RTMFP_Terminate();
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

void RTMFP_Terminate() {
	delete GlobalInvoker;
	GlobalInvoker = NULL;
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
	shared_ptr<RTMFPSession> pConn(new RTMFPSession(*GlobalInvoker, parameters->pOnSocketError, parameters->pOnStatusEvent, parameters->pOnMedia));
	unsigned int index = GlobalInvoker->addConnection(pConn);
	if (!pConn->connect(ex, url, host.c_str())) {
		ERROR("Error in connect : ", ex)
		GlobalInvoker->removeConnection(index);
		return 0;
	}

	if (parameters->isBlocking) {
		while (!pConn->connectReady) {
			pConn->connectSignal.wait(200);
			if (!GlobalInvoker)
				return 0;
			if (GlobalInvoker->isInterrupted()) {
				GlobalInvoker->removeConnection(index);
				return 0;
			}
		}
	}

	return index;
}

unsigned short RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName, int blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	shared_ptr<RTMFPSession> pConn;
	if (!GlobalInvoker->getConnection(RTMFPcontext, pConn))
		return 0;

	UInt16 mediaId = pConn->connect2Peer(peerId, streamName);

	if (blocking) {
		while (!pConn->p2pPlayReady) {
			pConn->p2pPlaySignal.wait(200);
			if (!GlobalInvoker || GlobalInvoker->isInterrupted())
				return 0;
		}
	}

	return mediaId;
}

unsigned short RTMFP_Connect2Group(unsigned int RTMFPcontext, const char* streamName, RTMFPGroupConfig* parameters, unsigned short audioReliable, unsigned short videoReliable) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	shared_ptr<RTMFPSession> pConn;
	if (!GlobalInvoker->getConnection(RTMFPcontext, pConn))
		return 0;
	
	UInt16 mediaId = pConn->connect2Group(streamName, parameters, audioReliable>0, videoReliable>0);
	if (!mediaId)
		return 0;

	if (parameters->isBlocking && parameters->isPublisher) {
		while (!pConn->publishReady) {
			pConn->publishSignal.wait(200);
			if (!GlobalInvoker || GlobalInvoker->isInterrupted())
				return 0;
		}
	}

	return mediaId;
}

unsigned short RTMFP_Play(unsigned int RTMFPcontext, const char* streamName) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	shared_ptr<RTMFPSession> pConn;
	if (!GlobalInvoker->getConnection(RTMFPcontext,pConn))
		return 0;

	return pConn->addStream(false, streamName, true, true);
}

unsigned short RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if (!pConn)
		return 0;
	
	UInt16 mediaId = pConn->addStream(true, streamName, audioReliable>0, videoReliable>0);

	if (mediaId && blocking) {
		while (!pConn->publishReady) {
			pConn->publishSignal.wait(200);
			if (!GlobalInvoker || GlobalInvoker->isInterrupted())
				return 0;
		}
	}

	return mediaId;
}

unsigned short RTMFP_PublishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (!pConn)
		return 0;

	if (!pConn->startP2PPublisher(streamName, audioReliable > 0, videoReliable > 0))
		return 0;

	if (blocking) {
		while (!pConn->p2pPublishReady) {
			pConn->p2pPublishSignal.wait(200);
			if (!GlobalInvoker || GlobalInvoker->isInterrupted())
				return 0;
		}
	}

	return 1;
}

unsigned short RTMFP_ClosePublication(unsigned int RTMFPcontext,const char* streamName) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext,pConn);
	if(!pConn || !pConn->closePublication(streamName))
		return 0;

	return 1;
}

void RTMFP_Close(unsigned int RTMFPcontext) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return;
	}
	DEBUG("RTMFP_Close called, trying to close connection ", RTMFPcontext)
	if (!RTMFPcontext)
		return;

	GlobalInvoker->removeConnection(RTMFPcontext);
	if (GlobalInvoker->empty()) // delete if no more connections (shortcut to avoid calling RTMFP_Terminate() after RTMFP_Close())
		RTMFP_Terminate();
}

int RTMFP_Read(unsigned short streamId, unsigned int RTMFPcontext,char *buf,unsigned int size) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return -1;
	}

	int nbRead = 0, ret = 0;
	shared_ptr<RTMFPSession> pConn;
	// Loop while the connection is available and no data is available
	while (GlobalInvoker && !GlobalInvoker->isInterrupted() && GlobalInvoker->getConnection(RTMFPcontext, pConn) && nbRead == 0) {
		if (!(ret = pConn->read(streamId, (UInt8*)buf, size, nbRead)))
			return ret;
			
		if (nbRead != 0)
			return nbRead;

		// Nothing read, wait for data
		DEBUG("Nothing available, sleeping...")
		pConn->readSignal.wait(100);
	}

	return 0;
}

int RTMFP_Write(unsigned int RTMFPcontext,const char *buf,int size) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
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
		ERROR("RTMFP_Init() has not been called, please call it first")
		return -1;
	}

	shared_ptr<RTMFPSession> pConn;
	GlobalInvoker->getConnection(RTMFPcontext, pConn);
	if (!pConn)
		return -1;
	
	return pConn->callFunction(function, nbArgs, args, peerId);
}

void RTMFP_LogSetCallback(void(* onLog)(unsigned int, const char*, long, const char*)) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return;
	}

	GlobalInvoker->setLogCallback(onLog);
}

void RTMFP_LogSetLevel(int level) {
	Logs::SetLevel(level);
}

void RTMFP_DumpSetCallback(void(*onDump)(const char*, const void*, unsigned int)) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return;
	}

	GlobalInvoker->setDumpCallback(onDump);
}

void RTMFP_InterruptSetCallback(int(*interruptCb)(void*), void* argument) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return;
	}

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
	Logs::SetDump("LIBRTMFP");
}

}
