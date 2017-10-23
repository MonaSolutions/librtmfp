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

#include "GroupBuffer.h"
#include "GroupStream.h"
#include "Base/Logs.h"

using namespace Base;
using namespace std;

GroupBuffer::GroupBuffer() : Thread("GroupBuffer") {
}

GroupBuffer::~GroupBuffer() {
	stop();
}

bool GroupBuffer::add(Exception& ex, UInt32 groupMediaId, const shared_ptr<GroupFragment>& pFragment) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!start(ex))
		return false;

	_waitingRequests.emplace_back(WaitRequest::ADD_FRAGMENT, groupMediaId, pFragment);
	wakeUp.set();
	return true;
}

bool GroupBuffer::removeBuffer(Exception& ex, UInt32 groupMediaId) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!start(ex))
		return false;

	_waitingRequests.emplace_back(WaitRequest::REMOVE_BUFFER, groupMediaId);
	wakeUp.set();
	return true;
}

bool GroupBuffer::removeFragments(Exception& ex, UInt32 groupMediaId, UInt64 fragmentId) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!start(ex))
		return false;

	_waitingRequests.emplace_back(WaitRequest::REMOVE_FRAGMENTS, groupMediaId, nullptr, fragmentId);
	wakeUp.set();
	return true;
}

bool GroupBuffer::startProcessing(Exception& ex, UInt32 groupMediaId) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (!start(ex))
		return false;

	_waitingRequests.emplace_back(WaitRequest::START_PROCESSING, groupMediaId);
	wakeUp.set();
	return true;
}

bool GroupBuffer::run(Exception&, const volatile bool& requestStop) {

	for (;;) {
		bool timeout = !wakeUp.wait(120000); // 2 mn of timeout
		for (;;) {

			// Get waiting fragments or handle stop
			deque<WaitRequest> fragments;
			{
				lock_guard<mutex> lock(_mutex);
				if (_waitingRequests.empty()) {
					if (!timeout && !requestStop)
						break; // wait more
					stop(); // to set _stop immediatly!
					return true;
				}
				fragments = move(_waitingRequests);
			}

			// Process requests
			Result result;
			for (WaitRequest& fragment : fragments)
				processRequest(result, fragment);

			// Forward packets if the result queue is not empty
			if (!result.empty())
				onNextPacket(result);
		}
	}
}

void GroupBuffer::processRequest(deque<RTMFP::MediaPacket>& result, WaitRequest& request) {

	auto itBuffer = _mapGroupMedia2fragments.lower_bound(request.groupMediaId);

	switch (request.command) {
	case WaitRequest::START_PROCESSING:

		// Create the buffer if it doesn't exist
		if (itBuffer == _mapGroupMedia2fragments.end() || itBuffer->first != request.groupMediaId)
			itBuffer = _mapGroupMedia2fragments.emplace_hint(itBuffer, piecewise_construct, forward_as_tuple(request.groupMediaId), forward_as_tuple());
		else {
			auto itFragment = itBuffer->second.begin();
			while (processFragment(result, itBuffer->first, itBuffer->second, itFragment))
				++itFragment;
		}
		itBuffer->second.started = true;
		break;
	case WaitRequest::ADD_FRAGMENT:

		processAddFragment(itBuffer, result, request);
		break;
	case WaitRequest::REMOVE_FRAGMENTS:

		processRemoveFragments(itBuffer, result, request);
		break;
	case WaitRequest::REMOVE_BUFFER:

		if (itBuffer != _mapGroupMedia2fragments.end()) {
			DEBUG("Deleting GroupMedia from GroupBuffer")
			_mapGroupMedia2fragments.erase(itBuffer);
		}
		break;
	}
}

void GroupBuffer::processAddFragment(map<UInt32, MediaBuffer>::iterator& itBuffer, deque<RTMFP::MediaPacket>& result, WaitRequest& request) {

	// Create the buffer if it doesn't exist
	if (itBuffer == _mapGroupMedia2fragments.end() || itBuffer->first != request.groupMediaId)
		itBuffer = _mapGroupMedia2fragments.emplace_hint(itBuffer, piecewise_construct, forward_as_tuple(request.groupMediaId), forward_as_tuple());

	// Add the new fragment
	auto itEmplace = itBuffer->second.emplace(request.pFragment->id, request.pFragment);
	if (!itEmplace.second)
		return; // emplace error

	// Try to process the new fragment
	auto itFragment = itEmplace.first;
	while (itBuffer->second.started && processFragment(result, itBuffer->first, itBuffer->second, itFragment))
		++itFragment;
}

