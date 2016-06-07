#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "Listener.h"
#include "NetGroup.h"
#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) : _pListener(NULL),
	_nbCreateStreams(0), _waitConnect(false), p2pPublishReady(false), publishReady(false), connectReady(false), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
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
	for(auto it : _mapPeersByAddress) {
		it.second->close();
	}

	// Close listener & publisher
	if (_pListener && _pPublisher) {
		_pPublisher->removeListener(_hostAddress.toString());
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

RTMFPFlow* RTMFPConnection::createSpecialFlow(UInt64 id, const string& signature) {
	if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true);
		RTMFPFlow* pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		return pFlow;
	}
	return NULL;
}

bool RTMFPConnection::connect(Exception& ex, const char* url, const char* host) {
	if (!_pInvoker) {
		ex.set(Exception::APPLICATION, "Invoker is not initialized");
		return false;
	}

	 _url = url;
	string tmpHost = host;
	if (!strrchr(host, ':'))
		tmpHost += ":1935"; // default port

	if (!_hostAddress.setWithDNS(ex, tmpHost) || !_outAddress.set(_hostAddress))
		return false;

	_pSocket.reset(new UDPSocket(_pInvoker->sockets, true));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	{
		// Wait the next handle before sending first handshake request
		lock_guard<recursive_mutex> lock(_mutexConnections);
		_waitConnect=true;
	}
	return !ex;
}

void RTMFPConnection::connect2Peer(const char* peerId, const char* streamName) {

	INFO("Connecting to peer ", peerId, "...")
	
	lock_guard<recursive_mutex> lock(_mutexConnections);

	shared_ptr<P2PConnection> pPeerConnection(new P2PConnection(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, false));
	
	// Record the group specifier ID if it is a NetGroup
	if (_group) {
		pPeerConnection->setGroup(_group);
		_group->addPeer(peerId, pPeerConnection);
	} else
		pPeerConnection->addCommand(NETSTREAM_PLAY, streamName); // command to be send when connection is established
	const string& tag = pPeerConnection->getTag();

	// Add it to waiting p2p sessions
	_mapPeersByTag.emplace(tag, pPeerConnection);
	
	// Add the connection request to the queue
	_waitingPeers.push_back(tag);
}

