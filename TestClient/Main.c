#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include "../librtmfp.h"

static int isWorking = 0;
static unsigned int context = 0;

// Windows CTrl+C handler
void ConsoleCtrlHandler(int dummy) {
	isWorking = 0;
}

void onSocketError(const char* error) {
	printf("Socket Error : %s\n", error);
	isWorking = 0;
}

void onStatusEvent(const char* code,const char* description) {
	printf("Status Event '%s' : %s\n", code, description);

	if(strcmp(code,"NetConnection.Connect.Success")==0)
		RTMFP_Play(context, "test123");
}

int main(int argc,char* argv[]) {

	signal(SIGINT, ConsoleCtrlHandler);

	/*Exception ex;
	CustomConnection conn;*/
	context = RTMFP_Connect("127.0.0.1", 1935, "rtmfp://localhost/MonaClients/", onSocketError, onStatusEvent);

	if(context) {
		isWorking = 1;
		while(isWorking) {
			//printf("test");
		}
		printf("Closing connection...\n");
		RTMFP_Close(context);

		RTMFP_Terminate();
	}
	system("pause");
}
