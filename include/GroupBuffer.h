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

#pragma once

#include "Base/Thread.h"
#include "PeerMedia.h"

/************************************************************************
GroupBuffer is a thread class used to queue fragments for unfragmenting 
It reorder the packets and merge splitted fragments and then
push the media packets to the invoker.

*/
struct GroupBuffer : private Base::Thread {
	struct Result : std::deque<RTMFP::MediaPacket>, virtual Base::Object {
		Result() {}
	};
	typedef Base::Event<void(Result&)>		ON(NextPacket); // Called when at least one media packet is ready for reading

	GroupBuffer();
	virtual ~GroupBuffer();

	// Add a fragment to the waiting requests for processing
	bool	add(Base::Exception& ex, Base::UInt32 groupMediaId, const std::shared_ptr<GroupFragment>& pFragment);

	// Remove the buffer of a deleted GroupMedia
	bool	removeBuffer(Base::Exception& ex, Base::UInt32 groupMediaId);

	// Remove the fragments inferior to fragmentId
	bool	removeFragments(Base::Exception& ex, Base::UInt32 groupMediaId, Base::UInt64 fragmentId);

	// Start processing a GroupMedia fragments (after receiving the first pull fragment)
	bool	startProcessing(Base::Exception& ex, Base::UInt32 groupMediaId);

private:
	bool	run(Base::Exception&, const volatile bool& requestStop);

	struct WaitRequest : virtual Object {
		enum Command {
			START_PROCESSING,
			ADD_FRAGMENT,
			REMOVE_FRAGMENTS,
			REMOVE_BUFFER
		};

		WaitRequest(Command command, Base::UInt32 groupMediaId, const std::shared_ptr<GroupFragment>& pFragment=nullptr, Base::UInt64 fragmentId=0) :
			pFragment(pFragment), fragmentId(fragmentId), groupMediaId(groupMediaId), command(command) {}

		std::shared_ptr<GroupFragment>				pFragment; // current fragment
		Base::UInt64								fragmentId; // Current fragment Id of the current GroupMedia for deletion of old fragments
		Base::UInt32								groupMediaId; // Current stream key
		Command										command; // request command
	};
	// Process request
	void	processRequest(std::deque<RTMFP::MediaPacket>& result, WaitRequest& request);

	struct MediaBuffer : MAP_FRAGMENTS, virtual Base::Object {
		MediaBuffer() : currentId(0), started(false) {}

		Base::UInt64	currentId; // current fragment id (last fragment sent)
		bool			started; // true if we can process the fragments (a pull fragment has been received)
	};
	// Process one fragment
	bool	processFragment(std::deque<RTMFP::MediaPacket>& result, Base::UInt32 groupMediaId, MediaBuffer& buffer, MAP_FRAGMENTS_ITERATOR& itFragment);

	// Remove the fragments & try to process remaining fragments
	void	processRemoveFragments(const std::map<Base::UInt32, MediaBuffer>::iterator& itBuffer, std::deque<RTMFP::MediaPacket>& result, WaitRequest& request);

	// Add the fragment & try to process
	void	processAddFragment(std::map<Base::UInt32, MediaBuffer>::iterator& itBuffer, std::deque<RTMFP::MediaPacket>& result, WaitRequest& request);

	std::mutex								_mutex;
	std::map<Base::UInt32, MediaBuffer>		_mapGroupMedia2fragments; // GroupMedia id to map of fragments
	std::deque<WaitRequest>					_waitingRequests; // Fragments waiting to be added
};
