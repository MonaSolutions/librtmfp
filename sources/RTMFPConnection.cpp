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

#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "Listener.h"
#include "NetGroup.h"
#include "Mona/DNS.h"
#include "Mona/Logs.h"
#include "librtmfp.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) : _pListener(NULL), _connectAttempt(0),
	_nbCreateStreams(0), _port("1935"), p2pPublishReady(false), publishReady(false), connectReady(false), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onStreamCreated = [this](UInt16 idStream) {
		handleStreamCreated(idStream);
	};
	onNewPeer = [this](const string& peerId) {
		handleNewGroupPeer(peerId);
	};

	_tag.resize(16);
	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes

	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnNewPeer::subscribe(onNewPeer);
}

RTMFPConnection::~RTMFPConnection() {

	// Unsubscribing to socket : we don't want to receive packets anymore
	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
	}

	// Close the NetGroup
	if (_group)
		_group->close();

	// Close peers
	for(auto it : _mapPeersById)
		it.second->close(true);
	_mapPeersById.clear();
	_mapPeersByAddress.clear();

	// Close listener & publisher
	if (_pListener && _pPublisher) {
		_pPublisher->removeListener(_targetAddress.toString());
		_pListener = NULL;
	}
	if (_pPublisher && _pPublisher->running())
		_pPublisher->stop();
	_pPublisher.reset();

	close();

	// Close socket
	if (_pSocket)
		_pSocket->close();

	if (_pMainStream) {
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream->OnNewPeer::unsubscribe(onNewPeer);
	}
}

RTMFPFlow* RTMFPConnection::createSpecialFlow(Exception& ex, UInt64 id, const string& signature) {

	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		DEBUG("Creating new Flow (", id, ") for NetConnection ", name())
		return new RTMFPFlow(id, signature, poolBuffers(), *this, _pMainStream);
	}
	else if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true); // TODO: see if it is really a GroupStream
		return new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
	}
	else {
		string tmp;
		ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	}
	return NULL;
}

void RTMFPConnection::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	FlowManager::initWriter(pWriter);

	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // newWriter will be associated to the NetConnection flow (first in _flow lists) 

	if (pWriter->signature.size() > 2 && pWriter->signature.compare(0, 3, "\x00\x47\x43", 3) == 0 && !_pGroupWriter)
		_pGroupWriter = pWriter; // save the group writer (TODO: see if it can happen, I think we are always the initiator of the group communication)
	if (pWriter->signature.size() > 4 && pWriter->signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0 && !_pMainWriter)
		_pMainWriter = pWriter;
}

bool RTMFPConnection::connect(Exception& ex, const char* url, const char* host) {
	if (!_pInvoker) {
		ex.set(Exception::APPLICATION, "Invoker is not initialized");
		return false;
	}

	_url = url;
	RTMFP::Write7BitValue(_rawUrl, strlen(url) + 1);
	String::Append(_rawUrl, '\x0A', url);
	string tmpHost = host;
	const char* port = strrchr(host, ':');
	if (port) {
		_port = (port + 1);
		tmpHost[port - host] = '\0';
	}

	// TODO: Create an RTMFPConnection for each _host.addresses()
	lock_guard<recursive_mutex> lock(_mutexConnections);
	DEBUG("Trying to resolve the address...")
	if (_targetAddress.set(ex, tmpHost, _port) || DNS::Resolve(ex, tmpHost, _host) || _targetAddress.setWithDNS(ex, tmpHost, _port)) {
		_pSocket.reset(new UDPSocket(_pInvoker->sockets));
		_pSocket->OnError::subscribe(onError);
		_pSocket->OnPacket::subscribe(onPacket);
		return true;
	}
	return false;
}

shared_ptr<P2PConnection> RTMFPConnection::createP2PConnection(const char* peerId, const char* streamOrTag, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& host, bool responder) {
	INFO("Connecting to peer ", peerId, "...")

	lock_guard<recursive_mutex> lock(_mutexConnections);
	shared_ptr<P2PConnection> pPeerConnection(new P2PConnection(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, addresses, host, responder, (bool)_group));
	
	if (responder)
		pPeerConnection->setTag(streamOrTag);
	else {
		if (!_group)
			pPeerConnection->addCommand(NETSTREAM_PLAY, streamOrTag); // command to be send when connection is established

		// Add it to waiting p2p sessions
		string tag = pPeerConnection->tag();
		_mapPeersByTag.emplace(tag, pPeerConnection);
	}

	return pPeerConnection;
}

