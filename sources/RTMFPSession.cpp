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
#include "FlowManager.h"
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
#include "RTMFPConnection.h"
#include "SocketHandler.h"

using namespace Mona;
using namespace std;

UInt32 RTMFPSession::RTMFPSessionCounter = 0x02000000;

RTMFPSession::RTMFPSession(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) : 
	_nbCreateStreams(0), _port("1935"), p2pPublishReady(false), publishReady(false), connectReady(false), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onStreamCreated = [this](UInt16 idStream) {
		handleStreamCreated(idStream);
	};
	onNewPeer = [this](const string& peerId) {
		handleNewGroupPeer(peerId);
	};
	onPeerHandshake30 = [this](const string& tag, const SocketAddress& address) {
		if (status != RTMFP::CONNECTED || !_pConnection)
			DEBUG("Message received but we are not connected, rejected")
		else if (!_group && !(_pPublisher && _pPublisher->isP2P))
			DEBUG("P2P connection received but we are not publisher and not in a netgroup, rejected")
		else {
			shared_ptr<RTMFPConnection> pConnection = _pSocketHandler->addConnection(address, NULL, true, true);
			pConnection->sendHandshake70(tag);
			// we don't know the peer id for now, P2PSession will be created after in onNewPeerId
		}
	};
	onPeerHandshake70 = [this](const string& peerId, const SocketAddress& address, const string& farKey, const string& cookie) {
		auto itSession = _mapPeersById.find(peerId);
		if (itSession == _mapPeersById.end()) {
			ERROR("Unknown peer ", peerId, " in onP2PAddresses()") // should not happen
			return;
		}
		shared_ptr<RTMFPConnection> pConnection = _pSocketHandler->addConnection(address, itSession->second.get(), false, true);
		itSession->second->subscribe(pConnection);

		if (itSession->second->status > RTMFP::HANDSHAKE30)
			DEBUG("Handshake 70 ignored for peer ", peerId, " we are already in state ", itSession->second->status)
		else
			pConnection->sendHandshake38(farKey, cookie);
	};
	onNewPeerId = [this](shared_ptr<RTMFPConnection>& pConn, const string& rawId, const string& peerId) {
		// If the peer session already exists we just update the addresses
		auto itPeer = _mapPeersById.lower_bound(peerId);
		if (itPeer != _mapPeersById.end() && itPeer->first == peerId) {
			itPeer->second->subscribe(pConn);
			return false;
		}

		itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId), 
			forward_as_tuple(new P2PSession(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, _pConnection->address(), true, (bool)_group)));
		itPeer->second->subscribe(pConn);

		if (_group)
			_group->addPeer2HeardList(itPeer->second->peerId, itPeer->second->rawId.data(), itPeer->second->addresses(), itPeer->second->hostAddress, true);
		return true;
	};
	onPeerIdBuilt = [this](const string& rawId, const string& peerId) {
		// Peer ID built, we save it
		_peerTxtId = peerId.c_str();
		memcpy(_rawId, BIN rawId.data(), PEER_ID_SIZE + 2);
	};
	onConnection = [this](shared_ptr<RTMFPConnection>& pConnection, const string& nameSession) {

		auto itPeer = _mapPeersById.find(nameSession);
		if (itPeer != _mapPeersById.end()) {
			itPeer->second->onConnection(pConnection);
		} else {
			INFO("Connection is now connected to ", name())

			status = RTMFP::CONNECTED;
			_pConnection = pConnection;
			if (!_pFlowNull)
				_pFlowNull.reset(new RTMFPFlow(0, String::Empty, _pMainStream, poolBuffers(), *pConnection));

			string signature("\x00\x54\x43\x04\x00", 5);
			new RTMFPWriter(FlashWriter::OPENED, signature, *pConnection);  // it will be automatically associated to RTMFPSession::_pMainWriter
		}
	};
	onWriterClose = [this](shared_ptr<RTMFPWriter>& pWriter) {
		// We reset the pointers before closure
		if (pWriter == _pGroupWriter)
			_pGroupWriter.reset();
		else if (pWriter == _pMainWriter)
			_pMainWriter.reset();
	};
	onP2PAddresses = [this](const string& peerId, const PEER_LIST_ADDRESS_TYPE& addresses) {
		auto itSession = _mapPeersById.find(peerId);
		if (itSession != _mapPeersById.end()) {

			if (_group)
				_group->addPeer2HeardList(itSession->second->peerId, itSession->second->rawId.data(), itSession->second->addresses(), itSession->second->hostAddress, true);

			if (itSession->second->status != RTMFP::STOPPED)
				DEBUG("Addresses ignored for peer ", peerId, " we are already in state ", itSession->second->status)
			else {
				for (auto itAddress : addresses) {
					shared_ptr<RTMFPConnection> pConnection = _pSocketHandler->addConnection(itAddress.first, itSession->second.get(), false, true);
					itSession->second->subscribe(pConnection);
				}
				return true;
			}
		}
		else
			ERROR("Unknown peer ", peerId, " in onP2PAddresses()") // should not happen
		return false;
	};


	_sessionId = ++RTMFPSessionCounter;

	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnNewPeer::subscribe(onNewPeer);

	_pSocketHandler.reset(new SocketHandler(invoker, this));
	_pSocketHandler->OnIdBuilt::subscribe(onPeerIdBuilt);
	_pSocketHandler->OnPeerHandshake30::subscribe(onPeerHandshake30);
	_pSocketHandler->OnPeerHandshake70::subscribe(onPeerHandshake70);
	_pSocketHandler->OnNewPeerId::subscribe(onNewPeerId);
	_pSocketHandler->OnConnection::subscribe(onConnection);
	_pSocketHandler->OnP2PAddresses::subscribe(onP2PAddresses);
}

