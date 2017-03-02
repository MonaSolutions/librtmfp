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

#include "RTMFPHandshaker.h"
#include "RTMFPSession.h"
#include "RTMFPSender.h"
#include "Mona/Util.h"

using namespace Mona;
using namespace std;

RTMFPHandshaker::RTMFPHandshaker(Invoker& invoker, RTMFPSession* pSession) : _pSession(pSession), _name("handshaker"), BandWriter(invoker) {
}

RTMFPHandshaker::~RTMFPHandshaker() {
	close();
}

void RTMFPHandshaker::close() {

	_mapTags.clear();
	_mapCookies.clear();
}

void RTMFPHandshaker::receive(const SocketAddress& address, const Packet& packet) {

	_address.set(address); // update address

	if (Logs::GetLevel() >= 7)
		DUMP("LIBRTMFP", packet.data(), packet.size(), "Request from ", address)

	BinaryReader reader(packet.data(), packet.size());
	UInt8 marker = reader.read8();
	_timeReceived = reader.read16();
	_lastReceptionTime.update();

	// Handshake
	if (marker != 0x0B) {
		WARN("Unexpected Handshake marker : ", String::Format<UInt8>("%02x", marker));
		return;
	}

	UInt8 type = reader.read8();
	UInt16 length = reader.read16();
	reader.shrink(length); // resize the buffer to ignore the padding bytes

	switch (type) {
	case 0x30:
		handleHandshake30(reader); break; // P2P only (and send handshake 70)
	case 0x38:
		sendHandshake78(reader); break; // P2P only
	case 0x70:
		handleHandshake70(reader); break; // (and send handshake 38)
	case 0x71:
		handleRedirection(reader); break; // p2p address exchange or server redirection
	default:
		ERROR("Unexpected p2p handshake type : ", String::Format<UInt8>("%.2x", (UInt8)type))
		break;
	}
}

bool  RTMFPHandshaker::startHandshake(shared_ptr<Handshake>& pHandshake, const SocketAddress& address, FlowManager* pSession, bool responder, bool p2p) {
	PEER_LIST_ADDRESS_TYPE mapAddresses;
	return startHandshake(pHandshake, address, mapAddresses, pSession, responder, p2p);
}

bool  RTMFPHandshaker::startHandshake(shared_ptr<Handshake>& pHandshake, const SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, FlowManager* pSession, bool responder, bool p2p) {
	const string& tag = pSession->tag();
	auto itHandshake = _mapTags.lower_bound(tag);
	if (itHandshake == _mapTags.end() || itHandshake->first != tag) {
		itHandshake = _mapTags.emplace_hint(itHandshake, piecewise_construct, forward_as_tuple(tag), forward_as_tuple(new Handshake(pSession, address, addresses, p2p)));
		itHandshake->second->pTag = &itHandshake->first;
		pHandshake = itHandshake->second;
		return true;
	}
	WARN("Handshake already exists, nothing done")
	pHandshake = itHandshake->second;
	return false;
}

void RTMFPHandshaker::sendHandshake70(const string& tag, const SocketAddress& address, const SocketAddress& host) {
	auto itHandshake = _mapTags.lower_bound(tag);
	if (itHandshake == _mapTags.end() || itHandshake->first != tag) {
		PEER_LIST_ADDRESS_TYPE addresses;
		addresses.emplace(address, RTMFP::ADDRESS_PUBLIC);
		itHandshake = _mapTags.emplace_hint(itHandshake, piecewise_construct, forward_as_tuple(tag.c_str()), forward_as_tuple(new Handshake(NULL, host, addresses, true)));
		itHandshake->second->pTag = &itHandshake->first;
		TRACE("Creating handshake for tag ", String::Hex(BIN itHandshake->second->pTag->c_str(), itHandshake->second->pTag->size()))
	}
	else { // Add the address if unknown
		auto itAddress = itHandshake->second->listAddresses.lower_bound(address);
		if (itAddress == itHandshake->second->listAddresses.end() || itAddress->first != address)
			itHandshake->second->listAddresses.emplace_hint(itAddress, address, RTMFP::ADDRESS_PUBLIC);
	}
	_address.set(address); // set address before sending
	sendHandshake70(tag, itHandshake->second);
}