void RTMFPConnection::connect2Peer(const char* peerId, const char* streamName) {
	PEER_LIST_ADDRESS_TYPE addresses;
	std::shared_ptr<P2PConnection> pPeer = createP2PConnection(peerId, streamName, addresses, _targetAddress, false);
	
	// Add the connection request to the queue
	lock_guard<recursive_mutex> lock(_mutexConnections);
	if (pPeer)
		_waitingPeers.emplace(pPeer->tag());
}

void RTMFPConnection::connect2Peer(const char* peerId, const char* streamName, const string& rawId, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress) {
	// Check if the peer is not already in the waiting queue
	for (auto it = _mapPeersByTag.begin(); it != _mapPeersByTag.end(); it++) {
		if (it->second->peerId == peerId) {
			TRACE("Connection ignored, we are already connecting to ", peerId)
			return;
		}
	}

	// Check if the peer is not already connecting/connected
	auto itPeer = _mapPeersById.find(peerId);
	if (itPeer != _mapPeersById.end()) {
		if (!itPeer->second->connected) {
			TRACE("Connection ignored, we are already connecting to ", peerId)
			return;
		}
		else if (_group) { // Group reconnection
			DEBUG("Peer ", peerId, " already connected, sending NetGroup Media Subscription...")
			if (_group->addPeer(peerId, itPeer->second))
				_group->sendGroupMedia(itPeer->second);
			return;
		}
	}

	std::shared_ptr<P2PConnection> pPeer = createP2PConnection(peerId, streamName, addresses, _targetAddress, false);
	pPeer->updateHostAddress(hostAddress);

	// Send the handshake 30 to peer
	for (auto itAddress : addresses) {
		DEBUG("Sending p2p handshake 30 to peer at ", itAddress.first.toString())
		pPeer->setOutAddress(itAddress.first);
		pPeer->sendHandshake0(rawId, pPeer->tag());
	}
	pPeer->lastTry.update();
	pPeer->attempt++;
}

