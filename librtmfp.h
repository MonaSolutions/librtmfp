
#ifdef __cplusplus
extern "C" {
#endif

// RTMFP Connection function
// return : index of the connection's context
unsigned int RTMFP_Connect(const char* url, void (* onSocketError)(const char*), void (* onStatusEvent)(const char*,const char*), 
				void (* onMedia)(unsigned int, const char*, unsigned int,int), int blocking);

// Connect to a peer via RTMFP P2P Connection (must be connected) and start playing streamName
int RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId, const char* streamName);

// RTMFP NetStream Play function
// return : 1 if the request succeed, 0 otherwise
int RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream Publish function
// return : 1 if the request succeed, 0 otherwise
int RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable);

// RTMFP P2P NetStream Publish function (equivalent of NetStream.DIRECT_CONNECTIONS)
// return : 1 if the request succeed, 0 otherwise
int RTMFP_PublishP2P(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable);

// RTMFP NetStream Unpublish function
// return : 1 if the request succeed, 0 otherwise
int RTMFP_ClosePublication(unsigned int RTMFPcontext, const char* streamName);

// Close the RTMFP connection
void RTMFP_Close(unsigned int RTMFPcontext);

// Read size bytes of flv data from the current connexion (Asynchronous read, to be called by ffmpeg)
// return the number of bytes read (always less or equal than size)
int RTMFP_Read(unsigned int RTMFPcontext, char *buf, unsigned int size);

// Write size bytes of data into the current connexion
// return the number of bytes used
int RTMFP_Write(unsigned int RTMFPcontext, const char *buf, int size);

// Set log callback
void RTMFP_LogSetCallback(void (* onLog)(unsigned int, int, const char*, long, const char*));

// Set dump callback
void RTMFP_DumpSetCallback(void (*onDump)(const char*, const void*, unsigned int));

// Set log level
void RTMFP_LogSetLevel(int level);

// Active RTMFP Dump
void RTMFP_ActiveDump();

// Set Interrupt callback (to check if caller need the hand)
void RTMFP_InterruptSetCallback(int (* interruptCb)(void*), void* argument);

// Retrieve publication name and url from original uri
void RTMFP_GetPublicationAndUrlFromUri(char* uri, char** publication);

#ifdef __cplusplus
}
#endif
