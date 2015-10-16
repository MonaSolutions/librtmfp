#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include "../librtmfp.h"
#if defined(_WIN32)
	#define	sleep			Sleep
	#include <windows.h>
#endif

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

// Synchronous read
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

	context = RTMFP_Connect("127.0.0.1", 1935, "rtmfp://localhost/MonaClients/", onSocketError, onStatusEvent, NULL);

	if(context) {
		isWorking = 1;

#if defined(WIN32)
		errno_t err;
		if((err = fopen_s(&fileOut,"out.flv","wb+"))==0) {
#else
		if((fileOut = fopen("out.flv","wb+")) != NULL) {
#endif
			printf("Output file out.flv opened\n");
			fwrite("\x46\x4c\x56\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00", sizeof(char), 13, fileOut);

			unsigned int readed = 0;
			char buf[20480];
			while(isWorking) {
				if(readed = RTMFP_Read(context,buf,20480)){ // Asynchronous read
					fwrite(buf, sizeof(char), readed, fileOut);
				}
				sleep(500);
			}
   
			fclose(fileOut);
			fileOut = NULL;
		} else
#if defined(WIN32)
			printf("Unable to open file out.flv : %d\n", err);
#else
			printf("Unable to open file out.flv\n");
#endif
		printf("Closing connection...\n");
		RTMFP_Close(context);
	}
	system("pause");
}