void RTMFPConnection::connect2Group(const char* streamName, RTMFPGroupConfig* parameters) {

	INFO("Connecting to group ", parameters->netGroup, "...")

	if (strncmp("G:", parameters->netGroup, 2) != 0) {
		ERROR("Group ID not well formated, it must begin with 'G:'")
		return;
	}

	string value;
	bool groupV2 = false;
	const char* endMarker = NULL;

	// Create the reader of NetGroup ID
	Buffer buff(strlen(parameters->netGroup));
	Util::UnformatHex(BIN (parameters->netGroup + 2), strlen(parameters->netGroup), buff);
	BinaryReader reader(buff.data(), buff.size());

	// Read each NetGroup parameters and save group version + end marker
	while (reader.available() > 0) {
		UInt8 size = reader.read8();
		if (size == 0) {
			endMarker = parameters->netGroup + 2*reader.position();
			break;
		}
		else if (reader.available() < size)
			break;

		reader.read(size, value);
		if (!groupV2 && value == "\x7f\x02")
			groupV2 = true;
	}

	// Keep the meanful part of the group ID (before end marker)
	if (!endMarker) {
		ERROR("Group ID not well formated")
		return;
	}
	string groupTxt(parameters->netGroup, endMarker), groupHex;

	// Compute the encrypted group specifier ID (2 consecutive sha256)
	UInt8 encryptedGroup[32];
	EVP_Digest(groupTxt.data(), groupTxt.size(), (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL);
	if (groupV2)
		EVP_Digest(encryptedGroup, 32, (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL); // v2 groupspec needs 2 sha256
	Util::FormatHex(encryptedGroup, 32, groupHex);
	DEBUG("Encrypted Group Id : ", groupHex)

	lock_guard<recursive_mutex> lock(_mutexConnections);
	_group.reset(new NetGroup(groupHex, groupTxt, streamName, *this, parameters));
	_waitingGroup.push_back(groupHex);
}

bool RTMFPConnection::read(const char* peerId, UInt8* buf, UInt32 size, int& nbRead) {
	
	bool res(true);
	if (!(res = readAsync(peerId, buf, size, nbRead))  || nbRead>0)
		return res; // quit if treated

	for (auto &it : _mapPeersById) {
		if (!(res = it.second->readAsync(peerId, buf, size, nbRead)) || nbRead>0)
			return res; // quit if treated
	}

	return true;
}

bool RTMFPConnection::write(const UInt8* buf, UInt32 size, int& pos) {
	pos = 0;
	if (_died) {
		pos = -1;
		return false; // to stop the parent loop
	}

	if(!_pPublisher || !_pPublisher->count()) {
		DEBUG("Can't write data because NetStream is not published")
		return true;
	}

	return _pPublisher->publish(buf, size, pos);
}

unsigned int RTMFPConnection::callFunction(const char* function, int nbArgs, const char** args, const char* peerId) {
	// Server call
	if (!peerId && _pMainStream && _pMainWriter)
		_pMainStream->callFunction(*_pMainWriter, function, nbArgs, args);
	// NetGroup call
	else if (strcmp(peerId, "all") == 0) {
		if (_group)
			return _group->callFunction(function, nbArgs, args);
	}
	// Peer call
	else {
		for (auto &it : _mapPeersById) {
			if (it.second->peerId == peerId)
				return it.second->callFunction(function, nbArgs, args);
		}
		ERROR("Unable to find the peer", peerId, " for sending the function call")
	}

	return 0;
}

void RTMFPConnection::handleStreamCreated(UInt16 idStream) {
	DEBUG("Stream ", idStream, " created, sending command...")

	// Get command
	lock_guard<recursive_mutex> lock(_mutexCommands);
	if (_waitingCommands.empty()) {
		ERROR("created stream without command")
		return;
	}
	const StreamCommand& command = _waitingCommands.back();

	// Manage publisher if it is a publish command
	if (command.type == NETSTREAM_PUBLISH) {
		if (_pPublisher) {
			ERROR("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			_waitingCommands.pop_back();
			return;
		}
	}

	// Stream created, now we create the writer before sending another request
	string signature;
	signature.append("\x00\x54\x43\x04", 4);
	RTMFP::Write7BitValue(signature, idStream);
	RTMFPWriter* pWriter = new RTMFPWriter(FlashWriter::OPENED, signature, *this); // (a shared pointer is automatically created)

	// Send createStream command and remove command type from waiting commands
	switch (command.type) {
	case NETSTREAM_PLAY: {
		AMFWriter& amfWriter = pWriter->writeInvocation("play", true);
		amfWriter.amf0 = true; // Important for p2p unicast play
		amfWriter.writeString(command.value.c_str(), command.value.size());
		pWriter->flush();
		break;
	}
	case NETSTREAM_PUBLISH: {
		AMFWriter& amfWriter = pWriter->writeInvocation("publish", true);
		amfWriter.writeString(command.value.c_str(), command.value.size());
		pWriter->flush();
		_pPublisher.reset(new Publisher(command.value, *_pInvoker, command.audioReliable, command.videoReliable));
		break;
	}
	default:
		ERROR("Unexpected command found on stream creation : ", command.type)
		return;
	}
	_waitingCommands.pop_back();
}

void RTMFPConnection::handleMessage(Exception& ex, const PoolBuffer& pBuffer, const SocketAddress& address) {
	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end()) {
		shared_ptr<P2PConnection> pPeer = it->second; // save peer object for eventual deletions
		pPeer->handleMessage(ex, pBuffer, address);
	} else
		FlowManager::handleMessage(ex, pBuffer, address);
}

void RTMFPConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		INFO("A peer is trying to contact us from ", _outAddress.toString())
		responderHandshake0(ex, reader); break; // p2p first handshake from initiator
	case 0x70:
		if (connected) // P2P handshake ?
			onP2PHandshake70(ex, reader, _outAddress);
		else
			sendHandshake1(ex, reader); 
		break;
	case 0x71:
		if (!connected)
			handleRedirection(ex, reader);
		else
			sendP2pRequests(ex, reader); // it is a p2p redirection
		break;
	case 0x78:
		sendConnect(ex, reader); break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected handshake type : ", type);
		break;
	}
}

