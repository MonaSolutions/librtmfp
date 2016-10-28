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

#include "RTMFPTrigger.h"

using namespace std;
using namespace Mona;

RTMFPTrigger::RTMFPTrigger(UInt32 delay, Mona::UInt8 cycles) : _time(1),_cycle(0),_running(false), _delay(delay), _cycles(cycles) {
	
}

void RTMFPTrigger::reset() {
	_timeElapsed.update();
	_time=1;
	_cycle=0;
}

void RTMFPTrigger::start() {
	if(_running)
		return;
	reset();
	_running=true;
}

UInt16 RTMFPTrigger::raise(Exception& ex) {
	if(!_running)
		return 0;
	// Wait at least _delay sec before to begin the repeat cycle
	if(!_timeElapsed.isElapsed(_delay))
		return 0;
	++_time;
	_timeElapsed.update();
	if(_time>=_cycle) {
		_time=1;
		++_cycle;
		if (_cycle == _cycles) {
			ex.set(Exception::PROTOCOL, "Repeat RTMFPTrigger failed");
			return 0;
		}
		return _cycle;
	}
	return 0;
}

