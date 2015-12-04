
#ifdef __cplusplus
extern "C" {
#endif

// RTMFP Connection function
// return : index of the connection's context
unsigned int RTMFP_Connect(const char* url, void (* onSocketError)(const char*), void (* onStatusEvent)(const char*,const char*), 
				void (* onMedia)(unsigned int, const char*, unsigned int,int), int blocking);

// RTMFP P2P Connection (must be connected)
void RTMFP_Connect2Peer(unsigned int RTMFPcontext, const char* peerId);

// RTMFP NetStream Play function
// return : 1 if the request succeed, 0 otherwise
int RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream Publish function
// return : 1 if the request succeed, 0 otherwise
int RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName, unsigned short audioReliable, unsigned short videoReliable);

// Close the RTMFP connection
void RTMFP_Close(unsigned int RTMFPcontext);

// Read size bytes of flv data from the current connexion (Asynchronous read, to be called by ffmpeg)
// return the number of bytes read (always less or equal than size)
int RTMFP_Read(unsigned int RTMFPcontext, char *buf, unsigned int size);

// Write size bytes of data into the current connexion
// return the number of bytes used
int RTMFP_Write(unsigned int RTMFPcontext, const char *buf, int size);

// Set log callback
void RTMFP_LogSetCallback(void (* onLog)(int, const char*));

// Set log level
void RTMFP_LogSetLevel(int level);

// Set Interrupt callback (to check if caller need the hand)
void RTMFP_InterruptSetCallback(int (* interruptCb)(void*), void* argument);

// Retrieve publication name and url from original uri
void RTMFP_GetPublicationAndUrlFromUri(char* uri, char** publication);

#ifdef __cplusplus
}
#endif