void RTMFPConnection::handleRedirection(Exception& ex, BinaryReader& reader) {
	if (_handshakeStep > 1) {
		DEBUG("Redirection message ignored, we have already received handshake 70")
		return;
	}

	DEBUG("Redirection messsage, sending back the handshake 0")
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if (memcmp(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) {
		ex.set(Exception::PROTOCOL, "Unexpected tag received");
		return;
	}
	SocketAddress address;
	while (reader.available() && *reader.current() != 0xFF) {
		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")
		if (address.family() == IPAddress::IPv6) {
			DEBUG("Ignored address ", address.toString(), ", IPV6 not supported yet") // TODO: support IPV6
			continue;
		}

		// Send handshake 30 request to the current address
		_outAddress = address;
		sendHandshake0(_rawUrl, _tag);
	}
}

void RTMFPConnection::sendHandshake1(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected Handshake 70 received at step ", _handshakeStep);
		return;
	}

	if (_outAddress != _targetAddress) {
		INFO("Updating server address to ", _outAddress.toString())
		_targetAddress = _outAddress;
	}

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if (memcmp(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) {
		ex.set(Exception::PROTOCOL, "Unexpected tag received");
		return;
	}

	// Normal NetConnection
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL, "Unexpected cookie size : ", cookieSize);
		return;
	}
	string cookie;
	reader.read(cookieSize, cookie);

	string certificat;
	reader.read(77, certificat);

	DEBUG("Server Certificate : ", Util::FormatHex(BIN certificat.data(), 77, LOG_BUFFER))

	// Write handshake1
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(0x02000000); // id

	writer.write7BitLongValue(cookie.size());
	writer.write(cookie); // Resend cookie

	if (!_diffieHellman.initialize(ex))
		return;
	_pubKey.resize(_diffieHellman.publicKeySize(ex));
	_diffieHellman.readPublicKey(ex, _pubKey.data());
	writer.write7BitLongValue(_pubKey.size() + 4);

	UInt32 idPos = writer.size();
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	_rawId[0] = '\x21';
	_rawId[1] = '\x0f';
	EVP_Digest(writer.data()+idPos, writer.size()-idPos, _rawId+2, NULL, EVP_sha256(), NULL);
	INFO("Peer ID : \n", Util::FormatHex(_rawId+2, PEER_ID_SIZE, _peerTxtId))

	_nonce.resize(0x4C, false);
	BinaryWriter nonceWriter(_nonce.data(), 0x4C);
	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	Util::Random(_nonce.data() + 5, 64); // nonce 64 random bytes
	nonceWriter.next(64);
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);
	writer.write8(0x58);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	if (!ex) {
		flush(0x0B, writer.size());
		_handshakeStep = 2;
	}
}

bool RTMFPConnection::onP2PHandshake70(Exception& ex, BinaryReader& reader, const SocketAddress& address) {

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 0x10) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return false;
	}
	string tagReceived;
	reader.read(16, tagReceived);

	auto it = _mapPeersByTag.find(tagReceived);
	if (it == _mapPeersByTag.end()) {
		DEBUG("Handshake 70 received but no p2p connection found with tag (possible old request)")
		return true;
	}
	DEBUG("Peer ", it->second->peerId, " has answered, the handshake continues")

	// Add the peer to each map 
	auto itAddress = _mapPeersByAddress.emplace(address, it->second);
	shared_ptr<P2PConnection> pPeer = itAddress.first->second;
	if (!itAddress.second && pPeer->peerId == "unknown") { // update the peer if it already exists
		TRACE("Updating peer ID of the waiting peer at ", address.toString())
		pPeer->peerId = it->second->peerId.c_str();
		pPeer->rawId.append(it->second->rawId.data()+2, it->second->rawId.size()-2);
		pPeer->_knownAddresses = it->second->_knownAddresses;
	}
	_mapPeersById.emplace(pPeer->peerId, pPeer);

	// Create a key for each known addresses of the peer
	for (auto itAddress : pPeer->_knownAddresses)
		_mapPeersByAddress.emplace(itAddress.first, pPeer);

	// Delete the temporary pointer
	_mapPeersByTag.erase(it);

	// Send the handshake 38 and add the peer to heard list
	pPeer->initiatorHandshake70(ex, reader, address);
	if (_group)
		_group->addPeer2HeardList(pPeer->peerId, pPeer->rawId.data(), pPeer->_knownAddresses, pPeer->hostAddress(), true);
	return !ex;
}

