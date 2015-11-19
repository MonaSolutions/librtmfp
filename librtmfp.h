
#ifdef __cplusplus
extern "C" {
#endif

// RTMFP Connection function
// return : index of the connection's context
unsigned int RTMFP_Connect(const char* url, unsigned short isPublisher, void (* onSocketError)(const char*), void (* onStatusEvent)(const char*,const char*), 
				void (* onMedia)(unsigned int, const char*, unsigned int,int), unsigned short audioReliable, unsigned short videoReliable);

unsigned int RTMFP_Connect2Peer(const char* host, const char* peerId, unsigned short isPublisher, void(*onSocketError)(const char*), void(*onStatusEvent)(const char*, const char*),
	void(*onMedia)(unsigned int, const char*, unsigned int, int), unsigned short audioReliable, unsigned short videoReliable);

/*// RTMFP NetStream Play function
void RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream Publish function
void RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName);*/

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

#ifdef __cplusplus
}
#endif
