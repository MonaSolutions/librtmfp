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

#if defined(_WIN32) && !defined(LIBRTMFP_STATIC)
	// Windows DLL declaration
	#if defined(LIBRTMFP_EXPORT)
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
	unsigned int	availabilityUpdatePeriod; // 100 by default, it is the time (in msec) between each fragments map messages (set by the publisher)
	char			availabilitySendToAll; // False by default, if True send the fragments map to all peer (set by the publisher)
	unsigned int	windowDuration; // 8000 by default, it is the time (in msec) to bufferize and keep fragments available (set by the publisher)
	unsigned int	relayMargin; // 2000 by default, it is additional time (in msec) to keep the fragments availabl
	unsigned int	fetchPeriod; // 2500 by default, it is the time (in msec) before trying to fetch the missing fragments (set by the publisher)
	unsigned short	pushLimit; // 4 by default, it is the number of neighbors minus one to which we want to push fragments

	char			disableRateControl; // False by default, if True the p2p rate control is disabled (no disconnection if rate is < to 5% of P2P connection success)
	char			disablePullTimeout; // False by default, if True the pull congestion timeout is disabled
} RTMFPGroupConfig;

LIBRTMFP_API typedef struct RTMFPConfig {
	short	isBlocking; // False by default, if True the function will return only when we are connected
	void	(*pOnStatusEvent)(const char* code, const char* description); // RTMFP Status Event callback
	void	(*pOnMedia)(unsigned short streamId, unsigned int time, const char* data, unsigned int size, unsigned int type); // In synchronous read mode this callback is called when receiving data
	const char*		swfUrl; // swfUrl Flash connection parameter
	const char*		app; // app Flash connection parameter
	const char*		pageUrl; // pageUrl Flash connection parameter
	const char*		flashVer; // flashVer Flash connection parameter
	const char*		host; // IPv4 host address to bind to (use this if you ave multiple interfaces)
	const char*		hostIPv6; // IPv6 host address to bind to (use this if you ave multiple interfaces)
	int				(*interruptCb)(void*); // interrupt callback function (NULL by default)
	void*			interruptArg; // interrupt callback argument for interrupt function
} RTMFPConfig;

LIBRTMFP_API typedef enum {
	RTMFP_UNDEFINED			= 0x00,
	RTMFP_CONNECTED			= 0x01,
	RTMFP_PUBLISHED			= 0x02,
	RTMFP_GROUP_CONNECTED	= 0x04,
	RTMFP_PEER_CONNECTED	= 0x08,
	RTMFP_P2P_PUBLISHED		= 0x10
} RTMFPMask;

// This function MUST be called before any other
// Initialize the RTMFP parameters with default values
// config : CANNOT be null, it is the main configuration parameter
// groupConfig : can be null, it is used for netgroup configuration
// createLogger : if 0 it will let the default log system (RTMFP_LogSetCallback will not work)
LIBRTMFP_API void RTMFP_Init(RTMFPConfig* config, RTMFPGroupConfig* groupConfig, int createLogger);

// Terminate all the connections brutaly
LIBRTMFP_API void RTMFP_Terminate();

// Return the version of librtmfp
// First byte : (main version)
// 2nd byte : feature number
// 3-4th bytes : minor correction number
LIBRTMFP_API int RTMFP_LibVersion();

// RTMFP Connection function
// return : index of the connection's context
LIBRTMFP_API unsigned int RTMFP_Connect(const char* url, RTMFPConfig* parameters);

// Connect to a peer via RTMFP P2P Connection (must be connected) and start playing streamName
// return the id of the stream (to call with RTMFP_Read) or 0 if an error occurs 
LIBRTMFP_API unsigned short RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName, int blocking);

// Connect to a NetGroup (in the G:... form)
// param audioReliable if True all audio packets losts are repeated, otherwise audio packets are not repeated
// param videoReliable if True all video packets losts are repeated, otherwise video packets are not repeated
// param fallbackUrl [optional] an rtmfp unicast url used if no data is coming from the NetGroup (be careful to use the same stream codecs to avoid undefined behavior)
// return the id of the stream (to call with RTMFP_Read) or 0 if an error occurs 
LIBRTMFP_API unsigned short RTMFP_Connect2Group(unsigned int RTMFPcontext, const char* streamName, RTMFPConfig* parameters, RTMFPGroupConfig* groupParameters, unsigned short audioReliable, unsigned short videoReliable, const char* fallbackUrl);

