#include "Mona/Exceptions.h"
#include "RTMFPConnection.h"

bool RTMFP_Connect(Mona::Exception& ex, RTMFPConnection& conn, const char* host, int port, const char* url);

void RTMFP_Close(RTMFPConnection& conn);

void RTMFP_Terminate();