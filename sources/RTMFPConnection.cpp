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
	onNewPeer = [this](const string& groupId, const string& peerId) {
		handleNewGroupPeer(groupId, peerId);
	};

	_tag.resize(16);
	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes

	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnNewPeer::subscribe(onNewPeer);
}

RTMFPConnection::~RTMFPConnection() {

	// Close the NetGroup
	if (_group)
		_group->close();

	// Close peers
	for(auto it : _mapPeersByAddress)
		it.second->close(true);

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
	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
		_pSocket->close();
	}

	if (_pMainStream) {
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream->OnNewPeer::unsubscribe(onNewPeer);
	}
}

RTMFPFlow* RTMFPConnection::createSpecialFlow(Exception& ex, UInt64 id, const string& signature) {
	if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true);
		RTMFPFlow* pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		return pFlow;
	}
	string tmp;
	ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	return NULL;
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
	if (DNS::Resolve(ex, tmpHost, _host) || _targetAddress.setWithDNS(ex, tmpHost, _port)) {
		lock_guard<recursive_mutex> lock(_mutexConnections);
		_pSocket.reset(new UDPSocket(_pInvoker->sockets));
		_pSocket->OnError::subscribe(onError);
		_pSocket->OnPacket::subscribe(onPacket);
		return true;
	}
	return false;
}

shared_ptr<P2PConnection> RTMFPConnection::createP2PConnection(const char* peerId, const char* streamOrTag, const SocketAddress& address, RTMFP::AddressType addressType, bool responder) {
	INFO("Connecting to peer ", peerId, "...")

	lock_guard<recursive_mutex> lock(_mutexConnections);
	shared_ptr<P2PConnection> pPeerConnection(new P2PConnection(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, address, addressType, responder));
	string tag;
	
	if (responder) {
		pPeerConnection->setTag(streamOrTag);
		tag = streamOrTag;
	}
	else {
		if (!_group)
			pPeerConnection->addCommand(NETSTREAM_PLAY, streamOrTag); // command to be send when connection is established

		// Add it to waiting p2p sessions
		tag = pPeerConnection->tag();
		_mapPeersByTag.emplace(tag, pPeerConnection);
	}

	return pPeerConnection;
}

void RTMFPConnection::connect2Peer(const char* peerId, const char* streamName) {
	std::shared_ptr<P2PConnection> pPeer = createP2PConnection(peerId, streamName, _targetAddress, RTMFP::ADDRESS_PUBLIC, false);
	
	// Add the connection request to the queue
	lock_guard<recursive_mutex> lock(_mutexConnections);
	if (pPeer)
		_waitingPeers.push_back(pPeer->tag());
}

