#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include "../librtmfp.h"
#if defined(_WIN32)
	#define strnicmp		_strnicmp
	#define stricmp			_stricmp
	#define	SLEEP			Sleep
	#include <windows.h>
#else
	#include <time.h>
	#define stricmp			strcasecmp
	#define strnicmp		strncasecmp
	static struct timespec ts;
	#define SLEEP(TIME)		ts.tv_sec=(time_t)(TIME/1000);ts.tv_nsec=(TIME%1000)*1000000;nanosleep(&ts,NULL)
#endif

// Global variables declaration
#define BUFFER_SIZE			20480
static char buf[BUFFER_SIZE];
static unsigned int cursor = BUFFER_SIZE; // File reader cursor
static unsigned short endOfWrite = 0; // If >0 write is finished
static unsigned int context = 0;
static FILE * pFile = NULL;
static unsigned short terminating = 0;
static enum TestOption {
	SYNC_READ,
	ASYNC_READ,
	WRITE
} _option = 0;

// Windows CTrl+C handler
void ConsoleCtrlHandler(int dummy) {
	terminating=1;
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
	terminating=1;
}

void onStatusEvent(const char* code,const char* description) {
	printf("Status Event '%s' : %s\n", code, description);
}

// Synchronous read
void onMedia(unsigned int time,const char* buf,unsigned int size,int audio) {
	if (pFile) {
		unsigned int tmp=0;
		fprintf(pFile, audio? "\x08" : "\x09");
		tmp = flip24(size);
		fwrite(&tmp, 3, 1, pFile);
		tmp = flip24(time);
		fwrite(&tmp, 3, 1, pFile);
		fwrite("\x00\x00\x00\x00", sizeof(char), 4, pFile);
		fwrite(buf, sizeof(char), size, pFile);
		tmp = flip32(11+size);
		fwrite(&tmp, 4, 1, pFile);
	}
}

void onManage() {
	int read = 0, res = 0, towrite = BUFFER_SIZE;

	// Asynchronous read
	if (_option == ASYNC_READ) {
		if((read = RTMFP_Read(context,buf,BUFFER_SIZE))>0)
			fwrite(buf, sizeof(char), read, pFile);
	}
	// Write
	else if (_option == WRITE && !endOfWrite) {

		// First we read the file
		if (cursor != 0) {
			towrite = fread(buf + (BUFFER_SIZE - cursor), sizeof(char), cursor, pFile) + (BUFFER_SIZE - cursor);
			if ((res = ferror(pFile)) > 0) {
				endOfWrite = 1;
				printf("Error while reading the input file, closing...\n");
				terminating=1; // Error encountered
				return;
			}
			else if ((res = feof(pFile)) > 0)
				endOfWrite = 1;
		}

		if ((read = RTMFP_Write(context, buf, towrite)) < 0)
			terminating=1; // Error encountered
		else if (!endOfWrite) {
			if ((cursor = read) > 0)
				memcpy(buf, buf + cursor, BUFFER_SIZE - cursor); // Move buffer
		}
		else {
			printf("End of file reached, goodbye!\n");
			terminating = 1;
		}
	}
}

// Main Function
int main(int argc,char* argv[]) {
	const char*		url = "rtmfp://127.0.0.1/test123";
	int				i=1;
	unsigned short	audioReliable=1;
	unsigned short	videoReliable=1;

	for(i; i<argc; i++) {
		if (stricmp(argv[i], "--syncread")==0) // default
			_option = SYNC_READ;
		else if (stricmp(argv[i], "--asyncread")==0)
			_option = ASYNC_READ;
		else if (stricmp(argv[i], "--write")==0)
			_option = WRITE;
		else if (stricmp(argv[i], "--audioUnbuffered") == 0) // for publish mode
			audioReliable = 0;
		else if (stricmp(argv[i], "--videoUnbuffered") == 0) // for publish mode
			videoReliable = 0;
		else if (strlen(argv[i]) > 4 && strnicmp(argv[i], "url=", 4)==0)
			url = argv[i]+4;
		else
			printf("Unknown option '%s'\n", argv[i]);
	}

	signal(SIGINT, ConsoleCtrlHandler);

	RTMFP_LogSetCallback(onLog);
	printf("Connection to '%s' - mode : %s\n", url, ((_option==SYNC_READ)? "Synchronous read" : ((_option==ASYNC_READ)? "Asynchronous read" : "Write")));
	context = RTMFP_Connect(url, _option==WRITE, onSocketError, onStatusEvent, (_option == SYNC_READ)? onMedia : NULL, audioReliable, videoReliable);

	if(context) {

#if defined(WIN32)
		errno_t err;
		if((err = fopen_s(&pFile,"out.flv", (_option == WRITE)? "rb" : "wb+"))!=0)
			printf("Unable to open file out.flv : %d\n", err);
	#else
		if((pFile = fopen("out.flv", (_option == WRITE) ? "rb" : "wb+")) == NULL)
			printf("Unable to open file out.flv\n");
	#endif
		else {
			printf("Output file out.flv opened\n");
			if (_option == SYNC_READ)
				fwrite("\x46\x4c\x56\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00", sizeof(char), 13, pFile);

			while (!terminating) {
				onManage();
				SLEEP(1000);
			}

			fclose(pFile);
			pFile = NULL;
		}
		printf("Closing connection...\n");
		RTMFP_Close(context);
	}

	printf("End of the program\n");
}
