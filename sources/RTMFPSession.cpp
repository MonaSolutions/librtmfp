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

#include "RTMFPSession.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "Listener.h"
#include "NetGroup.h"
#include "Base/DNS.h"
#include "Base/Logs.h"
#include "librtmfp.h"
#include "Base/Util.h"

using namespace Base;
using namespace std;

UInt32 RTMFPSession::RTMFPSessionCounter = 0x02000000;

RTMFPSession::RTMFPSession(Invoker& invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) : _rawId(PEER_ID_SIZE + 2, '\0'),
	_handshaker(this), _threadRcv(0), FlowManager(false, invoker, pOnSocketError, pOnStatusEvent), _pOnMedia(pOnMediaEvent), socketIPV4(_invoker.sockets), socketIPV6(_invoker.sockets) {

	socketIPV6.onPacket = socketIPV4.onPacket = [this](shared<Buffer>& pBuffer, const SocketAddress& address) {
		if (status > RTMFP::NEAR_CLOSED)
			return;
		if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
			ERROR("Invalid RTMFP packet on connection to ", _address)
			return;
		}

		BinaryReader reader(pBuffer->data(), pBuffer->size());
		UInt32 idSession = RTMFP::Unpack(reader);
		pBuffer->clip(reader.position());

		Exception ex;
		shared_ptr<RTMFP::Engine> pEngine;
		if (!idSession)
			pEngine = _handshaker.decoder();
		else {
			auto itSession = _mapSessions.find(idSession);
			if (itSession == _mapSessions.end()) {
				WARN("Unknown session ", String::Format<UInt32>("0x%.8x", idSession), " in packet from ", address)
				return;
			}
			pEngine = itSession->second->decoder();
		}
		if (!pEngine) {
			WARN("Unable to find the decoder related to packet from ", _address)
			return;
		}

		_invoker.decode(_id, idSession, address, pEngine, pBuffer, _threadRcv);
	};
	socketIPV6.onError = socketIPV4.onError = [this](const Exception& ex) {
		SocketAddress address;
		DEBUG("Socket error : ", ex)
	};
	_pMainStream->onStreamCreated = [this](UInt16 idStream, UInt16& idMedia) {
		// Get command
		if (_waitingStreams.empty()) {
			WARN("Stream created without command")
			return false;
		}
		const StreamCommand& command = _waitingStreams.front();
		DEBUG("Stream ", idStream, " created for Media ", command.idMedia, ", sending command ", command.publisher? "publish" : "play", " for stream ", command.value)

		// Manage publisher if it is a publish command
		if (command.publisher && _pPublisher) {
			ERROR("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			_waitingStreams.pop();
			return false;
		}

		// Stream created, now we create the writer before sending another request
		shared_ptr<Buffer> pSignature(new Buffer(4, "\x00\x54\x43\x04"));
		BinaryWriter(*pSignature).write7BitValue(idStream);
		shared_ptr<RTMFPWriter> pWriter = createWriter(Packet(pSignature), _mainFlowId);

		// Send command and remove command type from waiting commands
		if (command.publisher) {
			AMFWriter& amfWriter = pWriter->writeInvocation("publish", true);
			amfWriter.writeString(command.value.c_str(), command.value.size());
			pWriter->flush();
			// Create the publisher
			_pPublisher.reset(new Publisher(command.value.c_str(), _invoker, command.audioReliable, command.videoReliable, false));
		}
		else {
			AMFWriter& amfWriter = pWriter->writeInvocation("play", true);
			amfWriter.amf0 = true; // Important for p2p unicast play
			amfWriter.writeString(command.value.c_str(), command.value.size());
			pWriter->flush();
		}
		idMedia = command.idMedia;
		_waitingStreams.pop();
		return true;
	};
	_pMainStream->onNewPeer = [this](const string& rawId, const string& peerId) {
		handleNewGroupPeer(rawId, peerId);
	};
	onMediaPlay = _pMainStream->onMedia = [this](UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {
		if (!packet.size())
			return;

		// Synchronous read
		if (_pOnMedia) {
			_pOnMedia(mediaId, time, STR packet.data(), packet.size(), type);
			return;
		}
		
		_invoker.pushMedia(_id, mediaId, time, packet, lostRate, type);
	};

	_sessionId = RTMFPSessionCounter++;

	Exception ex;
	if (!socketIPV6.bind(ex, SocketAddress::Wildcard(IPAddress::IPv6)))
		WARN("Unable to bind [::], ipv6 will not work : ", ex)
	if (!socketIPV4.bind(ex, SocketAddress::Wildcard(IPAddress::IPv4)))
		WARN("Unable to bind localhost, ipv4 will not work : ", ex)

	// Add the session ID to the map
	_mapSessions.emplace(_sessionId, this);
}

RTMFPSession::~RTMFPSession() {
	DEBUG("Deletion of RTMFPSession ", name())

	closeSession();
}

void RTMFPSession::closeSession() {

	// Unsubscribing to socket : we don't want to receive packets anymore
	socketIPV4.onPacket = nullptr;
	socketIPV4.onError = nullptr;
	socketIPV6.onPacket = nullptr;
	socketIPV6.onError = nullptr;

	close(true);
}

void RTMFPSession::close(bool abrupt) {
	if (status == RTMFP::FAILED)
		return;

	// Close listener & publisher
	if (_pListener && _pPublisher) {
		_pPublisher->removeListener(name());
		_pListener = NULL;
	}
	if (_group)
		_group->stopListener();
	if (_pPublisher && _pPublisher->running())
		_pPublisher->stop();
	_pPublisher.reset();

	// Close the session & writers
	_pGroupWriter.reset();
	_pMainWriter.reset();
	FlowManager::close(abrupt);

	if (abrupt) {
		// Close the NetGroup
		if (_group) {
			_group->onMedia = nullptr;
			_group->onStatus = nullptr;
			_group->close();
		}

		// Close peers
		for (auto it : _mapPeersById)
			it.second->close(true);
		_mapPeersById.clear();
		_mapSessions.clear();

		// Remove all waiting handshakes
		_handshaker.close();

		if (_pMainStream) {
			_pMainStream->onStreamCreated = nullptr;
			_pMainStream->onNewPeer = nullptr;
		}
	}
}

RTMFPFlow* RTMFPSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature, UInt64 idWriterRef) {

	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		DEBUG("Creating new Flow (", id, ") for NetConnection ", name())
		_mainFlowId = id;
		return new RTMFPFlow(id, *this, _pMainStream, idWriterRef);
	}
	else if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true); // TODO: see if it is really a GroupStream
		return new RTMFPFlow(id,  pStream, *this, idWriterRef);
	}
	else {
		ex.set<Ex::Protocol>("Unhandled signature type : ",  String::Hex(BIN signature.data(), signature.size()), " , cannot create RTMFPFlow");
	}
	return NULL;
}

