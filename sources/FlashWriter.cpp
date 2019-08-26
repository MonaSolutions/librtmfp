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

#include "FlashWriter.h"
#include "AMF.h"
#include "Base/Util.h"
#include "Base/Logs.h"

using namespace std;
using namespace Base;


FlashWriter::FlashWriter() : _callbackHandleOnAbort(0),_callbackHandle(0),amf0(false),reliable(true),_state(OPENED) {
}

AMFWriter& FlashWriter::writeMessage() {
	AMFWriter& writer(writeInvocation("_result",_callbackHandleOnAbort = _callbackHandle));
	_callbackHandle = 0;
	return writer;
}

AMFWriter& FlashWriter::writeInvocation(const char* name, double callback, bool amf3) {
	AMFWriter& writer(write(AMF::TYPE_INVOCATION));
	RTMFP::WriteInvocation(writer, name, callback, amf3);
	writer.amf0 = amf0;
	return writer;
}

AMFWriter& FlashWriter::writeAMFState(const char* name,const char* code,const string& description,bool withoutClosing) {
	AMFWriter& writer = (AMFWriter&)writeInvocation(name,_callbackHandleOnAbort = _callbackHandle);
	_callbackHandle = 0;
	RTMFP::WriteAMFState(writer, name, code, description, amf0, withoutClosing);
	return writer;
}

AMFWriter& FlashWriter::writeAMFData(const string& name) {
	AMFWriter& writer(write(AMF::TYPE_DATA));
	writer.amf0 = true;
	writer.writeString(name.data(),name.size());
	writer.amf0 = false;
	return writer;
}

bool FlashWriter::writeMedia(MediaType type,UInt32 time, const Packet& packet) {
	
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
			write(AMF::TYPE_AUDIO, time, packet, reliable);
			break;
		case VIDEO:
			write(AMF::TYPE_VIDEO, time, packet, reliable);
			break;
		case DATA: {
			// Trick to forward Metadata
			shared_ptr<Buffer>	pBuffer(new Buffer(packet.size()+16));
			BinaryWriter writer(pBuffer->data(), pBuffer->size());
			writer.write(EXPAND("\x2\x0\r@setDataFrame"));
			writer.write(packet.data(), packet.size());
			write(AMF::TYPE_DATA, time, Packet(pBuffer), reliable);
			break;
		}
		default:
			WARN("writeMedia method not supported by RTMFP for ", String::Format<MediaType>("%.2x",type)," type")
	}
	return true;
}
