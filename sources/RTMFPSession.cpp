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

UInt32 RTMFPSession::RTMFPSessionCounter = 0x02000000;

RTMFPSession::RTMFPSession(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) : _rawId("\x21\x0f", PEER_ID_SIZE + 2),
	_handshaker(this), _nbCreateStreams(0), p2pPublishReady(false), p2pPlayReady(false), publishReady(false), connectReady(false), dataAvailable(false), FlowManager(false, invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onPacket = [this](PoolBuffer& pBuffer, const SocketAddress& address) {
		lock_guard<mutex> lock(_mutexConnections);
		if (status < RTMFP::NEAR_CLOSED) {

			// Decode the RTMFP data
			if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
				ERROR("Invalid RTMFP packet on connection to ", _address.toString())
				return;
			}

			BinaryReader reader(pBuffer.data(), pBuffer.size());
			UInt32 idSession = RTMFP::Unpack(reader);
			pBuffer->clip(reader.position());

			if (!idSession)
				_handshaker.process(address, pBuffer);
			else {
				auto itSession = _mapSessions.find(idSession);
				if (itSession == _mapSessions.end()) {
					WARN("Unknown session ", Format<UInt32>("0x%.8x", idSession), " in packet from ", address.toString())
					return;
				}
				itSession->second->process(address, pBuffer);
			}
		}
	};
	onError = [this](const Exception& ex) {
		SocketAddress address;
		DEBUG("Socket error : ", ex.error(), " from ", _pSocket->peerAddress(address).toString())
	};
	onStreamCreated = [this](UInt16 idStream) {
		return handleStreamCreated(idStream);
	};
	onNewPeer = [this](const string& rawId, const string& peerId) {
		handleNewGroupPeer(rawId, peerId);
	};

	_sessionId = RTMFPSessionCounter++;

	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnNewPeer::subscribe(onNewPeer);

	_pSocket.reset(new UDPSocket(_pInvoker->sockets));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);
	_pSocketIPV6.reset(new UDPSocket(_pInvoker->sockets));
	_pSocketIPV6->OnError::subscribe(onError);
	_pSocketIPV6->OnPacket::subscribe(onPacket);
	Exception ex;
	SocketAddress address(SocketAddress::Wildcard(IPAddress::IPv6));
	if (!_pSocketIPV6->bind(ex, address))
		WARN("Unable to bind [::], ipv6 will not work : ", ex.error())

	// Add the session ID to the map
	_mapSessions.emplace(_sessionId, this);
}

RTMFPSession::~RTMFPSession() {
	DEBUG("Deletion of RTMFPSession ", name())

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

	{
		lock_guard<std::mutex> lock(_mutexConnections);
		// Close the session & writers
		_pGroupWriter.reset();
		_pMainWriter.reset();
		FlowManager::close(abrupt);

		if (abrupt) {
			// Close the NetGroup
			if (_group)
				_group->close();

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
		}
	}

	// Close the sockets if we are closing abruptly
	if (abrupt) {

		if (_pMainStream) {
			_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
			_pMainStream->OnNewPeer::unsubscribe(onNewPeer);
		}
		// Unsubscribing to socket : we don't want to receive packets anymore
		if (_pSocket) {
			_pSocket->OnPacket::unsubscribe(onPacket);
			_pSocket->OnError::unsubscribe(onError);
			_pSocket.reset();
		}
		if (_pSocketIPV6) {
			_pSocketIPV6->OnPacket::unsubscribe(onPacket);
			_pSocketIPV6->OnError::unsubscribe(onError);
			_pSocketIPV6.reset();
		}
	}
}

RTMFPFlow* RTMFPSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature, UInt64 idWriterRef) {

	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		DEBUG("Creating new Flow (", id, ") for NetConnection ", name())
		_mainFlowId = id;
		return new RTMFPFlow(id, signature, poolBuffers(), *this, _pMainStream, idWriterRef);
	}
	else if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true); // TODO: see if it is really a GroupStream
		return new RTMFPFlow(id, signature, pStream, poolBuffers(), *this, idWriterRef);
	}
	else {
		string tmp;
		ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	}
	return NULL;
}

