
#ifdef __cplusplus
extern "C" {
#endif

unsigned int RTMFP_Connect(const char* host, int port, const char* url, void (__cdecl * onSocketError)(const char*), void (__cdecl * onStatusEvent)(const char*,const char*));

void RTMFP_Play(unsigned int RTMFPcontext, const char* streamName);

void RTMFP_Close(unsigned int RTMFPcontext);

void RTMFP_Terminate();

#ifdef __cplusplus
}
#endif
