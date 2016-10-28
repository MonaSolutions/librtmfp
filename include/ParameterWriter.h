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

#include "Mona/Mona.h"
#include "Mona/Parameters.h"
#include "DataWriter.h"
#include "DataReader.h"


class ParameterWriter : public DataWriter, public virtual Mona::Object {
public:

	ParameterWriter(Mona::Parameters& parameters) : _index(0),_parameters(parameters),_isProperty(false) {}
	Mona::UInt64 beginObject(const char* type = NULL) { return 0; }
	void   endObject() {}

	void writePropertyName(const char* value) { _property.assign(value); _isProperty=true; }

	Mona::UInt64 beginArray(Mona::UInt32 size) { return 0; }
	void   endArray(){}

	void writeNumber(double value) { std::string buffer;  set(Mona::String::Format(buffer, value)); }
	void writeString(const char* value, Mona::UInt32 size) { set(value, size); }
	void writeBoolean(bool value) { set(value ? "true" : "false");}
	void writeNull() { set("null",4); }
	Mona::UInt64 writeDate(const Mona::Date& date) { writeNumber((double)date); return 0; }
	Mona::UInt64 writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { set(STR data, size); return 0; }

	void   clear(Mona::UInt32 size=0) { _index = 0; _isProperty = false; _property.clear(); _parameters.clear(); }
private:
	
	template <typename ...Args>
	void set(Args&&... args) {
		if (!_isProperty)
			Mona::String::Format(_property, _index++);
		_parameters.setString(_property,args ...);
		_isProperty = false;
		_property.clear();
	}

	std::string					_property;
	bool						_isProperty;
	Mona::UInt32						_index;

	Mona::Parameters&					_parameters;
};
