
#pragma once

#include "Mona/Mona.h"
#include "Mona/PacketWriter.h"

class RTMFPWriter;
class BandWriter : public virtual Mona::Object {
public:
	BandWriter(): connected(false) {}

	virtual const Mona::PoolBuffers&		poolBuffers() = 0;
	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter)=0;
	//virtual std::shared_ptr<RTMFPWriter>	changeWriter(RTMFPWriter& writer) = 0;

	virtual bool						failed() const = 0;
	virtual bool						canWriteFollowing(RTMFPWriter& writer)=0;
	virtual Mona::UInt32				availableToWrite()=0;
	virtual Mona::BinaryWriter&			writeMessage(Mona::UInt8 type,Mona::UInt16 length,RTMFPWriter* pWriter=NULL)=0;
	virtual void						flush()=0;
	//virtual Mona::UInt16				ping() const = 0;

	bool								connected;
	
};
