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

#include "P2PSession.h"
#include "Invoker.h"
#include "RTMFPFlow.h"
#include "NetGroup.h"
#include "Base/Logs.h"
#include "Listener.h"
#include "RTMFPSession.h"
#include "RTMFPWriter.h"

using namespace Base;
using namespace std;

// P2P Session first counter, this number is considered sufficient to never been reached by RTMFPSession ID
UInt32 P2PSession::P2PSessionCounter = 0x03000000; // Notice that Flash uses incremental values from 3 and do a left align


P2PSession::P2PSession(RTMFPSession* parent, string id, Invoker& invoker, OnStatusEvent pOnStatusEvent, 
		const Base::SocketAddress& host, bool responder, bool group, UInt16 mediaId) : peerId(id), hostAddress(host), _parent(parent), _groupBeginSent(false), _peerMediaId(mediaId),
		groupReportInitiator(false), _groupConnectSent(false), _isGroup(group), groupFirstReportSent(false), FlowManager(responder, invoker, pOnStatusEvent) {
	_pMainStream->onMedia = [this](UInt16 mediaId, UInt32 time, const Packet& packet, double lostRate, AMF::Type type) {
		return _parent->onMediaPlay(_peerMediaId, time, packet, lostRate, type);
	};
	_pMainStream->onGroupHandshake = [this](const string& groupId, const string& key, const string& peerId) {
		return handleGroupHandshake(groupId, key, peerId);
	};
	_pMainStream->onGroupMedia = [this](BinaryReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {

		if (packet.available() < 0x24) {
			UInt64 lastFragment = packet.read7BitLongValue();
			DEBUG("GroupMedia Closure message received from ", peerId)
			auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
			if (itPeerMedia != _mapFlow2PeerMedia.end())
				onClosedMedia(*itPeerMedia->second->pStreamKey, lastFragment);
			return true;
		}

		// Read the name
		string streamName;
		UInt8 sizeName = packet.read8();
		if (sizeName <= 1) {
			WARN("GroupMedia Subscription message without name received from ", peerId)
			return false;
		}
		packet.next(); // 00
		packet.read(sizeName - 1, streamName);
		DEBUG("GroupMedia Subscription message received from ", peerId, " ; streamName=", streamName)

		string streamKey;
		packet.read(0x22, streamKey);

		// Create the PeerMedia and writer if it does not exists
		auto itStream = _mapStream2PeerMedia.lower_bound(streamKey);
		if (itStream == _mapStream2PeerMedia.end() || itStream->first != streamKey) {

			// Save the streamKey
			shared_ptr<RTMFPWriter> pWriter = createWriter(Packet(EXPAND("\x00\x47\x52\x11")), _mainFlowId);
			auto itPeerMedia = _mapWriter2PeerMedia.emplace(piecewise_construct, forward_as_tuple(pWriter->id), forward_as_tuple(new PeerMedia(this, pWriter))).first;
			itStream = _mapStream2PeerMedia.emplace_hint(itStream, streamKey, itPeerMedia->second);
			itPeerMedia->second->pStreamKey = &itStream->first;
		} 
		// else the stream already exists, it is a subscription
		else if (itStream->second->idFlow) {
			DEBUG("Peer ", peerId, " already subscribed to this stream, media subscription refused")
			return false;
		}
		_mapFlow2PeerMedia.emplace(flowId, itStream->second);

		// Save the flow ID
		itStream->second->idFlow = flowId;

		// If we accept the request and are responder => send the Media Subscription message
		if (!onNewMedia(peerId, itStream->second, streamName, streamKey, packet))
			itStream->second->close(false); // else we close the writer & flow
		return true;
	};
	_pMainStream->onGroupReport = [this](BinaryReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("NetGroup Report message received from ", peerId, " ; size=", packet.size())
		onPeerGroupReport(this, packet, _mapStream2PeerMedia.empty()); // no PeerMedia => no Group Media received for now, we can send the group media subscription message
	};
	_pMainStream->onGroupBegin = [this](UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("NetGroup Begin message received from ", peerId)
		onPeerGroupBegin(this);
	};
	_pMainStream->onGroupPlayPush = [this](BinaryReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("Group Push Out mode received from peer ", peerId, " : ", String::Format<UInt8>("%.2x", *packet.current()))

		auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end())
			itPeerMedia->second->setPushMode(packet.read8());
	};
	_pMainStream->onGroupPlayPull = [this](BinaryReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId, bool flush) {
		UInt64 fragment = packet.read7BitLongValue();
		TRACE("Group Pull message received from peer ", peerId, " - fragment : ", fragment)

		auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end())
			itPeerMedia->second->handlePlayPull(fragment, flush);
	};
	_pMainStream->onFragmentsMap = [this](BinaryReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		UInt64 counter = packet.read7BitLongValue();
		DEBUG("Group Fragments map (type 22) received from ", peerId, " : ", counter)

		auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end())
			itPeerMedia->second->handleFragmentsMap(counter, packet.current(), packet.available());

		packet.next(packet.available());
	};
	_pMainStream->onFragment = [this](UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, const Packet& packet, double lostRate, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		auto itPeerMedia = _mapWriter2PeerMedia.find(writerId);
		if (itPeerMedia != _mapWriter2PeerMedia.end()) {
			// save the media flow id if new
			if (!itPeerMedia->second->idFlowMedia)
				itPeerMedia->second->idFlowMedia = flowId;
			itPeerMedia->second->handleFragment(marker, id, splitedNumber, mediaType, time, packet, lostRate);
		}
	};
	_pMainStream->onGroupAskClose = [this](UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		bool accepted = onPeerGroupAskClose(peerId);
		DEBUG("NetGroup close message received from peer ", peerId, " : ", accepted ? "accepted" : "refused")
		return !accepted; // return False if accepted to close the session
	};

	_sessionId = P2PSessionCounter++;
	rawId.append("\x21\x0f", 2);
	String::ToHex(peerId, rawId);
}