bool RTMFPConnection::sendP2pRequests(Exception& ex, BinaryReader& reader) {
	DEBUG("Server has sent to us the peer addresses of responders") // (here we are the initiator)
	
	if (!connected) {
		ex.set(Exception::PROTOCOL, "Handshake type 71 received but the connection is not established");
		return false;
	}

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return false;
	}
	string tagReceived;
	reader.read(16, tagReceived);

	auto it = _mapPeersByTag.find(tagReceived);
	if (it == _mapPeersByTag.end()) {
		DEBUG("Handshake 71 received but no p2p connection found with tag (possible old request)")
		return true;
	}

	// Check if too many attempts
	if (it->second->attempt >= 11) {
		WARN("P2P handshake with ", it->second->peerId, " has reached 11 attempts without answer, deleting session...")
		if (_group)
			_group->removePeer(it->second->peerId);
		_mapPeersByTag.erase(it);
		return false;
	}

	// Send handshake 30 to peer addresses found
	SocketAddress hostAddress;
	if (P2PConnection::ReadAddresses(reader, it->second->_knownAddresses, hostAddress)) {
		for (auto itAddress : it->second->_knownAddresses) {
			DEBUG("Sending p2p handshake 30 to peer at ", itAddress.first.toString())
			it->second->setOutAddress(itAddress.first);
			it->second->sendHandshake0(it->second->rawId, tagReceived);
		}
	}
	// Send handshake to 30 to far peer
	if (hostAddress) {
		DEBUG("Sending p2p handshake 30 to far server (", hostAddress.toString(),") - requesting addresses")
		it->second->updateHostAddress(hostAddress);
		it->second->setOutAddress(hostAddress);
		it->second->sendHandshake0(it->second->rawId, tagReceived);
	}
	it->second->attempt++;
	it->second->lastTry.update();
	return true;
}

bool RTMFPConnection::sendConnect(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 2) {
		ex.set(Exception::PROTOCOL, "Unexpected Handshake 78 received at step ", _handshakeStep);
		return false;
	}

	_farId = reader.read32(); // id session?
	UInt32 nonceSize = (UInt32)reader.read7BitLongValue();
	if (nonceSize < 0x8A) {
		ex.set(Exception::PROTOCOL, "Incorrect nonce size : ", nonceSize, " (expected at least 138 bytes)");
		return false;
	}

	string nonce;
	reader.read(nonceSize, nonce);
	if (memcmp(nonce.data(), "\x03\x1A\x00\x00\x02\x1E\x00", 7) != 0) {
		ex.set(Exception::PROTOCOL, "Incorrect nonce : ", nonce);
		return false;
	}

	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ex.set(Exception::PROTOCOL, "Unexpected end of handshake 2 : ", endByte);
		return false;
	}

	// Compute keys for encryption/decryption
	string farPubKey = nonce.substr(11, nonceSize - 11);
	if (!computeKeys(ex, farPubKey, nonce, _nonce.data(), _nonce.size(), _sharedSecret, _pDecoder, _pEncoder))
		return false;

	string signature("\x00\x54\x43\x04\x00", 5);
	new RTMFPWriter(FlashWriter::OPENED, signature, *this);  // it will be automatically associated to _pMainWriter

	connected = true;
	_pMainStream->connect(*_pMainWriter, _url);
	_handshakeStep = 3;
	return true;
}

