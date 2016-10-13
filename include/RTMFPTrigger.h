
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
	Mona::Int8 cycle() { return _cycles; }
private:
	Mona::Time		_timeElapsed;
	Mona::Int8		_cycle;
	Mona::UInt8		_time;
	bool			_running;
	Mona::UInt32	_delay; // time (in msec) between each attempt
	Mona::UInt8		_cycles; // number of cycles to attempts before raising an exception
};

