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
#include "AMF.h"
#include "DataWriter.h"

struct AMFWriter : DataWriter, virtual Base::Object {
	AMFWriter(Base::Buffer& buffer, bool amf0 = false);

	bool repeat(Base::UInt64 reference);
	void clear();

	Base::UInt64 beginObject(const char* type = NULL);
	void   writePropertyName(const char* value);
	void   endObject() { endComplex(true); }

	Base::UInt64 beginArray(Base::UInt32 size);
	void   endArray() { endComplex(false); }

	Base::UInt64 beginObjectArray(Base::UInt32 size);

	Base::UInt64 beginMap(Base::Exception& ex, Base::UInt32 size, bool weakKeys = false);
	void   endMap() { endComplex(false); }

	void   writeNumber(double value);
	void   writeString(const char* value, Base::UInt32 size);
	void   writeBoolean(bool value);
	void   writeNull();
	Base::UInt64 writeDate(const Base::Date& date);
	Base::UInt64 writeBytes(const Base::UInt8* data, Base::UInt32 size);

	bool				amf0;

	static AMFWriter&    Null() { static AMFWriter Null; return Null; }

private:
	void endComplex(bool isObject);

	AMFWriter() : _amf3(false), amf0(false) {} // null version

	void writeText(const char* value, Base::UInt32 size);

	std::map<std::string, Base::UInt32>	_stringReferences;
	std::vector<Base::UInt8>			_references;
	Base::UInt32						_amf0References;
	bool								_amf3;
	std::vector<bool>					_levels; // true if amf3
};