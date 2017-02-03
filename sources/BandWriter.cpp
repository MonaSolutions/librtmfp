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

using namespace Mona;

BandWriter::BandWriter() : _farId(0), _timeReceived(0), _pThread(NULL),
	_pEncoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
	_pDecoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)) {

}

BandWriter::~BandWriter() {
	_pThread = NULL;
	_pSender.reset();
}

UInt8* BandWriter::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(poolBuffers(), _pEncoder));
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE, false);
	return _pSender->packet.data();
}

void BandWriter::flush(bool echoTime, UInt8 marker) {

	if (!_pSender)
		return;
	if (!failed() && _pSender->available()) {
		BinaryWriter& packet(_pSender->packet);

		// After 30 sec, send packet without echo time
		if (_lastReceptionTime.isElapsed(30000))
			echoTime = false;

		if (echoTime)
			marker += 4;
		else
			packet.clip(2);

		BinaryWriter writer(packet.data() + 6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		if (echoTime)
			writer.write16(_timeReceived + RTMFP::Time(_lastReceptionTime.elapsed()));

		_pSender->farId = _farId;
		_pSender->address.set(_address); // set the right address for sending

		if (packet.size() > RTMFP_MAX_PACKET_SIZE)
			ERROR(Exception::PROTOCOL, "Message exceeds max RTMFP packet size on connection (", packet.size(), ">", RTMFP_MAX_PACKET_SIZE, ")");

		// executed just in debug mode, or in dump mode
		if (Logs::GetLevel() >= 7)
			DUMP("RTMFP", packet.data() + 6, packet.size() - 6, "Response to ", _address.toString(), " (farId : ", _farId, ")")

		Exception ex;
		_pThread = socket(_address.family()).send<RTMFPSender>(ex, _pSender, _pThread);

		if (ex)
			ERROR("RTMFP flush, ", ex.error());
	}
	_pSender.reset();
}

bool BandWriter::decode(const SocketAddress& address, PoolBuffer& pBuffer) {
	// Decode the RTMFP data
	if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
		ERROR("Invalid RTMFP packet on connection to ", _address.toString())
		return false;
	}

#if defined(_DEBUG)
	Buffer copy(pBuffer.size());
	memcpy(copy.data(), pBuffer.data(), pBuffer.size());
#endif
	if (!_pDecoder->process(BIN pBuffer.data(), pBuffer.size())) {
		WARN("Bad RTMFP CRC sum computing (address : ", address.toString(), ", session : ", name(),")")
#if defined(_DEBUG)
		DUMP("RTMFP", copy.data(), copy.size(), "Raw request : ")
#endif
		return false;
	}
	return true;
}