void GroupBuffer::processRemoveFragments(const map<UInt32, MediaBuffer>::iterator& itBuffer, deque<RTMFP::MediaPacket>& result, WaitRequest& request) {
	if (itBuffer == _mapGroupMedia2fragments.end() || itBuffer->first != request.groupMediaId) {
		FATAL_ERROR("Unable to find the GroupMedia buffer ", request.groupMediaId) // implementation error
		return;
	}

	auto itBegin = itBuffer->second.begin();
	if (itBegin != itBuffer->second.end() && request.fragmentId > itBegin->first) {

		auto itCurrent = itBuffer->second.find(request.fragmentId);
		if (itCurrent == itBuffer->second.end()) {
			FATAL_ERROR("Unable to find the reference fragment ", request.fragmentId) // implementation error
			return;
		}
		TRACE("GroupMedia ", request.groupMediaId, " - Deletion of fragments ", itBegin->first, " to ", itCurrent->first)
		itBuffer->second.erase(itBegin, itCurrent);

		// Reset current fragment id if needed
		if (itBuffer->second.currentId < itCurrent->first) {
			WARN("GroupMedia ", request.groupMediaId, " - Deleting unread fragments to keep the window duration... (", itCurrent->first - itBuffer->second.currentId, " fragments ignored)")
			itBuffer->second.currentId = 0; // reset the current fragment Id

			// Try to process again the fragments
			while (itBuffer->second.started && processFragment(result, itBuffer->first, itBuffer->second, itCurrent))
				++itCurrent;
		}
	}
}

bool GroupBuffer::processFragment(std::deque<RTMFP::MediaPacket>& result, UInt32 groupMediaId, MediaBuffer& buffer, MAP_FRAGMENTS_ITERATOR& itFragment) {
	if (itFragment == buffer.end())
		return false;

	DEBUG("GroupMedia ", groupMediaId, " - processFragment ", itFragment->first, " ; marker : ", itFragment->second->marker)

	// Stand alone fragment (special case : sometimes Flash send media END without splitted fragments)
	if (itFragment->second->marker == GroupStream::GROUP_MEDIA_DATA || (itFragment->second->marker == GroupStream::GROUP_MEDIA_END && itFragment->first == buffer.currentId + 1)) {
		// Is it the next fragment?
		if (buffer.currentId == 0 || itFragment->first == buffer.currentId + 1) {
			buffer.currentId = itFragment->first;

			DEBUG("GroupMedia ", groupMediaId, " - Pushing Media Fragment ", itFragment->first)
			result.emplace_back(*itFragment->second, itFragment->second->time, itFragment->second->type);
			return true;
		}
		return false;
	}
	// else Splitted fragment

	// First fragment? Search for a start fragment
	if (buffer.currentId == 0) {
		// Delete first splitted fragments
		if (itFragment->second->marker != GroupStream::GROUP_MEDIA_START) {
			TRACE("GroupMedia ", groupMediaId, " - Ignoring splitted fragment ", itFragment->first, ", we are waiting for a starting fragment")
			buffer.erase(itFragment);
			return false;
		}
		TRACE("GroupMedia ", groupMediaId, " - First fragment is a Start Media Fragment")
		buffer.currentId = itFragment->first - 1; // -1 to be catched by the next fragment condition 
	}

	// Search the start fragment
	auto itStart = itFragment;
	while (itStart->second->marker != GroupStream::GROUP_MEDIA_START) {
		itStart = buffer.find(itStart->first - 1);
		if (itStart == buffer.end())
			return false; // ignore these fragments if there is a hole
	}

	// Is it the next fragment?
	if (itStart->first == buffer.currentId + 1) {

		// Check if all splitted fragments are present
		UInt8 nbFragments = itStart->second->splittedId + 1;
		UInt32 totalSize = itStart->second->size();
		auto itEnd = itStart;
		for (int i = 1; i < nbFragments; ++i) {
			if (++itEnd == buffer.end() || itEnd->first != (itStart->first + i))
				return false; // wait fulfil if there is a hole
			totalSize += itEnd->second->size();
		}

		// update the current fragment
		buffer.currentId = itEnd->first;
		itFragment = itEnd;

		// Buffer the fragments and forward the whole packet
		shared_ptr<Buffer>	pBuffer(new Buffer(totalSize));
		BinaryWriter writer(pBuffer->data(), pBuffer->size());
		auto itCurrent = itStart;
		do {
			writer.write(itCurrent->second->data(), itCurrent->second->size());
		} while (itCurrent++ != itEnd);

		DEBUG("GroupMedia ", groupMediaId, " - Pushing splitted packet ", itStart->first, " - ", nbFragments, " fragments for a total size of ", writer.size())
		result.emplace_back(Packet(pBuffer), itStart->second->time, itStart->second->type);
		return true;
	}
	return false;
}