void RTMFPConnection::responderHandshake0(Exception& ex, BinaryReader& reader) {

	if (!connected) {
		ex.set(Exception::PROTOCOL, "Handshake 30 received before connection succeed");
		return;
	}

	UInt64 peerIdSize = reader.read7BitLongValue();
	if (peerIdSize != 0x22)
		ex.set(Exception::PROTOCOL, "Unexpected peer id size : ", peerIdSize, " (expected 34)");
	if ((peerIdSize = reader.read7BitLongValue()) != 0x21)
		ex.set(Exception::PROTOCOL, "Unexpected peer id size : ", peerIdSize, " (expected 33)");
	if (reader.read8() != 0x0F)
		ex.set(Exception::PROTOCOL, "Unexpected marker : ", *reader.current(), " (expected 0x0F)");
	else {

		string buff, peerId, tag;
		reader.read(0x20, buff);
		reader.read(16, tag);
		Util::FormatHex(BIN buff.data(), buff.size(), peerId);
		if (peerId != _peerTxtId) {
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
		it->second->responderHandshake0(ex, tag, _outAddress);
	}
}

void RTMFPConnection::manage() {
	if (!_pMainStream)
		return;

	// Treat waiting commands
	createWaitingStreams();

	// Send waiting P2P connections
	sendConnections();

	// Manage NetGroup
	if (_group)
		_group->manage();

	// Flush writers
	flushWriters();

	auto it = _mapPeersById.begin();
	while (it != _mapPeersById.end()) {
		shared_ptr<P2PConnection>& pPeer(it->second);
		if (pPeer->failed()) { // delete if dead
			INFO("Deletion of p2p connection to ", pPeer->peerAddress().toString())
			if (_group)
				_group->removePeer(pPeer->peerId);
			for (auto address : pPeer->_knownAddresses)
				_mapPeersByAddress.erase(address.first); // remove all addresses keys
			_mapPeersById.erase(it++);
			continue;
		}
		// flush writers if not dead
		pPeer->flushWriters();
		++it;
	}
}

// TODO: see if we always need to manage a list of commands
void RTMFPConnection::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {
		
	_waitingCommands.emplace_back(command, streamName, audioReliable, videoReliable);
	if (command != NETSTREAM_CLOSE && command != NETSTREAM_PUBLISH_P2P)
		_nbCreateStreams++;
}

void RTMFPConnection::createWaitingStreams() {
	lock_guard<recursive_mutex>	lock(_mutexCommands);
	if (!connected || _waitingCommands.empty())
		return;

	// Manage waiting close and p2p publication commands
	auto itCommand = _waitingCommands.begin();
	while(itCommand != _waitingCommands.end()) {
		
		if(itCommand->type == NETSTREAM_CLOSE) {
			INFO("Unpublishing stream ", itCommand->value, "...")
			if(!_pPublisher)
				ERROR("Unable to find the publisher")
			else {
				_pPublisher->stop();
				_pPublisher->removeListener(_targetAddress.toString());
			}
			_waitingCommands.erase(itCommand++);
		} 
		else if (itCommand->type == NETSTREAM_PUBLISH_P2P) {
			INFO("Creating publisher for stream ", itCommand->value, "...")
			if (_pPublisher)
				ERROR("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			else
				_pPublisher.reset(new Publisher(itCommand->value, *_pInvoker, itCommand->audioReliable, itCommand->videoReliable));
			_waitingCommands.erase(itCommand++);
		}
		else
			++itCommand;
	}

	// Create waiting streams
	if (_nbCreateStreams) {
		if (!_pMainWriter) {
			ERROR("Unable to find the main writer related to the main stream")
			return;
		}
		_pMainStream->createStream(*_pMainWriter);
		_nbCreateStreams--;
	}
}

void RTMFPConnection::sendConnections() {
	lock_guard<recursive_mutex> lock(_mutexConnections);

	// Send server connection request
	if ((!_host.addresses().empty() || _targetAddress) && _connectAttempt == 0 || _handshakeStep == 1 && _connectAttempt <= 11 && _lastAttempt.isElapsed(1000)) {
		if (_connectAttempt > 10) {
			_connectAttempt++;
			_pOnSocketError("librtmpf has reached 11 attempts of connection to the server without answer");
			return;
		}

		Exception ex;
		if (_host.addresses().empty()) {
			_outAddress.set(_targetAddress);
			INFO("Connecting to ", _targetAddress.toString(), "...")
			sendHandshake0(_rawUrl, _tag);
		}
		else {
			for (auto itAddress : _host.addresses()) {
				if (itAddress.family() == IPAddress::IPv6)
					DEBUG("Ignored address ", itAddress.toString(), ", IPV6 not supported yet") // TODO: support IPV6
				else if (_targetAddress.set(ex, itAddress, _port) && _outAddress.set(_targetAddress)) {
					INFO("Connecting to ", _targetAddress.toString(), "...")
					sendHandshake0(_rawUrl, _tag);
				}
				else
					WARN("Error while reading host address : ", ex.error())
			}
		}
		_connectAttempt++;
		_lastAttempt.update();
	}

	// Send waiting p2p connections
	auto itTag = _waitingPeers.begin();
	while (connected && itTag != _waitingPeers.end()) {

		auto it = _mapPeersByTag.find(*itTag);
		if (it != _mapPeersByTag.end()) {
			INFO("Sending P2P handshake 30 to server (peerId : ", it->second->peerId, ")")
			it->second->setOutAddress(_targetAddress);
			it->second->sendHandshake0(it->second->rawId, *itTag);
			it->second->lastTry.update();
			it->second->attempt++;
		}
		else
			ERROR("Unable to find the peer object with tag ", *itTag)

		itTag = _waitingPeers.erase(itTag);
	}

	// Send waiting group connections
	while (connected && !_waitingGroup.empty()) {
		string& group = _waitingGroup.front();
		sendGroupConnection(group);
		_waitingGroup.pop_front();
	}

	// Send new p2p request if no answer after 1,5s
	// TODO: make the attempt and elapsed count parametrable
	auto itPeer = _mapPeersByTag.begin();
	while (itPeer != _mapPeersByTag.end()) {
		shared_ptr<P2PConnection> pPeer = itPeer->second;
		if (itPeer->first != "unknown" && pPeer->lastTry.isElapsed(pPeer->attempt * 1000)) {
			if (pPeer->attempt >= 11) {
				WARN("P2P handshake with ", pPeer->peerId," has reached 11 attempts without answer, deleting session...")
				if (_group)
					_group->removePeer(pPeer->peerId);
				_mapPeersByTag.erase(itPeer++);
				continue;
			}

			// No peer addresses? => send handshake to server
			if (pPeer->_knownAddresses.empty()) {
				pPeer->setOutAddress(pPeer->hostAddress());
				INFO("Sending new P2P handshake 30 to server at ", pPeer->_outAddress.toString(), " (peerId : ", pPeer->peerId, ")")
				pPeer->sendHandshake0(pPeer->rawId, pPeer->tag());
			}
			for (auto itAddress : pPeer->_knownAddresses) {
				pPeer->setOutAddress(itAddress.first);
				INFO("Sending new P2P handshake 30 to ", pPeer->_outAddress.toString(), " (peerId : ", pPeer->peerId, ")")
				pPeer->sendHandshake0(pPeer->rawId, pPeer->tag());
			}
			pPeer->attempt++;
			pPeer->lastTry.update();
		}
		++itPeer;
	}
}

bool RTMFPConnection::onConnect(Mona::Exception& ex) {

	// Record port for setPeerInfo request
	if (_pMainStream && _pMainWriter)
		_pMainStream->sendPeerInfo(*_pMainWriter, _pSocket->address().port());

	// We are connected : unlock the possible blocking RTMFP_Connect function
	connectReady = true;
	connectSignal.set();
	return true;
}

void RTMFPConnection::onPublished(FlashWriter& writer) {
	Exception ex;
	_pPublisher->start();
	if (!(_pListener = _pPublisher->addListener<FlashListener, FlashWriter&>(ex, _targetAddress.toString(), writer)))
		WARN(ex.error())

	publishReady = true;
	publishSignal.set();
}

RTMFPEngine* RTMFPConnection::getDecoder(UInt32 idStream, const SocketAddress& address) {
	if (address == _targetAddress)
		return FlowManager::getDecoder(idStream, address);

	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end())
		return it->second->getDecoder(idStream, address);

	return FlowManager::getDecoder(idStream, address);
}

void RTMFPConnection::stopListening(const std::string& peerId) {
	INFO("Deletion of the listener to ", peerId)
	if (_pPublisher)
		_pPublisher->removeListener(peerId);
}

bool RTMFPConnection::handlePlay(const string& streamName,FlashWriter& writer) {
	ERROR("Cannot handle play command on a RTMFP Connection") // target error (shouldn't happen)
	return false;
}

void RTMFPConnection::handleNewGroupPeer(const string& peerId) {
	
	//string& streamName = _mapGroup2stream[groupId];
	if (!_group || !_group->checkPeer(peerId)) {
		WARN("Unable to add the peer ", peerId, ", it can be a wrong group ID or the peer already exists")
		return;
	}
	connect2Peer(peerId.c_str(), _group->stream.c_str());
}

void RTMFPConnection::handleProtocolFailed() {
	//writeMessage(0x0C, 0);
	close();
}

void RTMFPConnection::handleWriterFailed(RTMFPWriter* pWriter) {
	Exception ex;
	pWriter->fail(ex, "Writer terminated on connection ", _targetAddress.toString());
	WARN(ex.error())
}

void RTMFPConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	// Handle 0x0f message from server (a peer is about to contact us)

	if (reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange first 3 bytes")
		return;
	}

	// Read our peer id and address of initiator
	string buff, peerId;
	reader.read(PEER_ID_SIZE, buff);
	SocketAddress address;
	RTMFP::ReadAddress(reader, address, reader.read8());

	string tag;
	reader.read(16, tag);
	DEBUG("A peer will contact us with address : ", address.toString())

	// If we are the P2P publisher we wait for the peer to contact us
	if (!_group && (_mapPeersByAddress.find(address) == _mapPeersByAddress.end())) {
		PEER_LIST_ADDRESS_TYPE addresses;
		_mapPeersByTag.emplace(tag, createP2PConnection("unknown", tag.c_str(), addresses, _targetAddress, true));
	}
}

