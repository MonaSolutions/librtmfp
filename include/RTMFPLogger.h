#include "Mona/Logger.h"

class RTMFPLogger: public Mona::Logger {
public:
	RTMFPLogger(): _onLog(NULL), _onDump(NULL) {

	}

	virtual void log(THREAD_ID threadId, Level level, const char *filePath, std::string& shortFilePath, long line, std::string& message) {

		if (_onLog)
			_onLog(threadId, level, shortFilePath.c_str(), line, message.c_str());
		else
			Logger::log(threadId, level, filePath, shortFilePath, line, message);
	}

	virtual void dump(const std::string& header, const Mona::UInt8* data, Mona::UInt32 size) {

		if (_onDump)
			_onDump(header.c_str(), data, size);
		else
			Logger::dump(header, data, size);
	}

	void setLogCallback(void(*onLog)(unsigned int, int, const char*, long, const char*)) { _onLog = onLog; }

	void setDumpCallback(void(*onDump)(const char*, const void*, unsigned int)) { _onDump = onDump; }

private:
	void (* _onLog)(unsigned int, int, const char*, long, const char*);
	void (*_onDump)(const char*, const void*, unsigned int);
};