void RTMFPSession::handleWriterClosed(shared_ptr<RTMFPWriter>& pWriter) {
	// We reset the pointers before closure
	if (pWriter == _pGroupWriter)
		_pGroupWriter.reset();
	else if (pWriter == _pMainWriter)
		_pMainWriter.reset();
}

bool RTMFPSession::connect(Exception& ex, const char* url, const char* host) {
	if (!_pInvoker) {
		ex.set(Exception::APPLICATION, "Invoker is not initialized");
		return false;
	}

	_url = url;
	_host = host;

	// Genereate the raw url
	RTMFP::Write7BitValue(_rawUrl, strlen(url) + 1);
	String::Append(_rawUrl, '\x0A', url);

	// Extract the port
	const char *port = strrchr(host, ':'), *ipv6End = strrchr(host, ']');
	if (port && (!ipv6End || port > ipv6End))
		_host.resize(port++ - host);
	else
		port = "1935";

	lock_guard<std::mutex> lock(_mutexConnections);
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
		address.clear();
		_handshaker.startHandshake(_pHandshake, address, addresses, this, false, false);
	} else
		return false;
	return true;
}

void RTMFPSession::connect2Peer(const char* peerId, const char* streamName) {
	lock_guard<std::mutex> lock(_mutexConnections);
	PEER_LIST_ADDRESS_TYPE addresses;
	connect2Peer(peerId, streamName, addresses, _address);
}

void RTMFPSession::connect2Peer(const string& peerId, const char* streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress) {
	if (status != RTMFP::CONNECTED) {
		ERROR("Cannot start a P2P connection before being connected to the server")
		return;
	}

	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer != _mapPeersById.end() && itPeer->first == peerId) {
		DEBUG("Unable to create the P2P session to ", peerId, ", we are already connecting/connected to it")
		return;
	}

	DEBUG("Connecting to peer ", peerId, "...")
	itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId), 
		forward_as_tuple(new P2PSession(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, hostAddress, false, (bool)_group)));
	_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

	shared_ptr<P2PSession> pPeer = itPeer->second;
	// P2P unicast : add command play to send when connected
	if (streamName) 
		pPeer->addCommand(NETSTREAM_PLAY, streamName);

	_handshaker.startHandshake(itPeer->second->handshake(), hostAddress, addresses, (FlowManager*)itPeer->second.get(), false, true);
}

void RTMFPSession::connect2Group(const char* streamName, RTMFPGroupConfig* parameters) {
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

	lock_guard<std::mutex> lock(_mutexConnections);
	_group.reset(new NetGroup(groupHex, groupTxt, streamName, *this, parameters));
	_waitingGroup.push_back(groupHex);
}

bool RTMFPSession::read(const char* peerId, UInt8* buf, UInt32 size, int& nbRead) {
	
	bool res(true);
	auto itPeer = _mapPeersById.find(peerId);
	if (itPeer != _mapPeersById.end() && (!(res = itPeer->second->readAsync(buf, size, nbRead)) || nbRead > 0))
		return res; // quit if treated

	if (!(res = readAsync(buf, size, nbRead)) || nbRead>0)
		return res; // quit if treated
	return true;
}

void RTMFPSession::handleDataAvailable(bool isAvailable) {
	dataAvailable = isAvailable; 
	if (dataAvailable)
		readSignal.set(); // notify the client that data is available
}

bool RTMFPSession::write(const UInt8* buf, UInt32 size, int& pos) {
	pos = 0;
	if (status == RTMFP::FAILED) {
		pos = -1;
		return false; // to stop the parent loop
	}

	if(!_pPublisher || !_pPublisher->count()) {
		DEBUG("Can't write data because NetStream is not published")
		return true;
	}

	return _pPublisher->publish(buf, size, pos);
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

bool RTMFPSession::handleStreamCreated(UInt16 idStream) {
	DEBUG("Stream ", idStream, " created, sending command...")

	// Get command
	if (_waitingCommands.empty()) {
		ERROR("created stream without command")
		return false;
	}
	const StreamCommand& command = _waitingCommands.back();

	// Manage publisher if it is a publish command
	if (command.type == NETSTREAM_PUBLISH) {
		if (_pPublisher) {
			ERROR("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			_waitingCommands.pop_back();
			return false;
		}
	}

	// Stream created, now we create the writer before sending another request
	string signature("\x00\x54\x43\x04", 4);
	RTMFP::Write7BitValue(signature, idStream);
	shared_ptr<RTMFPWriter> pWriter = createWriter(signature, _mainFlowId);

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
		_pPublisher.reset(new Publisher(command.value, *_pInvoker, command.audioReliable, command.videoReliable, false));
		break;
	}
	default:
		ERROR("Unexpected command found on stream creation : ", command.type)
		return false;
	}
	_waitingCommands.pop_back();
	return true;
}