void RTMFPConnection::connect2Group(const char* netGroup, const char* streamName, bool publisher, double availabilityUpdatePeriod, UInt16 windowDuration) {

	INFO("Connecting to group ", netGroup, "...")

	// Keep the meanful part of the group ID (before end marker)
	const char* endMarker = strstr(netGroup, "00");
	if (!endMarker) {
		ERROR("Group ID not well formated")
		return;
	}
	string groupTxt(netGroup, endMarker), groupHex;

	// Check if it is a v2 groupspec version
	bool groupV2 = strncmp("G:027f02", netGroup, 8) == 0;

	// Compute the encrypted group specifier ID (2 consecutive sha256)
	UInt8 encryptedGroup[32];
	EVP_Digest(groupTxt.data(), groupTxt.size(), (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL);
	if (groupV2)
		EVP_Digest(encryptedGroup, 32, (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL); // v2 groupspec needs 2 sha256
	Util::FormatHex(encryptedGroup, 32, groupHex);
	DEBUG("Encrypted Group Id : ", groupHex)

	lock_guard<recursive_mutex> lock(_mutexConnections);
	_group.reset(new NetGroup(groupHex, groupTxt, streamName, publisher, *this, availabilityUpdatePeriod, windowDuration));
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

void RTMFPConnection::handleStreamCreated(UInt16 idStream) {
	DEBUG("Stream ", idStream, " created, sending command...")

	// Get command
	lock_guard<recursive_mutex> lock(_mutexCommands);
	if (_waitingCommands.empty()) {
		ERROR("created stream without command")
		return;
	}
	const StreamCommand command = _waitingCommands.back();

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
			DEBUG("Redirection messsage, sending back the handshake 0")
			UInt8 tagSize = reader.read8();
			if (tagSize != 16) {
				ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
				return;
			}
			string tagReceived;
			reader.read(16, tagReceived);
			if (String::ICompare(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) {
				ex.set(Exception::PROTOCOL, "Unexpected tag received : ", tagReceived);
				return;
			}
			SocketAddress address;
			while (reader.available() && *reader.current() != 0xFF) {
				UInt8 addressType = reader.read8();
				RTMFP::ReadAddress(reader, address, addressType);
				DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")

				// Send handshake 30 request to the current address
				_outAddress = address;
				sendHandshake0(BASE_HANDSHAKE, _url, _tag);
			}
		} else
			sendP2pRequests(ex, reader); 
		break;
	case 0x78:
		sendConnect(ex, reader); break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected handshake type : ", type);
		break;
	}
}

void RTMFPConnection::sendHandshake1(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep != 1) {
		ex.set(Exception::PROTOCOL, "Unexpected Handshake 70 received at step ", _handshakeStep);
		return;
	}

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ex.set(Exception::PROTOCOL, "Unexpected tag size : ", tagSize);
		return;
	}
	string tagReceived;
	reader.read(16, tagReceived);
	if (String::ICompare(tagReceived.c_str(), (const char*)_tag.data(), 16) != 0) {
		ex.set(Exception::PROTOCOL, "Unexpected tag received : ", tagReceived);
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
	_diffieHellman.readPublicKey(ex, _pubKey.data());
	writer.write7BitLongValue(_pubKey.size() + 4);

	UInt32 idPos = writer.size();
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	EVP_Digest(writer.data()+idPos, writer.size()-idPos, _peerId, NULL, EVP_sha256(), NULL);
	INFO("peer id : \n", Util::FormatHex(_peerId, sizeof(_peerId), _peerTxtId))

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
	if (tagSize != 16) {
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
	itAddress->second->initiatorHandshake70(ex, reader, _outAddress);
	if (ex)
		return false;

	// Delete the temporary pointer
	_mapPeersByTag.erase(it);
	return true;
}

bool RTMFPConnection::sendP2pRequests(Exception& ex, BinaryReader& reader) {
	DEBUG("Server has sent to us the peer addresses")

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

	string id = it->second->peerId.c_str(); // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
	id = Util::UnformatHex(id);

	SocketAddress address;
	while (reader.available() && *reader.current() != 0xFF) {
		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")

		// Send handshake 30 request to the current address
		it->second->_outAddress = address;
		it->second->sendHandshake0(P2P_HANDSHAKE, id, tagReceived);
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
	if (String::ICompare(nonce, "\x03\x1A\x00\x00\x02\x1E\x00", 7) != 0) {
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

		string peerId, tag;
		reader.read(0x20, peerId);
		reader.read(16, tag);
		INFO("P2P Connection request from ", _outAddress.toString())

		auto it = _mapPeersByAddress.find(_outAddress);
		if (it != _mapPeersByAddress.end()) {
			WARN("The peer is already connected to us (same address)")
			return;
		}

		auto itTag = _mapPeersByTag.find(tag);
		if (itTag == _mapPeersByTag.end()) {
			WARN("No p2p waiting connection found (possibly already connected)")
			return;
		}

		// Add the connection to map by addresses and send the handshake 38
		it = _mapPeersByAddress.emplace(_outAddress, itTag->second).first;

		// Delete the temporary pointer
		_mapPeersByTag.erase(itTag);

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
			NOTE("Deletion of p2p connection to ", _outAddress.toString())
			if (_group)
				_group->removePeer(pPeer->peerId);
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
				_pPublisher->removeListener(_hostAddress.toString());
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

	// Send normal connection request
	if(_waitConnect) {
		INFO("Connecting to ", _hostAddress.toString(), "...")
		sendHandshake0(BASE_HANDSHAKE, _url, _tag);
		_waitConnect=false;
	}

	// Send waiting p2p connections
	string id;
	while (connected && !_waitingPeers.empty()) {

		string& tag = _waitingPeers.front();
		auto it = _mapPeersByTag.find(tag);
		if (it != _mapPeersByTag.end()) {
			INFO("Sending P2P handshake 30 to server (peerId : ", it->second->peerId, ")")
			id = it->second->peerId.c_str(); // To avoid memory sharing we use c_str() (copy-on-write implementation on linux)
			it->second->sendHandshake0(P2P_HANDSHAKE, Util::UnformatHex(id), tag);
			it->second->lastTry.start();
			it->second->attempt++;
		} else
			ERROR("flusP2PConnection - Unable to find the peer object with tag ", tag)

		_waitingPeers.pop_front();
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
		if (!itPeer->second->_responder && (itPeer->second->lastTry.elapsed() > 1500)) { // initiators
			if (itPeer->second->attempt > 5) {
				ERROR("P2P handshake has reached 6 attempts without answer, deleting session...")
				_mapPeersByTag.erase(itPeer++);
				continue;
			}

			INFO("Sending new P2P handshake 30 to server (peerId : ", itPeer->second->peerId, ")")
			string id = itPeer->second->peerId;  // To avoid memory sharing (linux)
			itPeer->second->_outAddress = _hostAddress;
			itPeer->second->sendHandshake0(P2P_HANDSHAKE, Util::UnformatHex(id), itPeer->first);
			itPeer->second->lastTry.restart();
			itPeer->second->attempt++;
		}
		++itPeer;
	}
}

bool RTMFPConnection::onConnect(Mona::Exception& ex) {

	// Record port for setPeerInfo request
	map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
	if (pFlow) {
		INFO("Sending peer info...")
		pFlow->sendPeerInfo(_pSocket->address().port());
	}

	// We are connected : unlock the possible blocking RTMFP_Connect function
	connectReady = true;
	connectSignal.set();
	return true;
}

void RTMFPConnection::onPublished(FlashWriter& writer) {
	Exception ex;
	_pPublisher->start();
	if (!(_pListener = _pPublisher->addListener<FlashListener, FlashWriter&>(ex, _hostAddress.toString(), writer)))
		WARN(ex.error())

	publishReady = true;
	publishSignal.set();
}

RTMFPEngine* RTMFPConnection::getDecoder(UInt32 idStream, const SocketAddress& address) {
	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end()) {
		//TRACE("P2P RTMFP request")
		return it->second->getDecoder(idStream, address);
	}
	
	//TRACE("Normal RTMFP request")
	return FlowManager::getDecoder(idStream, address);
}
/*
Listener* RTMFPConnection::startListening(Mona::Exception& ex, const std::string& streamName, const std::string& peerId, FlashWriter& writer) {
	
	if (!_pPublisher || _pPublisher->name() != streamName) {
		ex.set(Exception::APPLICATION, "No publication found with name ", streamName);
		return NULL;
	}

	_pPublisher->start();
	return _pPublisher->addListener<>(ex, peerId, writer);
}*/

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
	if (!_group || _group->idHex != groupId) {
		ERROR("Unable to find the stream name of group ID : ", groupId)
		return;
	}
	connect2Peer(peerId.c_str(), _group->stream.c_str());
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
	string peerId;
	reader.read(0x20, peerId);
	SocketAddress address;
	RTMFP::ReadAddress(reader, address, reader.read8());

	string tag;
	reader.read(16, tag);
	INFO("A peer will contact us with address : ", address.toString())

	shared_ptr<P2PConnection> pPeerConnection(new P2PConnection(this, "unknown", _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, true));

	pPeerConnection->setTag(tag);
	auto it = _mapPeersByTag.emplace(tag, pPeerConnection).first;
}

void RTMFPConnection::sendGroupConnection(const string& netGroup) {

	string signature("\x00\x47\x43", 3);
	RTMFPFlow* pFlow = createFlow(signature);
	if (!pFlow)
		return;

	pFlow->sendGroupConnect(netGroup);
}

void RTMFPConnection::updatePeerId(const Mona::SocketAddress& peerAddress, const string& peerId) {
	if (_group) {
		auto it = _mapPeersByAddress.find(peerAddress);
		if (it == _mapPeersByAddress.end())
			ERROR("Unable to find the peer with address ", peerAddress.toString())
		else {

			// Add the peer to group if it is a NetGroup
			it->second->setGroup(_group);
			_group->addPeer(peerId, it->second);
		}
	}
}
