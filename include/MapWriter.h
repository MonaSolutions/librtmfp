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
#include "DataWriter.h"
#include "Mona/Logs.h"

template<typename MapType>
struct MapWriter : DataWriter, virtual Mona::Object {

	MapWriter(MapType& map) : _layers({ { 0,0 } }), _map(map), _isProperty(false) {}

	Mona::UInt64 beginObject(const char* type = NULL) { return beginComplex(); }
	void   writePropertyName(const char* value) { _property.append(value); _isProperty = true; }
	void   endObject() { endComplex(); }

	void  clear() { _isProperty = false; _property.clear(); _key.clear(); _layers.assign({ { 0,0 } }); _map.clear(); }

	Mona::UInt64 beginArray(Mona::UInt32 size) { return beginComplex(); }
	void   endArray() { endComplex(); }

	Mona::UInt64 beginObjectArray(Mona::UInt32 size) { beginComplex(); beginComplex(true); return 0; }

	void writeString(const char* value, Mona::UInt32 size) { set(value, size); }
	void writeNumber(double value) { set(Mona::String(value)); }
	void writeBoolean(bool value) { set(value ? "true" : "false"); }
	void writeNull() { set(EXPAND("null")); }
	Mona::UInt64 writeDate(const Mona::Date& date) { set(Mona::String(date)); return 0; }
	Mona::UInt64 writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { set(STR data, size); return 0; }

private:
	Mona::UInt64 beginComplex(bool ignore = false) {
		_layers.emplace_back(_key.size(), 0);
		if (ignore || _layers.size()<3)
			return 0;
		if (_isProperty) {
			Mona::String::Append(_key, _property, '.');
			_isProperty = false;
		}
		else
			Mona::String::Append(_key, (++_layers.rbegin())->second++, '.');
		_property = _key;
		return 0;
	}

	void endComplex() {
		if (_layers.empty()) {
			ERROR("endComplex called without beginComplex calling");
			return;
		}
		_key.resize(_layers.back().first);
		_property = _key;
		_layers.pop_back();
	}

	template <typename ...Args>
	void set(Args&&... args) {
		if (!_isProperty)
			Mona::String::Append(_property, _layers.back().second++);
		_map.emplace(std::piecewise_construct, std::forward_as_tuple(_property), std::forward_as_tuple(std::forward<Args>(args)...));
		_isProperty = false;
		_property = _key;
	}

	MapType&											_map;
	std::string											_property;
	bool												_isProperty;
	std::vector<std::pair<Mona::UInt16, Mona::UInt16>>	_layers; // keySize + index
	std::string											_key;
};