// RTMFP NetStream Play function
// return the id of the stream (to call with RTMFP_Read) or 0 if an error occurs 
LIBRTMFP_API unsigned short RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream Publish function
// param audioReliable if True all audio packets losts are repeated, otherwise audio packets are not repeated
// param videoReliable if True all video packets losts are repeated, otherwise video packets are not repeated
// NOTE: For now librtmfp only support 1 publisher, ask us if you need to support more
// return the id of the stream or 0 if an error occurs 
LIBRTMFP_API unsigned short RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking);

// RTMFP P2P NetStream Publish function (equivalent of NetStream.DIRECT_CONNECTIONS)
// param audioReliable if True all audio packets losts are repeated, otherwise audio packets are not repeated
// param videoReliable if True all video packets losts are repeated, otherwise video packets are not repeated
// return the id of the stream or 0 if an error occurs
LIBRTMFP_API unsigned short RTMFP_PublishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable, int blocking);

// RTMFP NetStream Unpublish function
// return : 1 if the request succeed, 0 otherwise
LIBRTMFP_API unsigned short RTMFP_ClosePublication(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream closeStream function
// return : 1 if the request succeed, 0 otherwise
LIBRTMFP_API unsigned short RTMFP_CloseStream(unsigned int RTMFPcontext, unsigned short streamId);

// Close the RTMFP connection
LIBRTMFP_API void RTMFP_Close(unsigned int RTMFPcontext, unsigned short blocking);

// Read size bytes of flv data from the current connexion (Asynchronous read, to be called by ffmpeg)
// Note: blocking method, if no data is found it will wait until founding data or isInterrupted() == true
// peerId : the id of the peer or an empty string
// streamId : the id of the stream to read
// return : the number of bytes read (always less or equal than size) or -1 if an error occurs
LIBRTMFP_API int RTMFP_Read(unsigned short streamId, unsigned int RTMFPcontext, char *buf, unsigned int size);

// Write size bytes of data into the current connexion
// return the number of bytes used or -1 if an error occurs
LIBRTMFP_API int RTMFP_Write(unsigned int RTMFPcontext, const char *buf, int size);

// Call a function of a server, peer or NetGroup
// param peerId If set to 0 the call we be done to the server, if set to "all" to all the peers of a NetGroup, and to a peer otherwise
// return 1 if the call succeed, 0 otherwise
// TODO: add callback
LIBRTMFP_API unsigned int RTMFP_CallFunction(unsigned int RTMFPcontext, const char* function, int nbArgs, const char** args, const char* peerId);

// Wait for an event to happend on a specified connection
// return: True if the event happened, False if an error occurs
LIBRTMFP_API char RTMFP_WaitForEvent(unsigned int RTMFPcontext, RTMFPMask mask);

// Set log callback
LIBRTMFP_API void RTMFP_LogSetCallback(void (* onLog)(unsigned int, const char*, long, const char*));

// Set dump callback
LIBRTMFP_API void RTMFP_DumpSetCallback(void (*onDump)(const char*, const void*, unsigned int));

// Active RTMFP Dump
LIBRTMFP_API void RTMFP_ActiveDump();

// Retrieve publication name and url from original uri
LIBRTMFP_API void RTMFP_GetPublicationAndUrlFromUri(const char* uri, char** publication);

// Set a Global Parameter to the requested value
// Allowed parameters are :
// - logLevel (int) : log level of the application
// - socketReceiveSize (int) : socket size limit to be used with input packets
// - socketSendSize (int) : socket size limit to be used with output packets
LIBRTMFP_API void RTMFP_SetParameter(const char* parameter, const char* value);

// Set an integer Global Parameter to the requested value (int version)
LIBRTMFP_API void RTMFP_SetIntParameter(const char* parameter, int value);

#ifdef __cplusplus
}
#endif
