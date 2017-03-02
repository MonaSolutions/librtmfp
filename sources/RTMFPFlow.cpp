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

using namespace std;
using namespace Mona;

RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature, FlowManager& band, const shared_ptr<FlashConnection>& pMainStream, UInt64 idWriterRef) : _pStream(pMainStream),
	_lost(0),id(id),_writerRef(idWriterRef),_stage(0),_stageEnd(0),_band(band) {

	DEBUG("New main flow ", id, " on connection ", _band.name())
}

RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature,const shared_ptr<FlashStream>& pStream, FlowManager& band, UInt64 idWriterRef) : _pStream(pStream),
	_lost(0),id(id),_writerRef(idWriterRef),_stage(0), _stageEnd(0),_band(band) {

	DEBUG("New flow ", id, " on connection ", _band.name())
}


RTMFPFlow::~RTMFPFlow() {

	DEBUG("RTMFPFlow ", id, " consumed");

	// delete fragments
	_fragments.clear();


	_completeTime.update();
}

void RTMFPFlow::close() {
	BinaryWriter& writer = _band.writeMessage(0x5e, Binary::Get7BitValueSize(id) + 1);
	writer.write7BitLongValue(id);
	writer.write8(0); // finishing marker
	_band.flush();
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
		size += Binary::Get7BitValueSize(current);
		losts.emplace_back(current);
		current = it->first;
		while(++it!=_fragments.end() && it->first==(++current))
			++count;
		size += Binary::Get7BitValueSize(count);
		losts.emplace_back(count);
		--current;
		count=0;
	}

	UInt32 bufferSize = _pBuffer ? ((_fragments.size()>0x3F00) ? 0 : (0x3F00 - _fragments.size())) : 0x7F;
	BinaryWriter& ack = _band.writeMessage(0x51, Binary::Get7BitValueSize(id)+ Binary::Get7BitValueSize(bufferSize)+ Binary::Get7BitValueSize(_stage)+size);

	ack.write7BitLongValue(id);
	ack.write7BitValue(bufferSize);
	ack.write7BitLongValue(_stage);

	for(UInt64 lost : losts)
		ack.write7BitLongValue(lost);

	_band.flush();
}

void RTMFPFlow::input(UInt64 stage, UInt8 flags, const Packet& packet) {
	if (_stageEnd) {
		if (_fragments.empty()) {
			// if completed accept anyway to allow ack and avoid repetition
			_stage = stage;
			return; // completed!
		}
		if (stage > _stageEnd) {
			DEBUG("Stage ", stage, " superior to stage end ", _stageEnd, " on flow ", id);
			return;
		}
	}
	else if (flags&RTMFP::MESSAGE_END)
		_stageEnd = stage;

	UInt64 nextStage = _stage + 1;
	if (stage < nextStage) {
		DEBUG("Stage ", stage, " on flow ", id, " has already been received");
		return;
	}
	if (stage>nextStage) {
		// not following stage, bufferizes the stage
		if (!_fragments.emplace(piecewise_construct, forward_as_tuple(stage), forward_as_tuple(flags, packet)).second)
			DEBUG("Stage ", stage, " on flow ", id, " has already been received")
		else if (_fragments.size()>100)
			DEBUG("_fragments.size()=", _fragments.size());
	}
	else {
		onFragment(nextStage++, flags, packet);
		auto it = _fragments.begin();
		while (it != _fragments.end() && it->first <= nextStage) {
			onFragment(nextStage++, it->second.flags, it->second);
			it = _fragments.erase(it);
		}
		if (_fragments.empty() && _stageEnd)
			output(id, _lost, Packet::Null()); // end flow!
	}
}

void RTMFPFlow::onFragment(UInt64 stage, UInt8 flags, const Packet& packet) {
	
	_stage = stage;
	// If MESSAGE_ABANDON, abandon the current packet (happen on lost data)
	if (flags&RTMFP::MESSAGE_ABANDON) {
		if (_pBuffer) {
			_lost += packet.size(); // this fragment abandonned
			_lost += _pBuffer->size(); // the bufferized fragment abandonned
			DEBUG("Fragments lost on flow ", id);
			_pBuffer.reset();
		}
		return;
	}

	if (_pBuffer) {
		_pBuffer->append(packet.data(), packet.size());
		if (flags&RTMFP::MESSAGE_WITH_AFTERPART)
			return;
		Packet packet(_pBuffer);
		if (packet)
			output(id, _lost, packet);
		return;

	}
	if (flags&RTMFP::MESSAGE_WITH_AFTERPART) {
		_pBuffer.reset(new Buffer(packet.size(), packet.data()));
		return;
	}
	if (packet)
		output(id, _lost, packet);
}

void RTMFPFlow::output(UInt64 flowId, UInt32& lost, const Packet& packet) {

	if (!_pStream || !_pStream->process(packet, id, _writerRef, lost)) {
		close(); // first : send an exception
		//complete(); // do already the delete _pPacket
		return;
	}
}
