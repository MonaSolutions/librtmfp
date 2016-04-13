
#pragma once

#include "Mona/Mona.h"
#include "Mona/PacketWriter.h"
#include "Mona/Date.h"

class DataWriterNull;

class DataWriter : virtual public Mona::NullableObject {
public:
////  TO DEFINE ////
	virtual Mona::UInt64 beginObject(const char* type=NULL)=0;
	virtual void   writePropertyName(const char* value)=0;
	virtual void   endObject()=0;

	virtual Mona::UInt64 beginArray(Mona::UInt32 size)=0;
	virtual void   endArray()=0;

	virtual void   writeNumber(double value)=0;
	virtual void   writeString(const char* value, Mona::UInt32 size)=0;
	virtual void   writeBoolean(bool value)=0;
	virtual void   writeNull()=0;
	virtual Mona::UInt64 writeDate(const Mona::Date& date)=0;
	virtual Mona::UInt64 writeBytes(const Mona::UInt8* data, Mona::UInt32 size)=0;
////////////////////


////  OPTIONAL DEFINE ////
	// if serializer don't support a mixed object, set the object as the first element of the array
	virtual Mona::UInt64 beginObjectArray(Mona::UInt32 size) { Mona::UInt64 ref(beginArray(size+1)); beginObject(); return ref; }

	virtual Mona::UInt64 beginMap(Mona::Exception& ex, Mona::UInt32 size, bool weakKeys = false) { ex.set(Mona::Exception::FORMATTING,typeid(*this).name()," doesn't support map type, a object will be written rather");  return beginObject(); }
	virtual void   endMap() { endObject(); }

	virtual void   clear(Mona::UInt32 size=0) { packet.clear(size); }
	virtual bool   repeat(Mona::UInt64 reference) { return false; }

////////////////////

	void		   writeNullProperty(const char* name) { writePropertyName(name); writeNull(); }
	void		   writeDateProperty(const char* name,const Mona::Date& date) { writePropertyName(name); writeDate(date); }
	void		   writeNumberProperty(const char* name,double value) { writePropertyName(name); writeNumber(value); }
	void		   writeBooleanProperty(const char* name,bool value) { writePropertyName(name); writeBoolean(value); }
	void		   writeStringProperty(const char* name, const char* value, std::size_t size = std::string::npos) { writePropertyName(name); writeString(value, size==std::string::npos ? strlen(value) : size); }
	void		   writeStringProperty(const char* name,const std::string& value) { writePropertyName(name); writeString(value.data(), value.size()); }

	Mona::PacketWriter   packet;

	operator bool() const { return packet; }

    static DataWriterNull Null;
protected:
	DataWriter(const Mona::PoolBuffers& poolBuffers): packet(poolBuffers) {}
	DataWriter() : packet(Mona::Buffer::Null) {} // Null
};


class DataWriterNull : public DataWriter {
public:
	DataWriterNull() {}

	Mona::UInt64 beginObject(const char* type = NULL) { return 0;  }
	void writePropertyName(const char* value){}
    void endObject(){}

    Mona::UInt64 beginArray(Mona::UInt32 size){ return 0; }
    void endArray(){}
   
    void   writeNumber(double value){}
    void   writeString(const char* value, Mona::UInt32 size){}
    void   writeBoolean(bool value){}
    void   writeNull(){}
	Mona::UInt64 writeDate(const Mona::Date& date){ return 0; }
    Mona::UInt64 writeBytes(const Mona::UInt8* data,Mona::UInt32 size){ return 0;}
};

