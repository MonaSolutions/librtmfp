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

#include "RTMFPFlow.h"
#include "Mona/Util.h"
#include "Mona/PoolBuffer.h"
#include "RTMFPWriter.h"

using namespace std;
using namespace Mona;


class RTMFPPacket : public virtual Object {
public:
	RTMFPPacket(const PoolBuffers& poolBuffers,PacketReader& fragment) : fragments(1),_pMessage(NULL),_pBuffer(poolBuffers,fragment.available()) {
		if(_pBuffer->size()>0)
			memcpy(_pBuffer->data(),fragment.current(),_pBuffer->size());
	}
	~RTMFPPacket() {
		if(_pMessage)
			delete _pMessage;
	}

	void add(PacketReader& fragment) {
		_pBuffer->append(fragment.current(),fragment.available());
		++(UInt32&)fragments;
	}

	PacketReader* release() {
		if(_pMessage) {
			ERROR("RTMFPPacket already released!");
			return _pMessage;
		}
		_pMessage = new PacketReader(_pBuffer->size()==0 ? NULL : _pBuffer->data(),_pBuffer->size());
		return _pMessage;
	}

	const UInt32	fragments;

private:
	
	PoolBuffer		_pBuffer;
	PacketReader*  _pMessage;
};


class RTMFPFragment : public PoolBuffer, public virtual Object{
public:
	RTMFPFragment(const PoolBuffers& poolBuffers,PacketReader& packet,UInt8 flags) : flags(flags),PoolBuffer(poolBuffers,packet.available()) {
		packet.read((*this)->size(),(*this)->data());
	}
	UInt8					flags;
};


RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature,const PoolBuffers& poolBuffers, BandWriter& band, const shared_ptr<FlashConnection>& pMainStream, UInt64 idWriterRef) : _pStream(pMainStream),
	_poolBuffers(poolBuffers),_numberLostFragments(0),id(id),_writerRef(idWriterRef),_stage(0),_completed(false),_pPacket(NULL),_band(band) {

	DEBUG("New main flow ", id, " on connection ", band.name())
}

RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature,const shared_ptr<FlashStream>& pStream,const PoolBuffers& poolBuffers, BandWriter& band, UInt64 idWriterRef) : _pStream(pStream),_poolBuffers(poolBuffers),
	_numberLostFragments(0),id(id),_writerRef(idWriterRef),_stage(0),_completed(false),_pPacket(NULL),_band(band) {

	DEBUG("New flow ", id, " on connection ", band.name())
}


RTMFPFlow::~RTMFPFlow() {

	complete();
}

void RTMFPFlow::complete() {
	if(_completed)
		return;

	DEBUG("RTMFPFlow ",id," completed");

	// delete fragments
	_fragments.clear();

	// delete receive buffer
	if(_pPacket) {
		delete _pPacket;
		_pPacket=NULL;
	}

	_completed=true;
	_completeTime.update();
}

void RTMFPFlow::fail(const string& error) {
	ERROR("RTMFPFlow ",id," failed, ",error);
	close();
}

void RTMFPFlow::close() {
	if (_completed)
		return;
	BinaryWriter& writer = _band.writeMessage(0x5e, Util::Get7BitValueSize(id) + 1);
	writer.write7BitLongValue(id);
	writer.write8(0); // finishing marker
	//_band.flush();
}

void RTMFPFlow::commit() {

	// Lost informations!
	UInt32 size = 0;
	vector<UInt64> losts;
	UInt64 current=_stage;
	UInt32 count=0;
	auto it = _fragments.begin();
	while(it!=_fragments.end()) {
		current = it->first-current-2;
		size += Util::Get7BitValueSize(current);
		losts.emplace_back(current);
		current = it->first;
		while(++it!=_fragments.end() && it->first==(++current))
			++count;
		size += Util::Get7BitValueSize(count);
		losts.emplace_back(count);
		--current;
		count=0;
	}

	UInt32 bufferSize = _pPacket ? ((_pPacket->fragments>0x3F00) ? 0 : (0x3F00-_pPacket->fragments)) : 0x7F;
	BinaryWriter& ack = _band.writeMessage(0x51,Util::Get7BitValueSize(id)+Util::Get7BitValueSize(bufferSize)+Util::Get7BitValueSize(_stage)+size);

	ack.write7BitLongValue(id);
	ack.write7BitValue(bufferSize);
	ack.write7BitLongValue(_stage);

	for(UInt64 lost : losts)
		ack.write7BitLongValue(lost);

	_band.flush();
}

