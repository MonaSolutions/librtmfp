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
#include "GroupStream.h"

using namespace Base;
using namespace std;

UInt32 RTMFPSession::RTMFPSessionCounter = 0x02000000;

RTMFPSession::RTMFPSession(UInt32 id, Invoker& invoker, RTMFPConfig config) :
	_id(id), _rawId(PEER_ID_SIZE + 2, '\0'), _flashVer(EXPAND("WIN 20,0,0,286")), _app("live"), _handshaker(invoker.timer, this), _threadRcv(0), flags(0),
	FlowManager(false, invoker, config.pOnStatusEvent), _pOnMedia(config.pOnMedia), socketIPV4(_invoker.sockets), socketIPV6(_invoker.sockets),
	_interruptCb(config.interruptCb), _interruptArg(config.interruptArg) {

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
		BinaryWriter(*pSignature).write7Bit<UInt32>(idStream);
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
		_mapStreamWriters[command.idMedia] = pWriter; // save the writer id
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
		if (_pOnMedia)
			_pOnMedia(mediaId, time, STR packet.data(), packet.size(), type);
		else 
			_invoker.pushMedia(_id, mediaId, time, packet, lostRate, type); // asynchronous read, create a runner to save the packet
	};

	_sessionId = RTMFPSessionCounter++;

	// Bind addresses
	Exception ex;
	SocketAddress hostAddress(IPAddress::IPv6);
	if (config.hostIPv6 && !hostAddress.set(ex, config.hostIPv6, (UInt16)0))
		WARN("Unable to set IPv6 host address : ", ex)
	if (!socketIPV6.bind(ex, hostAddress))
		WARN("Unable to bind [::], ipv6 will not work : ", ex)

	// IPv4
	hostAddress.set(SocketAddress::Wildcard());
	if (config.host && !hostAddress.set(ex, config.host, (UInt16)0))
		WARN("Unable to set IP4 host address : ", ex)
	if (!socketIPV4.bind(ex, hostAddress))
		WARN("Unable to bind localhost, ipv4 will not work : ", ex)

	// Add the session ID to the map
	_mapSessions.emplace(_sessionId, this);
}

RTMFPSession::~RTMFPSession() {
	DEBUG("Deletion of RTMFPSession ", name())

	closeSession();
}

bool RTMFPSession::isInterrupted() {
	return !_pMainStream || (_interruptCb && _interruptCb(_interruptArg) == 1);
}

void RTMFPSession::setFlashProperties(const char* swfUrl, const char* app, const char* pageUrl, const char* flashVer) {
	if (swfUrl)
		_swfUrl = swfUrl;
	if (app)
		_app = app;
	if (pageUrl)
		_pageUrl = pageUrl;
	if (flashVer)
		_flashVer = flashVer;
}

void RTMFPSession::closeSession() {

	// Unsubscribing to socket : we don't want to receive packets anymore
	socketIPV4.onPacket = nullptr;
	socketIPV4.onError = nullptr;
	socketIPV6.onPacket = nullptr;
	socketIPV6.onError = nullptr;

	close(true, RTMFP::SESSION_CLOSED);
}

void RTMFPSession::close(bool abrupt, RTMFP::CLOSE_REASON reason) {
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
	FlowManager::close(abrupt, RTMFP::SESSION_CLOSED);

	if (abrupt) {
		// Close the NetGroup
		if (_group) {
			_group->onMedia = nullptr;
			_group->onStatus = nullptr;
			_group.reset();
		}

		// Close peers
		for (auto& it : _mapPeersById)
			it.second->close(true, RTMFP::SESSION_CLOSED);
		_mapPeersById.clear();
		_mapSessions.clear();

		// Remove all waiting handshakes
		_handshaker.close();

		if (_pMainStream) {
			_pMainStream->onStreamCreated = nullptr;
			_pMainStream->onNewPeer = nullptr;
		}
	}

	// reset state flags
	flags = 0;
}

RTMFPFlow* RTMFPSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature, UInt64 idWriterRef) {

	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		DEBUG("Creating new Flow (", id, ") for NetConnection ", name())
		_mainFlowId = id;
		return new RTMFPFlow(id, *this, _pMainStream, idWriterRef);
	}
	else if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream<GroupStream>(pStream); // TODO: see if it is really a GroupStream
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
		return false;
	}
	_url = url;
	_host = host;
	_rawUrl = rawUrl;

	if (address)
		_handshaker.startHandshake(_pHandshake, address, this, false);
	else
		_handshaker.startHandshake(_pHandshake, address, addresses, this, false, false);
	return true;
}

