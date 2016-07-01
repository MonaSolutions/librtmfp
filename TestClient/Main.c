#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include "librtmfp.h"
#include "TestLogger.h"
#include "UtilFunctions.h"

#if defined(_WIN32)
	#define strnicmp		_strnicmp
	#define stricmp			_stricmp
	#define snprintf		sprintf_s
	#define	SLEEP			Sleep
	#include <windows.h>
#else
	#define stricmp			strcasecmp
	#define strnicmp		strncasecmp
	#define _snprintf		snprintf
	static struct timespec ts;
	#define SLEEP(TIME)		ts.tv_sec=(time_t)(TIME/1000);ts.tv_nsec=(TIME%1000)*1000000;nanosleep(&ts,NULL)
#endif

// Global variables declaration
#define BUFFER_SIZE			20480
static char				buf[BUFFER_SIZE];
static unsigned int		cursor = BUFFER_SIZE; // File reader cursor
static unsigned short	endOfWrite = 0; // If >0 write is finished
static unsigned int		context = 0;
static FILE *			pInFile = NULL; // Input file for publication
static FILE *			pOutFile = NULL; // Output file for subscription
static unsigned short	terminating = 0;
static char*			publication = NULL; // publication name

static enum TestOption {
	SYNC_READ, // default
	ASYNC_READ,
	WRITE,
	P2P_WRITE
} _option = 0;

static unsigned int		nbPeers = 0;
static char*			listPeers[255];
static char*			listStreams[255];
static FILE*			listFiles[255];
static char*			listFileNames[255];

// Open the p2p configuration file for multiple peers
// Format :
// <peer id>;<stream name>;<output file name>
static void loadPeers(const char* path) {
	FILE* pConfigFile = NULL;
	char line[1024];
	char *semiColon = NULL, *semiColon2 = NULL, *eol = NULL;
	unsigned int i = 0;

	if (!openFile(&pConfigFile, path, "r"))
		return;

	while (!feof(pConfigFile)) {
		i++;
		fgets(line, 1024, pConfigFile);
		semiColon = strchr(line, ';');
		if (semiColon == NULL) {
			printf("Expected semi-colon in line %d of '%s', line ignored\n", i, path);
			continue;
		}
		semiColon2 = strchr(semiColon+1, ';');
		if (semiColon2 == NULL) {
			printf("Expected 2nd semi-colon in line %d of '%s', line ignored\n", i, path);
			continue;
		}

		// Peer id
		listPeers[nbPeers] = malloc(255*sizeof(char));
		strncpy(listPeers[nbPeers], line, 255);
		listPeers[nbPeers][semiColon - line] = '\0';

		// Stream name
		listStreams[nbPeers] = malloc(255 * sizeof(char));
		strncpy(listStreams[nbPeers], ++semiColon, 255);
		listStreams[nbPeers][semiColon2 - semiColon] = '\0';

		// File name
		listFileNames[nbPeers] = malloc(255 * sizeof(char));
		strncpy(listFileNames[nbPeers], ++semiColon2, 255);
		if (eol = strrchr(listFileNames[nbPeers], '\n'))
			listFileNames[nbPeers][eol-listFileNames[nbPeers]] = '\0';
		++nbPeers;
	}
}

// return : true if program must be interrupted
static int IsInterrupted(void * arg) {
	return terminating > 0;
}

// Windows CTrl+C handler
void ConsoleCtrlHandler(int dummy) {
	terminating=1;
}

unsigned int flip24(unsigned int value) { return ((value >> 16) & 0x000000FF) | (value & 0x0000FF00) | ((value << 16) & 0x00FF0000); }
unsigned int flip32(unsigned int value) { return ((value >> 24) & 0x000000FF) | ((value >> 8) & 0x0000FF00) | ((value << 8) & 0x00FF0000) | ((value << 24) & 0xFF000000); }

// Open the out/in files
void initFiles() {
	unsigned int i = 0;

	if (_option == WRITE || _option == P2P_WRITE) {
		if (openFile(&pInFile, "out.flv", "rb"))
			printf("Input file out.flv opened\n");
	}
	else if (nbPeers > 0) {
		for (i = 0; i < nbPeers; i++) {
			if (openFile(&listFiles[i], listFileNames[i], "wb+")) {
				printf("Output file %s opened\n", listFileNames[i]);

				if (_option == SYNC_READ)
					fwrite("\x46\x4c\x56\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00", sizeof(char), 13, listFiles[i]);
			}
		}
	}
	else { // Normal read file
		if (!openFile(&pOutFile, "out.flv", "wb+"))
			return;
		printf("Output file out.flv opened\n");
		if (_option == SYNC_READ)
			fwrite("\x46\x4c\x56\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00", sizeof(char), 13, pOutFile);
	}
}

