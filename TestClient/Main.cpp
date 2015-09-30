#include "../librtmfp.h"
#include "Mona/Exceptions.h"
#include "Mona/Logs.h"
#include "Mona/TerminateSignal.h"

using namespace Mona;

static TerminateSignal signal;

class CustomConnection: public RTMFPConnection {
public:
	virtual void onSocketError(const Mona::Exception& ex) {
		ERROR("Socket Error : ", ex.error())
		//signal.set();
	}

	virtual void onSocketConnected() {
		NOTE("Socket is now connected")
	}
};

// Windows CTrl+C handler
static int  __stdcall ConsoleCtrlHandler(unsigned long ctrlType) {
	switch (ctrlType) {
	case CTRL_C_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_BREAK_EVENT:
		signal.set();
		return TRUE;
	default:
		return FALSE;
	}
}

int main(int argc,char* argv[]) {

	SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

	Exception ex;
	CustomConnection conn;
	if (!RTMFP_Connect(ex, conn, "127.0.0.1", 1935, "rtmfp://localhost/MonaClients/"))
		INFO("Connection error : ", ex.error())
	
	signal.wait();
	INFO("Closing connection...")
	RTMFP_Close(conn);

	RTMFP_Terminate();
	system("pause");
}