bool RTMFPSession::connect2Peer(const string& peerId, const string& streamName, UInt16 mediaCount) {

	PEER_LIST_ADDRESS_TYPE emptyAddresses;
	SocketAddress emptyHost; // We don't know the peer's host address
	return connect2Peer(peerId, streamName, emptyAddresses, emptyHost, false, mediaCount); // direct P2P => no delay
}

bool RTMFPSession::connect2Peer(const string& peerId, const string& streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress, bool delay, UInt16 mediaId) {
	if (status != RTMFP::CONNECTED) {
		ERROR("Cannot start a P2P connection before being connected to the server")
		return false;
	}

	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer != _mapPeersById.end() && itPeer->first == peerId) {
		TRACE("Unable to create the P2P session to ", peerId, ", we are already connecting/connected to it")
		return false;
	}

	DEBUG("Connecting to peer ", peerId, "...")
	itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId), 
		forward_as_tuple(new P2PSession(this, peerId.c_str(), _invoker, _pOnStatusEvent, hostAddress, false, (bool)_group, mediaId)));
	_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

	shared_ptr<P2PSession> pPeer = itPeer->second;
	// P2P unicast : add command play to send when connected
	if (!streamName.empty()) 
		pPeer->setStreamName(streamName);

	_handshaker.startHandshake(itPeer->second->handshake(), hostAddress? hostAddress : _address, addresses, (FlowManager*)itPeer->second.get(), true, delay);
	return true;
}

bool RTMFPSession::connect2Group(const string& streamName, RTMFPGroupConfig* parameters, bool audioReliable, bool videoReliable, const string& groupHex, const string& groupTxt, const string& groupName, UInt16 mediaCount) {
	INFO("Connecting to group ", groupTxt, "00 (mediaId=", mediaCount, " ; audioReliable=", audioReliable, " ; videoReliable=", videoReliable, ")...")

	if (status != RTMFP::CONNECTED) {
		ERROR("Cannot start a NetGroup connection before being connected to the server") 
		return false;
	}

	if (parameters->isPublisher) {
		if (_pPublisher) {
			WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			return false;
		}
		_pPublisher.reset(new Publisher(streamName, _invoker, audioReliable, videoReliable, true));
	}

	_group.reset(new NetGroup(_invoker.timer, mediaCount, groupHex.c_str(), groupTxt.c_str(), groupName.c_str(), streamName, *this, parameters, audioReliable, videoReliable));
	_group->onMedia = onMediaPlay;
	//_group->onStatus = _pMainStream->onStatus; (Commented, onStatus is not thread safe)
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
void RTMFPSession::writeData(const Packet& packet, UInt32 time) {
	if (_pPublisher)
		_pPublisher->pushData(time, packet);
}
void RTMFPSession::writeFlush() {
	if (_pPublisher)
		_pPublisher->flush();
}

bool RTMFPSession::manage() {
	if (isInterrupted())
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
		else
			(itPeer++)->second->manage();
	}

	// Manage the flows
	FlowManager::manage();

	// Send waiting handshake requests
	if (status <= RTMFP::CONNECTED)
		_handshaker.manage();

	// Manage NetGroup
	if (_group && status == RTMFP::CONNECTED) {
		Exception ex;
		if (!_group->manage(ex)) {
			ERROR(ex);
			// /!\ Close the NetGroup, do not close the session to avoid closing the fallback (if exist) too
			if (_group) {
				_group->onMedia = nullptr;
				_group->onStatus = nullptr;
				_group.reset();
			}
			onNetGroupException(_id);
			return false;
		}
	}

	return !failed();
}

