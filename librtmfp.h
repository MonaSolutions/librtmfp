
#ifdef __cplusplus
extern "C" {
#endif

// RTMFP Connection function
// return : index of the connection's context
unsigned int RTMFP_Connect(const char* host, int port, const char* url, void (__cdecl * onSocketError)(const char*), void (__cdecl * onStatusEvent)(const char*,const char*));

// RTMFP NetStream Play function
void RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

// Close the RTMFP connection
void RTMFP_Close(unsigned int RTMFPcontext);

#ifdef __cplusplus
}
#endif
