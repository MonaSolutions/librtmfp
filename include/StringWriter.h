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

#include "Mona/Mona.h"
#include "DataWriter.h"

class StringWriter : public DataWriter, public virtual Mona::Object {
public:

	StringWriter(const Mona::PoolBuffers& poolBuffers) : _pString(NULL),DataWriter(poolBuffers) {}
	StringWriter(std::string& buffer) : _pString(&buffer) {}

	Mona::UInt64 beginObject(const char* type = NULL) { return 0; }
	void   endObject() {}

	void   writePropertyName(const char* name) { append(name);  }

	Mona::UInt64 beginArray(Mona::UInt32 size) { return 0; }
	void   endArray(){}

	void   writeNumber(double value) { append(value); }
	void   writeString(const char* value, Mona::UInt32 size) { append(value,size); }
	void   writeBoolean(bool value) { append( value ? "true" : "false"); }
	void   writeNull() { packet.write("null",4); }
	Mona::UInt64 writeDate(const Mona::Date& date) { std::string buffer; append(date.toString(Mona::Date::SORTABLE_FORMAT, buffer)); return 0; }
	Mona::UInt64 writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { append(data, size); return 0; }

	void   clear(Mona::UInt32 size = 0) { if (_pString) _pString->erase(size); else packet.clear(size); }
private:
	void append(const void* value, Mona::UInt32 size) {
		if (_pString)
			_pString->append(STR value, size);
		else
			packet.write(value, size);
	}

	template<typename ValueType>
	void append(const ValueType& value) {
		if (_pString)
			Mona::String::Append(*_pString, value);
		else
			Mona::String::Append(packet, value);
	}

	std::string* _pString;

};