bool RTMFPSession::addStream(UInt8 mask, const string& streamName, bool audioReliable, bool videoReliable, UInt16 mediaCount) {

	if (_pPublisher && (mask & RTMFP_PUBLISHED)) { // TODO: handle multiple publishers
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

	// If p2p publisher create directly the publisher
	if (mask & RTMFP_P2P_PUBLISHED)
		_pPublisher.reset(new Publisher(streamName, _invoker, audioReliable, videoReliable, true));
	// Otherwise create flash stream
	else {
		_pMainStream->createStream();
		_pMainWriter->writeInvocation("createStream");
		_pMainWriter->flush();
		_waitingStreams.emplace((mask & RTMFP_PUBLISHED) > 0, streamName, mediaCount, audioReliable, videoReliable);
	}
	INFO("Creation of the ", (mask & RTMFP_PUBLISHED) ? "publisher" : ((mask & RTMFP_P2P_PUBLISHED) ? "p2p publisher" : "player"), " stream ", mediaCount)
	return true;
}

bool RTMFPSession::closeStream(UInt16 mediaCount) {

	auto itWriter = _mapStreamWriters.find(mediaCount);
	if (itWriter == _mapStreamWriters.end())
		return false;

	itWriter->second->writeInvocation("closeStream", true);
	itWriter->second->close();
	return true;
}

bool RTMFPSession::closePublication(const char* streamName) {

	INFO("Unpublishing stream ", streamName, "...")
	if (!_pPublisher || _pPublisher->name() != streamName) {
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

	UInt16 port = socketIPV4->address().port();
	UInt16 portIPv6 = socketIPV6->address().port();
	INFO("Sending peer info (ipv4: ", socketIPV4->address(), " - ipv6 : ", socketIPV6->address(),")")
	AMFWriter& amfWriter = _pMainWriter->writeInvocation("setPeerInfo", false);
	amfWriter.amf0 = true; // Cirrus wants amf0

	Exception ex;
	vector<IPAddress> addresses;
	if (!IPAddress::GetLocals(ex, addresses)) {
		WARN("Error occurs while retrieving addresses : ", ex)
		if (addresses.empty())
			addresses.push_back(IPAddress::Loopback());
	}
	SocketAddress address;
	for (auto& it : addresses) {
		if (it.isLoopback() || it.isLinkLocal())
			continue; // ignore loopback and link-local addresses
		address.set(it, (it.family() == IPAddress::IPv4) ? port : portIPv6);
		amfWriter.writeString(address.c_str(), address.length());
	}
	_pMainWriter->flush();

	// We are connected : unlock the possible blocking function
	flags |= RTMFP_CONNECTED;
	onConnectionEvent(_id, RTMFP_CONNECTED);
}

void RTMFPSession::onPublished(UInt16 streamId) {
	Exception ex;
	_pPublisher->start();

	shared_ptr<Buffer> pSignature(new Buffer(4, "\x00\x54\x43\x04"));
	BinaryWriter(*pSignature).write7Bit<UInt32>(streamId);
	Packet signature(pSignature);
	shared_ptr<RTMFPWriter> pDataWriter = createWriter(signature,_mainFlowId);
	shared_ptr<RTMFPWriter> pAudioWriter = createWriter(signature, _mainFlowId);
	shared_ptr<RTMFPWriter> pVideoWriter = createWriter(signature, _mainFlowId);

	if (!(_pListener = _pPublisher->addListener<FlashListener, shared_ptr<RTMFPWriter>&>(ex, name(), pDataWriter, pAudioWriter, pVideoWriter)))
		WARN(ex)

	// Stream published : unlock the possible blocking function
	flags |= RTMFP_PUBLISHED;
	onConnectionEvent(_id, RTMFP_PUBLISHED);
}

void RTMFPSession::stopListening(const string& peerId) {
	INFO("Deletion of the listener to ", peerId)
	if (_pPublisher)
		_pPublisher->removeListener(peerId);
}

void RTMFPSession::handleNewGroupPeer(const string& rawId, const string& peerId) {
	DEBUG("NetGroup Peer ID ", peerId, " received")
	
	if (!_group || !_group->p2pNewPeer(peerId)) {
		DEBUG("Unable to add the peer ", peerId, ", the peer already exists")
		return;
	}

	PEER_LIST_ADDRESS_TYPE emptyAddresses;
	SocketAddress emptyHost; // We don't know the peer's host address
	connect2Peer(peerId.c_str(), "", emptyAddresses, _address, false); // not delayed when receiving the peer ID, contact the rendezvous service to get the addresses of the peer
	_group->newGroupPeer(peerId, rawId.c_str(), emptyAddresses, emptyHost);
}

void RTMFPSession::handleWriterException(shared_ptr<RTMFPWriter>& pWriter) {

	if (pWriter == _pGroupWriter)
		_pGroupWriter.reset();
	else if (pWriter == _pMainWriter)
		_pMainWriter.reset();
	else {
		for (auto it = _mapStreamWriters.begin(); it != _mapStreamWriters.end(); ++it) {
			if (pWriter != it->second)
				continue;
			_mapStreamWriters.erase(it);
			break;
		}
	}

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
	RTMFP::ReadAddress(reader, address);

	Exception ex;
	string tag;
	reader.read(16, tag);
	DEBUG("A peer will contact us with address : ", address)

	// If NetGroup we notify that a peer is trying to connect
	if (_group)
		_group->p2PAddressExchange(tag.c_str());

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
			forward_as_tuple(new P2PSession(this, peerId.c_str(), _invoker, _pOnStatusEvent, emptyHost, true, (bool)_group)));
		_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

		// associate the handshake & session
		pHandshake->pSession = itPeer->second.get();
		itPeer->second->handshake() = pHandshake;
		itPeer->second->setAddress(address);

		if (_group)
			_group->addPeer2HeardList(itPeer->second->peerId, STR itPeer->second->rawId.data(), itPeer->second->addresses(), itPeer->second->hostAddress, true);
		return true;
	}
	
	return itPeer->second->onHandshake38(address, pHandshake);
}

