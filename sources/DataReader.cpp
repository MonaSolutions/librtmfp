
#include "DataReader.h"
#include "Mona/Logs.h"

using namespace std;
using namespace Mona;

DataReaderNull  DataReader::Null;
DataWriterNull  DataWriter::Null;

DataReader::DataReader() : packet(PacketReader::Null),_pos(0),_nextType(END) {

}

DataReader::DataReader(PacketReader& packet): packet(packet),_pos(packet.position()),_nextType(END) {

}

bool DataReader::readNext(DataWriter& writer) {
	UInt8 type(nextType());
	_nextType = END; // to prevent recursive readNext call (and refresh followingType call)
	if(type!=END)
		return readOne(type, writer);
	return false;
}


UInt32 DataReader::read(DataWriter& writer, UInt32 count) {
	bool all(count == END);
	UInt32 results(0);
	UInt8 type(END);
	while ((all || count-- > 0) && readNext(writer))
		++results;
	return results;
}

bool DataReader::read(UInt8 type, DataWriter& writer) {
	if (nextType() != type)
		return false;
	UInt32 count(read(writer, 1));
	if (count>1) {
		WARN(typeid(*this).name(), " has written many object for just one reading of type ",type);
		return true;
	}
	return count==1;
}
