
#ifdef __cplusplus
extern "C" {
#endif

// RTMFP Connection function
// return : index of the connection's context
unsigned int RTMFP_Connect(const char* url, unsigned short isPublisher, void (* onSocketError)(const char*), void (* onStatusEvent)(const char*,const char*), 
				void (* onMedia)(unsigned int, const char*, unsigned int,int));

/*// RTMFP NetStream Play function
void RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// RTMFP NetStream Publish function
void RTMFP_Publish(unsigned int RTMFPcontext, const char* streamName);*/

// Close the RTMFP connection
void RTMFP_Close(unsigned int RTMFPcontext);

// Read size bytes of flv data from the current connexion (Asynchronous read, to be called by ffmpeg)
int RTMFP_Read(unsigned int RTMFPcontext, char *buf, unsigned int size);

// Write size bytes of data into the current connexion
int RTMFP_Write(unsigned int RTMFPcontext, const char *buf, int size);

// Set log callback
void RTMFP_LogSetCallback(void (* onLog)(int, const char*));

// Set log level
void RTMFP_LogSetLevel(int level);

// TODO: see if we keep these functions (used for TestClient)

// Set OnManage callback, function is called every second
void RTMFP_OnManageSetCallback(void (* onManage)());

// Terminate the processes
void RTMFP_Terminate();

// Wait for termination signal
void RTMFP_WaitTermination();

#ifdef __cplusplus
}
#endif
