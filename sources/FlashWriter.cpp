
#include "FlashWriter.h"
#include "AMF.h"
//#include "Mona/MIME.h"
#include "Mona/Util.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;


FlashWriter::FlashWriter(State state,const PoolBuffers& poolBuffers) : poolBuffers(poolBuffers),_callbackHandleOnAbort(0),_callbackHandle(0),amf0(false),reliable(true),_state(state) {
}

FlashWriter::FlashWriter(FlashWriter& other) : reliable(other.reliable), poolBuffers(other.poolBuffers),_callbackHandle(other._callbackHandle),_callbackHandleOnAbort(0),amf0(other.amf0) {
	other._callbackHandle = 0;
}

AMFWriter& FlashWriter::writeMessage() {
	AMFWriter& writer(writeInvocation("_result",_callbackHandleOnAbort = _callbackHandle));
	_callbackHandle = 0;
	return writer;
}

AMFWriter& FlashWriter::writeInvocation(const char* name, double callback, bool amf3) {
	AMFWriter& writer = write(amf3? AMF::INVOCATION_AMF3 : AMF::INVOCATION);
	BinaryWriter& packet = writer.packet;
	packet.write8(AMF_STRING).write16((UInt16)strlen(name)).write(name);
	packet.write8(AMF_NUMBER).writeNumber<double>(callback);
	if (amf3) // TODO: strange, without this connect or play doesn't work with AMS
		packet.write8(AMF_NULL);
	//TODO: see if needed :
	//writer.amf0 = amf0;
	return writer;
}

AMFWriter& FlashWriter::writeAMFState(const char* name,const char* code,const string& description,bool withoutClosing) {
	AMFWriter& writer = (AMFWriter&)writeInvocation(name,_callbackHandleOnAbort = _callbackHandle);
	_callbackHandle = 0;
	writer.amf0=true;
	writer.beginObject();
	if(strcmp(name,"_error")==0)
		writer.writeStringProperty("level","error");
	else
		writer.writeStringProperty("level","status");
	writer.writeStringProperty("code",code);
	writer.writeStringProperty("description", description);
	writer.amf0 = amf0;
	if(!withoutClosing)
		writer.endObject();
	return writer;
}

AMFWriter& FlashWriter::writeAMFData(const string& name) {
	AMFWriter& writer(write(AMF::DATA));
	writer.amf0 = true;
	writer.writeString(name.data(),name.size());
	writer.amf0 = false;
	return writer;
}

bool FlashWriter::writeMedia(MediaType type,UInt32 time, const UInt8* data, UInt32 size) {
	
	switch(type) {
		case START:
			/*if (time==DATA)
				writeAMFStatus("NetStream.Play.PublishNotify",string(STR data, size) + " is now published");*/
			break;
		case STOP:
			/*if (time==DATA)
				writeAMFStatus("NetStream.Play.UnpublishNotify",string(STR data, size) + " is now unpublished");*/
			break;
		case AUDIO:
			write(AMF::AUDIO,time,data,size);
			break;
		case VIDEO:
			write(AMF::VIDEO,time,data,size);
			break;
		case DATA: {
			// convert to AMF ?
			/*MIME::Type dataType((MIME::Type)(time >> 8));
			if (dataType!=MIME::AMF) {
				unique_ptr<DataReader> pReader;
				if (!MIME::CreateDataReader(dataType, packet,poolBuffers, pReader)) {
					ERROR("Impossible to convert streaming ", dataType, " data to AMF, data ignored")
					break;
				}
				AMFWriter& writer(write(AMF::DATA, 0));
				if (DataReader::STRING == pReader->nextType()) {
					// Write the handler name in AMF0!
					writer.amf0 = true;
					pReader->read(writer, 1);
					writer.amf0 = false;
				}
				pReader->read(writer); // to AMF
				break;
			}
			write(AMF::DATA, 0, packet.current(),packet.available());*/
			break;
		}
		default:
			WARN("writeMedia method not supported by RTMFP for ",Format<UInt8>("%.2x",(UInt8)type)," type")
	}
	return true;
}

void FlashWriter::close(Int32 code) {
	if(_state==CLOSED)
		return;
	_state=CLOSED; // before flush to get MESSAGE_END!
	flush();
	//OnClose::raise(code);
}