P2PSession::~P2PSession() {
	DEBUG("Deletion of P2PSession ", peerId)
	close(true, RTMFP::SESSION_CLOSED);

	_pMainStream->onMedia = nullptr;
	_pMainStream->onGroupMedia = nullptr;
	_pMainStream->onGroupReport = nullptr;
	_pMainStream->onGroupPlayPush = nullptr;
	_pMainStream->onGroupPlayPull = nullptr;
	_pMainStream->onFragmentsMap = nullptr;
	_pMainStream->onGroupBegin = nullptr;
	_pMainStream->onFragment = nullptr;
	_pMainStream->onGroupHandshake = nullptr;
	_pMainStream->onGroupAskClose = nullptr;
	_parent = NULL;
}

void P2PSession::close(bool abrupt, RTMFP::CLOSE_REASON reason) {
	if ((abrupt && (status == RTMFP::FAILED)) || (!abrupt && (status == RTMFP::NEAR_CLOSED)))
		return;

	DEBUG("Closing P2PSession ", peerId, " (abrupt : ", abrupt, ", status : ", status, ")")

	// NetGroup 
	if (abrupt) {
		// Full close : we also close the NetGroup Report writer
		_groupConnectSent = _groupBeginSent = groupFirstReportSent = false;
		_pReportWriter.reset();
	}

	for (auto& itPeerMedia : _mapWriter2PeerMedia)
		itPeerMedia.second->close(abrupt);
	_mapStream2PeerMedia.clear();
	_mapWriter2PeerMedia.clear();
	_mapFlow2PeerMedia.clear();
	onPeerClose(peerId);

	if (_pListener) {
		_parent->stopListening(peerId);
		_pListener = NULL;
	}

	FlowManager::close(abrupt, reason);
}

RTMFPFlow* P2PSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature, UInt64 idWriterRef) {

	if (signature.size()>6 && signature.compare(0, 6, "\x00\x54\x43\x04\xFA\x89", 6) == 0) { // Direct P2P NetStream
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 6, signature.length() - 6).read7BitValue());
		DEBUG("Creating new Flow (2) for P2PSession ", name())
		_pMainStream->addStream(idSession, pStream);
		return new RTMFPFlow(id, pStream, *this, idWriterRef);
	}
	else if (signature.size() > 3 && ((signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)  // NetGroup Report stream (main flow)
		|| (signature.compare(0, 4, "\x00\x47\x52\x19", 4) == 0)  // NetGroup Data stream
		|| (signature.compare(0, 4, "\x00\x47\x52\x1D", 4) == 0)  // NetGroup Message stream
		|| (signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)  // NetGroup Media Report stream (fragments Map & Media Subscription)
		|| (signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0))) {  // NetGroup Media stream
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true);

		DEBUG("Creating new flow (", id, ") for P2PSession ", peerId)
		if (signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)
			_mainFlowId = id;
		return new RTMFPFlow(id, pStream, *this,  idWriterRef);
	}
	ex.set<Ex::Protocol>("Unhandled signature type : ", String::Hex((const UInt8*)signature.data(), signature.size()), " , cannot create RTMFPFlow");
	return NULL;
}

