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

#include "DefaultConnection.h"
#include "SocketHandler.h"

using namespace Mona;
using namespace std;

DefaultConnection::DefaultConnection(SocketHandler* pHandler) : Connection(pHandler) {

}

DefaultConnection::~DefaultConnection() {
	//close();
}

/*
void DefaultConnection::close() {

	Connection::close();
}*/

void DefaultConnection::handleMessage(const PoolBuffer& pBuffer) {

	BinaryReader reader(pBuffer.data(), pBuffer->size());
	reader.next(2); // TODO: CRC, don't share this part in onPacket() 

	if (Logs::GetLevel() >= 7)
		DUMP("RTMFP", reader.current(), reader.available(), "Request from ", _address.toString())

	UInt8 marker = reader.read8();
	_timeReceived = reader.read16();
	_lastReceptionTime.update();

	// Handshake
	if (marker == 0x0B) {
		UInt8 type = reader.read8();
		UInt16 length = reader.read16();

		switch (type) {
		case 0x30:
			handleHandshake30(reader); break;
		case 0x70:
			handleHandshake70(reader); break;
		case 0x71:
			handleP2pAddresses(reader);
			break;
		default:
			ERROR("Unexpected p2p handshake type : ", Format<UInt8>("%.2x", (UInt8)type))
			break;
		}
	} else
		ERROR("Unexpected marker type on unkown address ", _address.toString(), " : ", Format<UInt8>("%.2x", (UInt8)marker))
}

void DefaultConnection::handleHandshake30(BinaryReader& reader) {
	DEBUG("A peer is trying to connect to us from ", _address.toString())

	UInt64 peerIdSize = reader.read7BitLongValue();
	if (peerIdSize != 0x22)
		ERROR("Unexpected peer id size : ", peerIdSize, " (expected 34)")
	else if ((peerIdSize = reader.read7BitLongValue()) != 0x21)
		ERROR("Unexpected peer id size : ", peerIdSize, " (expected 33)")
	else if (reader.read8() != 0x0F)
		ERROR("Unexpected marker : ", *reader.current(), " (expected 0x0F)")
	else {

		string buff, peerId, tag;
		reader.read(0x20, buff);
		reader.read(16, tag);
		Util::FormatHex(BIN buff.data(), buff.size(), peerId);
		_pParent->onPeerHandshake30(peerId, tag, _address);

		/*if (peerId != _pParent->peerId()) {
			WARN("Incorrect Peer ID in handshake 30 : ", peerId)
			return;
		}
		INFO("P2P Connection request from ", _outAddress.toString())

			auto it = _mapPeersByAddress.find(_outAddress);
		if (it == _mapPeersByAddress.end()) {

			auto itTag = _mapPeersByTag.find(tag);
			shared_ptr<P2PConnection> pPeerConnection;
			if (itTag != _mapPeersByTag.end()) {
				pPeerConnection = itTag->second;
				_mapPeersByTag.erase(itTag);
			}
			else if (_group) { // NetGroup : we accept direct connections

				DEBUG("It is a direct P2P connection request from the NetGroup")
					PEER_LIST_ADDRESS_TYPE addresses;
				addresses.emplace(_outAddress, RTMFP::ADDRESS_PUBLIC);
				pPeerConnection = createP2PConnection("unknown", tag.c_str(), addresses, _targetAddress, true);
			}
			else {
				WARN("No p2p waiting connection found (possibly already connected)")
					return;
			}

			it = _mapPeersByAddress.emplace_hint(it, _outAddress, pPeerConnection);
		}
		else {
			WARN("The peer is already connected to us (same address)")
				return;
		}

		// Send response (handshake 70)
		it->second->responderHandshake0(ex, tag, _outAddress);*/
	}
}

void DefaultConnection::handleHandshake70(BinaryReader& reader) {
	string tagReceived, cookie, farKey;

	/*if (_status > RTMFP::HANDSHAKE30) {
		DEBUG("Handshake 70 ignored, we are already in ", _pSession->status, " state")
		return;
	}*/

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ERROR("Unexpected tag size : ", tagSize)
			return;
	}

	reader.read(16, tagReceived);
	/*if (memcmp(tagReceived.c_str(), _pSession->tag().data(), 16) != 0) {
		ERROR("Unexpected tag received")
		return;
	}*/

	// Normal NetConnection
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ERROR("Unexpected cookie size : ", cookieSize)
			return;
	}
	reader.read(cookieSize, cookie);

	UInt32 keySize = (UInt32)reader.read7BitLongValue() - 2;
	if (keySize != 0x80 && keySize != 0x7F) {
		ERROR("Unexpected responder key size : ", keySize)
		return;
	}
	if (reader.read16() != 0x1D02) {
		ERROR("Unexpected signature before responder key (expected 1D02)")
		return;
	}
	reader.read(keySize, farKey);

	_pParent->onPeerHandshake70(tagReceived, farKey, cookie, _address, true);
}
