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

#include "Base/Mona.h"
#include "DataReader.h"

using namespace Base;

struct ReferableReader : DataReader {
	UInt32	read(DataWriter& writer, UInt32 count = END);
	bool	read(UInt8 type, DataWriter& writer) { return DataReader::read(type, writer); }
	void	reset() { _references.clear(); DataReader::reset(); }

protected:
	struct Reference {
		friend struct ReferableReader;
	private:
		UInt64		value;
		UInt8		level;
	};

	ReferableReader(const Packet& packet = Packet::Null(), UInt8 type = END) : DataReader(packet, type), _recursive(false) {}

	Reference*	beginObject(DataWriter& writer, UInt64 reference, const char* type = NULL) { return beginRepeatable(reference, writer.beginObject(type)); }
	Reference*	beginArray(DataWriter& writer, UInt64 reference, UInt32 size) { return beginRepeatable(reference, writer.beginArray(size)); }
	Reference*	beginObjectArray(DataWriter& writer, UInt64 reference, UInt32 size);
	Reference*	beginMap(DataWriter& writer, UInt64 reference, Exception& ex, UInt32 size, bool weakKeys = false) { return beginRepeatable(reference, writer.beginMap(ex, size, weakKeys)); }

	void		endObject(DataWriter& writer, Reference* pReference) { writer.endObject();  endRepeatable(pReference); }
	void		endArray(DataWriter& writer, Reference* pReference) { writer.endArray();  endRepeatable(pReference); }
	void		endMap(DataWriter& writer, Reference* pReference) { writer.endMap();  endRepeatable(pReference); }

	void		writeDate(DataWriter& writer, UInt64 reference, const Date& date) { writeRepeatable(reference, writer.writeDate(date)); }
	void		writeByte(DataWriter& writer, UInt64 reference, const Packet& bytes) { writeRepeatable(reference, writer.writeByte(bytes)); }

	bool		writeReference(DataWriter& writer, UInt64 reference);
	bool		tryToRepeat(DataWriter& writer, UInt64 reference);

private:
	Reference*  beginRepeatable(UInt64 readerRef, UInt64 writerRef);
	void		endRepeatable(Reference* pReference) { if (pReference) --pReference->level; }
	void		writeRepeatable(UInt64 readerRef, UInt64 writerRef);

	std::map<UInt64, Reference> _references;
	bool						_recursive;
};
