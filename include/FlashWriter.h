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
#include "AMF.h"
#include "AMFWriter.h"
#include "Mona/Packet.h"
#include "Mona/Parameters.h"
#include "RTMFP.h"

/*************************************************
Writer of AMF messages, must be inherited
for each protocol (just RTMFP for now)
*/
class FlashWriter : public virtual Mona::Object {
public:
	enum MediaType {
		INIT,
		AUDIO,
		VIDEO,
		DATA,
		START,
		STOP
	};
	enum State {
		OPENING,
		OPENED,
		NEAR_CLOSED,
		CLOSED
	};

	bool					reliable;

	void					open() { if(_state==OPENING) _state = OPENED;}
	
	bool					amf0;
	
	virtual void			writeRaw(const Mona::UInt8* data, Mona::UInt32 size) = 0; // TODO: see we need a GroupWriter
	AMFWriter&				writeMessage();
	AMFWriter&				writeInvocation(const char* name, bool amf3=false) { return writeInvocation(name,0,amf3); }

	AMFWriter&				writeAMFSuccess(const char* code, const std::string& description, bool withoutClosing = false) { return writeAMFState("_result", code, description, withoutClosing); }
	AMFWriter&				writeAMFStatus(const char* code, const std::string& description, bool withoutClosing = false) { return writeAMFState("onStatus", code, description, withoutClosing); }
	AMFWriter&				writeAMFError(const char* code, const std::string& description, bool withoutClosing = false) { return writeAMFState("_error", code, description, withoutClosing); }
	bool					writeMedia(MediaType type, Mona::UInt32 time, const Mona::Packet& packet);

	AMFWriter&				writeAMFData(const std::string& name);

	void					setCallbackHandle(double value) { _callbackHandle = value; _callbackHandleOnAbort = 0; }
	virtual void			clear() { _callbackHandle = _callbackHandleOnAbort; } // must erase the queueing messages (don't change the writer state)

protected:
	FlashWriter();

	AMFWriter&				write(AMF::Type type, Mona::UInt32 time = 0) { return write(type, time, RTMFP::TYPE_AMF, Mona::Packet::Null(), reliable); }
	AMFWriter&				write(AMF::Type type, Mona::UInt32 time, const Mona::Packet& packet, bool reliable) { return write(type, time, RTMFP::TYPE_AMF, packet, reliable); }
	virtual AMFWriter&		write(AMF::Type type, Mona::UInt32 time, RTMFP::DataType packetType, const Mona::Packet& packet, bool reliable) = 0;
	AMFWriter&				writeInvocation(const char* name,double callback,bool amf3=false);
	AMFWriter&				writeAMFState(const char* name,const char* code,const std::string& description,bool withoutClosing=false);

	State					_state;
private:
	std::string				_onAudio;
	std::string				_onVideo;
	double					_callbackHandleOnAbort;
	double					_callbackHandle;
};