void RTMFPFlow::receive(UInt64 stage,UInt64 deltaNAck,PacketReader& fragment,UInt8 flags) {
	if(_completed)
		return;

	UInt64 nextStage = _stage+1;

	if(stage < nextStage) {
		DEBUG("Stage ",stage," on flow ",id," has already been received");
		return;
	}

	if(deltaNAck>stage) {
		WARN("DeltaNAck ",deltaNAck," superior to _stage ",stage," on flow ",id);
		deltaNAck=stage;
	}
	
	if(_stage < (stage-deltaNAck)) {
		auto it=_fragments.begin();
		while(it!=_fragments.end()) {
			if( it->first > stage) 
				break;
			// leave all stages <= _stage
			PacketReader packet(it->second->data(),it->second->size());
			onFragment(it->first,packet,it->second.flags);
			if(_completed || it->second.flags&MESSAGE_END) {
				complete();
				return; // to prevent a crash bug!! (double fragments deletion)
			}
			_fragments.erase(it++);
		}

		nextStage = stage;
	}
	
	if(stage>nextStage) {
		// not following _stage, bufferizes the _stage
		auto it = _fragments.lower_bound(stage);
		if(it==_fragments.end() || it->first!=stage) {
			_fragments.emplace_hint(it,piecewise_construct,forward_as_tuple(stage),forward_as_tuple(_poolBuffers,fragment,flags));
			if(_fragments.size()>100)
				DEBUG("_fragments.size()=",_fragments.size());
		} else
			DEBUG("Stage ",stage," on flow ",id," has already been received");
	} else {
		onFragment(nextStage++,fragment,flags);
		if(flags&MESSAGE_END)
			complete();
		auto it=_fragments.begin();
		while(it!=_fragments.end()) {
			if( it->first > nextStage)
				break;
			PacketReader packet(it->second->data(), it->second->size());
			onFragment(nextStage++,packet,it->second.flags);
			if(_completed || it->second.flags&MESSAGE_END) {
				complete();
				return; // to prevent a crash bug!! (double fragments deletion)
			}
			_fragments.erase(it++);
		}

	}
}

void RTMFPFlow::onFragment(UInt64 stage,PacketReader& fragment,UInt8 flags) {
	if(stage<=_stage) {
		ERROR("Stage ",stage," not sorted on flow ",id);
		return;
	}
	if(stage>(_stage+1)) {
		// not following _stage!
		UInt32 lostCount = (UInt32)(stage-_stage-1);
		(UInt64&)_stage = stage;
		if(_pPacket) {
			delete _pPacket;
			_pPacket = NULL;
		}
		if(flags&MESSAGE_WITH_BEFOREPART) {
			_numberLostFragments += (lostCount+1);
			return;
		}
		_numberLostFragments += lostCount;
	} else
		(UInt64&)_stage = stage;

	// If MESSAGE_ABANDONMENT, content is not the right normal content!
	if(flags&MESSAGE_ABANDONMENT) {
		if(_pPacket) {
			delete _pPacket;
			_pPacket = NULL;
		}
		return;
	}

	PacketReader* pMessage(&fragment);
	double lostRate(1);

	if(flags&MESSAGE_WITH_BEFOREPART){
		if(!_pPacket) {
			WARN("A received message tells to have a 'beforepart' and nevertheless partbuffer is empty, certainly some packets were lost");
			++_numberLostFragments;
			delete _pPacket;
			_pPacket = NULL;
			return;
		}
		
		_pPacket->add(fragment);

		if(flags&MESSAGE_WITH_AFTERPART)
			return;

		lostRate = _pPacket->fragments;
		pMessage = _pPacket->release();
	} else if(flags&MESSAGE_WITH_AFTERPART) {
		if(_pPacket) {
			ERROR("A received message tells to have not 'beforepart' and nevertheless partbuffer exists");
			_numberLostFragments += _pPacket->fragments;
			delete _pPacket;
		}
		_pPacket = new RTMFPPacket(_poolBuffers,fragment);
		return;
	}

	lostRate = _numberLostFragments/(lostRate+_numberLostFragments);

	if (!_pStream || !_pStream->process(*pMessage, id, _writerRef, lostRate)) {
		close(); // first : send an exception
		//complete(); // do already the delete _pPacket
		return;
	}

	_numberLostFragments=0;

	if(_pPacket) {
		delete _pPacket;
		_pPacket=NULL;
	}
}