unsigned int P2PSession::callFunction(const string& function, queue<string>& arguments) {

	if (!_pNetStreamWriter)
		_pNetStreamWriter = createWriter(Packet(EXPAND("\x00\x54\x43\x04\xFA\x89\x01")), 0);  // stream id = 1

	AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation(function.c_str(), true);
	while (!arguments.empty()) {
		string& arg = arguments.front();
		amfWriter.writeString(arg.data(), arg.size());
		arguments.pop();
	}
	_pNetStreamWriter->flush();
	return 0;
}

// Only in responder mode
bool P2PSession::handlePlay(const string& streamName, UInt16 streamId, UInt64 flowId, double cbHandler) {
	DEBUG("The peer ", peerId, " is trying to play '", streamName, "'...")

	// Create the writers, signature is same as flow/stream and flowId must be set to flow id
	shared_ptr<Buffer> pSignature(new Buffer(6, "\x00\x54\x43\x04\xFA\x89"));
	BinaryWriter(*pSignature).write7BitValue(streamId);
	Packet signature(pSignature);
	shared_ptr<RTMFPWriter> pDataWriter = createWriter(signature, flowId);
	shared_ptr<RTMFPWriter> pAudioWriter = createWriter(signature, flowId);
	shared_ptr<RTMFPWriter> pVideoWriter = createWriter(signature, flowId);

	Exception ex;
	if(!(_pListener = _parent->startListening<FlashListener, shared_ptr<RTMFPWriter>&>(ex, streamName, peerId, pDataWriter, pAudioWriter, pVideoWriter))) {
		// TODO : See if we can send a specific answer
		WARN(ex)
		return false;
	}
	INFO("Stream ", streamName, " found, sending start answer")

	// Write the stream play request to other end
	pDataWriter->setCallbackHandle(cbHandler);
	pDataWriter->writeRaw().write16(0).write32(2000000 + streamId); // stream begin
	pDataWriter->writeAMFStatus("NetStream.Play.Reset", "Playing and resetting " + streamName); // for entire playlist
	pDataWriter->writeAMFStatus("NetStream.Play.Start", "Started playing " + streamName); // for item
	AMFWriter& amf(pDataWriter->writeAMFData("|RtmpSampleAccess"));
	// TODO: determinate if video and audio are available
	amf.writeBoolean(true); // audioSampleAccess
	amf.writeBoolean(true); // videoSampleAccess
	pDataWriter->flush();
	pDataWriter->setCallbackHandle(0); // reset callback handler

	// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
	_parent->setP2pPublisherReady();
	return true;
}

bool P2PSession::handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id) {
	if (!_isGroup)
		return false;

	// TODO: Is it a reconnection? => add the peer to group
	/*if (_isGroupDisconnected && !_parent->addPeer2Group(peerId))
		return;*/

	if (String::ICompare(groupId, _parent->groupIdHex()) != 0)
		WARN("Unexpected group ID received from ", peerId, ", group connect request ignored")
	else if (memcmp(id.data(), _parent->rawId().data(), PEER_ID_SIZE+2) != 0)
		WARN("Unexpected peer ID received from ", peerId, ", group connect request ignored")
	else if (!_groupExpectedKey || memcmp(key.data(), _groupExpectedKey->data(), Crypto::SHA256_SIZE) != 0)
		WARN("Unexpected group key received from ", peerId, ", group connect request ignored")
	// Send the group connection request to peer if not already sent
	else {
		if (!_groupConnectSent)
			sendGroupPeerConnect();
		return true;
	}
	return false;
}

