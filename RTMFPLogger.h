#include "Mona/Logger.h"

class RTMFPLogger: public Mona::Logger {
public:
	RTMFPLogger(void(* onLog)(int,const char*)): _onLog(onLog) {

	}

	virtual void log(THREAD_ID threadId, Level level, const char *filePath, std::string& shortFilePath, long line, std::string& message) {

		_onLog(level, message.c_str());
	}

	virtual void dump(const std::string& header, const Mona::UInt8* data, Mona::UInt32 size) {
		// TODO: not implemented
		Logger::dump(header, data, size);
	}

private:
	void (* _onLog)(int,const char*);
};