void RTMFPConnection::sendGroupConnection(const string& netGroup) {

	string signature("\x00\x47\x43", 3);
	new RTMFPWriter(FlashWriter::OPENED, signature, *this); // it will be automatically associated to _pGroupWriter

	_pGroupWriter->writeGroupConnect(netGroup);
	_pGroupWriter->flush();
}

bool RTMFPConnection::addPeer2Group(const SocketAddress& peerAddress, const string& peerId) {
	if (_group) {
		auto it = _mapPeersByAddress.find(peerAddress);
		if (it == _mapPeersByAddress.end())
			ERROR("Unable to find the peer with address ", peerAddress.toString())
		else {

			// Inform the NetGroup about the new peer
			if (!_group->addPeer(peerId, it->second))
				return false;
			return true;
		}
	}
	return false;
}

void RTMFPConnection::onPeerConnect(const PEER_LIST_ADDRESS_TYPE& peerAddresses, const SocketAddress& hostAddress, const string& peerId, const char* rawId, const Mona::SocketAddress& address) {
	if (_group) 
		_group->addPeer2HeardList(peerId, rawId, peerAddresses, hostAddress, true); // Inform the NetGroup about the new peer

	// If there is a waiting connexion to that peer, destroy it
	lock_guard<recursive_mutex> lock(_mutexConnections);
	for (auto it = _mapPeersByTag.begin(); it != _mapPeersByTag.end(); it++) {
		if (it->second->peerId == peerId) {
			DEBUG("Deleting the temporary P2PConnection ", peerId, " to ", it->second->_outAddress.toString())
			auto itWait = _waitingPeers.find(it->first);
			if (itWait != _waitingPeers.end())
				_waitingPeers.erase(itWait);

			_mapPeersByTag.erase(it);
			break; // TODO: is it possible to have other waiting connexions for same peer?
		}
	}

	auto itAddress = _mapPeersByAddress.find(address);

	// Add the peer to the map, if it already exists we destroy the duplicate pointer
	auto itPeer = _mapPeersById.emplace(peerId, itAddress->second);
	if (!itPeer.second) {
		DEBUG("The P2PConnection to ", peerId, " already exists, deleting the duplicate pointer...")
		itAddress->second = itPeer.first->second;
	}
}

const string& RTMFPConnection::groupIdHex(){ 
	FATAL_ASSERT(_group)
	return _group->idHex;
}

const string& RTMFPConnection::groupIdTxt() { 
	FATAL_ASSERT(_group)
	return _group->idTxt;
}
