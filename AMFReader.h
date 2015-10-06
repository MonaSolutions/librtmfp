#include "Mona/Mona.h"
#include "AMF.h"
#include "ReferableReader.h"
#include <vector>

#pragma once

class AMFReader : public ReferableReader, public virtual Mona::Object {
public:
	AMFReader(Mona::PacketReader& reader);

	enum {
		OBJECT =	OTHER,
		ARRAY =		OTHER+1,
		MAP =		OTHER+2,
		AMF0_REF =	OTHER+3
	};


	void			startReferencing() { _referencing = true; }
	void			stopReferencing() { _referencing = false; }

	void			reset();

private:

	Mona::UInt8			followingType();

	bool			readOne(Mona::UInt8 type, DataWriter& writer);
	bool			writeOne(Mona::UInt8 type, DataWriter& writer);

	const char*		readText(Mona::UInt32& size,bool nullIfEmpty=false);

	std::vector<Mona::UInt32>		_stringReferences;
	std::vector<Mona::UInt32>		_classDefReferences;
	std::vector<Mona::UInt32>		_references;
	std::vector<Mona::UInt32>		_amf0References;

	Mona::UInt8						_amf3;
	bool							_referencing;

};
