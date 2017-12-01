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
#include "Base/Util.h"

using namespace Base;
using namespace std;

RTMFPHandshaker::RTMFPHandshaker(const Timer& timer, RTMFPSession* pSession) : _pSession(pSession), _name("handshaker"), _first(true), _timer(timer) {
	_onManage = [this](UInt32 count) {
		processManage();
		return DELAY_MANAGE;
	};
}

RTMFPHandshaker::~RTMFPHandshaker() {
	close();
}

void RTMFPHandshaker::close() {

	_timer.set(_onManage, 0);
	_mapTags.clear();
	_mapCookies.clear();
}

void RTMFPHandshaker::receive(const SocketAddress& address, const Packet& packet) {

	_address.set(address); // update address
	BinaryReader reader(packet.data(), packet.size());
	UInt8 marker = reader.read8();
	reader.next(2); // time received

	// Handshake
	if (marker != 0x0B) {
		WARN("Unexpected Handshake marker : ", String::Format<UInt8>("%02x", marker), " received from ", address);
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
		ERROR(_address, " - Unexpected p2p handshake type : ", String::Format<UInt8>("%.2x", (UInt8)type))
		break;
	}
}

bool  RTMFPHandshaker::startHandshake(shared_ptr<Handshake>& pHandshake, const SocketAddress& address, FlowManager* pSession, bool p2p) {
	PEER_LIST_ADDRESS_TYPE mapAddresses;
	return startHandshake(pHandshake, address, mapAddresses, pSession, p2p, false); // direct P2P => no delay
}

bool  RTMFPHandshaker::startHandshake(shared_ptr<Handshake>& pHandshake, const SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, FlowManager* pSession, bool p2p, bool delay) {
	const string& tag = pSession->tag();
	auto itHandshake = _mapTags.lower_bound(tag);
	if (itHandshake == _mapTags.end() || itHandshake->first != tag) {
		itHandshake = _mapTags.emplace_hint(itHandshake, piecewise_construct, forward_as_tuple(tag.c_str(), tag.size()), forward_as_tuple(new Handshake(pSession, address, addresses, p2p, delay)));
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
		itHandshake = _mapTags.emplace_hint(itHandshake, piecewise_construct, forward_as_tuple(tag.data(), tag.size()), forward_as_tuple(new Handshake(NULL, host, addresses, true, false)));
		itHandshake->second->pTag = &itHandshake->first;
		TRACE("Creating handshake for tag ", String::Hex(BIN itHandshake->second->pTag->c_str(), itHandshake->second->pTag->size()))
	}
	else { // Add the address if unknown
		auto itAddress = itHandshake->second->addresses.lower_bound(address);
		if (itAddress == itHandshake->second->addresses.end() || itAddress->first != address)
			itHandshake->second->addresses.emplace_hint(itAddress, address, RTMFP::ADDRESS_PUBLIC);
	}
	_address.set(address); // set address before sending
	sendHandshake70(tag, itHandshake->second);
}

void RTMFPHandshaker::manage() {

	if (_first) {
		processManage();
		_timer.set(_onManage, DELAY_MANAGE);
		_first = false;
	}
}