void RTMFPSession::manage() {
	if (!_pMainStream)
		return;
	lock_guard<std::mutex> lock(_mutexConnections);

	// Release closed P2P connections
	auto itConnection = _mapPeersById.begin();
	while (itConnection != _mapPeersById.end()) {
		if (itConnection->second->closed()) {
			DEBUG("RTMFPSession management - Deleting closed P2P session to ", itConnection->first)
			_mapSessions.erase(itConnection->second->sessionId());
			_mapPeersById.erase(itConnection++);
		}
		else {
			itConnection->second->manage();
			++itConnection;
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
}

// TODO: see if we always need to manage a list of commands
void RTMFPSession::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {
		
	_waitingCommands.emplace_back(command, streamName, audioReliable, videoReliable);
	if (command != NETSTREAM_CLOSE && command != NETSTREAM_PUBLISH_P2P)
		_nbCreateStreams++;
}

void RTMFPSession::createWaitingStreams() {
	//lock_guard<std::mutex>	lock(_mutexConnections);
	if (status != RTMFP::CONNECTED || _waitingCommands.empty())
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
				if (_pListener) {
					_pPublisher->removeListener(name());
					_pListener = NULL;
				}
				if (_group)
					_group->stopListener();
			}
			_waitingCommands.erase(itCommand++);
		} 
		else if (itCommand->type == NETSTREAM_PUBLISH_P2P) {
			INFO("Creating publisher for stream ", itCommand->value, "...")
			if (_pPublisher)
				ERROR("A publisher already exists (name : ", _pPublisher->name(), "), command ignored")
			else
				_pPublisher.reset(new Publisher(itCommand->value, *_pInvoker, itCommand->audioReliable, itCommand->videoReliable, true));
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
		_pMainStream->createStream();
		AMFWriter& amfWriter = _pMainWriter->writeInvocation("createStream");
		_pMainWriter->flush();
		_nbCreateStreams--;
	}
}

void RTMFPSession::sendConnections() {

	// Send waiting group connections
	while (status == RTMFP::CONNECTED && !_waitingGroup.empty()) {
		string& group = _waitingGroup.front();
		sendGroupConnection(group);
		_waitingGroup.pop_front();
	}
}

void RTMFPSession::onConnect() {

	// Record port for setPeerInfo request
	if (!_pMainWriter) {
		ERROR("Unable to find the main writer to send setPeerInfo request")
		return;
	}
	UInt16 port = _pSocket->address().port();
	UInt16 portIPv6 = _pSocketIPV6->address().port();
	INFO("Sending peer info (port : ", port, " - port ipv6 : ", portIPv6,")")
	AMFWriter& amfWriter = _pMainWriter->writeInvocation("setPeerInfo");

	vector<IPAddress> addresses = IPAddress::Locals();
	string buf;
	for (auto it : addresses) {
		if (it.isLoopback() || it.isLinkLocal())
			continue; // ignore loopback and link-local addresses
		String::Format(buf, it.toString(), ":", (it.family() == IPAddress::IPv4)? port : portIPv6);
		amfWriter.writeString(buf.c_str(), buf.size());
	}

	// We are connected : unlock the possible blocking RTMFP_Connect function
	connectReady = true;
	connectSignal.set();
}

