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

#include "DataReader.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;

DataReaderNull  DataReader::Null;
DataWriterNull  DataWriter::Null;

DataReader::DataReader() : packet(PacketReader::Null),_pos(0),_nextType(END) {

}

DataReader::DataReader(PacketReader& packet): packet(packet),_pos(packet.position()),_nextType(END) {

}

bool DataReader::readNext(DataWriter& writer) {
	UInt8 type(nextType());
	_nextType = END; // to prevent recursive readNext call (and refresh followingType call)
	if(type!=END)
		return readOne(type, writer);
	return false;
}


UInt32 DataReader::read(DataWriter& writer, UInt32 count) {
	bool all(count == END);
	UInt32 results(0);
	UInt8 type(END);
	while ((all || count-- > 0) && readNext(writer))
		++results;
	return results;
}

bool DataReader::read(UInt8 type, DataWriter& writer) {
	if (nextType() != type)
		return false;
	UInt32 count(read(writer, 1));
	if (count>1) {
		WARN(typeid(*this).name(), " has written many object for just one reading of type ",type);
		return true;
	}
	return count==1;
}