bool RTMFPSession::connect(const string& url, const string& host, const SocketAddress& address, const PEER_LIST_ADDRESS_TYPE& addresses, shared_ptr<Buffer>& rawUrl) {

	if (_rawUrl) {
		ERROR("You cannot call connect 2 times on the same session")
		onConnectSucceed(); // to exit from the parent loop
		return false;
	}
	_url = url;
	_host = host;
	_rawUrl = rawUrl;

	if (address)
		_handshaker.startHandshake(_pHandshake, address, this, false, false);
	else
		_handshaker.startHandshake(_pHandshake, address, addresses, this, false, false);
	return true;
}

bool RTMFPSession::connect2Peer(const string& peerId, const string& streamName, UInt16 mediaCount) {

	PEER_LIST_ADDRESS_TYPE emptyAddresses;
	SocketAddress emptyHost; // We don't know the peer's host address
	return connect2Peer(peerId, streamName, emptyAddresses, emptyHost, mediaCount);
}

bool RTMFPSession::connect2Peer(const string& peerId, const string& streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress, UInt16 mediaId) {
	if (status != RTMFP::CONNECTED) {
		ERROR("Cannot start a P2P connection before being connected to the server")
		onConnected2Peer(); // to exit from the parent loop 
		return false;
	}

	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer != _mapPeersById.end() && itPeer->first == peerId) {
		DEBUG("Unable to create the P2P session to ", peerId, ", we are already connecting/connected to it")
		onConnected2Peer(); // to exit from the parent loop
		return false;
	}

	DEBUG("Connecting to peer ", peerId, "...")
	itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId), 
		forward_as_tuple(new P2PSession(this, peerId.c_str(), _invoker, _pOnSocketError, _pOnStatusEvent, hostAddress, false, (bool)_group, mediaId)));
	_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

	shared_ptr<P2PSession> pPeer = itPeer->second;
	// P2P unicast : add command play to send when connected
	if (!streamName.empty()) 
		pPeer->setStreamName(streamName);

	_handshaker.startHandshake(itPeer->second->handshake(), hostAddress? hostAddress : _address, addresses, (FlowManager*)itPeer->second.get(), false, true);
	return true;
}