void RTMFPHandshaker::manage() {
	// TODO: maybe add a timer to not loop every call of manage()

	// Ask server to send p2p addresses
	auto itHandshake = _mapTags.begin();
	while (itHandshake != _mapTags.end()) {
		shared_ptr<Handshake> pHandshake = itHandshake->second;
		switch (pHandshake->status) {
		case RTMFP::STOPPED:
		case RTMFP::HANDSHAKE30:
			
			if (pHandshake->pSession && (!pHandshake->attempt || pHandshake->lastAttempt.isElapsed(pHandshake->attempt * 1500))) {
				if (pHandshake->attempt++ == 11) {
					DEBUG("Connection to ", pHandshake->pSession->name(), " has reached 11 attempt without answer, closing...")
					removeHandshake((itHandshake++)->second);
					continue;
				}

				DEBUG("Sending new handshake 30 to server (target : ", pHandshake->pSession->name(), "; ", pHandshake->attempt, "/11)")
				if (pHandshake->hostAddress) {
					_address.set(pHandshake->hostAddress);
					sendHandshake30(pHandshake->pSession->epd(), itHandshake->first);
				}
				for (auto itAddresses : pHandshake->listAddresses) {
					_address.set(itAddresses.first);
					sendHandshake30(pHandshake->pSession->epd(), itHandshake->first);
				}
				if (pHandshake->status == RTMFP::STOPPED)
					pHandshake->status = RTMFP::HANDSHAKE30;
				pHandshake->lastAttempt.update();
			}
			break;
		case RTMFP::HANDSHAKE38:

			if (pHandshake->pSession && pHandshake->lastAttempt.isElapsed(pHandshake->attempt * 1500)) {
				if (pHandshake->attempt++ == 11) {
					DEBUG("Connection to ", pHandshake->pSession->name(), " has reached 11 attempt without answer, closing...")
					removeHandshake((itHandshake++)->second);
					continue;
				}

				DEBUG("Sending new handshake 38 to ", pHandshake->pSession->address(), " (target : ", pHandshake->pSession->name(), "; ", pHandshake->attempt, "/11)")
				_address.set(pHandshake->pSession->address());
				sendHandshake38(pHandshake, pHandshake->cookieReceived);
				pHandshake->lastAttempt.update();
			}
			break;
		default:
			break;
		}
		++itHandshake;
	}

	// Release cookies after 95s
	auto itCookie = _mapCookies.begin(); 
	while (itCookie != _mapCookies.end()) {
		if (itCookie->second->cookieCreation.isElapsed(95000))
			removeHandshake((itCookie++)->second);
		else
			++itCookie;
	}
}

void RTMFPHandshaker::sendHandshake30(const Binary& epd, const string& tag) {
	// (First packets are encoded with default key)
	BinaryWriter& writer = packet();
	writer.next(3); // header + type and size

	writer.write7BitLongValue(epd.size());
	writer.write(epd.data(), epd.size());

	writer.write(tag);

	BinaryWriter(BIN writer.data(), 3).write8(0x30).write16(writer.size() - 3);
	flush(0x0B);
}

void RTMFPHandshaker::handleHandshake30(BinaryReader& reader) {

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
		String::Assign(peerId, String::Hex(BIN buff.data(), buff.size()));
		if (peerId != _pSession->peerId()) {
			WARN("Incorrect Peer ID in p2p handshake 30 : ", peerId)
			return;
		}
		TRACE("Handshake 30 received from ", _address)

		sendHandshake70(tag, _address, _pSession->address());
	}
}

void RTMFPHandshaker::sendHandshake70(const string& tag, shared_ptr<Handshake>& pHandshake) {

	if (!pHandshake->pCookie) {
		string cookie(COOKIE_SIZE, '\0');
		Util::Random(BIN cookie.data(), COOKIE_SIZE);
		TRACE("Creating cookie ", String::Hex(BIN cookie.data(), cookie.size()))
		auto itCookie = _mapCookies.emplace(piecewise_construct, forward_as_tuple(cookie), forward_as_tuple(pHandshake)).first;
		pHandshake->pCookie = &itCookie->first;
		pHandshake->cookieCreation.update();
	}	

	// Write Response
	BinaryWriter& writer = packet();
	writer.next(3); // header + type and size

	writer.write8(16);
	writer.write(tag);

	writer.write8(COOKIE_SIZE);
	writer.write(BIN pHandshake->pCookie->c_str(), COOKIE_SIZE);

	if (!computePublicKey())
		return;
	
	writer.write7BitValue(_publicKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_publicKey);

	BinaryWriter(BIN writer.data(), 3).write8(0x70).write16(writer.size() - 3);
	flush(0x0B);
	pHandshake->status = RTMFP::HANDSHAKE70;
}

