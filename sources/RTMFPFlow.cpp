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
#include "Base/Util.h"

using namespace std;
using namespace Base;

RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature, FlowManager& band, const shared_ptr<FlashConnection>& pMainStream, UInt64 idWriterRef) : _pStream(pMainStream),
	_lost(0),id(id),_writerRef(idWriterRef),_stage(0),_stageEnd(0),_band(band), fragmentation(0) {

	DEBUG("New main flow ", id, " on connection ", _band.name())
}

RTMFPFlow::RTMFPFlow(UInt64 id,const string& signature,const shared_ptr<FlashStream>& pStream, FlowManager& band, UInt64 idWriterRef) : _pStream(pStream),
	_lost(0),id(id),_writerRef(idWriterRef),_stage(0), _stageEnd(0),_band(band), fragmentation(0) {

	DEBUG("New flow ", id, " on connection ", _band.name())
}


RTMFPFlow::~RTMFPFlow() {

	DEBUG("RTMFPFlow ", id, " consumed");

	// delete fragments
	_fragments.clear();
}

UInt64 RTMFPFlow::buildAck(vector<UInt64>& losts, UInt16& size) {
	// Lost informations!
	UInt64 stage = _stage;
	auto it = _fragments.begin();
	while (it != _fragments.end()) {
		stage = it->first - stage - 2;
		size += Binary::Get7BitValueSize(stage);
		losts.emplace_back(stage); // lost count
		UInt32 buffered(0);
		stage = it->first;
		while (++it != _fragments.end() && it->first == (++stage))
			++buffered;
		size += Binary::Get7BitValueSize(buffered);
		losts.emplace_back(buffered);
		--stage;
	}
	_completeTime.update(); // update the complete time to wait at least 120s before destruction of the flow
	return _stage;
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

	if (flags&RTMFP::MESSAGE_ABANDON) {
		DEBUG("Fragments ", nextStage, " to ", stage, " definitely lost on flow ", id, " in session ", _band.name());
		nextStage = stage + 1;
		if (_pBuffer) {
			_lost += packet.size(); // this fragment abandonned
			_lost += _pBuffer->size(); // the bufferized fragment abandonned
			_pBuffer.reset();
		}
	}
	else if (stage>nextStage) {
		// not following stage, bufferizes the stage
		if (_fragments.empty())
			DEBUG("Wait stage ", nextStage, " lost on flow ", id, " in session ", _band.name());
		if (_fragments.emplace(piecewise_construct, forward_as_tuple(stage), forward_as_tuple(flags, packet)).second) {
			fragmentation += packet.size();
			if (_fragments.size() > 100)
				DEBUG("fragments buffer increasing on flow ", id, " in session ", _band.name(), " : ", _fragments.size());
		}
		else
			DEBUG("Stage ", stage, " on flow ", id, " has already been received in session ", _band.name())
		return;
	}
	else
		onFragment(nextStage++, flags, packet);

	auto it = _fragments.begin();
	while (it != _fragments.end() && it->first <= nextStage) {
		TRACE("Processing stage ", it->first, " buffered received on flow ", id, " in session ", _band.name())
		onFragment(nextStage++, it->second.flags, it->second);
		fragmentation -= it->second.size();
		it = _fragments.erase(it);
	}
	if (_fragments.empty() && _stageEnd)
		output(id, _lost, Packet::Null()); // end flow!
	
}

void RTMFPFlow::onFragment(UInt64 stage, UInt8 flags, const Packet& packet) {
	
	_stage = stage;

	if (_pBuffer) {
		_pBuffer->append(packet.data(), packet.size());
		if (flags&RTMFP::MESSAGE_WITH_AFTERPART)
			return;
		Packet packet(_pBuffer);
		if (packet)
			output(id, _lost, packet);
		return;

	}
	if (flags&RTMFP::MESSAGE_WITH_BEFOREPART) {
		_lost += packet.size();
		return; // the beginning of this message is lost, ignore it!
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
		_band.closeFlow(id); // send an exception
		return;
	}
}