bool RTMFPSession::connect2Group(const string& streamName, RTMFPGroupConfig* parameters, bool audioReliable, bool videoReliable, const string& groupHex, const string& groupTxt, UInt16 mediaCount) {
	INFO("Connecting to group ", parameters->netGroup, " (mediaId=", mediaCount, " ; audioReliable=", audioReliable, " ; videoReliable=", videoReliable, ")...")

	if (status != RTMFP::CONNECTED) {
		ERROR("Cannot start a NetGroup connection before being connected to the server")
		onConnected2Group(); // to exit from the parent loop 
		return false;
	}

	if (parameters->isPublisher) {
		if (_pPublisher) {
			WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			onConnected2Group(); // to exit from the parent loop 
			return false;
		}
		_pPublisher.reset(new Publisher(streamName, _invoker, audioReliable, videoReliable, true));
	}

	_group.reset(new NetGroup(mediaCount, groupHex.c_str(), groupTxt.c_str(), streamName, *this, parameters, audioReliable, videoReliable));
	_group->onMedia = onMediaPlay;
	_group->onStatus = _pMainStream->onStatus;
	sendGroupConnection(groupHex);
	return true;
}

unsigned int RTMFPSession::callFunction(const string& function, queue<string>& arguments, const string& peerId) {
	// Server call
	if (peerId.empty() && _pMainStream && _pMainWriter) {
		// TODO: refactorize with P2PSession code
		AMFWriter& amfWriter = _pMainWriter->writeInvocation(function.c_str(), true);
		while (!arguments.empty()) {
			string& arg = arguments.front() ;
			amfWriter.writeString(arg.data(), arg.size());
			arguments.pop();
		}
		_pMainWriter->flush();
	// NetGroup call
	} else if (peerId=="all") {
		if (_group)
			return _group->callFunction(function, arguments);
	}
	// Peer call
	else {
		for (auto &it : _mapPeersById) {
			if (it.second->peerId == peerId)
				return it.second->callFunction(function, arguments);
		}
		ERROR("Unable to find the peer", peerId, " for sending the function call")
	}

	return 0;
}

void RTMFPSession::writeAudio(const Packet& packet, UInt32 time) {
	if (_pPublisher)
		_pPublisher->pushAudio(time, packet);
}
void RTMFPSession::writeVideo(const Packet& packet, UInt32 time) {
	if (_pPublisher)
		_pPublisher->pushVideo(time, packet);
}
void RTMFPSession::writeFlush() {
	if (_pPublisher)
		_pPublisher->flush();
}

bool RTMFPSession::manage() {
	if (!_pMainStream)
		return false;

	// Release closed P2P connections
	auto itPeer = _mapPeersById.begin();
	while (itPeer != _mapPeersById.end()) {
		if (itPeer->second->failed()) {
			DEBUG("RTMFPSession management - Deleting closed P2P session to ", itPeer->first)
			auto nbRemoved = _mapSessions.erase(itPeer->second->sessionId());
			if (nbRemoved != 1)
				WARN("RTMFPSession management - Error to remove P2P session ", itPeer->first, " (", itPeer->second->sessionId(),") : ", nbRemoved)
			_mapPeersById.erase(itPeer++);
		}
		else {
			itPeer->second->manage();
			++itPeer;
		}
	}

	// Manage the flows
	FlowManager::manage();

	// Send waiting handshake requests
	_handshaker.manage();

	// Manage NetGroup
	if (_group)
		_group->manage();

	return !failed();
}

bool RTMFPSession::addStream(bool publisher, const string& streamName, bool audioReliable, bool videoReliable, UInt16 mediaCount) {

	if (publisher && _pPublisher) { // TODO: handle multiple publishers
		WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
		return false;
	}
	
	if (status != RTMFP::CONNECTED) {
		WARN("You cannot create a new stream before being connected")
		return false;
	}

	if (!_pMainWriter) {
		ERROR("Unable to find the main writer related to the main stream")
		return false;
	}

	_pMainStream->createStream();
	_pMainWriter->writeInvocation("createStream");
	_pMainWriter->flush();

	_waitingStreams.emplace(publisher, streamName, mediaCount, audioReliable, videoReliable);
	INFO("Creation of the ", publisher? "publisher" : "player", " stream ", mediaCount)
	return true;
}


void RTMFPSession::startP2PPublisher(const string& streamName, bool audioReliable, bool videoReliable) {

	INFO("Creating publisher for stream ", streamName, "...")
	if (_pPublisher) {
		WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
		onPublishP2P(false);
	}
	else
		_pPublisher.reset(new Publisher(streamName, _invoker, audioReliable, videoReliable, true));
}