RTMFPSession::~RTMFPSession() {

	// Close the NetGroup
	if (_group)
		_group->close();

	// Close peers
	for(auto it : _mapPeersById)
		it.second->close();
	_mapPeersById.clear();

	// Close listener & publisher
	if (_pListener && _pPublisher) {
		_pPublisher->removeListener(name());
		_pListener = NULL;
	}
	if (_pPublisher && _pPublisher->running())
		_pPublisher->stop();
	_pPublisher.reset();

	if (_pMainStream) {
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream->OnNewPeer::unsubscribe(onNewPeer);
	}

	// Close the flows & the main stream
	close();

	// And finally close the writers and socket
	if (_pSocketHandler) {
		_pSocketHandler->close();
		_pSocketHandler->OnIdBuilt::unsubscribe(onPeerIdBuilt);
		_pSocketHandler->OnPeerHandshake30::unsubscribe(onPeerHandshake30);
		_pSocketHandler->OnPeerHandshake70::unsubscribe(onPeerHandshake70);
		_pSocketHandler->OnNewPeerId::unsubscribe(onNewPeerId);
		_pSocketHandler->OnConnection::unsubscribe(onConnection);
		_pSocketHandler->OnP2PAddresses::unsubscribe(onP2PAddresses);
	}
}

void RTMFPSession::addConnection(const Mona::SocketAddress& address) {

	if (address.family() == IPAddress::IPv6)
		DEBUG("Ignored address ", address.toString(), ", IPV6 not supported yet") // TODO: support IPV6
	else {
		shared_ptr<RTMFPConnection> pConnection = _pSocketHandler->addConnection(address, this, false, false);
		subscribe(pConnection);
	}
}

RTMFPFlow* RTMFPSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature) {
	FATAL_ASSERT(_pConnection)

	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		DEBUG("Creating new Flow (", id, ") for NetConnection ", name())
		return new RTMFPFlow(id, signature, poolBuffers(), *((BandWriter*)_pConnection.get()), _pMainStream);
	}
	else if (signature.size() > 2 && signature.compare(0, 3, "\x00\x47\x43", 3) == 0) { // NetGroup
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true); // TODO: see if it is really a GroupStream
		return new RTMFPFlow(id, signature, pStream, poolBuffers(), *((BandWriter*)_pConnection.get()));
	}
	else {
		string tmp;
		ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	}
	return NULL;
}

