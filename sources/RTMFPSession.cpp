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
	_handshaker(this), _isWaitingStream(false), _mediaCount(0), p2pPublishReady(false), p2pPlayReady(false), publishReady(false), connectReady(false), dataAvailable(false), _threadRcv(0),
	FlowManager(false, invoker, pOnSocketError, pOnStatusEvent), _pOnMedia(pOnMediaEvent), _pSocket(new UDPSocket(_invoker.sockets)), _pSocketIPV6(new UDPSocket(_invoker.sockets)) {

	_pSocketIPV6->onPacket = _pSocket->onPacket = [this](shared<Buffer>& pBuffer, const SocketAddress& address) {
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

		shared_ptr<RTMFPDecoder> pDecoder(new RTMFPDecoder(idSession, address, pEngine, pBuffer, _invoker.handler));
		pDecoder->onDecoded = _onDecoded;
		AUTO_ERROR(_invoker.threadPool.queue(ex, pDecoder, _threadRcv), "RTMFP Decode")
	};
	_pSocketIPV6->onError = _pSocket->onError = [this](const Exception& ex) {
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
			_pPublisher.reset(new Publisher(command.value, _invoker, command.audioReliable, command.videoReliable, false));
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
		auto itMedia = _mapPlayers.find(mediaId);
		if (itMedia == _mapPlayers.end()) {
			WARN("Unable to find media ", mediaId) // implementation error
			return;
		}
		MediaPlayer& media = itMedia->second;

		if (!media.codecInfosRead && type == AMF::TYPE_VIDEO) {
			if (!RTMFP::IsVideoCodecInfos(packet.data(), packet.size())) {
				DEBUG("Video frame dropped to wait first key frame");
				return;
			}
			INFO("Video codec infos found, starting to read")
			media.codecInfosRead = true;
		}
		// AAC : we wait for the sequence header packet
		else if (!media.AACsequenceHeaderRead && type == AMF::TYPE_AUDIO && (packet.size() > 1 && (*packet.data() >> 4) == 0x0A)) { // TODO: save the codec type
			if (!RTMFP::IsAACCodecInfos(packet.data(), packet.size())) {
				DEBUG("AAC frame dropped to wait first key frame (sequence header)");
				return;
			}
			INFO("AAC codec infos found, starting to read audio part")
			media.AACsequenceHeaderRead = true;
		}

		if (_pOnMedia) // Synchronous read
			_pOnMedia(mediaId, time, STR packet.data(), packet.size(), type);
		else { // Asynchronous read
			media.mediaPackets.emplace_back(new MediaPlayer::RTMFPMediaPacket(packet, time, type));
			if (!dataAvailable)
				dataAvailable = true;
		}
	};
	onPushAudio = [this](MediaPacket& packet) { _pPublisher->pushAudio(packet.time, packet); };
	onPushVideo = [this](MediaPacket& packet) { _pPublisher->pushVideo(packet.time, packet); };
	onFlushPublisher = [this]() { _pPublisher->flush(); };
	_onDecoded = [this](RTMFPDecoder::Decoded& decoded) {

		lock_guard<mutex> lock(_mutexConnections);
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
	};

	_sessionId = RTMFPSessionCounter++;

	Exception ex;
	if (!_pSocketIPV6->bind(ex, SocketAddress::Wildcard(IPAddress::IPv6)))
		WARN("Unable to bind [::], ipv6 will not work : ", ex)
	if (!_pSocket->bind(ex, SocketAddress::Wildcard(IPAddress::IPv4)))
		WARN("Unable to bind localhost, ipv4 will not work : ", ex)

	// Add the session ID to the map
	_mapSessions.emplace(_sessionId, this);
}

RTMFPSession::~RTMFPSession() {
	DEBUG("Deletion of RTMFPSession ", name())

	_onDecoded = nullptr;
	closeSession();
	onPushAudio = nullptr;
	onPushVideo = nullptr;
	onFlushPublisher = nullptr;
}

