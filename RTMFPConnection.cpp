#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPConnection(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent): 
	_nbCreateStreams(0), _waitConnect(false), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {

	_tag.resize(16);
	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes
}

RTMFPConnection::~RTMFPConnection() {
	// Close peers
	for(auto it : _mapPeersByAddress) {
		it.second->close();
	}

	close();

	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
		_pSocket->close();
	}
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

	if (!_hostAddress.set(ex, tmpHost) || !_outAddress.set(_hostAddress))
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

bool RTMFPConnection::connect2Peer(Exception& ex, const char* peerId, const char* streamName) {

	INFO("Connecting to peer ", peerId, "...")
	
	lock_guard<recursive_mutex> lock(_mutexConnections);

	shared_ptr<P2PConnection> pPeerConnection(new P2PConnection(*this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, false));
	
	// Add the command to be send when connection is established
	pPeerConnection->addCommand(NETSTREAM_PLAY, streamName);
	string tag = pPeerConnection->getTag();

	auto it = _mapPeersByTag.emplace(tag, pPeerConnection).first;
	
	// Add the connection request to the queue
	_waitingPeers.push_back(tag);
	return true;
}

bool RTMFPConnection::read(UInt8* buf, UInt32 size, int& nbRead) {
	
	bool res(true);
	if (!(res = readAsync(buf, size, nbRead))  || nbRead>0)
		return res; // quit if treated

	for (auto &it : _mapPeersByAddress) {
		if (!(res = it.second->readAsync(buf, size, nbRead)) || nbRead>0)
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

	if(!_pPublisher) {
		for (auto &it : _mapPeersByAddress) {
			if (it.second->_pPublisher)
				return it.second->_pPublisher->publish(buf, size, pos);
		}
		DEBUG("Can't write data because NetStream is not published")
		return true;
	}

	return _pPublisher->publish(buf, size, pos);
}

void RTMFPConnection::handleStreamCreated(UInt16 idStream) {
	shared_ptr<FlashStream> pStream;
	_pMainStream->addStream(idStream, pStream);

	// Stream created, now we create the flow before sending another request
	string signature;
	signature.append("\x00\x54\x43\x04", 4);
	RTMFP::Write7BitValue(signature, idStream);
	UInt64 id = _flows.size();
	RTMFPFlow * pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
	_waitingFlows[idStream] = pFlow;

	// Send createStream command and add command type to waiting commands
	lock_guard<recursive_mutex> lock(_mutexCommands);
	if (_waitingCommands.empty()) {
		ERROR("created stream without command")
		return;
	}
	const StreamCommand command = _waitingCommands.back();
	switch (command.type) {
	case NETSTREAM_PLAY:
		pFlow->sendPlay(command.value);
		break;
	case NETSTREAM_PUBLISH:
		pFlow->sendPublish(command.value);
		_pPublisher.reset(new Publisher(poolBuffers(), *_pInvoker, command.audioReliable, command.videoReliable));
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
		//DEBUG("Handshake 71 received, ignored for now")
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

	// Write handshake1
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(0x02000000); // id

	writer.write7BitLongValue(cookieSize);
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
	INFO("peer id : \n", Util::FormatHex(_peerId, sizeof(_peerId), LOG_BUFFER))

	Util::Random(_nonce.data(), _nonce.size()); // nonce is a serie of 77 random bytes
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);
	// TODO: see if we need to add 58 at the end + the stable part of nonce/certificate

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
		WARN("Handshake 70 received but no p2p connection found with tag ", tagReceived, " (possible old request)")
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
		WARN("Handshake 71 received but no p2p connection found with tag ", tagReceived, " (possible old request)")
		return true;
	}

	string id((const char*)_peerId, 0x20);
	/*string id = it->second->peerId;
	id = Util::UnformatHex(id);*/

	SocketAddress address;
	while (reader.available() && *reader.current() != 0xFF) {
		UInt8 addressType = reader.read8();
		RTMFP::ReadAddress(reader, address, addressType);
		DEBUG("Address added : ", address.toString(), " (type : ", addressType, ")")

		// Send handshake 30 request to the current address
		_outAddress = address;
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
	if (nonceSize != 0x8B) {
		ex.set(Exception::PROTOCOL, "Incorrect nonce size : ", nonceSize, " (expected 139)");
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
	if (!computeKeys(ex, farPubKey, nonce, _nonce.data(), _nonce.size(), _pDecoder, _pEncoder))
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

		auto it = _mapPeersByAddress.lower_bound(_outAddress);
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

	// Flush writers
	flushWriters();

	auto it = _mapPeersByAddress.begin();
	while (it != _mapPeersByAddress.end()) {
		shared_ptr<P2PConnection>& pPeer(it->second);
		if (pPeer->consumed()) { // delete if dead
			NOTE("Deletion of p2p connection to ", _outAddress.toString())
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

	lock_guard<recursive_mutex> lock(_mutexCommands);
	if(command == NETSTREAM_PUBLISH_P2P) {
		pair<bool,bool> params(audioReliable,videoReliable);
		_mapP2pPublications.emplace(streamName, params);
		return;
	}
		
	_waitingCommands.emplace_back(command, streamName, audioReliable, videoReliable);
	if (command != NETSTREAM_CLOSE)
		_nbCreateStreams++;
}

void RTMFPConnection::createWaitingStreams() {
	lock_guard<recursive_mutex>	lock(_mutexCommands);
	if (!connected || _waitingCommands.empty())
		return;

	// Manage waiting close publication commands
	auto itCommand = _waitingCommands.begin();
	while(itCommand != _waitingCommands.end()) {
		
		// TODO: manage more than one publisher + protect the _pPublisher by mutex
		if(itCommand->type == NETSTREAM_CLOSE) {
			INFO("Unpublishing stream ", itCommand->value, "...")
			if(!_pPublisher)
				ERROR("Unable to find the publisher")
			else
				_pPublisher->unpublish();
			_waitingCommands.erase(itCommand++);
		} else
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
	while (connected && !_waitingPeers.empty()) {

		string& tag = _waitingPeers.front();
		auto it = _mapPeersByTag.find(tag);
		if (it != _mapPeersByTag.end()) {
			INFO("Sending P2P handshake 0 to peer ", it->second->peerId)
			string id = it->second->peerId;
			it->second->sendHandshake0(P2P_HANDSHAKE, Util::UnformatHex(id), tag);
		} else
			ERROR("flusP2PConnection - Unable to find the peer object with tag ", tag)

		_waitingPeers.pop_front();
	}
}

bool RTMFPConnection::onConnect(Mona::Exception& ex) {
	
	// Bind the current port for p2p requests
	/*SocketAddress address(IPAddress::Wildcard(), _pSocket->address().port());
	if (!_pSocket->bind(ex, address))
		return false;*/

	// Record port for setPeerInfo request
	map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
	if (pFlow) {
		INFO("Sending peer info...")
		//pFlow->sendPeerInfo(address.port());
		pFlow->sendPeerInfo(_pSocket->address().port());
	}

	connectSignal.set();
	return true;
}

RTMFPEngine* RTMFPConnection::getDecoder(UInt32 idStream, const SocketAddress& address) {
	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end()) {
		TRACE("P2P RTMFP request")
		return it->second->getDecoder(idStream, address);
	}
	
	TRACE("Normal RTMFP request")
	return FlowManager::getDecoder(idStream, address);
}

bool RTMFPConnection::getPublishStream(const string& streamName,bool& audioReliable,bool& videoReliable) {
	lock_guard<recursive_mutex> lock(_mutexCommands);

	// Search the publication with name streamName
	auto it = _mapP2pPublications.find(streamName);
	if(it != _mapP2pPublications.end()) {
		audioReliable = it->second.first;
		videoReliable = it->second.second;
		return true;
	}

	return false;
}

void RTMFPConnection::handlePlay(const string& streamName,FlashWriter& writer) {
	ERROR("Cannot handle play command on a RTMFP Connection") // target error (shouldn't happen)
}

void RTMFPConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	// Handle 0x0f message from server (a peer is about to contact us)

	if(reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange first 3 bytes")
		return;
	}

	string tmp, peerId;
	reader.read(0x20, tmp);
	Util::FormatHex((const UInt8*)tmp.data(), tmp.size(), peerId);
	SocketAddress address;
	RTMFP::ReadAddress(reader, address, reader.read8());
	
	string tag;
	reader.read(16, tag);
	INFO("A peer will contact us with address : ", address.toString())

	shared_ptr<P2PConnection> pPeerConnection(new P2PConnection(*this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, true));

	pPeerConnection->setTag(tag);
	auto it = _mapPeersByTag.emplace(tag, pPeerConnection).first;
}
