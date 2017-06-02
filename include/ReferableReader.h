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

struct ReferableReader : DataReader {
	Base::UInt32	read(DataWriter& writer, Base::UInt32 count = END);
	bool	read(Base::UInt8 type, DataWriter& writer) { return DataReader::read(type, writer); }
	void	reset() { _references.clear(); DataReader::reset(); }

protected:
	struct Reference {
		friend struct ReferableReader;
	private:
		Base::UInt64	value;
		Base::UInt8		level;
	};

	ReferableReader(const Base::UInt8* data, Base::UInt32 size) : DataReader(data, size), _recursive(false) {}
	ReferableReader() : DataReader() {}

	Reference*	beginObject(DataWriter& writer, Base::UInt64 reference, const char* type = NULL) { return beginRepeatable(reference, writer.beginObject(type)); }
	Reference*	beginArray(DataWriter& writer, Base::UInt64 reference, Base::UInt32 size) { return beginRepeatable(reference, writer.beginArray(size)); }
	Reference*	beginObjectArray(DataWriter& writer, Base::UInt64 reference, Base::UInt32 size);
	Reference*	beginMap(DataWriter& writer, Base::UInt64 reference, Base::Exception& ex, Base::UInt32 size, bool weakKeys = false) { return beginRepeatable(reference, writer.beginMap(ex, size, weakKeys)); }

	void		endObject(DataWriter& writer, Reference* pReference) { writer.endObject();  endRepeatable(pReference); }
	void		endArray(DataWriter& writer, Reference* pReference) { writer.endArray();  endRepeatable(pReference); }
	void		endMap(DataWriter& writer, Reference* pReference) { writer.endMap();  endRepeatable(pReference); }

	void		writeDate(DataWriter& writer, Base::UInt64 reference, const Base::Date& date) { writeRepeatable(reference, writer.writeDate(date)); }
	void		writeBytes(DataWriter& writer, Base::UInt64 reference, const Base::UInt8* data, Base::UInt32 size) { writeRepeatable(reference, writer.writeBytes(data, size)); }

	bool		writeReference(DataWriter& writer, Base::UInt64 reference);
	bool		tryToRepeat(DataWriter& writer, Base::UInt64 reference);

private:
	Reference*  beginRepeatable(Base::UInt64 readerRef, Base::UInt64 writerRef);
	void		endRepeatable(Reference* pReference) { if (pReference) --pReference->level; }
	void		writeRepeatable(Base::UInt64 readerRef, Base::UInt64 writerRef);

	std::map<Base::UInt64, Reference>	_references;
	bool								_recursive;
};
