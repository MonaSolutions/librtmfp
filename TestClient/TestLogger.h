#pragma once

#include <stdio.h>
#include <time.h>

#if defined(_WIN32)
	#define FATAL_COLOR 12
	#define CRITIC_COLOR 12
	#define ERROR_COLOR 13
	#define WARN_COLOR 14
	#define NOTE_COLOR 10
	#define INFO_COLOR 15
	#define DEBUG_COLOR 7
	#define TRACE_COLOR 8
	#define BEGIN_CONSOLE_TEXT_COLOR(color) HANDLE ConsoleHandle = GetStdHandle(STD_OUTPUT_HANDLE); SetConsoleTextAttribute(ConsoleHandle, color)
	#define END_CONSOLE_TEXT_COLOR			SetConsoleTextAttribute(ConsoleHandle, LevelColors[6])
	#include <windows.h>
#else
	#define FATAL_COLOR "\033[01;31m"
	#define	CRITIC_COLOR "\033[01;31m"
	#define ERROR_COLOR "\033[01;35m"
	#define WARN_COLOR "\033[01;33m"	
	#define NOTE_COLOR "\033[01;32m"
	#define INFO_COLOR "\033[01;37m"
	#define DEBUG_COLOR "\033[0m"
	#define TRACE_COLOR "\033[01;30m"
	#define BEGIN_CONSOLE_TEXT_COLOR(color) fprintf(stdout,"%s",color)
	#define END_CONSOLE_TEXT_COLOR			BEGIN_CONSOLE_TEXT_COLOR(LevelColors[6])
#endif

#if defined(_WIN32)
static int			LevelColors[] = { FATAL_COLOR, CRITIC_COLOR, ERROR_COLOR, WARN_COLOR, NOTE_COLOR, INFO_COLOR, DEBUG_COLOR, TRACE_COLOR };
#else
static const char*  LevelColors[] = { FATAL_COLOR, CRITIC_COLOR, ERROR_COLOR, WARN_COLOR, NOTE_COLOR, INFO_COLOR, DEBUG_COLOR, TRACE_COLOR };
#endif

// Log file handle
static FILE *			pLogFile = NULL;

void onLog(unsigned int threadID, int level, const char* fileName, long line, const char* message) {

	char* logType = "Unkwnown level";
	switch (level) {
	case 1: logType = "FATAL"; break;
	case 2: logType = "CRITIC"; break;
	case 3: logType = "ERROR"; break;
	case 4: logType = "WARN"; break;
	case 5: logType = "NOTE"; break;
	case 6: logType = "INFO"; break;
	case 7: logType = "DEBUG"; break;
	case 8: logType = "TRACE"; break;
	}
	time_t t = time(NULL);
	struct tm tm;
#if defined(_WIN32)
	localtime_s(&tm, &t);
#else
	tm = *localtime(&t);
#endif
	BEGIN_CONSOLE_TEXT_COLOR(LevelColors[level-1]);
	printf("%s[%ld] %s\n", fileName, line, message);
	END_CONSOLE_TEXT_COLOR;

	if (pLogFile) {
		fprintf(pLogFile, "%.2d/%.2d %.2d:%.2d:%.2d\t%s %s[%ld] %s\n", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, logType, fileName, line, message);
		fflush(pLogFile);
	}
}

void onDump(const char* header, const void* data, unsigned int size) {
	printf("%s\n", header);
	fwrite(data, sizeof(char), size, stdout);
	if (pLogFile) {
		fprintf(pLogFile, "%s\n", header);
		fwrite(data, sizeof(char), size, pLogFile);
		fflush(pLogFile);
	}
}