void RTMFPSession::closeSession() {

	// Unsubscribing to socket : we don't want to receive packets anymore
	if (_pSocket) {
		_pSocket->onPacket = nullptr;
		_pSocket->onError = nullptr;
	}
	if (_pSocketIPV6) {
		_pSocketIPV6->onPacket = nullptr;
		_pSocketIPV6->onError = nullptr;
	}

	{
		lock_guard<mutex> lock(_mutexConnections);
		close(true);
	}

	if (_pSocket) {
		_pSocket->close();
		_pSocket.reset();
	}

	if (_pSocketIPV6) {
		_pSocketIPV6->close();
		_pSocketIPV6.reset();
	}
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

		// Set all the signals to exit properly
		connectSignal.set();
		p2pPublishSignal.set();
		p2pPlaySignal.set();
		publishSignal.set();
		readSignal.set();

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
		return new RTMFPFlow(id, signature, *this, _pMainStream, idWriterRef);
	}
	else if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true); // TODO: see if it is really a GroupStream
		return new RTMFPFlow(id, signature, pStream, *this, idWriterRef);
	}
	else {
		ex.set<Ex::Protocol>("Unhandled signature type : ",  String::Hex(BIN signature.data(), signature.size()), " , cannot create RTMFPFlow");
	}
	return NULL;
}

bool RTMFPSession::connect(Exception& ex, const char* url, const char* host) {

	lock_guard<mutex> lock(_mutexConnections);
	_url = url;
	_host = host;

	// Generate the raw url
	BinaryWriter urlWriter(_rawUrl);
	urlWriter.write7BitValue(strlen(url) + 1);
	urlWriter.write8('\x0A').write(url);

	// Extract the port
	const char *port = strrchr(host, ':'), *ipv6End = strrchr(host, ']');
	if (port && (!ipv6End || port > ipv6End))
		_host.resize(port++ - host);
	else
		port = "1935";

	DEBUG("Trying to resolve the host address...")
	HostEntry hostEntry;
	SocketAddress address;
	if (address.set(ex, _host, port))
		_handshaker.startHandshake(_pHandshake, address, this, false, false);
	else if (DNS::Resolve(ex, _host, hostEntry)){
		PEER_LIST_ADDRESS_TYPE addresses;
		for (auto itAddress : hostEntry.addresses()) {
			if (address.set(ex, itAddress, port))
				addresses.emplace(address, RTMFP::ADDRESS_PUBLIC);
		}
		if (addresses.empty())
			return false;
		address.reset();
		_handshaker.startHandshake(_pHandshake, address, addresses, this, false, false);
	} else
		return false;
	return true;
}

UInt16 RTMFPSession::connect2Peer(const char* peerId, const char* streamName) {
	lock_guard<mutex> lock(_mutexConnections);
	PEER_LIST_ADDRESS_TYPE emptyAddresses;
	SocketAddress emptyHost; // We don't know the peer's host address
	if (connect2Peer(peerId, streamName, emptyAddresses, emptyHost, _mediaCount + 1)) {
		_mapPlayers.emplace(piecewise_construct, forward_as_tuple(++_mediaCount), forward_as_tuple());
		return _mediaCount;
	}
	return 0;
}

bool RTMFPSession::connect2Peer(const string& peerId, const char* streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress, UInt16 mediaId) {
	if (status != RTMFP::CONNECTED) {
		ERROR("Cannot start a P2P connection before being connected to the server")
		return false;
	}

	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer != _mapPeersById.end() && itPeer->first == peerId) {
		DEBUG("Unable to create the P2P session to ", peerId, ", we are already connecting/connected to it")
		return false;
	}

	DEBUG("Connecting to peer ", peerId, "...")
	itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId), 
		forward_as_tuple(new P2PSession(this, peerId, _invoker, _pOnSocketError, _pOnStatusEvent, hostAddress, false, (bool)_group, mediaId)));
	_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

	shared_ptr<P2PSession> pPeer = itPeer->second;
	// P2P unicast : add command play to send when connected
	if (streamName) 
		pPeer->setStreamName(streamName);

	_handshaker.startHandshake(itPeer->second->handshake(), hostAddress? hostAddress : _address, addresses, (FlowManager*)itPeer->second.get(), false, true);
	return true;
}

UInt16 RTMFPSession::connect2Group(const char* streamName, RTMFPGroupConfig* parameters) {
	INFO("Connecting to group ", parameters->netGroup, "...")

	if (strncmp("G:", parameters->netGroup, 2) != 0) {
		ERROR("Group ID not well formated, it must begin with 'G:'")
		return 0;
	}

	string value;
	bool groupV2 = false;
	const char* endMarker = NULL;

	// Create the reader of NetGroup ID
	Buffer buff;
	String::ToHex(parameters->netGroup + 2, buff);
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
		return 0;
	}
	string groupTxt(parameters->netGroup, endMarker);

	// Compute the encrypted group specifier ID (2 consecutive sha256)
	UInt8 encryptedGroup[32];
	EVP_Digest(groupTxt.data(), groupTxt.size(), (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL);
	if (groupV2)
		EVP_Digest(encryptedGroup, 32, (unsigned char *)encryptedGroup, NULL, EVP_sha256(), NULL); // v2 groupspec needs 2 sha256s
	String groupHex(String::Hex(encryptedGroup, 32));
	DEBUG("Encrypted Group Id : ", groupHex)

	{
		lock_guard<mutex> lock(_mutexConnections);
		if (parameters->isPublisher) {
			if (_pPublisher) {
				WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
				return 0;
			}
			_pPublisher.reset(new Publisher(streamName, _invoker, true, true, true));
		} else // Create the player
			_mapPlayers.emplace(piecewise_construct, forward_as_tuple(_mediaCount+1), forward_as_tuple());

		_group.reset(new NetGroup(++_mediaCount, groupHex, groupTxt, streamName, *this, parameters));
		_group->onMedia = onMediaPlay;
		_group->onStatus = _pMainStream->onStatus;
		_waitingGroup.push_back(groupHex);
		return _mediaCount;
	}
}