void RTMFPHandshaker::handleHandshake70(BinaryReader& reader) {
	string tagReceived, cookie, farKey;

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		WARN("Unexpected tag size : ", tagSize)
		return;
	}
	reader.read(16, tagReceived);
	auto itHandshake = _mapTags.find(tagReceived);
	if (itHandshake == _mapTags.end()) {
		DEBUG("Unexpected tag received from ", _address, ", possible old request")
		return;
	}
	shared_ptr<Handshake> pHandshake = itHandshake->second;
	if (!pHandshake->pSession) {
		WARN("Unexpected handshake 70 received on responder session")
		return;
	}
	DEBUG("Peer ", pHandshake->pSession->name(), " has answered, handshake continues")

	// Normal NetConnection
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ERROR("Unexpected cookie size : ", cookieSize)
		return;
	}
	reader.read(cookieSize, cookie);

	if (!pHandshake->isP2P) {
		string certificat;
		reader.read(77, certificat);
		DEBUG("Server Certificate : ", String::Hex(BIN certificat.data(), 77))
	}
	else {
		UInt32 keySize = (UInt32)reader.read7BitLongValue() - 2;
		if (keySize != 0x80 && keySize != 0x7F) {
			ERROR("Unexpected responder key size : ", keySize)
			return;
		}
		if (reader.read16() != 0x1D02) {
			ERROR("Unexpected signature before responder key (expected 1D02)")
			return;
		}
		shared_ptr<Buffer> pFarKey(new Buffer(keySize));
		reader.read(keySize, *pFarKey);
		pHandshake->farKey.set(pFarKey);
	}

	// Handshake 70 accepted? => We send the handshake 38
	if (pHandshake->pSession->onPeerHandshake70(_address, pHandshake->farKey, cookie)) {
		pHandshake->cookieReceived.assign(cookie.data(), cookie.size());
		sendHandshake38(pHandshake, pHandshake->cookieReceived);
		pHandshake->attempt = 1;
		pHandshake->lastAttempt.update();
		pHandshake->status = RTMFP::HANDSHAKE38;
		pHandshake->pSession->status = RTMFP::HANDSHAKE38;
	}
}

void RTMFPHandshaker::sendHandshake38(const shared_ptr<Handshake>& pHandshake, const string& cookie) {

	// Write handshake
	BinaryWriter& writer = packet();
	writer.next(3); // header + type and size

	writer.write32(pHandshake->pSession->sessionId()); // id

	writer.write7BitLongValue(cookie.size());
	writer.write(cookie); // Resend cookie

	if (!computePublicKey())
		return;

	writer.write7BitLongValue(_publicKey.size() + 4);

	UInt32 idPos = writer.size();
	writer.write7BitValue(_publicKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_publicKey);

	// Build and save Peer ID if it is RTMFPSession
	pHandshake->pSession->buildPeerID(writer.data() + idPos, writer.size() - idPos);

	const Packet& nonce = pHandshake->pSession->getNonce();
	writer.write7BitValue(nonce.size());
	writer.write(nonce);
	writer.write8(0x58);

	BinaryWriter(BIN writer.data(), 3).write8(0x38).write16(writer.size() - 3);
	flush(0x0B);
}


void RTMFPHandshaker::sendHandshake78(BinaryReader& reader) {

	UInt32 farId = reader.read32(); // id session

	string cookie;
	if (reader.read8() != 0x40) {
		ERROR("Cookie size should be 64 bytes but found : ", *(reader.current() - 1))
		return;
	}
	reader.read(0x40, cookie);
	auto itHandshake = _mapCookies.find(cookie);
	if (itHandshake == _mapCookies.end()) {
		DEBUG("No cookie found for handshake 38, possible old request, ignored")
		return;
	}
	shared_ptr<Handshake> pHandshake = itHandshake->second;

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84)
		DEBUG("Public key size should be 132 bytes but found : ", publicKeySize)
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82)
		DEBUG("Public key size should be 130 bytes but found : ", publicKeySize)
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ERROR("Expected signature 1D02 but found : ", String::Format<UInt16>("%.4x", signature))
		removeHandshake(pHandshake);
		return;
	}
	shared_ptr<Buffer> pFarKey(new Buffer(publicKeySize-2));
	reader.read(publicKeySize - 2, *pFarKey);
	pHandshake->farKey.set(pFarKey);

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ERROR("Responder Nonce size should be 76 bytes but found : ", nonceSize)
		removeHandshake(pHandshake);
		return;
	}
	shared_ptr<Buffer> pNonce(new Buffer(nonceSize));
	reader.read(nonceSize, *pNonce);
	pHandshake->farNonce.set(pNonce);

	UInt8 endByte;
	if ((endByte = reader.read8()) != 0x58) {
		ERROR("Unexpected end byte : ", endByte, " (expected 0x58)")
		removeHandshake(pHandshake);
		return;
	}

	// Build peer ID and update the parent
	string rawId("\x21\x0f"), peerId;
	UInt8 id[PEER_ID_SIZE];
	EVP_Digest(reader.data() + idPos, publicKeySize + 2, id, NULL, EVP_sha256(), NULL);
	rawId.append(STR id, PEER_ID_SIZE);
	String::Assign(peerId, String::Hex(id, PEER_ID_SIZE));
	DEBUG("peer ID calculated from public key : ", peerId)

	// Create the session, if already exists and connected we ignore the request
	if (!_pSession->onNewPeerId(_address, pHandshake, farId, rawId, peerId)) {
		removeHandshake(pHandshake);
		return;
	}
	FlowManager* pSession = pHandshake->pSession;

	// Write Response
	BinaryWriter& writer = packet();
	writer.next(3); // header + type and size

	writer.write32(pSession->sessionId());
	writer.write8(0x49); // nonce is 73 bytes long
	const Packet& nonce = pSession->getNonce();
	writer.write(nonce.data(), nonce.size());
	writer.write8(0x58);

	// Important: send this before computing encoder key because we need the default encoder
	_farId = farId;
	BinaryWriter(BIN writer.data(), 3).write8(0x78).write16(writer.size() - 3);
	flush(0x0B);
	_farId = 0; // reset far Id to default

	// Compute P2P keys for decryption/encryption if not already computed
	if (pSession->status < RTMFP::HANDSHAKE78) {
		pSession->computeKeys(farId);
		pSession->status = RTMFP::HANDSHAKE78;
	}
	pHandshake->status = RTMFP::HANDSHAKE78;
}

