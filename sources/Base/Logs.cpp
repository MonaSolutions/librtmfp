/*
This file is a part of MonaSolutions Copyright 2017
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or
modify it under the terms of the the Mozilla Public License v2.0.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
Mozilla Public License v. 2.0 received along this program for more
details (or else see http://mozilla.org/MPL/2.0/).

*/

#include "Base/Util.h"
#include <iostream>
#include "Base/Logs.h"

using namespace std;

namespace Base {


mutex					Logs::_Mutex;

volatile bool			Logs::_Dumping(false);

Int32					Logs::_DumpLimit(-1);
volatile bool			Logs::_DumpRequest(true);
volatile bool			Logs::_DumpResponse(true);
#if defined(_DEBUG)
atomic<LOG_LEVEL>		Logs::_Level(LOG_DEBUG); // default log level
#else
atomic<LOG_LEVEL>		Logs::_Level(LOG_INFO); // default log level
#endif
Logger*					Logs::_PLogger(&DefaultLogger());


void Logs::SetDump(const char* name) {
	lock_guard<mutex> lock(_Mutex);
	_DumpResponse = _DumpRequest = true;
	string& filter = DumpFilter();
	if (!name) {
		_Dumping = false;
		filter.clear();
		return;
	}
	_Dumping = true;
	filter.assign(name);
	if (filter.empty())
		return;
	if (filter.back() == '>') {
		_DumpRequest = false;
		filter.pop_back();
	} else if (filter.back() == '<') {
		_DumpResponse = false;
		filter.pop_back();
	}
}

void Logs::Dump(const string& header, const UInt8* data, UInt32 size) {
	Buffer out;
	Util::Dump(data, (_DumpLimit<0 || size<UInt32(_DumpLimit)) ? size : _DumpLimit, out);
	_PLogger->dump(header, out.data(), out.size());
}


} // namespace Base