void P2PSession::handleWriterException(shared_ptr<RTMFPWriter>& pWriter) {

	if (pWriter == _pReportWriter) {
		DEBUG(peerId, " want to close the report writer ", pWriter->id, " we close the session")
		_pReportWriter.reset();
		close(false, RTMFP::OTHER_EXCEPTION);
	}
	else if (pWriter == _pNetStreamWriter)
		_pNetStreamWriter.reset();
	else if (pWriter->signature.size() > 3 && memcmp(pWriter->signature.data(), "\x00\x47\x52\x11", 4) == 0) {
		auto itWriter = _mapWriter2PeerMedia.find(pWriter->id);
		if (itWriter != _mapWriter2PeerMedia.end()) {
			DEBUG(peerId, " want to close the subscription media report writer ", pWriter->id)

			// Close the PeerMedia before deletion
			itWriter->second->close(false);

			FATAL_CHECK(itWriter->second->pStreamKey) // implementation error
			auto itStream = _mapStream2PeerMedia.find(*itWriter->second->pStreamKey);
			FATAL_CHECK(itStream != _mapStream2PeerMedia.end())
			if (itStream->second->idFlow)
				_mapFlow2PeerMedia.erase(itStream->second->idFlow);
			_mapStream2PeerMedia.erase(itStream);
			_mapWriter2PeerMedia.erase(itWriter);
		}
	}
	else if (pWriter->signature.size() > 3 && memcmp(pWriter->signature.data(), "\x00\x47\x52\x12", 4) == 0) {
		auto itPeerMedia = _mapFlow2PeerMedia.find(pWriter->flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end()) {
			DEBUG(peerId, " want to close the media writer ", pWriter->id)
			itPeerMedia->second->closeMediaWriter(false);
		}
	} 
	else
		DEBUG(peerId, " want to close the unknown writer ", pWriter->id, " (possible already closed writer)")
	
	pWriter->close(false);
}

bool P2PSession::sendGroupBegin() {
	if (_groupBeginSent)
		return false;
	
	if (!_pReportWriter) {
		WARN("Unable to find the Report writer of peer ", peerId)
		return false;
	}

	DEBUG("Sending Group Begin message")
	_pReportWriter->writeGroupBegin();
	_pReportWriter->flush();
	_groupBeginSent = true;
	return true;
}

shared_ptr<PeerMedia>& P2PSession::getPeerMedia(const string& streamKey) {

	// Create a new writer if the stream key is unknown
	auto itStream = _mapStream2PeerMedia.lower_bound(streamKey);
	if (itStream == _mapStream2PeerMedia.end() || itStream->first != streamKey) {
		shared_ptr<RTMFPWriter> pWriter = createWriter(Packet(EXPAND("\x00\x47\x52\x11")), _mainFlowId);
		auto itPeerMedia = _mapWriter2PeerMedia.emplace(piecewise_construct, forward_as_tuple(pWriter->id), forward_as_tuple(new PeerMedia(this, pWriter))).first;
		itStream = _mapStream2PeerMedia.emplace_hint(itStream, streamKey, itPeerMedia->second);
		itPeerMedia->second->pStreamKey = &itStream->first;
		return itPeerMedia->second;
	}
	else 
		return itStream->second;
}

void P2PSession::sendGroupReport(const UInt8* data, UInt32 size) {

	if (!_pReportWriter) {
		WARN("Unable to find the Group Report writer of peer ", peerId)
		return;
	}
	_pReportWriter->writeRaw(data, size);
	if (!groupFirstReportSent)
		groupFirstReportSent = true;

	// Send group begin if not sent, otherwise flush
	if (!sendGroupBegin())
		_pReportWriter->flush();
}

void P2PSession::sendGroupPeerConnect() {
	if (!_pReportWriter)
		_pReportWriter = createWriter(Packet(EXPAND("\x00\x47\x52\x1C")), 0);

	DEBUG("Sending group connection request to peer ", peerId)
	_pReportWriter->writePeerGroup(_parent->groupIdHex(), _groupConnectKey->data(), rawId);
	_pReportWriter->flush();
	_groupConnectSent = true;
	sendGroupBegin();
}

bool P2PSession::createMediaWriter(shared_ptr<RTMFPWriter>& pWriter, UInt64 flowIdRef) {

	pWriter = createWriter(Packet(EXPAND("\x00\x47\x52\x12")), flowIdRef);
	return true;
}

void P2PSession::onConnection() {
	INFO("P2PSession is now connected to ", name(), (_responder)? " (responder)" : " (initiator)")
	removeHandshake(_pHandshake);
	status = RTMFP::CONNECTED;

	if (_isGroup) {
		if (_parent->addPeer2Group(peerId)) {
			buildGroupKey();
			if (!_responder) // TODO: not sure, I think initiator must do the peer connect first
				sendGroupPeerConnect();
		} else
			close(false, RTMFP::OTHER_EXCEPTION);
	// Start playing
	} else if (!_parent->isPublisher()) {
		INFO("Sending play request to peer for stream '", _streamName, "'")
		_pNetStreamWriter = createWriter(Packet(EXPAND("\x00\x54\x43\x04\xFA\x89\x01")), _mainFlowId); // stream id = 1
		AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation("play", true);
		amfWriter.amf0 = true; // Important for p2p unicast play
		amfWriter.writeString(_streamName.c_str(), _streamName.size());
		_pNetStreamWriter->flush();
		_parent->setP2PPlayReady();
	}
}

