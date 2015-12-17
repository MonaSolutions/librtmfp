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
}

RTMFPConnection::~RTMFPConnection() {
	close();
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

	INFO("Connecting to ", _hostAddress.host().toString(), "...")
	if (!_pSocket->bind(ex, SocketAddress::Wildcard())) // TODO: deal with IPV6 too
		return false;

	_pMainStream->setPort(_pSocket->address().port()); // Record port for setPeerInfo request

	{
		// Wait the next handle before sending first handshake request
		lock_guard<recursive_mutex> lock(_mutexConnections);
		_waitConnect=true;
	}
	return !ex;
}

bool RTMFPConnection::connect2Peer(Exception& ex, const char* peerId, const char* streamName) {

	INFO("Connecting to peer ", peerId, "...")

	auto it = _mapPeersById.emplace(piecewise_construct, forward_as_tuple(peerId),forward_as_tuple(*this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, _tag)).first;
	if (!it->second.connect(ex))
		return false;

	// Add the command to be send when connection is established
	it->second.addCommand(NETSTREAM_PLAY, streamName);
	
	// Add the connection request to the queue
	lock_guard<recursive_mutex> lock(_mutexConnections);
	_waitingPeers.push_back(peerId);
	return true;
}

bool RTMFPConnection::read(UInt8* buf, UInt32 size, int& nbRead) {
	
	bool res(true);
	if (!(res = readAsync(buf, size, nbRead))  || nbRead>0)
		return res; // quit if treated

	for (auto &it : _mapPeersById) {
		if (!(res = it.second.readAsync(buf, size, nbRead)) || nbRead>0)
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
			if (it.second._pPublisher)
				return it.second._pPublisher->publish(buf, size, pos);
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
	}
	_waitingCommands.pop_back();
}

void RTMFPConnection::handleMessage(Exception& ex, const PoolBuffer& pBuffer, const SocketAddress& address) {
	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end())
		it->second.handleMessage(ex, pBuffer, address);
	else
		FlowManager::handleMessage(ex, pBuffer, address);
}

void RTMFPConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		responderHandshake0(ex, reader); break; // p2p
	case 0x70:
		sendHandshake1(ex, reader); break;
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
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

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
	pFlow->sendConnect(_url, _pSocket->address().port());
	_handshakeStep = 3;
	return true;
}

void RTMFPConnection::responderHandshake0(Exception& ex, BinaryReader& reader) {

	if (!connected) {
		ex.set(Exception::PROTOCOL, "Handshake 30 received before connection succeed");
		return;
	}

	auto it = _mapPeersByAddress.lower_bound(_outAddress);
	if (it != _mapPeersByAddress.end()) {
		ex.set(Exception::PROTOCOL, "A P2P connection already exists on address ", _outAddress.toString(), " (id : ", it->second.peerId, ")");
		return;
	}
	it = _mapPeersByAddress.emplace_hint(it, piecewise_construct, forward_as_tuple(_outAddress), forward_as_tuple(*this, "", _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, _tag));

	it->second.responderHandshake0(ex, reader, _farId, _outAddress);
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
	for (auto& it : _mapPeersByAddress)
		it.second.flushWriters();
	for (auto& it : _mapPeersById)
		it.second.flushWriters();
}

// TODO: see if we always need to manage a list of commands
void RTMFPConnection::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {

	lock_guard<recursive_mutex> lock(_mutexCommands);
	if(command == NETSTREAM_PUBLISH_P2P) {
		pair<bool,bool> params(audioReliable,videoReliable);
		_mapP2pPublications.emplace(streamName, params);
		return;
	}
		
	_waitingCommands.emplace_front(command, streamName, audioReliable, videoReliable);
	_nbCreateStreams++;
}

void RTMFPConnection::createWaitingStreams() {
	lock_guard<recursive_mutex>	lock(_mutexCommands);
	if (!connected || !_nbCreateStreams)
		return;

	map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
	if (pFlow) {
		INFO("Creating a new stream...")
		pFlow->createStream();
		_nbCreateStreams--;
	}
}

void RTMFPConnection::sendConnections() {

	lock_guard<recursive_mutex> lock(_mutexConnections);

	// Send normal connection request
	if(_waitConnect) {
		sendHandshake0(BASE_HANDSHAKE, _url);
		_waitConnect=false;
	}

	// Send waiting p2p connections
	while (!_waitingPeers.empty()) {

		string& peerId = _waitingPeers.front();
		auto it = _mapPeersById.find(peerId);
		if (it != _mapPeersById.end()) {
			INFO("Sending P2P handshake 0 to peer ", peerId)
			it->second.sendHandshake0(P2P_HANDSHAKE, Util::UnformatHex<string>(peerId));
		} else
			ERROR("flusP2PConnection - Unable to find the peer object with id ", peerId)

		_waitingPeers.pop_front();
	}
}

RTMFPEngine* RTMFPConnection::getDecoder(UInt32 idStream, const SocketAddress& address) {
	auto it = _mapPeersByAddress.find(address);
	if (it != _mapPeersByAddress.end())
		return it->second.getDecoder(idStream, address);
	
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

// Handle a P2P address exchange message (Only for P2PConnection)
void RTMFPConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	//ERROR("Cannot handle P2P Address Exchange command on a RTMFP Connection") // target error (shouldn't happen)

	if(reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange first 3 bytes")
		return;
	}

	string tmp, peerId;
	reader.read(0x20, peerId);
	Util::FormatHex((const UInt8*)peerId.data(), peerId.size(), tmp);
	SocketAddress address;
	RTMFP::ReadAddress(reader, address, reader.read8());
	
	string tag;
	reader.read(16, tag);
	INFO("P2P address exchange from peer with address : ", address.toString())

	auto it = _mapPeersByAddress.lower_bound(address);
	if (it != _mapPeersByAddress.end()) {
		//ex.set(Exception::PROTOCOL, "A P2P connection already exists on address ", address.toString(), " (id : ", it->second.peerId, ")");
		return;
	}
	// TODO: see if _tag is equal to tag
	it = _mapPeersByAddress.emplace_hint(it, piecewise_construct, forward_as_tuple(address), forward_as_tuple(*this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _hostAddress, _pubKey, _tag));

	//it->second.connect(ex);
	Exception ex2;
	SocketAddress test(IPAddress::Wildcard(), _pSocket->address().port());
	if(!it->second.bind(ex2,test)) {
		WARN("Unable to connect to the peer, deletion of the instance (", ex2.error(), ")")
		_mapPeersByAddress.erase(it);
	}
	//it->second.responderHandshake0(ex, tag, _farId, _outAddress);
}
