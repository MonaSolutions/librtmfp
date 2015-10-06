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
			String::Append(*_pString, value);
		else
			String::Append(packet, value);
	}

	std::string* _pString;

};