void RTMFPConnection::connect2Peer(const char* peerId, const char* streamName, const string& rawId, const SocketAddress& address, RTMFP::AddressType addressType, const SocketAddress& hostAddress) {
	// Check if the peer is not already in the waiting queue
	for (auto it = _mapPeersByTag.begin(); it != _mapPeersByTag.end(); it++) {
		if (it->second->peerId == peerId) {
			TRACE("Connection ignored, we are already connecting to ", peerId)
			return;
		}
	}

	// NetGroup : Check if we are not already connected to peer
	if (_group) {
		for (auto itAddress = _mapPeersByAddress.begin(); itAddress != _mapPeersByAddress.end(); itAddress++) {
			if (itAddress->second->peerId == peerId && itAddress->second->connected) {
				DEBUG("Peer ", itAddress->second->peerId, " already connected, sending NetGroup connection request...")
				if (addPeer2Group(itAddress->first, itAddress->second->peerId))
					itAddress->second->sendGroupPeerConnect();
				return;
			}
		}
	}

	std::shared_ptr<P2PConnection> pPeer = createP2PConnection(peerId, streamName, address, addressType, false);
	pPeer->updateHostAddress(hostAddress);

	// Send the handshake 30 to peer
	if (pPeer) {
		pPeer->sendHandshake0(rawId, pPeer->tag());
		pPeer->lastTry.update();
		pPeer->attempt++;
	}
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

	for (auto &it : _mapPeersByAddress) {
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
	if (!peerId) {
		map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
		RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
		if (pFlow) {
			pFlow->call(function, nbArgs, args);
			return 1;
		}
		ERROR("Unable to find the flow 2 for sending the function call")
	}
	// NetGroup call
	else if (strcmp(peerId, "all") == 0) {
		if (_group)
			return _group->callFunction(function, nbArgs, args);

	}
	// Peer call
	else {
		for (auto &it : _mapPeersByAddress) {
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

	// Create the stream
	shared_ptr<FlashStream> pStream;
	_pMainStream->addStream(idStream, pStream);

	// Stream created, now we create the flow before sending another request
	string signature;
	signature.append("\x00\x54\x43\x04", 4);
	RTMFP::Write7BitValue(signature, idStream);
	UInt64 id = _flows.size();
	RTMFPFlow * pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
	_waitingFlows[idStream] = pFlow;

	// Send createStream command and remove command type from waiting commands
	switch (command.type) {
	case NETSTREAM_PLAY:
		pFlow->sendPlay(command.value);
		break;
	case NETSTREAM_PUBLISH:
		pFlow->sendPublish(command.value);
		_pPublisher.reset(new Publisher(command.value, *_pInvoker, command.audioReliable, command.videoReliable));
		break;
	default:
		ERROR("Unexpected command found on stream creation : ", command.type)
		return;
	}
	_waitingCommands.pop_back();
}

void RTMFPConnection::handleMessage(Exception& ex, const PoolBuffer& pBuffer, const SocketAddress& address) {
	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end())
		it->second->handleMessage(ex, pBuffer, address);
	else
		FlowManager::handleMessage(ex, pBuffer, address);
}

void RTMFPConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		responderHandshake0(ex, reader); break; // p2p first handshake from initiator (in middle mode)
	case 0x70:
		if (connected) { // P2P handshake ?
			handleP2PHandshake(ex, reader);
		} else
			sendHandshake1(ex, reader); 
		break;
	case 0x71:
		if (!connected) {
			handleRedirection(ex, reader);
		} else
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

bool RTMFPConnection::handleP2PHandshake(Exception& ex, BinaryReader& reader) {

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

	INFO("Peer ", it->second->peerId," has answered, the handshake continues")

	// Add the connection to map by addresses and send the handshake 38
	auto itAddress = _mapPeersByAddress.emplace(_outAddress, it->second).first;
	// Delete the temporary pointer
	_mapPeersByTag.erase(it);
	itAddress->second->initiatorHandshake70(ex, reader, _outAddress);
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
			_group->removePeer(it->second->peerId, true);
		_mapPeersByTag.erase(it);
		return false;
	}

	SocketAddress address;
	while (reader.available() && *reader.current() != 0xFF) {
		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")

		// Send handshake 30 request to the current address
		// TODO: Record the address to ignore if it has already been received
		switch (addressType & 0x0F) {
			case RTMFP::ADDRESS_REDIRECTION:
				it->second->updateHostAddress(address);
			case RTMFP::ADDRESS_LOCAL:
			case RTMFP::ADDRESS_PUBLIC:
				DEBUG("Sending p2p handshake 30 to ", ((addressType & 0x0F) != RTMFP::ADDRESS_REDIRECTION ? "peer" : "far server - requesting addresses"))
				it->second->peerType = (RTMFP::AddressType)addressType;
				it->second->_outAddress = address;
				it->second->sendHandshake0(it->second->rawId, tagReceived);
				it->second->attempt++;
				it->second->lastTry.update();
				break;
		}
	}
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
	RTMFPFlow* pFlow = createFlow(2, signature);
	if (!pFlow)
		return false;

	connected = true;
	pFlow->sendConnect(_url);
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
			WARN("Incorrect Peer ID in handshake 70 : ", peerId)
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
			else if (_group) { // NetGroup : we accept direct connexions
				DEBUG("It is a direct P2P connection request from the NetGroup")

				if (!(pPeerConnection = createP2PConnection("unknown", tag.c_str(), _outAddress, RTMFP::ADDRESS_PUBLIC, true)))
					return;
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

	auto it = _mapPeersByAddress.begin();
	while (it != _mapPeersByAddress.end()) {
		shared_ptr<P2PConnection>& pPeer(it->second);
		if (pPeer->failed()) { // delete if dead
			NOTE("Deletion of p2p connection to ", pPeer->peerAddress().toString())
			if (_group)
				_group->removePeer(pPeer->peerId, true);
			_mapPeersByAddress.erase(it++);
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
		INFO("Creating a new stream...")
		map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
		RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
		if(!pFlow) {
			ERROR("Unable to found the flow 2")
			return;
		}
		pFlow->createStream();
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
		if (itPeer->second->lastTry.isElapsed(itPeer->second->attempt * 1000)) {
			if (itPeer->second->attempt >= 11) {
				WARN("P2P handshake with ", itPeer->second->peerId," has reached 11 attempts without answer, deleting session...")
				if (_group)
					_group->removePeer(itPeer->second->peerId, true);
				_mapPeersByTag.erase(itPeer++);
				continue;
			}

			if (itPeer->second->_responder) // responders
				itPeer->second->_outAddress = itPeer->second->hostAddress();
			else // initiators
				itPeer->second->_outAddress = (_group) ? itPeer->second->peerAddress() : _targetAddress;

			INFO("Sending new P2P handshake 30 to ", itPeer->second->_outAddress.toString(), " (peerId : ", itPeer->second->peerId, ")")
			itPeer->second->sendHandshake0(itPeer->second->rawId, itPeer->first);
			itPeer->second->attempt++;
			itPeer->second->lastTry.update();
		}
		++itPeer;
	}
}

bool RTMFPConnection::onConnect(Mona::Exception& ex) {

	// Record port for setPeerInfo request
	map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
	if (pFlow)
		pFlow->sendPeerInfo(_pSocket->address().port());

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

void RTMFPConnection::handleNewGroupPeer(const string& groupId, const string& peerId) {
	
	//string& streamName = _mapGroup2stream[groupId];
	if (!_group || !_group->checkPeer(groupId, peerId)) {
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
	pWriter->fail("Writer terminated on connection");
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
	INFO("A peer will contact us with address : ", address.toString())
}

void RTMFPConnection::sendGroupConnection(const string& netGroup) {

	string signature("\x00\x47\x43", 3);
	RTMFPFlow* pFlow = createFlow(signature);
	if (!pFlow)
		return;

	pFlow->sendGroupConnect(netGroup);
}

void RTMFPConnection::addPeer2HeardList(const SocketAddress& peerAddress, const SocketAddress& hostAddress, const string& peerId, const char* rawId) {
	if (_group)
		_group->addPeer2HeardList(peerId, rawId, peerAddress, RTMFP::ADDRESS_PUBLIC, hostAddress); // Inform the NetGroup about the new peer

	// If there is a waiting connexion to that peer, destroy it
	lock_guard<recursive_mutex> lock(_mutexConnections);
	for (auto it = _mapPeersByTag.begin(); it != _mapPeersByTag.end(); it++) {
		if (it->second->peerId == peerId) {
			for (auto itWait = _waitingPeers.begin(); itWait != _waitingPeers.end(); itWait++) {
				if (*itWait == it->first) {
					_waitingPeers.erase(itWait);
					break;
				}
			}
			_mapPeersByTag.erase(it);
			break;
		}
	}
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
			it->second->setGroup(_group);
			return true;
		}
	}
	return false;
}