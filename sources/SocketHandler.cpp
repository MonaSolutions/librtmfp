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

#include "SocketHandler.h"
#include "Invoker.h"
#include "RTMFPSession.h"
#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

SocketHandler::SocketHandler(Invoker* invoker, RTMFPSession* pSession) : _pInvoker(invoker), _acceptAll(false), _pMainSession(pSession) {
	onPacket = [this](PoolBuffer& pBuffer, const SocketAddress& address) {
		if (_pMainSession->status == RTMFP::FAILED)
			return; // ignore

		lock_guard<recursive_mutex> lock(_mutexConnections);
		auto itConnection = _mapAddress2Connection.find(address);
		if (itConnection != _mapAddress2Connection.end())
			itConnection->second->process(pBuffer);
		else {
			DEBUG("Input packet from a new address : ", address.toString());
			_pDefaultConnection->setAddress(address);
			_pDefaultConnection->process(pBuffer);
		}
	};
	onError = [this](const Exception& ex) {
		ERROR("Socket error : ", ex.error())
	};
	onConnection = [this](const SocketAddress& address, const string& name) {
		lock_guard<recursive_mutex> lock(_mutexConnections);
		auto itConnection = _mapAddress2Connection.find(address);
		OnConnection::raise(itConnection->second, name);
	};

	_pSocket.reset(new UDPSocket(_pInvoker->sockets));
	_pSocket->OnError::subscribe(onError);
	_pSocket->OnPacket::subscribe(onPacket);

	_pDefaultConnection.reset(new DefaultConnection(this));
}

SocketHandler::~SocketHandler() {
	close();
}

void SocketHandler::close() {
	lock_guard<recursive_mutex> lock(_mutexConnections);
	for (auto itConnection = _mapAddress2Connection.begin(); itConnection != _mapAddress2Connection.end(); itConnection++)
		deleteConnection(itConnection);
	_mapAddress2Connection.clear();

	// Unsubscribing to socket : we don't want to receive packets anymore
	if (_pSocket) {
		_pSocket->OnPacket::unsubscribe(onPacket);
		_pSocket->OnError::unsubscribe(onError);
		_pSocket->close();
	}
}

const PoolBuffers& SocketHandler::poolBuffers() {
	return _pInvoker->poolBuffers;
}

const string& SocketHandler::peerId() { 
	return _pMainSession->peerId();
}

bool SocketHandler::diffieHellman(DiffieHellman * &pDh) {
	if (!_diffieHellman.initialized()) {
		Exception ex;
		if (!_diffieHellman.initialize(ex)) {
			ERROR("Unable to initialize diffie hellman object : ", ex.error())
			return false;
		}
	}
	pDh = &_diffieHellman;
	return true;
}

void SocketHandler::addP2PConnection(const string& rawId, const string& peerId, const string& tag, const SocketAddress& hostAddress) {

	lock_guard<recursive_mutex> lock(_mutexConnections);
	_mapTag2Peer.emplace(piecewise_construct, forward_as_tuple(tag), forward_as_tuple(rawId, peerId, hostAddress));
}

bool SocketHandler::onNewPeerId(const string& rawId, const string& peerId, const SocketAddress& address) {
	lock_guard<recursive_mutex> lock(_mutexConnections);
	auto itConnection = _mapAddress2Connection.find(address);
	FATAL_ASSERT(itConnection != _mapAddress2Connection.end())
	return OnNewPeerId::raise<false>(itConnection->second, rawId, peerId);
}

shared_ptr<RTMFPConnection>  SocketHandler::addConnection(const SocketAddress& address, FlowManager* session, bool responder, bool p2p) {
	lock_guard<recursive_mutex> lock(_mutexConnections);
	auto itConnection = _mapAddress2Connection.lower_bound(address);
	if (itConnection == _mapAddress2Connection.end() || itConnection->first != address) {
		itConnection = _mapAddress2Connection.emplace_hint(itConnection, piecewise_construct, forward_as_tuple(address), forward_as_tuple(new RTMFPConnection(address, this, session, responder, p2p)));
		itConnection->second->OnIdBuilt::subscribe((OnIdBuilt&)*this);
		itConnection->second->OnConnected::subscribe(onConnection);
	} else
		DEBUG("Connection already exist at address ", address.toString(), ", nothing done")
	return itConnection->second;
}

void SocketHandler::deleteConnection(const MAP_ADDRESS2CONNECTION::iterator& itConnection) {
	itConnection->second->close();
	itConnection->second->OnIdBuilt::unsubscribe((OnIdBuilt&)*this);
	itConnection->second->OnConnected::unsubscribe(onConnection);
}

void SocketHandler::manage() {
	lock_guard<recursive_mutex> lock(_mutexConnections);

	// Send the waiting p2p connection requests (ask server for addresses)
	if (!_mapTag2Peer.empty()) {
		lock_guard<recursive_mutex> lock(_mutexConnections);

		for (auto& itPeer : _mapTag2Peer) {
			if (!itPeer.second.received) {
				INFO("Sending new P2P handshake 30 to server (peerId : ", itPeer.second.peerId, ")")
				_pDefaultConnection->setAddress(itPeer.second.hostAddress);
				_pDefaultConnection->sendHandshake30(itPeer.second.rawId, itPeer.first);
				itPeer.second.received = true;
			}
		}
	}

	for (auto itConnection : _mapAddress2Connection)
		itConnection.second->manage();

	// Delete old connections
	auto itConnection2 = _mapAddress2Connection.begin();
	while (itConnection2 != _mapAddress2Connection.end()) {
		if (itConnection2->second->failed())
			_mapAddress2Connection.erase(itConnection2++);
		else
			++itConnection2;
	}

	_pDefaultConnection->manage();
}

void SocketHandler::onP2PAddresses(const string& tagReceived, const PEER_LIST_ADDRESS_TYPE& addresses, const SocketAddress& hostAddress) {
	lock_guard<recursive_mutex> lock(_mutexConnections);
	auto it = _mapTag2Peer.find(tagReceived);
	if (it == _mapTag2Peer.end()) {
		DEBUG("Handshake 71 received but no p2p connection found with tag (possible old request)")
		return;
	}

	// Update addresses and send handshake 30 if no handshake 70 received
	if (OnP2PAddresses::raise<false>(it->second.peerId, addresses) && hostAddress) {
		// Send handshake 30 to far server
		_pDefaultConnection->setAddress(hostAddress);
		_pDefaultConnection->sendHandshake30(it->second.rawId, it->first);
	}
}

void SocketHandler::onPeerHandshake30(const string& id, const string& tag, const SocketAddress& address) {

	if (id != peerId()) {
		ERROR("Unexpected peer ID in handshake 30 : ", id, ", connection rejected")
		return;
	}

	auto itPeer = _mapTag2Peer.find(tag);
	if (itPeer == _mapTag2Peer.end())
		OnPeerHandshake30::raise(tag, address);
	// TODO: else
}

void SocketHandler::onPeerHandshake70(const string& tagReceived, const string& farkey, const string& cookie, const SocketAddress& address, bool createConnection) {

	auto itPeer = _mapTag2Peer.find(tagReceived);
	if (itPeer != _mapTag2Peer.end()) {

		// If it is an unknown address, we create the connection
		if (createConnection)
			OnPeerHandshake70::raise(itPeer->second.peerId, address, farkey, cookie);
		_mapTag2Peer.erase(tagReceived);
	} else
		TRACE("Unknown tag received with handshake 70 from address ", address.toString(), " possible direct connection")
}