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

#include "Mona/Mona.h"
#include "FlashConnection.h"
#include "Mona/Buffer.h"
#include "FlowManager.h"

/**************************************************************
RTMFPFlow is the receiving class for one NetStream of a 
connection, it is associated to an RTMFPWriter for
sending RTMFP answers
It manages acknowledgments and lost count of messages received
*/
class RTMFPFlow : public virtual Mona::Object {
public:
	RTMFPFlow(Mona::UInt64 id,const std::string& signature, FlowManager& band, const std::shared_ptr<FlashConnection>& pMainStream, Mona::UInt64 idWriterRef);
	RTMFPFlow(Mona::UInt64 id,const std::string& signature,const std::shared_ptr<FlashStream>& pStream, FlowManager& band, Mona::UInt64 idWriterRef);
	virtual ~RTMFPFlow();

	const Mona::UInt64		id;

	// Handle fragments received
	void	input(Mona::UInt64 stage, Mona::UInt8 flags, const Mona::Packet& packet);

	// Build acknowledgment
	Mona::UInt64	buildAck(std::vector<Mona::UInt64>& losts, Mona::UInt16& size);

	bool			consumed() { return _stageEnd && _fragments.empty() && _completeTime.isElapsed(120000); } // Wait 120s before closing the flow definetly

	Mona::UInt32	fragmentation;

private:
	// Handle on fragment received
	void	onFragment(Mona::UInt64 stage, Mona::UInt8 flags, const Mona::Packet& packet);

	void	output(Mona::UInt64 flowId, Mona::UInt32& lost, const Mona::Packet& packet);

	struct Fragment : Mona::Packet, virtual Mona::Object {
		Fragment(Mona::UInt8 flags, const Mona::Packet& packet) : flags(flags), Mona::Packet(std::move(packet)) {}
		const Mona::UInt8 flags;
	};

	Mona::UInt64						_stageEnd; // If not 0 it is completed
	Mona::Time							_completeTime; // Time before closing definetly the flow
	FlowManager&						_band; // RTMFP session to send messages
	Mona::UInt64						_stage; // Current stage (index) of messages received
	std::shared_ptr<FlashStream>		_pStream; // NetStream handler of the flow
	Mona::UInt64						_writerRef; // Id of the writer linked to (read into fullduplex header part)
	std::shared_ptr<Mona::Buffer>		_pBuffer;
	Mona::UInt32						_lost;
	std::map<Mona::UInt64, Fragment>	_fragments; // map of all fragments received and not handled for now
};
