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
	if (GlobalInvoker) {
		delete GlobalInvoker;
		GlobalInvoker = NULL;
	}
}

int RTMFP_LibVersion() {
	return RTMFP_LIB_VERSION;
}

unsigned int RTMFP_Connect(const char* url, RTMFPConfig* parameters) {
	if (!parameters->pOnStatusEvent) {
		ERROR("Callbacks onSocketError and onStatusEvent must be not null")
		return 0;
	}
	else if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it before trying to connect")
		return 0;
	}

	return GlobalInvoker->connect(url, parameters);
}

unsigned short RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName, int blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	return GlobalInvoker->connect2Peer(RTMFPcontext, peerId, streamName, blocking>0);
}

unsigned short RTMFP_Connect2Group(unsigned int RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, unsigned short audioReliable, unsigned short videoReliable, const char* fallbackUrl) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	UInt16 mediaId = GlobalInvoker->connect2Group(RTMFPcontext, streamName, parameters, groupParameters, audioReliable>0, videoReliable>0);

	if (mediaId && fallbackUrl)
		GlobalInvoker->connect2FallbackUrl(RTMFPcontext, parameters, fallbackUrl, mediaId);

	return mediaId;
}

unsigned short RTMFP_Play(unsigned int RTMFPcontext, const char* streamName) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	return GlobalInvoker->addStream(RTMFPcontext, false, streamName, true, true, false);
}

unsigned short RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	return GlobalInvoker->addStream(RTMFPcontext, true, streamName, audioReliable>0, videoReliable>0, blocking>0);
}

unsigned short RTMFP_PublishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	return GlobalInvoker->publishP2P(RTMFPcontext, streamName, audioReliable, videoReliable, blocking);
}

unsigned short RTMFP_ClosePublication(unsigned int RTMFPcontext,const char* streamName) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	return GlobalInvoker->closePublication(RTMFPcontext, streamName);
}

unsigned short RTMFP_CloseStream(unsigned int RTMFPcontext, unsigned short streamId) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return 0;
	}

	return GlobalInvoker->closeStream(RTMFPcontext, streamId);
}

void RTMFP_Close(unsigned int RTMFPcontext, unsigned short blocking) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return;
	}
	DEBUG("RTMFP_Close called, trying to close connection ", RTMFPcontext)
	if (!RTMFPcontext)
		return;

	GlobalInvoker->removeConnection(RTMFPcontext, blocking>0);
}

int RTMFP_Read(unsigned short streamId, unsigned int RTMFPcontext, char *buf, unsigned int size) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return -1;
	}

	return GlobalInvoker->read(RTMFPcontext, streamId, BIN buf, size);
}

int RTMFP_Write(unsigned int RTMFPcontext,const char *buf,int size) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return -1;
	}

	return GlobalInvoker->write(RTMFPcontext, BIN buf, size);
}

unsigned int RTMFP_CallFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return -1;
	}

	return GlobalInvoker->callFunction(RTMFPcontext, function, nbArgs, args, peerId);
}

void RTMFP_LogSetCallback(void(* onLog)(unsigned int, const char*, long, const char*)) {
	if (!GlobalInvoker) {
		ERROR("RTMFP_Init() has not been called, please call it first")
		return;
	}

	GlobalInvoker->setLogCallback(onLog);
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

void RTMFP_SetIntParameter(const char* parameter, int value) {

	if (String::ICompare(parameter, "logLevel") == 0) {
		Logs::SetLevel(value);
	}
	else if (String::ICompare(parameter, "socketReceiveSize") == 0) {
		Net::SetRecvBufferSize(value);
	}
	else if (String::ICompare(parameter, "socketSendSize") == 0) {
		Net::SetSendBufferSize(value);
	}
	else
		FATAL_ERROR("Unknown parameter ", parameter)
}

void RTMFP_SetParameter(const char* parameter, const char* value) {

	RTMFP_SetIntParameter(parameter, atoi(value));
}

}