void RTMFPSession::handleNewWriter(shared_ptr<RTMFPWriter>& pWriter) {

	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // newWriter will be associated to the NetConnection flow (first in _flow lists) 

	if (pWriter->signature.size() > 2 && pWriter->signature.compare(0, 3, "\x00\x47\x43", 3) == 0 && !_pGroupWriter)
		_pGroupWriter = pWriter; // save the group writer (TODO: see if it can happen, I think we are always the initiator of the group communication)
	if (pWriter->signature.size() > 4 && pWriter->signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0 && !_pMainWriter) {
		_pMainWriter = pWriter;
		_pMainStream->connect(*_pMainWriter, _url); // send the connect request
	}
}

bool RTMFPSession::connect(Exception& ex, const char* url, const char* host) {
	if (!_pInvoker) {
		ex.set(Exception::APPLICATION, "Invoker is not initialized");
		return false;
	}

	_url = url;
	RTMFP::Write7BitValue(_rawUrl, strlen(url) + 1);
	String::Append(_rawUrl, '\x0A', url);
	_host = host;
	const char* port = strrchr(host, ':');
	if (port) {
		_port = (port + 1);
		_host[port - host] = '\0';
	}

	lock_guard<recursive_mutex> lock(_mutexConnections);
	DEBUG("Trying to resolve the host address...")
	HostEntry hostEntry;
	SocketAddress address;
	if (address.set(ex, _host, _port))
		addConnection(address);
	else if (DNS::Resolve(ex, _host, hostEntry)) {
		for (auto itAddress : hostEntry.addresses()) {
			if (address.set(ex, itAddress, _port))
				addConnection(address);
			else
				WARN("Error with address ", itAddress.toString(), " : ", ex.error())
		}
	} else
		return false;
	return true;
}

void RTMFPSession::connect2Peer(const char* peerId, const char* streamName) {
	if (!_pConnection) {
		ERROR("Cannot start a P2P connection before being connected to the server")
		return;
	}

	PEER_LIST_ADDRESS_TYPE addresses;
	connect2Peer(peerId, streamName, addresses, _pConnection->address());
}

void RTMFPSession::connect2Peer(const string& peerId, const char* streamName, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress) {
	INFO("Connecting to peer ", peerId, "...")

	lock_guard<recursive_mutex> lock(_mutexConnections);
	auto itPeer = _mapPeersById.lower_bound(peerId);
	if (itPeer != _mapPeersById.end() && itPeer->first == peerId) {
		DEBUG("Unable to create the P2P session to ", peerId, ", we are already connecting/connected to it")
		return;
	}
	if (status != RTMFP::CONNECTED || !_pConnection) {
		ERROR("Cannot start a P2P connection before being connected to the server")
		return;
	}

	itPeer = _mapPeersById.emplace_hint(itPeer, piecewise_construct, forward_as_tuple(peerId), 
		forward_as_tuple(new P2PSession(this, peerId, _pInvoker, _pOnSocketError, _pOnStatusEvent, _pOnMedia, /*addresses,*/ hostAddress, false, (bool)_group)));

	shared_ptr<P2PSession> pPeer = itPeer->second;
	// P2P unicast : add command play to send when connected
	if (streamName) 
		pPeer->addCommand(NETSTREAM_PLAY, streamName);

	// P2P multicast : create
	for (auto itAddress : addresses) {
		shared_ptr<RTMFPConnection> pConnection = _pSocketHandler->addConnection(itAddress.first, pPeer.get(), false, true);
		pPeer->subscribe(pConnection);
	}

	// Ask server for addresses if needed
	if (addresses.empty() || hostAddress != _pConnection->address())
		_pSocketHandler->addP2PConnection(pPeer->rawId, peerId, pPeer->tag(), hostAddress);
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

	lock_guard<recursive_mutex> lock(_mutexConnections);
	_group.reset(new NetGroup(groupHex, groupTxt, streamName, *this, parameters));
	_waitingGroup.push_back(groupHex);
}

