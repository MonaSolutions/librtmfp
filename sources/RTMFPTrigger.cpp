
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