void RTMFPHandshaker::processManage() {

	// Ask server to send p2p (or host) addresses
	auto itHandshake = _mapTags.begin();
	while (itHandshake != _mapTags.end()) {
		shared_ptr<Handshake> pHandshake = itHandshake->second;
		if (pHandshake->pSession) {

			switch (pHandshake->status) {
			case RTMFP::STOPPED:
			case RTMFP::HANDSHAKE30:

				if (!pHandshake->attempt || pHandshake->lastTry.isElapsed(pHandshake->attempt * 1500)) {
					if (pHandshake->attempt++ >= 11) {
						DEBUG("Connection to ", pHandshake->pSession->name(), " has reached 11 attempt without answer, closing...")
						removeHandshake((itHandshake++)->second);
						continue;
					}

					DEBUG("Sending new handshake 30 to server (session : ", pHandshake->pSession->name(), " attempts : ", pHandshake->attempt, "/11)")

					// Send to host Address
					if (pHandshake->hostAddress) {

						if (!pHandshake->rdvDelayed)
							sendHandshake30(pHandshake->hostAddress, pHandshake->pSession->epd(), itHandshake->first);
						// If it is the 3rd attempt without rendezvous service we disable the delay, in 0.5s there will be the first request with rendezvous service
						else if (pHandshake->attempt == 2) {

							pHandshake->rdvDelayed = false;
							pHandshake->attempt = 0;
						}
					}

					// Send to all addresses
					for (auto& itAddress : pHandshake->addresses)
						sendHandshake30(itAddress.first, pHandshake->pSession->epd(), itHandshake->first);

					if (pHandshake->status == RTMFP::STOPPED)
						pHandshake->status = RTMFP::HANDSHAKE30;
					pHandshake->lastTry.update();
				}

				break;
			case RTMFP::HANDSHAKE38:

				if (pHandshake->lastTry.isElapsed(pHandshake->attempt * 1500)) {
					if (pHandshake->attempt++ == 11) {
						DEBUG("Connection to ", pHandshake->pSession->name(), " has reached 11 handshake 38 without answer, closing...")
						removeHandshake((itHandshake++)->second);
						continue;
					}

					_address.set(pHandshake->pSession->address());
					sendHandshake38(pHandshake, pHandshake->cookieReceived);
				}
				break;
			default:
				break;
			}
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

void RTMFPHandshaker::sendHandshake30(const SocketAddress& address, const Binary& epd, const string& tag) {

	shared<Buffer> pBuffer;
	RTMFP::InitBuffer(pBuffer, 0x0B);
	BinaryWriter writer(*pBuffer);

	writer.write8(0x30).next(2); // header type and size
	writer.write7BitLongValue(epd.size());
	writer.write(epd.data(), epd.size());
	writer.write(tag);

	BinaryWriter(pBuffer->data() + 10, 2).write16(pBuffer->size() - 12);  // write size header

	_address.set(address);
	RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(pBuffer, 0, _address)), _address);
	TRACE("Sending handshake 30 to ", _address)
}

void RTMFPHandshaker::handleHandshake30(BinaryReader& reader) {

	UInt64 peerIdSize = reader.read7BitLongValue();
	if (peerIdSize != 0x22)
		ERROR(_address, " - Unexpected peer id size : ", peerIdSize, " (expected 34)")
	else if ((peerIdSize = reader.read7BitLongValue()) != 0x21)
		ERROR(_address, " - Unexpected peer id size : ", peerIdSize, " (expected 33)")
	else if (reader.read8() != 0x0F)
		ERROR(_address, " - Unexpected marker : ", *reader.current(), " (expected 0x0F)")
	else {

		string buff, peerId, tag;
		reader.read(0x20, buff);
		reader.read(16, tag);
		String::Assign(peerId, String::Hex(BIN buff.data(), buff.size()));
		if (peerId != _pSession->peerId()) {
			WARN(_address, " - Incorrect Peer ID in p2p handshake 30 : ", peerId)
			return;
		}
		TRACE(_address, " - Handshake 30 received, tag : ", String::Hex(BIN tag.data(), tag.size()))

		sendHandshake70(tag, _address, _pSession->address());
	}
}

void RTMFPHandshaker::sendHandshake70(const string& tag, shared_ptr<Handshake>& pHandshake) {

	if (!pHandshake->pCookie) {
		string cookie(COOKIE_SIZE, '\0');
		Util::Random(BIN cookie.data(), COOKIE_SIZE);
		TRACE(_address, " - Creating cookie ", String::Hex(BIN cookie.data(), cookie.size()))
		auto itCookie = _mapCookies.emplace(piecewise_construct, forward_as_tuple(cookie.data(), cookie.size()), forward_as_tuple(pHandshake));
		if (!itCookie.second) {
			WARN("Unable to add duplicate cookie ", String::Hex(BIN cookie.data(), cookie.size()))
			return;
		}
		pHandshake->pCookie = &itCookie.first->first;
		pHandshake->cookieCreation.update();
	}	

	// Write Response
	shared<Buffer> pBuffer;
	RTMFP::InitBuffer(pBuffer, 0x0B);
	BinaryWriter writer(*pBuffer);

	writer.write8(0x70).next(2); // header type and size
	writer.write8(16).write(tag);
	writer.write8(COOKIE_SIZE);
	writer.write(BIN pHandshake->pCookie->c_str(), COOKIE_SIZE);

	if (!computePublicKey())
		return;
	
	writer.write7BitValue(_publicKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_publicKey);

	BinaryWriter(pBuffer->data() + 10, 2).write16(pBuffer->size() - 12);  // write size header
	RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(pBuffer, 0, _address)), _address);
	pHandshake->status = RTMFP::HANDSHAKE70;
}

