#include "ReferableReader.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;

UInt32 ReferableReader::read(DataWriter& writer, UInt32 count) {
	if (_recursive)
		return DataReader::read(writer, count);
	// start read (new writer!)
	_recursive = true;
	UInt32 result(DataReader::read(writer,count));
	for (auto& it : _references) {
		if (it.second.level>0)
			WARN(typeid(*this).name()," has open some complex objects withoiut closing them")
	}
	_references.clear();
	_recursive = false;
	return result;
} 

ReferableReader::Reference* ReferableReader::beginRepeatable(UInt64 readerRef, UInt64 writerRef) {
	if (!readerRef)
		return NULL;
	Reference& reference(_references[readerRef]);
	reference.value  = writerRef;
	reference.level = 1;
	return &reference;
}

ReferableReader::Reference*	ReferableReader::beginObjectArray(DataWriter& writer, UInt64 readerRef, UInt32 size){
	if (!readerRef) {
		writer.beginObjectArray(size);
		return NULL;
	}
	Reference& reference(_references[readerRef]);
	reference.value  = writer.beginObjectArray(size);
	reference.level = 2;
	return &reference;
}

void ReferableReader::writeRepeatable(UInt64 readerRef, UInt64 writerRef) {
	if (!readerRef)
		return;
	Reference& reference(_references[readerRef]);
	reference.value  = writerRef;
	reference.level = 0;
}

bool ReferableReader::tryToRepeat(DataWriter& writer, UInt64 reference) {
	if (reference == 0) {
		ERROR(typeid(*this).name()," reference can't be null");
		writer.writeNull();
		return true;
	}
	auto it(_references.find(reference));
	if (it == _references.end())
		return false;
	if (it->second.value > 0 && writer.repeat(it->second.value))
		return true;
	if (it->second.level) {
		ERROR("Impossible to repeat ",typeid(*this).name()," reference, ", typeid(writer).name(), " doesn't support fully cyclic referencing")
		writer.writeNull();
		return true;
	}
	return false;
}

bool ReferableReader::writeReference(DataWriter& writer, UInt64 reference) {
	if (reference == 0) {
		ERROR(typeid(*this).name()," reference can't be null");
		writer.writeNull();
		return true;
	}
	auto it(_references.find(reference));
	if (it == _references.end()) {
		ERROR(typeid(*this).name()," reference ",reference," unfound");
		writer.writeNull();
		return true;
	}
	if (it->second.value > 0 && writer.repeat(it->second.value))
		return true;
	if (it->second.level) {
		ERROR("Impossible to repeat ",typeid(*this).name()," reference, ", typeid(writer).name(), " doesn't support fully cyclic referencing")
		writer.writeNull();
		return true;
	}
	return false;
}