bool P2PSession::askPeer2Disconnect() {
	if (_pReportWriter && _lastTryDisconnect.isElapsed(NETGROUP_DISCONNECT_DELAY)) {
		DEBUG("Best Peer - Asking ", peerId, " to close")
		_pReportWriter->writeRaw(BIN "\x0C", 1);
		_pReportWriter->flush(); // not sure
		_lastTryDisconnect.update();
		return true;
	}
	return false;
}

bool P2PSession::onHandshake38(const SocketAddress& address, shared_ptr<Handshake>& pHandshake) {
	// This is an existing peer, is it already connected?
	if (status > RTMFP::HANDSHAKE78) {
		DEBUG("Handshake 38 from ", address, " ignored, session is already in state ", status)
		return false;
	}
	// is it a concurrent connection ?
	else if (!_responder) {
		if (status < RTMFP::HANDSHAKE38)
			DEBUG("Concurrent handshake from ", address, ", initiator has not received any answer, continuing")
		else if (_parent->peerId() > peerId) {
			DEBUG("Concurrent handshake from ", address, ", our ID is bigger than peer, ignoring the handshake 38")
			return false;
		} else 
			DEBUG("Concurrent handshake from ", address, ", our ID is smaller than peer, continuing") // TODO: check how Flash manage concurrent connection
		// First remove the other handshake
		removeHandshake(_pHandshake);

		_responder = true;
		_nonce.reset(); // reset the nonce to avoid handshake error
		_parent->handleConcurrentSwitch();
	}
	else
		DEBUG("Handshake 38 received from ", address, " sending handshake 78...")

	// Reset parameters (concurrent connection or old handshake)
	if (_pHandshake && _pHandshake->pSession)
		_pHandshake->pSession = NULL;
	_pHandshake = pHandshake;
	_pHandshake->pSession = this;
	_address.set(address);
	return true;
}

const shared_ptr<Socket>& P2PSession::socket(IPAddress::Family family) { 
	return _parent->socket(family); 
}

void P2PSession::removeHandshake(shared_ptr<Handshake>& pHandshake) {
	_parent->removeHandshake(pHandshake);
}

DiffieHellman&	P2PSession::diffieHellman() {
	return _parent->diffieHellman();
}

void P2PSession::addAddress(const SocketAddress& address, RTMFP::AddressType type) {
	if ((type & 0x0f) == RTMFP::ADDRESS_REDIRECTION)
		hostAddress = address;
	else if (_knownAddresses.size() < RTMFP_MAX_ADDRESSES) {
		_knownAddresses.emplace(address, type);

		if (_pHandshake) {
			auto itAddress = _pHandshake->addresses.lower_bound(address);
			if (itAddress != _pHandshake->addresses.end() && itAddress->first == address)
				return; // already known

			// Save the address
			_pHandshake->addresses.emplace_hint(itAddress, piecewise_construct, forward_as_tuple(address), forward_as_tuple(type));
		}
	}
}

void P2PSession::buildGroupKey() {

	// Compile encrypted keys
	if (!_groupConnectKey) {
		_groupConnectKey.reset(new Buffer(Crypto::SHA256_SIZE));
		_groupExpectedKey.reset(new Buffer(Crypto::SHA256_SIZE));
		UInt8 mdp1[Crypto::SHA256_SIZE];
		Crypto::HMAC::SHA256(_sharedSecret.data(), _sharedSecret.size(), _farNonce->data(), _farNonce->size(), mdp1);
		Crypto::HMAC::SHA256(_parent->groupIdTxt().data(), _parent->groupIdTxt().size(), mdp1, Crypto::SHA256_SIZE, _groupConnectKey->data());
		Crypto::HMAC::SHA256(_sharedSecret.data(), _sharedSecret.size(), _nonce->data(), _nonce->size(), mdp1);
		Crypto::HMAC::SHA256(_parent->groupIdTxt().data(), _parent->groupIdTxt().size(), mdp1, Crypto::SHA256_SIZE, _groupExpectedKey->data());
	}
}
