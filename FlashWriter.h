
#pragma once

#include "Mona/Mona.h"
#include "AMF.h"
#include "AMFWriter.h"
#include "Mona/PacketReader.h"
#include "Mona/Parameters.h"

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
		CLOSED
	};

	bool					reliable;

	State					state() { return _state; }
	void					open() { if(_state==OPENING) _state = OPENED;}

	virtual bool			flush() { return false;  } // return true if something has been sent!
	
	bool					amf0;
	
	Mona::BinaryWriter&		writeRaw() { return write(AMF::RAW).packet; }
	AMFWriter&				writeMessage();
	AMFWriter&				writeInvocation(const char* name, bool amf3=false) { return writeInvocation(name,0,amf3); }
	virtual FlashWriter&	newWriter() { return *this; }

	AMFWriter&				writeAMFSuccess(const char* code, const std::string& description, bool withoutClosing = false) { return writeAMFState("_result", code, description, withoutClosing); }
	AMFWriter&				writeAMFStatus(const char* code, const std::string& description, bool withoutClosing = false) { return writeAMFState("onStatus", code, description, withoutClosing); }
	AMFWriter&				writeAMFError(const char* code, const std::string& description, bool withoutClosing = false) { return writeAMFState("_error", code, description, withoutClosing); }
	bool					writeMedia(MediaType type,Mona::UInt32 time, const Mona::UInt8* data, Mona::UInt32 size);

	AMFWriter&				writeAMFData(const std::string& name);

	void					writePing() { writeRaw().write16(0x0006).write32((Mona::UInt32)Mona::Time::Now()); }
	void					writePong(Mona::UInt32 pingTime) { writeRaw().write16(0x0007).write32(pingTime); }
	
	// Note: netGroup must be in binary format (32 bytes)
	virtual void			writeGroup(const std::string& netGroup)=0;

	// Note: netGroup must be in hexa format (64 bytes)
	virtual void			writePeerGroup(const std::string& netGroup, const Mona::UInt8* key, const std::string& peerId)=0;

	void					setCallbackHandle(double value) { _callbackHandle = value; _callbackHandleOnAbort = 0; }
	virtual void			clear() { _callbackHandle = _callbackHandleOnAbort; } // must erase the queueing messages (don't change the writer state)

	/**	The main Writer of one session should close the entiere session
	If code==0, it's a normal close
	If code>0, it's a user close (from server application script)
	If code<0, it's a system core close
	-1 => Publisher close!				*/
	virtual void			close(Mona::Int32 code = 0);

protected:
	FlashWriter(State state,const Mona::PoolBuffers& poolBuffers);
	FlashWriter(FlashWriter& other);

	virtual AMFWriter&		write(AMF::ContentType type,Mona::UInt32 time=0,const Mona::UInt8* data=NULL,Mona::UInt32 size=0)=0;
	AMFWriter&				writeInvocation(const char* name,double callback,bool amf3=false);
	AMFWriter&				writeAMFState(const char* name,const char* code,const std::string& description,bool withoutClosing=false);

	const Mona::PoolBuffers&		poolBuffers;
private:
	std::string				_onAudio;
	std::string				_onVideo;
	double					_callbackHandleOnAbort;
	double					_callbackHandle;

	State					_state;
};