int RTMFPSession::read(UInt16 mediaId, UInt8* buf, UInt32 size, int& nbRead) {
	
	lock_guard<mutex> lock(_mutexConnections);
	if (status != RTMFP::CONNECTED) {
		WARN("Connection is not established, cannot read data")
		return 0; // to stop the parent loop
	}
	if (nbRead != 0) {
		ERROR("Parameter nbRead must equal zero in readAsync()")
		return -1;
	}
	auto itMedia = _mapPlayers.find(mediaId);
	if (itMedia == _mapPlayers.end()) {
		WARN("Unable to find media ", mediaId, ", it can be closed")
		return 0;
	}

	bool available = false;
	if (!itMedia->second.mediaPackets.empty()) {
		// First read => send header
		BinaryWriter writer(buf, size);
		if (itMedia->second.firstRead && size > 13) {
			writer.write(EXPAND("FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00"));
			itMedia->second.firstRead = false;
		}

		// While media packets are available and buffer is not full
		while (!itMedia->second.mediaPackets.empty() && (writer.size() < size - 15)) {

			// Read next packet
			std::shared_ptr<MediaPlayer::RTMFPMediaPacket>& packet = itMedia->second.mediaPackets.front();
			UInt32 bufferSize = packet->size() - packet->pos;
			UInt32 toRead = (bufferSize > (size - writer.size() - 15)) ? size - writer.size() - 15 : bufferSize;

			// header
			if (!packet->pos) {
			writer.write8(packet->type);
			writer.write24(packet->size()); // size on 3 bytes
			writer.write24(packet->time); // time on 3 bytes
			writer.write32(0); // unknown 4 bytes set to 0
			}
			writer.write(packet->data() + packet->pos, toRead); // payload

			// If packet too big : save position and exit, else write footer
			if (bufferSize > toRead) {
				packet->pos += toRead;
				break;
			}
			writer.write32(11 + packet->size()); // footer, size on 4 bytes
			itMedia->second.mediaPackets.pop_front();
		}
		// Finally update the nbRead & available
		nbRead = writer.size();
		available = !itMedia->second.mediaPackets.empty();
	}
	if (!available && dataAvailable)
		dataAvailable = false;
	return 1;
}

bool RTMFPSession::write(const UInt8* data, UInt32 size, int& pos) {
	{
		lock_guard<mutex> lock(_mutexConnections);
		if (!_pPublisher || !_pPublisher->count()) {
			DEBUG("Can't write data because NetStream is not published")
			return true;
		}
		if (status >= RTMFP::NEAR_CLOSED) {
			pos = -1;
			return false; // to stop the parent loop
		}
	}

	pos = 0;
	BinaryReader reader(data, size);
	if (reader.available()<14) {
		DEBUG("Packet too small")
		return false;
	}

	const UInt8* cur = reader.current();
	if (*cur == 'F' && *(++cur) == 'L' && *(++cur) == 'V') { // header
		reader.next(13);
		pos = 13;
	}

	// Send all packets
	while (reader.available()) {
		if (reader.available() < 11) // smaller than flv header
			break;

		UInt8 type = reader.read8();
		UInt32 bodySize = reader.read24();
		UInt32 time = reader.read24();
		reader.next(4); // ignored

		if (reader.available() < bodySize + 4)
			break; // we will wait for further data

		if (type == AMF::TYPE_AUDIO)
			_invoker.handler.queue(onPushAudio, time, Packet(reader.current(), bodySize));
		else if (type == AMF::TYPE_VIDEO)
			_invoker.handler.queue(onPushVideo, time, Packet(reader.current(), bodySize));
		else
			WARN("Unhandled packet type : ", type)
		reader.next(bodySize);
		UInt32 sizeBis = reader.read32();
		pos += bodySize + 15;
		if (sizeBis != bodySize + 11) {
			ERROR("Unexpected size found after payload : ", sizeBis, " (expected: ", bodySize + 11, ")")
			break;
		}
	}
	_invoker.handler.queue(onFlushPublisher); // TODO: see if it is executed in sequence
	return true;
}

