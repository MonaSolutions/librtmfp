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

#include "BandWriter.h"
#include "RTMFPSender.h"

using namespace std;
using namespace Mona;

BandWriter::BandWriter(Invoker& invoker) : _farId(0), _timeReceived(0), _threadSend(0), _invoker(invoker),
	_pEncoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
	_pDecoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)) {
}

BandWriter::~BandWriter() {
	_pSender.reset();
}

BinaryWriter& BandWriter::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(socket(_address.family()), _pEncoder));
	return *_pSender;
}	

void BandWriter::flush(bool echoTime, UInt8 marker) {
	if (!_pSender)
		return;

	if (!failed() && _pSender->size()) {

		// After 30 sec, send packet without echo time
		if (_lastReceptionTime.isElapsed(30000))
			echoTime = false;

		if (echoTime)
			marker += 4;
		else
			_pSender->clip(2);

		BinaryWriter writer(_pSender->buffer().data() + 6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		if (echoTime)
			writer.write16(_timeReceived + RTMFP::Time(_lastReceptionTime.elapsed()));

		_pSender->farId = _farId;
		_pSender->address.set(_address); // set the right address for sending

		if (_pSender->buffer().size() > RTMFP_MAX_PACKET_SIZE)
			ERROR("Message exceeds max RTMFP packet size on connection (", _pSender->buffer().size(), ">", RTMFP_MAX_PACKET_SIZE, ")");

		// executed just in debug mode, or in dump mode
		if (Logs::GetLevel() >= 7)
			DUMP("LIBRTMFP", _pSender->buffer().data() + 6, _pSender->buffer().size() - 6, "Response to ", _address, " (farId : ", _farId, ")")

		Exception ex;
		AUTO_ERROR(_invoker.threadPool.queue(ex, _pSender, _threadSend), "RTMFP flush")
	}
	_pSender.reset();
}
