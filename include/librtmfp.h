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

#if defined(_WIN32)
	// Windows DLL declaration
	#ifdef LIBRTMFP_EXPORT
		#define LIBRTMFP_API	__declspec(dllexport)
	#else
		#define LIBRTMFP_API	__declspec(dllimport)
	#endif
#else
	#define LIBRTMFP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

LIBRTMFP_API typedef struct RTMFPGroupConfig {
	const char*		netGroup; // [Required] netGroup identifier (groupspec)
	char			isPublisher; // False by default, if True the stream will be published
	char			isBlocking; // False by default, if True the function will return only when the first peer is connected
	unsigned int	availabilityUpdatePeriod; // 100 by default, it is the time (in msec) between each fragments map messages (multicastAvailabilityUpdatePeriod)
	char			availabilitySendToAll; // False by default, if True send the fragments map to all peer (multicastAvailabilitySendToAll)
	unsigned int	windowDuration; // 8000 by default, it is the time (in msec) to bufferize and keep fragments available
	unsigned int	relayMargin; // 2000 by default, it is additional time (in msec) to keep the fragments available (cannot be changed)
	unsigned int	fetchPeriod; // 2500 by default, it is the time (in msec) before trying to fetch the missing fragments
	unsigned short	pushLimit; // 4 by default, it is the number of neighbors (-1) to which we want to push fragments (cannot be changed)
} RTMFPGroupConfig;

LIBRTMFP_API typedef struct RTMFPConfig {
	short	isBlocking; // False by default, if True the function will return only when we are connected
	void	(*pOnSocketError)(const char*); // Socket Error callback
	void	(*pOnStatusEvent)(const char*, const char*); // RTMFP Status Event callback
	void	(*pOnMedia)(const char *, const char*, unsigned int, const char*, unsigned int, int); // In synchronous read mode this callback is called when receiving data
} RTMFPConfig;

// This function MUST be called before any other
// Initialize the RTMFP parameters with default values
LIBRTMFP_API void RTMFP_Init(RTMFPConfig*, RTMFPGroupConfig*);

// Return the version of librtmfp
// First byte : (main version)
// 2nd byte : feature number
// 3-4th bytes : minor correction number
LIBRTMFP_API int RTMFP_LibVersion();

// RTMFP Connection function
// return : index of the connection's context
LIBRTMFP_API unsigned int RTMFP_Connect(const char* url, RTMFPConfig* parameters);

// Connect to a peer via RTMFP P2P Connection (must be connected) and start playing streamName
LIBRTMFP_API  int RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName, int blocking);

// Connect to a NetGroup (in the G:... form)
LIBRTMFP_API int RTMFP_Connect2Group(unsigned int RTMFPcontext, const char* streamName, RTMFPGroupConfig* parameters);

// RTMFP NetStream Play function
// return : 1 if the request succeed, 0 otherwise
LIBRTMFP_API int RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream Publish function
// return : 1 if the request succeed, 0 otherwise
LIBRTMFP_API int RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking);

// RTMFP P2P NetStream Publish function (equivalent of NetStream.DIRECT_CONNECTIONS)
// return : 1 if the request succeed, 0 otherwise
LIBRTMFP_API int RTMFP_PublishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking);

// RTMFP NetStream Unpublish function
// return : 1 if the request succeed, 0 otherwise
LIBRTMFP_API int RTMFP_ClosePublication(unsigned int RTMFPcontext, const char* streamName);

// Close the RTMFP connection
LIBRTMFP_API void RTMFP_Close(unsigned int RTMFPcontext);

// Read size bytes of flv data from the current connexion (Asynchronous read, to be called by ffmpeg)
// peerId : the id of the peer or an empty string
// return : the number of bytes read (always less or equal than size) or -1 if an error occurs
LIBRTMFP_API int RTMFP_Read(const char* peerId, unsigned int RTMFPcontext, char *buf, unsigned int size);

// Write size bytes of data into the current connexion
// return the number of bytes used
LIBRTMFP_API int RTMFP_Write(unsigned int RTMFPcontext, const char *buf, int size);

// Call a function of a server, peer or NetGroup
// param peerId If set to 0 the call we be done to the server, if set to "all" to all the peers of a NetGroup, and to a peer otherwise
// return 1 if the call succeed, 0 otherwise
// TODO: add callback
LIBRTMFP_API unsigned int RTMFP_CallFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId);

// Set log callback
LIBRTMFP_API void RTMFP_LogSetCallback(void (* onLog)(unsigned int, int, const char*, long, const char*));

// Set dump callback
LIBRTMFP_API void RTMFP_DumpSetCallback(void (*onDump)(const char*, const void*, unsigned int));

// Set log level
LIBRTMFP_API void RTMFP_LogSetLevel(int level);

// Active RTMFP Dump
LIBRTMFP_API void RTMFP_ActiveDump();

// Set Interrupt callback (to check if caller need the hand)
LIBRTMFP_API void RTMFP_InterruptSetCallback(int (* interruptCb)(void*), void* argument);

// Retrieve publication name and url from original uri
LIBRTMFP_API void RTMFP_GetPublicationAndUrlFromUri(const char* uri, char** publication);

#ifdef __cplusplus
}
#endif