unsigned int RTMFPSession::callFunction(const char* function, int nbArgs, const char** args, const char* peerId) {
	// Server call
	if (!peerId && _pMainStream && _pMainWriter) {
		// TODO: refactorize with P2PSession code
		AMFWriter& amfWriter = _pMainWriter->writeInvocation(function, true);
		for (int i = 0; i < nbArgs; i++) {
			if (args[i])
				amfWriter.writeString(args[i], strlen(args[i]));
		}
		_pMainWriter->flush();
	// NetGroup call
	} else if (strcmp(peerId, "all") == 0) {
		if (_group)
			return _group->callFunction(function, nbArgs, args);
	}
	// Peer call
	else {
		for (auto &it : _mapPeersById) {
			if ((strcmp(peerId, "all") == 0) || it.second->peerId == peerId)
				return it.second->callFunction(function, nbArgs, args);
		}
		ERROR("Unable to find the peer", peerId, " for sending the function call")
	}

	return 0;
}

void RTMFPSession::manage() {
	lock_guard<mutex> lock(_mutexConnections);
	if (!_pMainStream)
		return;

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

	// Treat waiting commands
	createWaitingStreams();

	// Send waiting P2P connections
	sendConnections();

	// Send waiting handshake requests
	_handshaker.manage();

	// Manage NetGroup
	if (_group)
		_group->manage();

	// notify the client that data is available to flush
	if (dataAvailable)
		readSignal.set();
}

Base::UInt16 RTMFPSession::addStream(bool publisher, const char* streamName, bool audioReliable, bool videoReliable) {
	lock_guard<mutex> lock(_mutexConnections);

	if (publisher && _pPublisher) {
		WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
		return 0;
	}
		
	_waitingStreams.emplace(publisher, streamName, ++_mediaCount, audioReliable, videoReliable);
	if (!publisher)
		_mapPlayers.emplace(piecewise_construct, forward_as_tuple(_mediaCount), forward_as_tuple());
	INFO("Creation of the ", publisher? "publisher" : "player", " stream ", _mediaCount)
	return _mediaCount;
}


bool RTMFPSession::startP2PPublisher(const char* streamName, bool audioReliable, bool videoReliable) {
	lock_guard<mutex> lock(_mutexConnections);

	INFO("Creating publisher for stream ", streamName, "...")
	if (_pPublisher) {
		WARN("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
		return false;
	}
	
	_pPublisher.reset(new Publisher(streamName, _invoker, audioReliable, videoReliable, true));
	return true;
}

bool RTMFPSession::closePublication(const char* streamName) {
	lock_guard<mutex> lock(_mutexConnections);

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

bool RTMFPSession::createWaitingStreams() {
	if (status != RTMFP::CONNECTED || _waitingStreams.empty() || _isWaitingStream)
		return false;

	if (!_pMainWriter) {
		ERROR("Unable to find the main writer related to the main stream")
		return false;
	}

	_pMainStream->createStream();
	_pMainWriter->writeInvocation("createStream");
	_pMainWriter->flush();
	return true;
}

void RTMFPSession::sendConnections() {

	// Send waiting group connections
	while (status == RTMFP::CONNECTED && !_waitingGroup.empty()) {
		string& group = _waitingGroup.front();
		sendGroupConnection(group);
		_waitingGroup.pop_front();
	}
}

void RTMFPSession::onNetConnectionSuccess() {
	if (!_pMainWriter) {
		ERROR("Unable to find the main writer to send setPeerInfo request")
		return;
	}

	UInt16 port = _pSocket->socket()->address().port();
	UInt16 portIPv6 = _pSocketIPV6->socket()->address().port();
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
	connectReady = true;
	connectSignal.set();
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

	publishReady = true;
	publishSignal.set();
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

bool RTMFPSession::onNewPeerId(const SocketAddress& address, shared_ptr<Handshake>& pHandshake, UInt32 farId, const string& rawId, const string& peerId) {

	// If the peer session doesn't exists we create it
	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer == _mapPeersById.end() || itPeer->first != peerId) {
		SocketAddress emptyHost; // We don't know the peer's host address
		itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId),
			forward_as_tuple(new P2PSession(this, peerId, _invoker, _pOnSocketError, _pOnStatusEvent, emptyHost, true, (bool)_group)));
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
