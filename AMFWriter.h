
#include "Mona/Mona.h"
#include "AMF.h"
#include "DataWriter.h"
#include <map>
#include <vector>

class AMFWriter : public DataWriter, public virtual Mona::Object {
public:
	AMFWriter(const Mona::PoolBuffers& poolBuffers);

	bool repeat(Mona::UInt64 reference);
	void clear(Mona::UInt32 size=0);

	Mona::UInt64 beginObject(const char* type=NULL);
	void   writePropertyName(const char* value);
	void   endObject() { endComplex(true); }

	Mona::UInt64 beginArray(Mona::UInt32 size);
	void   endArray() { endComplex(false); }

	Mona::UInt64 beginObjectArray(Mona::UInt32 size);

	Mona::UInt64 beginMap(Mona::Exception& ex, Mona::UInt32 size,bool weakKeys=false);
	void   endMap() { endComplex(false); }

	void   writeNumber(double value);
	void   writeString(const char* value, Mona::UInt32 size);
	void   writeBoolean(bool value);
	void   writeNull();
	Mona::UInt64 writeDate(const Mona::Date& date);
	Mona::UInt64 writeBytes(const Mona::UInt8* data,Mona::UInt32 size);
	
	bool				amf0;

	static AMFWriter    Null;

private:
	void endComplex(bool isObject);

	AMFWriter() : _amf3(false), amf0(false) {} // null version

	void writeText(const char* value,Mona::UInt32 size);

	std::map<std::string,Mona::UInt32>	_stringReferences;
	std::vector<Mona::UInt8>			_references;
	Mona::UInt32						_amf0References;
	bool							_amf3;
	std::vector<bool>				_levels; // true if amf3
};