void RTMFPHandshaker::handleHandshake70(BinaryReader& reader) {
	string tagReceived, cookie, farKey;

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		WARN(_address, " - Unexpected tag size : ", tagSize)
		return;
	}
	reader.read(16, tagReceived);
	auto itHandshake = _mapTags.find(tagReceived);
	if (itHandshake == _mapTags.end()) {
		DEBUG(_address, " - Unexpected tag received from, possible old request")
		return;
	}
	shared_ptr<Handshake> pHandshake = itHandshake->second;
	if (!pHandshake->pSession) {
		WARN(_address, " - Unexpected handshake 70 received on responder session")
		return;
	}
	DEBUG(_address, " - Peer ", pHandshake->pSession->name(), " has answered, handshake continues")

	// Normal NetConnection
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ERROR(_address, " - Unexpected cookie size : ", cookieSize)
		return;
	}
	reader.read(cookieSize, cookie);

	if (!pHandshake->isP2P) {
		string certificat;
		reader.read(77, certificat);
		DEBUG(_address, " - Server Certificate : ", String::Hex(BIN certificat.data(), 77))
	}
	else {
		UInt32 keySize = (UInt32)reader.read7BitLongValue() - 2;
		if (keySize != 0x80 && keySize != 0x7F) {
			ERROR(_address, " - Unexpected responder key size : ", keySize)
			return;
		}
		if (reader.read16() != 0x1D02) {
			ERROR(_address, " - Unexpected signature before responder key (expected 1D02)")
			return;
		}
		pHandshake->farKey.reset(new Buffer(keySize));
		reader.read(keySize, *pHandshake->farKey);
	}

	// Handshake 70 accepted? => We send the handshake 38
	if (pHandshake->pSession->onPeerHandshake70(_address, pHandshake->farKey, cookie)) {
		pHandshake->cookieReceived.assign(cookie.data(), cookie.size());

		// Reset attempt status
		pHandshake->attempt = 1;

		// Send handshake 38
		sendHandshake38(pHandshake, pHandshake->cookieReceived);

		pHandshake->status = RTMFP::HANDSHAKE38;
		pHandshake->pSession->status = RTMFP::HANDSHAKE38;
	}
}

void RTMFPHandshaker::sendHandshake38(const shared_ptr<Handshake>& pHandshake, const string& cookie) {

	DEBUG("Sending new handshake 38 to ", _address, " (target : ", pHandshake->pSession->name(), "; ", pHandshake->attempt, "/11)")
	pHandshake->lastTry.update();

	// Write handshake
	shared<Buffer> pBuffer;
	RTMFP::InitBuffer(pBuffer, 0x0B);
	BinaryWriter writer(*pBuffer);

	writer.write8(0x38).next(2); // header type and size
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

	const shared_ptr<Buffer>& nonce = pHandshake->pSession->getNonce();
	writer.write7BitValue(nonce->size());
	writer.write(*nonce);
	writer.write8(0x58);

	BinaryWriter(pBuffer->data() + 10, 2).write16(pBuffer->size() - 12);  // write size header
	RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(pBuffer, 0, _address)), _address);
}


