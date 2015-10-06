
#include "Mona/Mona.h"
#include "Mona/PacketReader.h"
#include "DataWriter.h"

#pragma once

class DataReaderNull;

class DataReader : public virtual Mona::NullableObject {
public:
	enum {
		END=0, // keep equal to 0!
		NIL,
		BOOLEAN,
		NUMBER,
		STRING,
		DATE,
		BYTES,
		OTHER
	};

	void			next() { read(DataWriter::Null,1); }
	Mona::UInt8			nextType() { if (_nextType == END) _nextType = followingType(); return _nextType; }

	// return the number of writing success on writer object
	// can be override to capture many reading on the same writer
	virtual Mona::UInt32	read(DataWriter& writer,Mona::UInt32 count=END);

	bool			read(Mona::UInt8 type, DataWriter& writer);
	bool			available() { return nextType()!=END; }

////  OPTIONAL DEFINE ////
	virtual void	reset() { packet.reset(_pos); }
////////////////////


	bool			readString(std::string& value) { return read(STRING,wrapper(&value)); }
	bool			readNumber(double& number) {  return read(NUMBER,wrapper(&number)); }
	bool			readBoolean(bool& value) {  return read(BOOLEAN,wrapper(&value)); }
	bool			readDate(Mona::Date& date)  {  return read(DATE,wrapper(&date)); }
	bool			readNull() { return read(NIL,wrapper(NULL)); }
	template <typename BufferType>
	bool			readBytes(BufferType& buffer) { BytesWriter<BufferType> writer(buffer); return read(BYTES, writer); }

	operator bool() const { return packet; }

	Mona::PacketReader&				packet;

	static DataReaderNull		Null;

protected:
	DataReader(Mona::PacketReader& packet);
	DataReader(); // Null


	bool			readNext(DataWriter& writer);

private:
	
////  TO DEFINE ////
	// must return true if something has been written in DataWriter object (so if DataReader has always something to read and write, !=END)
	virtual bool	readOne(Mona::UInt8 type, DataWriter& writer) = 0;
	virtual Mona::UInt8	followingType()=0;
////////////////////

	Mona::UInt32						_pos;
	Mona::UInt8						_nextType;
	
	class WrapperWriter : public DataWriter {
	public:
		Mona::UInt64	beginObject(const char* type) { return 0; }
		void	writePropertyName(const char* value) {}
		void	endObject() {}
		
		Mona::UInt64	beginArray(Mona::UInt32 size) { return 0; }
		void	endArray() {}

		Mona::UInt64	writeDate(const Mona::Date& date) { if(pData) ((Mona::Date*)pData)->update(date); return 0; }
		void	writeNumber(double value) { if(pData) *((double*)pData) = value; }
		void	writeString(const char* value, Mona::UInt32 size) { if(pData)  ((std::string*)pData)->assign(value,size); }
		void	writeBoolean(bool value) { if(pData)  *((bool*)pData) = value; }
		void	writeNull() {}
		Mona::UInt64	writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { return 0; }

		void*	pData;
	};

	template<typename BufferType>
	class BytesWriter : public DataWriter {
	public:
		BytesWriter(BufferType& buffer) : _buffer(buffer) {}
		Mona::UInt64	beginObject(const char* type, Mona::UInt32 size) { return 0; }
		void	writePropertyName(const char* value) {}
		void	endObject() {}
		
		Mona::UInt64	beginArray(Mona::UInt32 size) { return 0; }
		void	endArray() {}

		Mona::UInt64	writeDate(const Mona::Date& date) { Mona::Int64 time(date.time()); _buffer.write(&time, sizeof(time)); return 0; }
		void	writeNumber(double value) { _buffer.write(&value, sizeof(value)); }
		void	writeString(const char* value, Mona::UInt32 size) { _buffer.write(value, size); }
		void	writeBoolean(bool value) { _buffer.write(&value, sizeof(value)); }
		void	writeNull() {}
		Mona::UInt64	writeBytes(const Mona::UInt8* data, Mona::UInt32 size) { _buffer.write(data, size); return 0; }
	private:
		BufferType&  _buffer;
	};

	DataWriter&					wrapper(void* pData) { _wrapper.pData = pData; return _wrapper; }
	WrapperWriter				_wrapper;
};

class DataReaderNull : public DataReader {
public:
	DataReaderNull() {}

private:
	bool	readOne(Mona::UInt8 type, DataWriter& writer) { return false; }
	Mona::UInt8	followingType() { return END; }
};
