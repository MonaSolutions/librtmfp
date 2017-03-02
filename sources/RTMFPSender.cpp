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

#include "RTMFPSender.h"
#include "Mona/Crypto.h"

using namespace Mona;

bool RTMFPSender::run(Exception& ex) {
	int paddingBytesLength = (0xFFFFFFFF-_pBuffer->size()+5)&0x0F;
	// Padd the plain request with paddingBytesLength of value 0xff at the end
	while (paddingBytesLength-->0)
		write8(0xFF);
	// Write CRC (at the beginning of the request)
	BinaryReader reader(_pBuffer->data()+6, _pBuffer->size()-6);
	BinaryWriter(_pBuffer->data()+4,2).write16(Crypto::ComputeChecksum(reader));
	// Encrypt the resulted request
	_pEncoder->process(_pBuffer->data()+4, _pBuffer->size()-4);
	RTMFP::Pack(*_pBuffer,farId);

	if (_pSocket->write(ex, Packet(_pBuffer), address) < 0 || ex)
		DEBUG(ex);
	return true;
}