void RTMFPHandshaker::sendHandshake78(BinaryReader& reader) {

	UInt32 farId = reader.read32(); // id session

	string cookie;
	if (reader.read8() != 0x40) {
		ERROR(_address, " - Cookie size should be 64 bytes but found : ", *(reader.current() - 1))
		return;
	}
	reader.read(0x40, cookie);
	auto itHandshake = _mapCookies.find(cookie);
	if (itHandshake == _mapCookies.end()) {
		DEBUG(_address, " - No cookie found for handshake 38, possible old request, ignored")
		return;
	}
	shared_ptr<Handshake> pHandshake = itHandshake->second;

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84)
		DEBUG(_address, " - Public key size should be 132 bytes but found : ", publicKeySize)
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82)
		DEBUG(_address, " - Public key size should be 130 bytes but found : ", publicKeySize)
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ERROR(_address, " - Expected signature 1D02 but found : ", String::Format<UInt16>("%.4x", signature))
		removeHandshake(pHandshake);
		return;
	}
	pHandshake->farKey.reset(new Buffer(publicKeySize-2));
	reader.read(publicKeySize - 2, *pHandshake->farKey);

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ERROR(_address, " - Responder Nonce size should be 76 bytes but found : ", nonceSize)
		removeHandshake(pHandshake);
		return;
	}
	pHandshake->farNonce.reset(new Buffer(nonceSize));
	reader.read(nonceSize, *pHandshake->farNonce);

	UInt8 endByte;
	if ((endByte = reader.read8()) != 0x58) {
		ERROR(_address, " - Unexpected end byte : ", endByte, " (expected 0x58)")
		removeHandshake(pHandshake);
		return;
	}

	// Build peer ID and update the parent
	string rawId("\x21\x0f"), peerId;
	UInt8 id[PEER_ID_SIZE];
	EVP_Digest(reader.data() + idPos, publicKeySize + 2, id, NULL, EVP_sha256(), NULL);
	rawId.append(STR id, PEER_ID_SIZE);
	String::Assign(peerId, String::Hex(id, PEER_ID_SIZE));
	DEBUG(_address, " - peer ID calculated from public key : ", peerId)

	// Create the session, if already exists and connected we ignore the request
	if (!_pSession->onNewPeerId(_address, pHandshake, farId, peerId)) {
		removeHandshake(pHandshake);
		return;
	}
	FlowManager* pSession = pHandshake->pSession;

	// Write Response
	shared<Buffer> pBuffer;
	RTMFP::InitBuffer(pBuffer, 0x0B);
	BinaryWriter writer(*pBuffer);

	writer.write8(0x78).next(2); // header type and size
	writer.write32(pSession->sessionId());
	writer.write8(0x49); // nonce is 73 bytes long
	const shared_ptr<Buffer>& nonce = pSession->getNonce();
	writer.write(*nonce);
	writer.write8(0x58);

	// Important: send this before computing encoder key because we need the default encoder
	BinaryWriter(pBuffer->data() + 10, 2).write16(pBuffer->size() - 12);  // write size header
	RTMFP::Send(*socket(_address.family()), Packet(_pEncoder->encode(pBuffer, farId, _address)), _address);

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
		ERROR(_address, " - Unexpected tag size : ", tagSize)
		return;
	}
	string tag;
	reader.read(16, tag);

	auto itTag = _mapTags.find(tag);
	if (itTag == _mapTags.end()) {
		DEBUG(_address, " - Unexpected tag received, possible old request")
		return;
	}
	shared_ptr<Handshake> pHandshake(itTag->second);

	if (!pHandshake->pSession) {
		WARN(_address, " - Unable to find the session related to handshake 71")
		return;
	} else if (pHandshake->pSession->status > RTMFP::HANDSHAKE30) {
		DEBUG(_address, " - Redirection message ignored, we have already received handshake 70")
		return;
	}
	DEBUG(_address, " - ", pHandshake->isP2P ? "Server has sent to us the peer addresses of " : "Server redirection message from ", pHandshake->pSession->name())

	// Read addresses
	bool disconnected(false);
	SocketAddress hostAddress;
	RTMFP::ReadAddresses(reader, pHandshake->addresses, pHandshake->hostAddress, [this, pHandshake, hostAddress, tag, &disconnected](const SocketAddress& address, RTMFP::AddressType type) {

		// Trick with MonaServer to delete a disconnected peer
		if (!address || type == RTMFP::ADDRESS_UNSPECIFIED) {
			disconnected = true;
			return;
		}

		// Add address to session and handshake (TODO: can be redundant)
		pHandshake->pSession->addAddress(address, type);

		if ((type & 0x0f) != RTMFP::ADDRESS_REDIRECTION)
			sendHandshake30(address, pHandshake->pSession->epd(), tag);
	});

	if (disconnected)
		_pSession->handlePeerDisconnection(pHandshake->pSession->name());
}

const shared_ptr<Socket>& RTMFPHandshaker::socket(Base::IPAddress::Family family) { 
	return _pSession->socket(family); 
}

// Return true if the session has failed
bool RTMFPHandshaker::failed() { 
	return _pSession->failed(); 
}

// Remove the handshake properly
void RTMFPHandshaker::removeHandshake(std::shared_ptr<Handshake> pHandshake) {
	TRACE("Deleting ", pHandshake->isP2P ? "P2P" : "", " handshake to ", pHandshake->pSession ? pHandshake->pSession->name() : "unknown session")

	// Close the session if needed
	if (pHandshake->pSession) {
		FlowManager* pSession = pHandshake->pSession;
		pHandshake->pSession = NULL; // reset pSession pointer to not loop
		// Session p2p : 90s before retrying (avoid p2p rendez-vous overload), otherwise 19s is sufficient
		pSession->close(pHandshake->isP2P? false : true, RTMFP::OTHER_EXCEPTION);
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
