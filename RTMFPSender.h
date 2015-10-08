
#pragma once

#include "Mona/Mona.h"
#include "Mona/UDPSender.h"
#include "Mona/PacketWriter.h"
#include "RTMFP.h"

class RTMFPSender : public Mona::UDPSender, public virtual Mona::Object {
public:
	RTMFPSender(const Mona::PoolBuffers& poolBuffers,const std::shared_ptr<RTMFPEngine>& pEncoder): _pEncoder(pEncoder),Mona::UDPSender("RTMFPSender"),packet(poolBuffers),farId(0) {
		packet.next(RTMFP_HEADER_SIZE);
	}
	
	Mona::UInt32		farId;
	Mona::PacketWriter	packet;

private:
	const Mona::UInt8*	data() const { return packet.size() < RTMFP_MIN_PACKET_SIZE ? NULL : packet.data(); }
	Mona::UInt32		size() const { return packet.size(); }
	
	bool			run(Mona::Exception& ex);

	const std::shared_ptr<RTMFPEngine>	_pEncoder;
};
