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
#include "Mona/BinaryReader.h"
#include "DataWriter.h"

struct DataReader : virtual Mona::NullableObject {
	enum {
		END = 0, // keep equal to 0!
		NIL,
		BOOLEAN,
		NUMBER,
		STRING,
		DATE,
		BYTES,
		OTHER
	};

	void			next(Mona::UInt32 count = 1) { read(DataWriter::Null(), count); }
	Mona::UInt8		nextType() { if (_nextType == END) _nextType = followingType(); return _nextType; }

	// return the number of writing success on writer object
	// can be override to capture many reading on the same writer
	virtual Mona::UInt32	read(DataWriter& writer, Mona::UInt32 count = END);

	bool			read(Mona::UInt8 type, DataWriter& writer);
	bool			available() { return nextType() != END; }

	////  OPTIONAL DEFINE ////
	virtual void	reset() { reader.reset(); }
	////////////////////

	template <typename BufferType>
	bool			readString(BufferType& buffer) { BufferWriter<BufferType> writer(buffer); return read(STRING, writer); }
	template <typename NumberType>
	bool			readNumber(NumberType& value) { NumberWriter<NumberType> writer(value); return read(NUMBER, writer); }
	bool			readBoolean(bool& value) { BoolWriter writer(value); return read(BOOLEAN, writer); }
	bool			readDate(Mona::Date& date) { DateWriter writer(date); return read(DATE, writer); }
	bool			readNull() { return read(NIL, DataWriter::Null()); }
	template <typename BufferType>
	bool			readBytes(BufferType& buffer) { BufferWriter<BufferType> writer(buffer); return read(BYTES, writer); }

	operator bool() const { return reader.operator bool(); }
	Mona::BinaryReader*		operator->() { return &reader; }
	const Mona::BinaryReader*	operator->() const { return &reader; }
	Mona::BinaryReader&		operator*() { return reader; }
	const Mona::BinaryReader&	operator*() const { return reader; }

	static DataReader&	Null();

protected:
	DataReader(const Mona::UInt8* data, Mona::UInt32 size) : reader(data, size, Mona::Byte::ORDER_NETWORK), _nextType(END) {}
	DataReader() : reader(NULL, 0, Mona::Byte::ORDER_NETWORK), _nextType(END) {}

	Mona::BinaryReader	reader;

	bool			readNext(DataWriter& writer);

private:

	////  TO DEFINE ////
	// must return true if something has been written in DataWriter object (so if DataReader has always something to read and write, !=END)
	virtual bool		readOne(Mona::UInt8 type, DataWriter& writer) = 0;
	virtual Mona::UInt8	followingType() = 0;
	////////////////////

	Mona::UInt8			_nextType;

	template<typename NumberType>
	struct NumberWriter : DataWriter {
		NumberWriter(NumberType& value) : _value(value) {}
		Mona::UInt64	beginObject(const char* type) { return 0; }
		void	writePropertyName(const char* value) {}
		void	endObject() {}

		Mona::UInt64	beginArray(Mona::UInt32 size) { return 0; }
		void	endArray() {}

		Mona::UInt64	writeDate(const Mona::Date& date) { _value = (NumberType)date.time(); return 0; }
		void	writeNumber(double value) { _value = (NumberType)value; }
		void	writeString(const char* value, Mona::UInt32 size) { Mona::String::ToNumber(value, size, _value); }
		void	writeBoolean(bool value) { _value = (value ? 1 : 0); }
		void	writeNull() { _value = 0; }
		Mona::UInt64	writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { writeString(STR data, size); return 0; }
	private:
		NumberType&	_value;
	};
	struct DateWriter : DataWriter {
		DateWriter(Mona::Date& date) : _date(date) {}
		Mona::UInt64	beginObject(const char* type) { return 0; }
		void	writePropertyName(const char* value) {}
		void	endObject() {}

		Mona::UInt64	beginArray(Mona::UInt32 size) { return 0; }
		void	endArray() {}

		Mona::UInt64	writeDate(const Mona::Date& date) { _date = date; return 0; }
		void	writeNumber(double value) { _date = (Mona::Int64)value; }
		void	writeString(const char* value, Mona::UInt32 size) { Mona::Exception ex; _date.update(ex, value, size); }
		void	writeBoolean(bool value) { _date = (value ? Mona::Time::Now() : 0); }
		void	writeNull() { _date = 0; }
		Mona::UInt64	writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { writeString(STR data, size); return 0; }

	private:
		Mona::Date&	_date;
	};
	struct BoolWriter : DataWriter {
		BoolWriter(bool& value) : _value(value) {}
		Mona::UInt64	beginObject(const char* type) { return 0; }
		void	writePropertyName(const char* value) {}
		void	endObject() {}

		Mona::UInt64	beginArray(Mona::UInt32 size) { return 0; }
		void	endArray() {}

		Mona::UInt64	writeDate(const Mona::Date& date) { _value = date ? true : false; return 0; }
		void	writeNumber(double value) { _value = value ? true : false; }
		void	writeString(const char* value, Mona::UInt32 size) { _value = !Mona::String::IsFalse(value, size); }
		void	writeBoolean(bool value) { _value = value; }
		void	writeNull() { _value = false; }
		Mona::UInt64	writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { writeString(STR data, size); return 0; }

	private:
		bool&	_value;
	};

	template<typename BufferType>
	struct BufferWriter : DataWriter {
		BufferWriter(BufferType& buffer) : _buffer(buffer) { }
		Mona::UInt64	beginObject(const char* type) { return 0; }
		void	writePropertyName(const char* value) {}
		void	endObject() {}

		Mona::UInt64	beginArray(Mona::UInt32 size) { return 0; }
		void	endArray() {}

		Mona::UInt64	writeDate(const Mona::Date& date) { Mona::String::Assign(_buffer, date.time()); return 0; }
		void	writeNumber(double value) { Mona::String::Assign(_buffer, value); }
		void	writeString(const char* value, Mona::UInt32 size) { _buffer.clear(); _buffer.append(value, size); }
		void	writeBoolean(bool value) { Mona::String::Assign(_buffer, value); }
		void	writeNull() { writeString(EXPAND("null")); }
		Mona::UInt64	writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { writeString(STR data, size); return 0; }
	private:
		BufferType&  _buffer;
	};
};
