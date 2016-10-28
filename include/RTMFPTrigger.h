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

#pragma once

#include "Mona/Mona.h"
#include "Mona/Time.h"
#include "Mona/Exceptions.h"

class RTMFPTrigger : public virtual Mona::Object {
public:
	RTMFPTrigger(Mona::UInt32 delay = 1000, Mona::UInt8 cycles = 8);
	
	Mona::UInt16 raise(Mona::Exception& ex);
	void start();
	void reset();
	void stop() { _running = false; }
	Mona::Int8 cycle() { return _cycle; }
private:
	Mona::Time		_timeElapsed;
	Mona::Int8		_cycle;
	Mona::UInt8		_time;
	bool			_running;
	Mona::UInt32	_delay; // time (in msec) between each attempt
	Mona::UInt8		_cycles; // number of cycles to attempts before raising an exception
};

