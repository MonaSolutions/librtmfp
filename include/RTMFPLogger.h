/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Base/ConsoleLogger.h"

class RTMFPLogger: public Base::ConsoleLogger {
public:
	RTMFPLogger(void(*onLog)(unsigned int, const char*, long, const char*), void(*onDump)(const char*, const void*, unsigned int)): _onLog(onLog), _onDump(onDump) {}

	virtual bool log(Base::LOG_LEVEL level, const Base::Path& file, long line, const std::string& message) {

		if (_onLog)
			_onLog(level, file.name().c_str(), line, message.c_str());
		else
			ConsoleLogger::log(level, file, line, message);
		return true;
	}

	virtual bool dump(const std::string& header, const Base::UInt8* data, Base::UInt32 size) {

		if (_onDump)
			_onDump(header.c_str(), data, size);
		else
			ConsoleLogger::dump(header, data, size);
		return true;
	}

private:
	void(* _onLog)(unsigned int, const char*, long, const char*);
	void(*_onDump)(const char*, const void*, unsigned int);
};