bool RTMFPSession::read(const char* peerId, UInt8* buf, UInt32 size, int& nbRead) {
	
	bool res(true);
	if (!(res = readAsync(peerId, buf, size, nbRead))  || nbRead>0)
		return res; // quit if treated

	for (auto &it : _mapPeersById) {
		if (!(res = it.second->readAsync(peerId, buf, size, nbRead)) || nbRead>0)
			return res; // quit if treated
	}

	return true;
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

void RTMFPSession::handleStreamCreated(UInt16 idStream) {
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
	RTMFPWriter* pWriter = new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection); // (a shared pointer is automatically created)

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
		return;
	}
	_waitingCommands.pop_back();
}

void RTMFPSession::manage() {
	if (!_pMainStream)
		return;

	// Treat waiting commands
	createWaitingStreams();

	// Send waiting P2P connections
	sendConnections();

	if (_pSocketHandler)
		_pSocketHandler->manage();

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
	lock_guard<recursive_mutex>	lock(_mutexCommands);
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
				_pPublisher->removeListener(name());
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
		_pMainStream->createStream(*_pMainWriter);
		_nbCreateStreams--;
	}
}

void RTMFPSession::sendConnections() {
	lock_guard<recursive_mutex> lock(_mutexConnections);

	// Send waiting group connections
	while (status == RTMFP::CONNECTED && !_waitingGroup.empty()) {
		string& group = _waitingGroup.front();
		sendGroupConnection(group);
		_waitingGroup.pop_front();
	}
}

void RTMFPSession::onConnect() {

	// Record port for setPeerInfo request
	if (_pMainStream && _pMainWriter)
		_pMainStream->sendPeerInfo(*_pMainWriter, _pSocketHandler->socket().address().port());

	// We are connected : unlock the possible blocking RTMFP_Connect function
	connectReady = true;
	connectSignal.set();
}

void RTMFPSession::onPublished(FlashWriter& writer) {
	Exception ex;
	_pPublisher->start();
	if (!(_pListener = _pPublisher->addListener<FlashListener, FlashWriter&>(ex, name(), writer)))
		WARN(ex.error())

	publishReady = true;
	publishSignal.set();
}

void RTMFPSession::stopListening(const std::string& peerId) {
	INFO("Deletion of the listener to ", peerId)
	if (_pPublisher)
		_pPublisher->removeListener(peerId);
}

bool RTMFPSession::handlePlay(const string& streamName,FlashWriter& writer) {
	ERROR("Cannot handle play command on a RTMFP Connection") // target error (shouldn't happen)
	return false;
}

void RTMFPSession::handleNewGroupPeer(const string& peerId) {
	
	if (!_group || !_group->checkPeer(peerId)) {
		WARN("Unable to add the peer ", peerId, ", it can be a wrong group ID or the peer already exists")
		return;
	}
	connect2Peer(peerId.c_str(), "");
}

void RTMFPSession::handleProtocolFailed() {
	close();
}

void RTMFPSession::handleWriterFailed(std::shared_ptr<RTMFPWriter>& pWriter) {
	Exception ex;
	pWriter->fail(ex, "Writer terminated on connection ", name());

	if (pWriter == _pGroupWriter)
		_pGroupWriter.reset();
	else if (pWriter == _pMainWriter)
		_pMainWriter.reset();

	WARN(ex.error())
}

void RTMFPSession::handleP2PAddressExchange(PacketReader& reader) {
	// Handle 0x0f message from server (a peer is about to contact us)

	if (reader.read24() != 0x22210F) {
		ERROR("Unexpected P2P address exchange header")
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

	// TODO: see if we should send the handshake 70 directly
}

void RTMFPSession::sendGroupConnection(const string& netGroup) {

	string signature("\x00\x47\x43", 3);
	new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection); // it will be automatically associated to _pGroupWriter

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

const PoolBuffers& RTMFPSession::poolBuffers() {
	return _pInvoker->poolBuffers;
}