void RTMFPSession::onPublished(UInt16 streamId) {
	Exception ex;
	_pPublisher->start();

	string signature("\x00\x54\x43\x04",4);
	RTMFP::Write7BitValue(signature, streamId);
	shared_ptr<RTMFPWriter> pDataWriter = createWriter(signature,_mainFlowId);
	shared_ptr<RTMFPWriter> pAudioWriter = createWriter(signature, _mainFlowId);
	shared_ptr<RTMFPWriter> pVideoWriter = createWriter(signature, _mainFlowId);

	if (!(_pListener = _pPublisher->addListener<FlashListener, shared_ptr<RTMFPWriter>&>(ex, name(), pDataWriter, pAudioWriter, pVideoWriter)))
		WARN(ex.error())

	publishReady = true;
	publishSignal.set();
}

void RTMFPSession::stopListening(const std::string& peerId) {
	INFO("Deletion of the listener to ", peerId)
	if (_pPublisher)
		_pPublisher->removeListener(peerId);
}

void RTMFPSession::handleNewGroupPeer(const string& rawId, const string& peerId) {
	
	if (!_group || !_group->checkPeer(peerId)) {
		DEBUG("Unable to add the peer ", peerId, ", it can be a wrong group ID or the peer already exists")
		return;
	}

	PEER_LIST_ADDRESS_TYPE addresses;
	connect2Peer(peerId.c_str(), "", addresses, _address);
	_group->addPeer2HeardList(peerId, rawId.c_str(), addresses, _address);
}

void RTMFPSession::handleWriterException(std::shared_ptr<RTMFPWriter>& pWriter) {
	Exception ex;
	pWriter->fail(ex, "Writer terminated on connection ", name());

	if (pWriter == _pGroupWriter)
		_pGroupWriter.reset();
	else if (pWriter == _pMainWriter)
		_pMainWriter.reset();

	WARN(ex.error())
}

void RTMFPSession::handleP2PAddressExchange(PacketReader& reader) {
	if (reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange header")
		return;
	}

	// Read our peer id and address of initiator
	string buff;
	reader.read(PEER_ID_SIZE, buff);
	SocketAddress address;
	UInt8 addressType;
	RTMFP::ReadAddress(reader, address, addressType);

	string tag;
	reader.read(16, tag);
	DEBUG("A peer will contact us with address : ", address.toString())

	// Send the handshake 70 to the peer
	_handshaker.sendHandshake70(tag, address, _address);
}

void RTMFPSession::sendGroupConnection(const string& netGroup) {

	string signature("\x00\x47\x43", 3);
	_pGroupWriter = createWriter(signature, _mainFlowId);
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
	FATAL_ASSERT(_group)
	return _group->idHex;
}

const string& RTMFPSession::groupIdTxt() { 
	FATAL_ASSERT(_group)
	return _group->idTxt;
}

void RTMFPSession::buildPeerID(const UInt8* data, UInt32 size) {
	// Peer ID built, we save it
	EVP_Digest(data, size, BIN(_rawId.data() + 2), NULL, EVP_sha256(), NULL);
	INFO("Peer ID : \n", Util::FormatHex(BIN(_rawId.data() + 2), PEER_ID_SIZE, _peerTxtId))
}

bool RTMFPSession::onNewPeerId(const SocketAddress& address, shared_ptr<Handshake>& pHandshake, UInt32 farId, const string& rawId, const string& peerId) {

	// If the peer session doesn't exists we create it
	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer == _mapPeersById.end() || itPeer->first != peerId) {
		itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId),
			forward_as_tuple(new P2PSession(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _address, true, (bool)_group)));
		_mapSessions.emplace(itPeer->second->sessionId(), itPeer->second.get());

		// associate the handshake & session
		pHandshake->pSession = itPeer->second.get();
		itPeer->second->handshake() = pHandshake;
		itPeer->second->setAddress(address);

		if (_group)
			_group->addPeer2HeardList(itPeer->second->peerId, itPeer->second->rawId.data(), itPeer->second->addresses(), itPeer->second->hostAddress, true);
	}
	else
		return itPeer->second->onHandshake38(address, pHandshake);
	
	return true;
}

void RTMFPSession::onConnection() {
	INFO("RTMFPSession is now connected to ", name())

	string signature("\x00\x54\x43\x04\x00", 5);
	_pMainWriter = createWriter(signature, 0);

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
