
#include "RTMFPFlow.h"
//#include "Mona/Invoker.h"
#include "Mona/Logs.h"
#include "Mona/Util.h"
#include "Mona/PoolBuffer.h"

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


RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature,/*Peer& peer,*/const PoolBuffers& poolBuffers, BandWriter& band, const shared_ptr<FlashConnection>& pMainStream) : /*_pGroup(NULL), _peer(peer), */_pStream(pMainStream),_poolBuffers(poolBuffers),_numberLostFragments(0),id(id),_stage(0),_completed(false),_pPacket(NULL),_band(band) {
	
	// MAIN Stream flow OR Null flow

	RTMFPWriter* pWriter = new RTMFPWriter(band.connected ? FlashWriter::OPENED : FlashWriter::OPENING,signature, band, _pWriter);

	if (!_pStream) {
		pWriter->open(); // FlowNull, must be opened
		return;
	}

	(bool&)pWriter->critical = _pStream.use_count()<=2;
	((UInt64&)pWriter->flowId) = id;
}

RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature,const shared_ptr<FlashStream>& pStream,/*Peer& peer,*/const PoolBuffers& poolBuffers, BandWriter& band) : /*_pGroup(NULL), _peer(peer), */_pStream(pStream),_poolBuffers(poolBuffers),_numberLostFragments(0),id(id),_stage(0),_completed(false),_pPacket(NULL),_band(band) {
	
	new RTMFPWriter(band.connected ? FlashWriter::OPENED : FlashWriter::OPENING,signature, band, _pWriter);

}


RTMFPFlow::~RTMFPFlow() {
	/*if(_pGroup)
		_peer.unjoinGroup(*_pGroup);*/

	complete();
	if (_pStream)
		_pStream->disengage(_pWriter.get());
	_pWriter->close();
}

void RTMFPFlow::setId(UInt64 idFlow) {
	((UInt64&)id) = idFlow;
	((UInt64&)_pWriter->flowId) = id;
}

void RTMFPFlow::complete() {
	if(_completed)
		return;

	if(_pStream) // FlowNull instance, not display the message in FullNull case
		DEBUG("RTMFPFlow ",id," consumed");

	// delete fragments
	_fragments.clear();

	// delete receive buffer
	if(_pPacket) {
		delete _pPacket;
		_pPacket=NULL;
	}

	_completed=true;
}

void RTMFPFlow::fail(const string& error) {
	ERROR("RTMFPFlow ",id," failed, ",error);
	if (_completed)
		return;
	BinaryWriter& writer = _band.writeMessage(0x5e,Util::Get7BitValueSize(id)+1);
	writer.write7BitLongValue(id);
	writer.write8(0); // unknown
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
	if(!_pStream)
		bufferSize=0; // not proceed a packet sur FlowNull

	BinaryWriter& ack = _band.writeMessage(0x51,Util::Get7BitValueSize(id)+Util::Get7BitValueSize(bufferSize)+Util::Get7BitValueSize(_stage)+size);

	ack.write7BitLongValue(id);
	ack.write7BitValue(bufferSize);
	ack.write7BitLongValue(_stage);

	for(UInt64 lost : losts)
		ack.write7BitLongValue(lost);

	if(_pStream)
		_pStream->flush();
	_pWriter->flush();
}

void RTMFPFlow::receive(UInt64 stage,UInt64 deltaNAck,PacketReader& fragment,UInt8 flags) {
	if(_completed)
		return;

	if(!_pStream) { // if this==FlowNull
		fail("RTMFPMessage received for a RTMFPFlow unknown");
		(UInt64&)_stage = stage;
		return;
	}

//	TRACE("RTMFPFlow ",id," _stage ",_stage);

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
			if(it->second.flags&MESSAGE_END) {
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
			if(it->second.flags&MESSAGE_END) {
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

	UInt32 time(0);
	AMF::ContentType type = (AMF::ContentType)pMessage->read8();
	switch(type) {
		case AMF::AUDIO:
		case AMF::VIDEO:
			time = pMessage->read32();
		case AMF::CHUNKSIZE:
			break;
		default:
			pMessage->next(4);
			break;
	}

	lostRate = _numberLostFragments/(lostRate+_numberLostFragments);

	if (!_pStream || !_pStream->process(type, time, *pMessage, *_pWriter, lostRate)) {
		complete(); // do already the delete _pPacket
		return;
	}

	_numberLostFragments=0;

	if(_pPacket) {
		delete _pPacket;
		_pPacket=NULL;
	}
}

void RTMFPFlow::sendConnect(const string& url) {
	if(_pStream)
		_pStream->connect(*_pWriter, url);
}

void RTMFPFlow::createStream() {
	if(_pStream)
		_pStream->createStream(*_pWriter);
}

void RTMFPFlow::sendPlay(const string& name, bool amf3) {
	if(_pStream)
		_pStream->play(*_pWriter, name, amf3);
}

void RTMFPFlow::sendPublish(const string& name) {
	if(_pStream)
		_pStream->publish(*_pWriter, name);
}

void RTMFPFlow::sendPeerInfo(UInt16 port) {
	if(_pStream)
		_pStream->sendPeerInfo(*_pWriter, port);
}
