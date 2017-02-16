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

#include "Decoder.h"
#include "Mona/Crypto.h"

using namespace std;
using namespace Mona;

UInt32 RTMFPDecoder::decoding(Exception& ex, UInt8* data,UInt32 size) {
	// Decrypt
	_pDecoder->process(data, size);
	// Check CRC
	BinaryReader reader(data, size);
	UInt16 crc(reader.read16());
	if (Crypto::ComputeCRC(reader) == crc)
		receive(reader.current(),reader.available());
	else
		ex.set(Exception::CRYPTO, "Bad RTMFP CRC sum computing");	
	return size;
}

void RTMFPDecoder::resetDecoder(const Mona::UInt8* decryptKey) {
	_pDecoder.reset(new RTMFPEngine(decryptKey, RTMFPEngine::DECRYPT));
}
