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

#include "RTMFP.h"
#include "Mona/Util.h"
#include "AMF.h"

using namespace std;
using namespace Mona;

bool RTMFP::ReadAddress(BinaryReader& reader, SocketAddress& address, AddressType& addressType) {
	string data;
	addressType = (AddressType)reader.read8();
	reader.read<string>((addressType & 0x80) ? sizeof(in6_addr) : sizeof(in_addr), data);
	in_addr addrV4;
	in6_addr addrV6;
	IPAddress addr;
	if (addressType & 0x80) {
		memcpy(&addrV6, data.data(), sizeof(in6_addr));
		addr.set(addrV6);
	}
	else {
		memcpy(&addrV4, data.data(), sizeof(in_addr));
		addr.set(addrV4);
	}
	address.set(addr, reader.read16());
	return true;
}

BinaryWriter& RTMFP::WriteAddress(BinaryWriter& writer,const SocketAddress& address,AddressType type) {
	const IPAddress& host = address.host();
	if (host.family() == IPAddress::IPv6)
		writer.write8(type | 0x80);
	else
		writer.write8(type);
	NET_SOCKLEN size(host.size());
	const UInt8* bytes = (const UInt8*)host.data();
	for(NET_SOCKLEN i=0;i<size;++i)
		writer.write8(bytes[i]);
	return writer.write16(address.port());
}

UInt32 RTMFP::Unpack(BinaryReader& reader) {
	reader.reset();
	UInt32 id=0;
	for(int i=0;i<3;++i)
		id ^= reader.read32();
	reader.reset(4);
	return id;
}

void RTMFP::Pack(Buffer& buffer,UInt32 farId) {
	BinaryReader reader(buffer.data()+4, buffer.size()-4);
	BinaryWriter(buffer.data(),4).write32(reader.read32()^reader.read32()^farId);
}


void RTMFP::ComputeAsymetricKeys(const Binary& sharedSecret, const UInt8* initiatorNonce,UInt32 initNonceSize, const UInt8* responderNonce, UInt32 respNonceSize, UInt8* requestKey,UInt8* responseKey) {

	Crypto::HMAC::SHA256(responderNonce, respNonceSize, initiatorNonce, initNonceSize, requestKey);
	Crypto::HMAC::SHA256(initiatorNonce, initNonceSize, responderNonce, respNonceSize, responseKey);
	// now doing HMAC-sha256 of both result with the shared secret DH key
	Crypto::HMAC::SHA256(sharedSecret.data(), sharedSecret.size(), requestKey, Crypto::SHA256_SIZE, requestKey);
	Crypto::HMAC::SHA256(sharedSecret.data(), sharedSecret.size(), responseKey, Crypto::SHA256_SIZE, responseKey);
}

bool RTMFP::ReadAddresses(BinaryReader& reader, PEER_LIST_ADDRESS_TYPE& addresses, SocketAddress& hostAddress, function<void(const SocketAddress&, AddressType)> onNewAddress) {

	// Read all addresses
	SocketAddress address;
	AddressType addressType;
	while (reader.available()) {

		RTMFP::ReadAddress(reader, address, addressType);
		switch (addressType & 0x0F) {
		case RTMFP::ADDRESS_LOCAL:
		case RTMFP::ADDRESS_PUBLIC: {
			auto itAddress = addresses.lower_bound(address);
			if (itAddress == addresses.end() || itAddress->first != address) { // new address?
				addresses.emplace_hint(itAddress, address, addressType);
				onNewAddress(address, addressType);
			}
			break;
		}
		case RTMFP::ADDRESS_REDIRECTION:
			if (hostAddress != address) { // new address?
				hostAddress = address;
				onNewAddress(address, addressType);
			}
			break;
		}
		TRACE("IP Address : ", address, " - type : ", addressType)
	}
	return !addresses.empty() || hostAddress;
}

