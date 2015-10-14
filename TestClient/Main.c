#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include "../librtmfp.h"

static int isWorking = 0;
static unsigned int context = 0;
static FILE * fileOut = NULL;

// Windows CTrl+C handler
void ConsoleCtrlHandler(int dummy) {
	isWorking = 0;
}

unsigned int flip24(unsigned int value) { return ((value >> 16) & 0x000000FF) | (value & 0x0000FF00) | ((value << 16) & 0x00FF0000); }
unsigned int flip32(unsigned int value) { return ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) | ((value << 8) & 0x00FF0000) | ((value << 24) & 0xFF000000); }

void onSocketError(const char* error) {
	printf("Socket Error : %s\n", error);
	isWorking = 0;
}

void onStatusEvent(const char* code,const char* description) {
	printf("Status Event '%s' : %s\n", code, description);

	if(strcmp(code,"NetConnection.Connect.Success")==0)
		RTMFP_Play(context, "test123");
}

void onMedia(unsigned int time,const char* buf,unsigned int size,int audio) {
	if (fileOut) {
		unsigned int tmp=0;
		fprintf(fileOut, audio? "\x08" : "\x09");
		tmp = flip24(size);
		fwrite(&tmp, 3, 1, fileOut);
		tmp = flip24(time);
		fwrite(&tmp, 3, 1, fileOut);
		fwrite("\x00\x00\x00\x00", sizeof(char), 4, fileOut);
		fwrite(buf, sizeof(char), size, fileOut);
		tmp = flip32(11+size);
		fwrite(&tmp, 4, 1, fileOut);
	}
}

int main(int argc,char* argv[]) {

	signal(SIGINT, ConsoleCtrlHandler);

	context = RTMFP_Connect("127.0.0.1", 1935, "rtmfp://localhost/MonaClients/", onSocketError, onStatusEvent, onMedia);

	if(context) {
		isWorking = 1;

		errno_t err;
		if((err = fopen_s(&fileOut,"out.flv","wb+"))==0) {
			fwrite("\x46\x4c\x56\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00", sizeof(char), 13, fileOut);

			while(isWorking) {

			}

			/*unsigned int val = flip24(15);
			fwrite(&val, 3, 1, fileOut);
			fwrite(test, sizeof(char), 6, fileOut);*/
   
			fclose(fileOut);
			fileOut = NULL;
		} else
			printf("Unable to open file out.flv : %d", err);

		printf("Closing connection...\n");
		RTMFP_Close(context);
	}
	system("pause");
}