bool RTMFPSession::closePublication(const char* streamName) {

	INFO("Unpublishing stream ", streamName, "...")
	if (!_pPublisher && _pPublisher->name() == streamName) {
		WARN("Unable to find the publisher ", streamName)
		return false;
	}
	
	_pPublisher->stop();
	if (_pListener) {
		_pPublisher->removeListener(name());
		_pListener = NULL;
	}
	if (_group)
		_group->stopListener();
	return true;
}

void RTMFPSession::onNetConnectionSuccess() {
	if (!_pMainWriter) {
		ERROR("Unable to find the main writer to send setPeerInfo request")
		return;
	}

	UInt16 port = socketIPV4.socket()->address().port();
	UInt16 portIPv6 = socketIPV6.socket()->address().port();
	INFO("Sending peer info (port : ", port, " - port ipv6 : ", portIPv6,")")
	AMFWriter& amfWriter = _pMainWriter->writeInvocation("setPeerInfo", false);
	amfWriter.amf0 = true; // Cirrus wants amf0

	Exception ex;
	vector<IPAddress> addresses = IPAddress::Locals(ex);
	if (ex) {
		WARN("Error occurs while retrieving addresses : ", ex)
		if (addresses.empty())
			addresses.push_back(IPAddress::Loopback());
	}
	SocketAddress address;
	for (auto it : addresses) {
		if (it.isLoopback() || it.isLinkLocal())
			continue; // ignore loopback and link-local addresses
		address.set(it, (it.family() == IPAddress::IPv4) ? port : portIPv6);
		amfWriter.writeString(address.c_str(), address.length());
	}
	_pMainWriter->flush();

	// We are connected : unlock the possible blocking RTMFP_Connect function
	onConnectSucceed();
}

void RTMFPSession::onPublished(UInt16 streamId) {
	Exception ex;
	_pPublisher->start();

	shared_ptr<Buffer> pSignature(new Buffer(4, "\x00\x54\x43\x04"));
	BinaryWriter(*pSignature).write7BitValue(streamId);
	Packet signature(pSignature);
	shared_ptr<RTMFPWriter> pDataWriter = createWriter(signature,_mainFlowId);
	shared_ptr<RTMFPWriter> pAudioWriter = createWriter(signature, _mainFlowId);
	shared_ptr<RTMFPWriter> pVideoWriter = createWriter(signature, _mainFlowId);

	if (!(_pListener = _pPublisher->addListener<FlashListener, shared_ptr<RTMFPWriter>&>(ex, name(), pDataWriter, pAudioWriter, pVideoWriter)))
		WARN(ex)

	onStreamPublished();
}

void RTMFPSession::stopListening(const string& peerId) {
	INFO("Deletion of the listener to ", peerId)
	if (_pPublisher)
		_pPublisher->removeListener(peerId);
}

void RTMFPSession::handleNewGroupPeer(const string& rawId, const string& peerId) {
	DEBUG("NetGroup Peer ID ", peerId, " from server ", _address)
	
	if (!_group || !_group->checkPeer(peerId)) {
		DEBUG("Unable to add the peer ", peerId, ", it can be a wrong group ID or the peer already exists")
		return;
	}

	PEER_LIST_ADDRESS_TYPE emptyAddresses;
	SocketAddress emptyHost; // We don't know the peer's host address
	connect2Peer(peerId.c_str(), "", emptyAddresses, _address);
	_group->addPeer2HeardList(peerId, rawId.c_str(), emptyAddresses, emptyHost);
}

void RTMFPSession::handleWriterException(shared_ptr<RTMFPWriter>& pWriter) {

	if (pWriter == _pGroupWriter)
		_pGroupWriter.reset();
	else if (pWriter == _pMainWriter)
		_pMainWriter.reset();

	WARN("Writer ", pWriter->id, " terminated on session ", name())
	pWriter->close(false);
}

void RTMFPSession::handleP2PAddressExchange(BinaryReader& reader) {
	if (reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange header")
		return;
	}

	// Read our peer id and address of initiator
	string buff;
	reader.read(PEER_ID_SIZE, buff);
	SocketAddress address;
	RTMFP::AddressType addressType;
	RTMFP::ReadAddress(reader, address, addressType);

	Exception ex;
	string tag;
	reader.read(16, tag);
	if (address.host().isLoopback() && !_address.host().isLoopback())
		address.set(ex, _address.host(), address.port());
	else if (address.host().isLocal() && !_address.host().isLocal())
		address.set(ex, _address.host(), address.port());
	DEBUG("A peer will contact us with address : ", address)

	// Send the handshake 70 to the peer
	_handshaker.sendHandshake70(tag, address, _address);
}

