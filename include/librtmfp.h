
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

// RTMFP Connection function
// return : index of the connection's context
LIBRTMFP_API unsigned int RTMFP_Connect(const char* url, void (* onSocketError)(const char*), void (* onStatusEvent)(const char*,const char*), 
				void (* onMedia)(const char *, const char*, unsigned int, const char*, unsigned int,int), int blocking);

// Connect to a peer via RTMFP P2P Connection (must be connected) and start playing streamName
LIBRTMFP_API  int RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName);

// Connect to a NetGroup (in the G:... form)
LIBRTMFP_API int RTMFP_Connect2Group(unsigned int RTMFPcontext, const char* netGroup, const char* streamName, int publisher, double availabilityUpdatePeriod, unsigned int windowDuration, int blocking);

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
// return the number of bytes read (always less or equal than size)
// peerId : the id of the peer or an empty string
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
LIBRTMFP_API void RTMFP_GetPublicationAndUrlFromUri(char* uri, char** publication);

#ifdef __cplusplus
}
#endif
