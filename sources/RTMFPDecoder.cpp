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

#include "RTMFPDecoder.h"
#include "Mona/Crypto.h"

using namespace std;
using namespace Mona;

bool RTMFPDecoder::run(Exception& ex) {
	// Decrypt
	_pDecoder->process(_pBuffer->data(), _pBuffer->size());
	// Check CRC
	BinaryReader reader(_pBuffer->data(), _pBuffer->size());
	UInt16 crc(reader.read16());
	if (Crypto::ComputeChecksum(reader) == crc) {
		_pBuffer->clip(reader.position());
		_handler.queue(onDecoded, _idSession, _address, _pBuffer);
		return true;
	}
	ex.set<Ex::Extern::Crypto>("Bad RTMFP CRC sum computing");
	return false;
}