void RTMFPHandshaker::handleRedirection(BinaryReader& reader) {

	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ERROR("Unexpected tag size : ", tagSize)
			return;
	}
	string tag;
	reader.read(16, tag);

	auto itTag = _mapTags.find(tag);
	if (itTag == _mapTags.end()) {
		DEBUG("Unexpected tag received from ", _address, ", possible old request")
		return;
	}
	shared_ptr<Handshake> pHandshake(itTag->second);

	if (!pHandshake->pSession) {
		WARN("Unable to find the session related to handshake 71 from ", _address)
		return;
	} else if (pHandshake->pSession->status > RTMFP::HANDSHAKE30) {
		DEBUG("Redirection message ignored, we have already received handshake 70")
		return;
	}
	DEBUG(pHandshake->isP2P ? "Server has sent to us the peer addresses of responders" : "Server redirection messsage, sending back the handshake 30")

	// Read addresses
	SocketAddress hostAddress;
	RTMFP::ReadAddresses(reader, pHandshake->listAddresses, pHandshake->hostAddress, [this, pHandshake, hostAddress, tag](const SocketAddress& address, RTMFP::AddressType type) {
		if (pHandshake->isP2P)
			pHandshake->pSession->addAddress(address, type);

		// Send the handshake 30 to new address
		if (type != RTMFP::ADDRESS_REDIRECTION) {
			_address.set(address);
			sendHandshake30(pHandshake->pSession->epd(), tag);
		}
	});
}

void RTMFPHandshaker::flush(UInt8 marker) {
	if (!_pSender)
		return;
	BandWriter::flush(false, marker);
}

const shared_ptr<Socket>& RTMFPHandshaker::socket(Mona::IPAddress::Family family) { 
	return _pSession->socket(family); 
}

// Return true if the session has failed
bool RTMFPHandshaker::failed() { 
	return _pSession->failed(); 
}

// Remove the handshake properly
void RTMFPHandshaker::removeHandshake(std::shared_ptr<Handshake> pHandshake) {
	TRACE("Deleting ", pHandshake->isP2P ? "P2P" : "", " handshake to ", pHandshake->pSession ? pHandshake->pSession->name() : "unknown session")

	// Set the session to failed state
	if (pHandshake->pSession) {
		pHandshake->pSession->close(true);
		pHandshake->pSession = NULL;
	}

	// We can now erase the handshake object
	if (pHandshake->pCookie)
		_mapCookies.erase(*pHandshake->pCookie);
	if (pHandshake->pTag)
		_mapTags.erase(*pHandshake->pTag);
	pHandshake->pCookie = pHandshake->pTag = NULL;
}

bool RTMFPHandshaker::computePublicKey() {
	if (_publicKey)
		return true;
	
	Exception ex;
	if (!_pSession->diffieHellman().computeKeys(ex)) {
		WARN(ex)
		return false;
	}
	shared_ptr<Buffer> pPubKey(new Buffer(_pSession->diffieHellman().publicKeySize()));
	_pSession->diffieHellman().readPublicKey(pPubKey->data());
	_publicKey.set(pPubKey);
	return true;
}
