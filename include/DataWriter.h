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
#include "Base/BinaryWriter.h"
#include "Base/Date.h"
#include "Base/Exceptions.h"

struct DataWriter : virtual Base::NullableObject {
	////  TO DEFINE ////
	virtual Base::UInt64 beginObject(const char* type = NULL) = 0;
	virtual void   writePropertyName(const char* value) = 0;
	virtual void   endObject() = 0;

	virtual Base::UInt64 beginArray(Base::UInt32 size) = 0;
	virtual void   endArray() = 0;

	virtual void   writeNumber(double value) = 0;
	virtual void   writeString(const char* value, Base::UInt32 size) = 0;
	virtual void   writeBoolean(bool value) = 0;
	virtual void   writeNull() = 0;
	virtual Base::UInt64 writeDate(const Base::Date& date) = 0;
	virtual Base::UInt64 writeBytes(const Base::UInt8* data, Base::UInt32 size) = 0;
	////////////////////


	////  OPTIONAL DEFINE ////
	// if serializer don't support a mixed object, set the object as the first element of the array
	virtual Base::UInt64 beginObjectArray(Base::UInt32 size) { Base::UInt64 ref(beginArray(size + 1)); beginObject(); return ref; }

	virtual Base::UInt64 beginMap(Base::Exception& ex, Base::UInt32 size, bool weakKeys = false) { ex.set<Base::Ex::Format>(typeof(*this), " doesn't support map type, a object will be written rather");  return beginObject(); }
	virtual void   endMap() { endObject(); }

	virtual void   clear() { writer.clear(); }
	virtual bool   repeat(Base::UInt64 reference) { return false; }

	////////////////////

	void		   writeNullProperty(const char* name) { writePropertyName(name); writeNull(); }
	void		   writeDateProperty(const char* name, const Base::Date& date) { writePropertyName(name); writeDate(date); }
	void		   writeNumberProperty(const char* name, double value) { writePropertyName(name); writeNumber(value); }
	void		   writeBooleanProperty(const char* name, bool value) { writePropertyName(name); writeBoolean(value); }
	void		   writeStringProperty(const char* name, const char* value, std::size_t size = std::string::npos) { writePropertyName(name); writeString(value, size == std::string::npos ? strlen(value) : size); }
	void		   writeStringProperty(const char* name, const std::string& value) { writePropertyName(name); writeString(value.data(), value.size()); }

	operator bool() const { return writer.operator bool(); }

	Base::BinaryWriter*		operator->() { return &writer; }
	const Base::BinaryWriter*	operator->() const { return &writer; }
	Base::BinaryWriter&		operator*() { return writer; }
	const Base::BinaryWriter&	operator*() const { return writer; }

	static DataWriter& Null();
protected:
	DataWriter(Base::Buffer& buffer) : writer(buffer, Base::Byte::ORDER_NETWORK) {}
	DataWriter() : writer(Base::Buffer::Null()) {}

	Base::BinaryWriter   writer;
};
