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

#include "Base/Mona.h"
#include "FlashConnection.h"
#include "Base/Buffer.h"
#include "FlowManager.h"

/**************************************************************
RTMFPFlow is the receiving class for one NetStream of a 
connection, it is associated to an RTMFPWriter for
sending RTMFP answers
It manages acknowledgments and lost count of messages received
*/
class RTMFPFlow : public virtual Base::Object {
public:
	RTMFPFlow(Base::UInt64 id,const std::string& signature, FlowManager& band, const std::shared_ptr<FlashConnection>& pMainStream, Base::UInt64 idWriterRef);
	RTMFPFlow(Base::UInt64 id,const std::string& signature,const std::shared_ptr<FlashStream>& pStream, FlowManager& band, Base::UInt64 idWriterRef);
	virtual ~RTMFPFlow();

	const Base::UInt64		id;

	// Handle fragments received
	void	input(Base::UInt64 stage, Base::UInt8 flags, const Base::Packet& packet);

	// Build acknowledgment
	Base::UInt64	buildAck(std::vector<Base::UInt64>& losts, Base::UInt16& size);

	bool			consumed() { return _stageEnd && _fragments.empty() && _completeTime.isElapsed(120000); } // Wait 120s before closing the flow definetly

	Base::UInt32	fragmentation;

private:
	// Handle on fragment received
	void	onFragment(Base::UInt64 stage, Base::UInt8 flags, const Base::Packet& packet);

	void	output(Base::UInt64 flowId, Base::UInt32& lost, const Base::Packet& packet);

	struct Fragment : Base::Packet, virtual Base::Object {
		Fragment(Base::UInt8 flags, const Base::Packet& packet) : flags(flags), Base::Packet(std::move(packet)) {}
		const Base::UInt8 flags;
	};

	Base::UInt64						_stageEnd; // If not 0 it is completed
	Base::Time							_completeTime; // Time before closing definetly the flow
	FlowManager&						_band; // RTMFP session to send messages
	Base::UInt64						_stage; // Current stage (index) of messages received
	std::shared_ptr<FlashStream>		_pStream; // NetStream handler of the flow
	Base::UInt64						_writerRef; // Id of the writer linked to (read into fullduplex header part)
	std::shared_ptr<Base::Buffer>		_pBuffer;
	Base::UInt32						_lost;
	std::map<Base::UInt64, Fragment>	_fragments; // map of all fragments received and not handled for now
};
