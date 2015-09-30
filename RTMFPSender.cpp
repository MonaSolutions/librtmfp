#include "RTMFPSender.h"
#include "Mona/Crypto.h"

using namespace Mona;

bool RTMFPSender::run(Exception& ex) {
	// paddingBytesLength=(0xffffffff-plainRequestLength+5)&0x0F
	/*int paddingBytesLength = (0xFFFFFFFF-packet.size()+5)&0x0F;
	// Padd the plain request with paddingBytesLength of value 0xff at the end
	while (paddingBytesLength-->0)
		packet.write8(0xFF);*/
	// Write CRC (at the beginning of the request)
	BinaryReader reader(packet.data()+6,packet.size()-6);
	BinaryWriter(packet.data()+4,2).write16(Crypto::ComputeCRC(reader));
	// Encrypt the resulted request
	_pEncoder->process((UInt8*)packet.data()+4,packet.size()-4);
	RTMFP::Pack(packet,farId);

	return UDPSender::run(ex);
}