// Close all files
void closeFiles() {
	unsigned int i = 0;
	printf("Closing files...\n");

	if (pInFile) {
		fclose(pInFile);
		pInFile = NULL;
	}
	if (nbPeers > 0) {
		for (i = 0; i < nbPeers; i++) {
			if (listFiles[i]) {
				fclose(listFiles[i]);
				listFiles[i] = NULL;
			}
		}
	}
	if (pOutFile) {
		fclose(pOutFile);
		pOutFile = NULL;
	}
}

// Get the file pointer for a p2p stream
FILE* getFile(const char* peerId) {
	unsigned int i = 0;

	for (i = 0; i < nbPeers; i++) {
		if (stricmp(listPeers[i], peerId) == 0)
			return listFiles[i];
	}
	return NULL;
}

void onSocketError(const char* error) {
	onLog(0, 7, "Main.cpp", __LINE__, error);
}

void onStatusEvent(const char* code,const char* description) {
	char statusMessage[1024];
	snprintf(statusMessage, 1024, "Status Event '%s' : %s", code, description);
	onLog(0, 6, "Main.cpp", __LINE__, statusMessage);

	if (strcmp(code, "NetConnection.Connect.Closed") == 0)
		terminating=1;
}

// Synchronous read
void onMedia(const char * peerId,const char* stream, unsigned int time,const char* buf,unsigned int size,int audio) {
	FILE* pFile = getFile(peerId);
	if (!pFile)
		pFile = pOutFile;

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

// Call each second to read/write asynchronously
void onManage() {
	int read = 0, res = 0, towrite = BUFFER_SIZE;
	unsigned int i = 0;

	// Asynchronous read
	if (_option == ASYNC_READ) {
		if (nbPeers>0) {
			for (i = 0; i < nbPeers; i++) {
				if ((read = RTMFP_Read(listPeers[i], context, buf, BUFFER_SIZE))>0)
					fwrite(buf, sizeof(char), read, listFiles[i]);
			}
		}
		else if((read = RTMFP_Read("",context,buf,BUFFER_SIZE))>0)
			fwrite(buf, sizeof(char), read, pOutFile);
	}
	// Write
	else if ((_option == WRITE || _option == P2P_WRITE) && !endOfWrite) {

		// First we read the file
		if (cursor != 0) {
			towrite = fread(buf + (BUFFER_SIZE - cursor), sizeof(char), cursor, pInFile) + (BUFFER_SIZE - cursor);
			if ((res = ferror(pInFile)) > 0) {
				endOfWrite = 1;
				onLog(0, 3, "Main.lua", __LINE__, "Error while reading the input file, closing...");
				terminating=1; // Error encountered
				return;
			}
			else if ((res = feof(pInFile)) > 0) {
				onLog(0, 5, "Main.lua", __LINE__, "End of file reached, we send last data and unpublish");
				endOfWrite = 1;
				RTMFP_Write(context, buf, towrite);
				if (_option == WRITE)
					RTMFP_ClosePublication(context, publication);
				return;
			}
		}

		if ((read = RTMFP_Write(context, buf, towrite)) < 0)
			terminating=1; // Error encountered
		else if (!endOfWrite) {
			if ((cursor = read) > 0)
				memcpy(buf, buf + cursor, BUFFER_SIZE - cursor); // Move buffer
		}
	}
}

////////////////////////////////////////////////////////
// Main Function
int main(int argc,char* argv[]) {
	char 			url[1024];
	int				i=1;
	unsigned int	indexPeer = 0;
	const char*		peerId = NULL;
	const char*		netGroup = NULL;
	unsigned short	audioReliable = 1, videoReliable = 1, p2pPlay = 1;
	double			updatePeriod = 0.2;
	unsigned int	windowDuration = 10;
	snprintf(url, 1024, "rtmfp://127.0.0.1/test123");

	for(i; i<argc; i++) {
		if (stricmp(argv[i], "--syncread")==0) // default
			_option = SYNC_READ;
		else if (stricmp(argv[i], "--asyncread")==0)
			_option = ASYNC_READ;
		else if (stricmp(argv[i], "--write")==0)
			_option = WRITE;
		else if (stricmp(argv[i], "--p2pWrite")==0)
			_option = P2P_WRITE;
		else if (stricmp(argv[i], "--dump") == 0) {
			RTMFP_ActiveDump();
			RTMFP_DumpSetCallback(onDump);
		} 
		else if (stricmp(argv[i], "--audioUnbuffered") == 0) // for publish mode
			audioReliable = 0;
		else if (stricmp(argv[i], "--videoUnbuffered") == 0) // for publish mode
			videoReliable = 0;
		else if (strlen(argv[i]) > 15 && strnicmp(argv[i], "--updatePeriod=", 15)==0) // for NetGroup mode (multicastAvailabilityUpdatePeriod)
			sscanf(argv[i] + 15, "%lf", &updatePeriod);
		else if (strlen(argv[i]) > 17 && strnicmp(argv[i], "--windowDuration=", 17) == 0) // for NetGroup mode (multicastWindowDuration)
			windowDuration = atoi(argv[i] + 17);
		else if (strlen(argv[i]) > 6 && strnicmp(argv[i], "--url=", 6)==0)
			snprintf(url, 1024, "%s", argv[i] + 6);
		else if (strlen(argv[i]) > 9 && strnicmp(argv[i], "--peerId=", 9) == 0) // p2p direct
			peerId = argv[i] + 9;
		else if (strlen(argv[i]) > 11 && strnicmp(argv[i], "--netGroup=", 11) == 0) // groupspec for NetGroup
			netGroup = argv[i] + 11;
		else if (strlen(argv[i]) > 6 && strnicmp(argv[i], "--log=", 6) == 0)
			RTMFP_LogSetLevel(atoi(argv[i] + 6));
		else if (strlen(argv[i]) > 12 && strnicmp(argv[i], "--peersFile=", 12) == 0) // p2p direct with multiple peers
			loadPeers(argv[i] + 12);
		else {
			printf("Unknown option '%s'\n", argv[i]);
			exit(-1);
		}
	}

	if (signal(SIGINT, ConsoleCtrlHandler) == SIG_ERR)
		printf("Cannot catch SIGINT\n");

	// Open log file
	if (openFile(&pLogFile, "log.0", "w")) {

		RTMFP_LogSetCallback(onLog);
		RTMFP_InterruptSetCallback(IsInterrupted, NULL);
		RTMFP_GetPublicationAndUrlFromUri(url, &publication);

		printf("Connection to url '%s' - mode : %s\n", url, ((_option == SYNC_READ) ? "Synchronous read" : ((_option == ASYNC_READ) ? "Asynchronous read" : "Write")));
		context = RTMFP_Connect(url, onSocketError, onStatusEvent, (_option == SYNC_READ) ? onMedia : NULL, 1);

		if (context) {
			if (peerId != NULL) {
				nbPeers = 1;
				listPeers[0] = (char*)peerId;
				listStreams[0] = publication;
				listFileNames[0] = "out.flv";
			}

			if (netGroup)
				RTMFP_Connect2Group(context, netGroup, publication, (_option == WRITE || _option == P2P_WRITE), updatePeriod, windowDuration, 1);
			else if (_option == WRITE)
				RTMFP_Publish(context, publication, audioReliable, videoReliable, 1);
			else if (_option == P2P_WRITE)
				RTMFP_PublishP2P(context, publication, audioReliable, videoReliable, 1);
			else if (nbPeers > 0) { // P2p Play
				for (indexPeer = 0; indexPeer < nbPeers; indexPeer++)
					RTMFP_Connect2Peer(context, listPeers[indexPeer], listStreams[indexPeer]);
			}
			else if (_option == SYNC_READ || _option == ASYNC_READ)
				RTMFP_Play(context, publication);

			initFiles();
			while (!IsInterrupted(NULL)) {
				onManage();
				SLEEP(1000);
			}

			printf("Closing connection...\n");
			RTMFP_Close(context);
			closeFiles();
		}

		fclose(pLogFile);
		pLogFile = NULL;
	}
	printf("End of the program\n");
}
