#include "librtmfp.h"

using namespace Mona;

static Invoker GlobalInvoker(0);

bool RTMFP_Connect(Exception& ex, RTMFPConnection& conn, const char* host, int port, const char* url) {
	// Start Socket Manager if needed
	if(!GlobalInvoker.start(ex)) {
		return false;
	}

	if (!conn.connect(ex, &GlobalInvoker, host, port, url))
		return false;

	return true;
}

void RTMFP_Close(RTMFPConnection& conn) {
	conn.close();
}

void RTMFP_Terminate() {
	GlobalInvoker.stop();
}

