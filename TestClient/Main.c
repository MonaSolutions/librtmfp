#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include "../librtmfp.h"
#if defined(_WIN32)
	#define strnicmp		_strnicmp
	#define stricmp			_stricmp
	#define	sleep			Sleep
	#include <windows.h>
#else
	#define stricmp			strcasecmp
	#define strnicmp		strncasecmp
#endif

// Global variables declaration
static unsigned int context = 0;
static FILE * fileOut = NULL;
static enum TestOption {
	SYNC_READ,
	ASYNC_READ,
	WRITE
} _option = 0;

// Windows CTrl+C handler
void ConsoleCtrlHandler(int dummy) {
	RTMFP_Terminate();
}

unsigned int flip24(unsigned int value) { return ((value >> 16) & 0x000000FF) | (value & 0x0000FF00) | ((value << 16) & 0x00FF0000); }
unsigned int flip32(unsigned int value) { return ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) | ((value << 8) & 0x00FF0000) | ((value << 24) & 0xFF000000); }

void onLog(int level,const char* message) {

	char* logType = "Unkwnown level";
	switch(level) {
		case 1: logType = "FATAL"; break;
		case 2: logType = "CRITIC"; break;
		case 3: logType = "ERROR"; break;
		case 4: logType = "WARN"; break;
		case 5: logType = "NOTE"; break;
		case 6: logType = "INFO"; break;
		case 7: logType = "DEBUG"; break;
		case 8: logType = "TRACE"; break;
	}
	printf("%s - %s\n", logType, message);
}

void onSocketError(const char* error) {
	printf("Socket Error : %s\n", error);
	RTMFP_Terminate();
}

void onStatusEvent(const char* code,const char* description) {
	printf("Status Event '%s' : %s\n", code, description);
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

void onManage() {
	unsigned int readed = 0;
	char buf[20480];

	// Asynchronous read
	if (_option == ASYNC_READ) {
		if(readed = RTMFP_Read(context,buf,20480))
			fwrite(buf, sizeof(char), readed, fileOut);
	}
}

// Main Function
int main(int argc,char* argv[]) {
	char*	url = "rtmfp://127.0.0.1/";
	int		i=1;

	for(i; i<argc; i++) {
		if (stricmp(argv[i], "--syncread")==0) // default
			_option = SYNC_READ;
		else if (stricmp(argv[i], "--asyncread")==0)
			_option = ASYNC_READ;
		else if (stricmp(argv[i], "--write")==0)
			_option = WRITE;
		/*else if (strlen(argv[i]) > 5 && strnicmp(argv[i], "host=", 5)==0)
			host = argv[i]+5;
		else if (strlen(argv[i]) > 5 && strnicmp(argv[i], "port=", 5)==0)
			port = atoi(argv[i]+5);*/
		else if (strlen(argv[i]) > 4 && strnicmp(argv[i], "url=", 4)==0)
			url = argv[i]+4;
		else
			printf("Unknown option '%s'\n", argv[i]);
	}

	signal(SIGINT, ConsoleCtrlHandler);

	RTMFP_LogSetCallback(onLog);
	printf("Connection to '%s'\n", url);
	context = RTMFP_Connect(url, _option==WRITE, onSocketError, onStatusEvent, (_option == SYNC_READ)? onMedia : NULL);

	if(context) {

#if defined(WIN32)
		errno_t err;
		if(_option != WRITE && (err = fopen_s(&fileOut,"out.flv","wb+"))!=0)
			printf("Unable to open file out.flv : %d\n", err);
	#else
		if(_option != WRITE && (fileOut = fopen("out.flv","wb+")) == NULL)
			printf("Unable to open file out.flv\n");
	#endif
		else {
			if(_option != WRITE) {
				printf("Output file out.flv opened\n");
				if (_option == SYNC_READ)
					fwrite("\x46\x4c\x56\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00", sizeof(char), 13, fileOut);
			}

			RTMFP_OnManageSetCallback(onManage);
			RTMFP_WaitTermination();

			if(_option != WRITE) {
				fclose(fileOut);
				fileOut = NULL;
			}
		}

		printf("Closing connection...\n");
		RTMFP_Close(context);
	}
	printf("End of the program\n");
}