void RTMFPSession::sendGroupConnection(const string& netGroup) {

	_pGroupWriter = createWriter(Packet(EXPAND("\x00\x47\x43")), _mainFlowId);
	_pGroupWriter->writeGroupConnect(netGroup);
	_pGroupWriter->flush();
}

bool RTMFPSession::addPeer2Group(const string& peerId) {
	if (_group) {
		auto it = _mapPeersById.find(peerId);
		if (it == _mapPeersById.end())
			ERROR("Unable to find the peer ", peerId) // should not happen
		// Inform the NetGroup about the new peer
		else if (_group->addPeer(peerId, it->second))
			return true;
	}
	return false;
}

const string& RTMFPSession::groupIdHex(){ 
	FATAL_CHECK(_group)
	return _group->idHex;
}

const string& RTMFPSession::groupIdTxt() { 
	FATAL_CHECK(_group)
	return _group->idTxt;
}

void RTMFPSession::buildPeerID(const UInt8* data, UInt32 size) {
	if (!_peerTxtId.empty())
		return;

	// Peer ID built, we save it
	BinaryWriter writer(BIN _rawId.data(), _rawId.size());
	writer.write("\x21\x0f");
	EVP_Digest(data, size, BIN(_rawId.data() + 2), NULL, EVP_sha256(), NULL);
	String::Assign(_peerTxtId, String::Hex(BIN _rawId.data() + 2, PEER_ID_SIZE));
	INFO("Peer ID : \n", _peerTxtId)
}

bool RTMFPSession::onNewPeerId(const SocketAddress& address, shared_ptr<Handshake>& pHandshake, UInt32 farId, const string& peerId) {

	// If the peer session doesn't exists we create it
	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer == _mapPeersById.end() || itPeer->first != peerId) {
		SocketAddress emptyHost; // We don't know the peer's host address
		itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId),
			forward_as_tuple(new P2PSession(this, peerId.c_str(), _invoker, _pOnSocketError, _pOnStatusEvent, emptyHost, true, (bool)_group)));
		_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

		// associate the handshake & session
		pHandshake->pSession = itPeer->second.get();
		itPeer->second->handshake() = pHandshake;
		itPeer->second->setAddress(address);

		if (_group)
			_group->addPeer2HeardList(itPeer->second->peerId, STR itPeer->second->rawId.data(), itPeer->second->addresses(), itPeer->second->hostAddress, true);
	}
	else
		return itPeer->second->onHandshake38(address, pHandshake);
	
	return true;
}

void RTMFPSession::onConnection() {
	INFO("RTMFPSession is now connected to ", name())
	removeHandshake(_pHandshake);
	status = RTMFP::CONNECTED;
	_pMainWriter = createWriter(Packet(EXPAND("\x00\x54\x43\x04\x00")), 0);

	// Send the connect request
	AMFWriter& amfWriter = _pMainWriter->writeInvocation("connect");

	// TODO: add parameters to configuration
	bool amf = amfWriter.amf0;
	amfWriter.amf0 = true;
	amfWriter.beginObject();
	amfWriter.writeStringProperty("app", "live");
	amfWriter.writeStringProperty("flashVer", EXPAND("WIN 20,0,0,286")); // TODO: change at least this
	amfWriter.writeStringProperty("swfUrl", "");
	amfWriter.writeStringProperty("tcUrl", _url);
	amfWriter.writeBooleanProperty("fpad", false);
	amfWriter.writeNumberProperty("capabilities", 235);
	amfWriter.writeNumberProperty("audioCodecs", 3575);
	amfWriter.writeNumberProperty("videoCodecs", 252);
	amfWriter.writeNumberProperty("videoFunction", 1);
	amfWriter.writeStringProperty("pageUrl", "");
	amfWriter.writeNumberProperty("objectEncoding", 3);
	amfWriter.endObject();
	amfWriter.amf0 = amf;

	_pMainWriter->flush();
}

void RTMFPSession::removeHandshake(shared_ptr<Handshake>& pHandshake) { 
	pHandshake->pSession = NULL; // to not close the session
	_handshaker.removeHandshake(pHandshake); 
	pHandshake.reset(); 
}

void RTMFPSession::receive(RTMFPDecoder::Decoded& decoded) {
	if (status == RTMFP::FAILED)
		return;

	if (!decoded.idSession)
		_handshaker.receive(decoded.address, decoded);
	else {
		auto itSession = _mapSessions.find(decoded.idSession);
		if (itSession == _mapSessions.end()) {
			WARN("Unknown session ", String::Format<UInt32>("0x%.8x", decoded.idSession), ", possibly deleted (", decoded.address, ")")
				return;
		}
		itSession->second->receive(decoded.address, decoded);
	}
}