void RTMFPSession::onConnection() {
	INFO("RTMFPSession ", _id, " is now connected to ", name(), " (", _address, ")")
	removeHandshake(_pHandshake);
	status = RTMFP::CONNECTED;
	_pMainWriter = createWriter(Packet(EXPAND("\x00\x54\x43\x04\x00")), 0);

	// Send the connect request
	AMFWriter& amfWriter = _pMainWriter->writeInvocation("connect");

	// TODO: add parameters to configuration
	bool amf = amfWriter.amf0;
	amfWriter.amf0 = true;
	amfWriter.beginObject();
	amfWriter.writeStringProperty("app", _app);
	amfWriter.writeStringProperty("flashVer", _flashVer);
	amfWriter.writeStringProperty("swfUrl", _swfUrl);
	amfWriter.writeStringProperty("tcUrl", _url);
	amfWriter.writeBooleanProperty("fpad", false);
	amfWriter.writeNumberProperty("capabilities", 235);
	amfWriter.writeNumberProperty("audioCodecs", 3575);
	amfWriter.writeNumberProperty("videoCodecs", 252);
	amfWriter.writeNumberProperty("videoFunction", 1);
	amfWriter.writeStringProperty("pageUrl", _pageUrl);
	amfWriter.writeNumberProperty("objectEncoding", 3);
	amfWriter.endObject();
	amfWriter.amf0 = amf;

	_pMainWriter->flush();
}

void RTMFPSession::removeHandshake(shared_ptr<Handshake>& pHandshake) { 

	if (pHandshake->pSession) {
		pHandshake->pSession = NULL;
		_handshaker.removeHandshake(pHandshake);
		pHandshake.reset();
	} // else already deleted
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

void RTMFPSession::removePeer(const string& peerId) {

	auto itPeer = _mapPeersById.find(peerId);
	if (itPeer == _mapPeersById.end())
		return;

	bool remove = itPeer->second->status < RTMFP::CONNECTED;

	// Close the peer session (and remove the handshake)
	itPeer->second->close(true, RTMFP::OTHER_EXCEPTION);

	// If the peer was not connected we delete it, no need to wait
	if (remove) {
		_mapSessions.erase(itPeer->second->sessionId());
		_mapPeersById.erase(itPeer);
	}
}

void RTMFPSession::updatePeerAddress(const std::string& peerId, const Base::SocketAddress& address, RTMFP::AddressType type) {

	auto itPeer = _mapPeersById.find(peerId);
	if (itPeer == _mapPeersById.end())
		return;

	itPeer->second->addAddress(address, type);
}

void RTMFPSession::handleConcurrentSwitch() {
	if (_group) 
		_group->handleConcurrentSwitch();
}

void RTMFPSession::handlePeerDisconnection(const string& peerId) {
	auto itPeer = _mapPeersById.find(peerId);
	if (itPeer == _mapPeersById.end())
		return;

	DEBUG("Address empty found, the peer ", peerId, " has been deleted")
	itPeer->second->close(true, RTMFP::OTHER_EXCEPTION);

	if (_group)
		_group->handlePeerDisconnection(peerId);
}

void RTMFPSession::setP2pPublisherReady() {

	// P2P Viewer connected : unlock the possible blocking function
	flags |= RTMFP_P2P_PUBLISHED;
	onConnectionEvent(_id, RTMFP_P2P_PUBLISHED);
}

void RTMFPSession::handleFirstPeer() {

	// First peer connected : unlock the possible blocking function
	flags |= RTMFP_GROUP_CONNECTED;
	onConnectionEvent(_id, RTMFP_GROUP_CONNECTED);

	// Send an event
	if (_pOnStatusEvent)
		_pOnStatusEvent("NetGroup.Publish.Start", "First peer connected, starting to publish");
}

void RTMFPSession::setP2PPlayReady() {

	// Connected to peer publisher : unlock the possible blocking function
	flags |= RTMFP_PEER_CONNECTED;
	onConnectionEvent(_id, RTMFP_PEER_CONNECTED);
}
