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
#include "Mona/Logs.h"
#include "Listener.h"
#include "RTMFPSession.h"
#include "RTMFPWriter.h"

using namespace Mona;
using namespace std;

// P2P Session first counter, this number is considered sufficient to never been reached by RTMFPSession ID
UInt32 P2PSession::P2PSessionCounter = 0x03000000; // Notice that Flash uses incremental values from 3 and do a left align


P2PSession::P2PSession(RTMFPSession* parent, string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, 
		const Mona::SocketAddress& host, bool responder, bool group) : peerId(id), rawId("\x21\x0f"), hostAddress(host), _parent(parent), _groupBeginSent(false), 
		groupReportInitiator(false), _groupConnectSent(false), _isGroup(group), groupFirstReportSent(false), FlowManager(responder, invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onGroupHandshake = [this](const string& groupId, const string& key, const string& peerId) {
		return handleGroupHandshake(groupId, key, peerId);
	};
	onGroupMedia = [this](PacketReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("Group Media Subscription message received from ", peerId)

		if (packet.available() < 0x24) {
			UInt64 lastFragment = packet.read7BitLongValue();
			DEBUG("Group Media is closing, ignoring the request (last fragment : ", lastFragment,")")
			return true;
		}

		string streamName;
		const UInt8* posStart = packet.current() - 1; // Record the whole packet for sending back

		// Read the name
		UInt8 sizeName = packet.read8();
		if (sizeName <= 1) {
			WARN("New stream available without name")
			return false;
		}
		packet.next(); // 00
		packet.read(sizeName - 1, streamName);

		string streamKey;
		packet.read(0x22, streamKey);

		// Create the PeerMedia and writer if it does not exists
		auto itStream = _mapStream2PeerMedia.lower_bound(streamKey);
		if (itStream == _mapStream2PeerMedia.end() || itStream->first != streamKey) {

			// Save the streamKey
			string signature("\x00\x47\x52\x11", 4);
			shared_ptr<RTMFPWriter> pWriter = createWriter(signature, _mainFlowId);
			auto itPeerMedia = _mapWriter2PeerMedia.emplace(piecewise_construct, forward_as_tuple(pWriter->id), forward_as_tuple(new PeerMedia(this, pWriter))).first;
			itStream = _mapStream2PeerMedia.emplace_hint(itStream, streamKey, itPeerMedia->second);
			itPeerMedia->second->pStreamKey = &itStream->first;
		} 
		// else the stream already exists, it is a subscription
		else if (itStream->second->idFlow) {
			WARN("Already subscribed to this stream, media subscription refused")
			return false;
		}
		_mapFlow2PeerMedia.emplace(flowId, itStream->second);

		// Save the flow ID
		itStream->second->idFlow = flowId;

		// If we accept the request and are responder => send the Media Subscription message
		if (!OnNewMedia::raise<false>(peerId, itStream->second, streamName, streamKey, packet))
			itStream->second->close(false); // else we close the writer & flow
		return true;
	};
	onGroupReport = [this](PacketReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("NetGroup Report message received from ", peerId)
		OnPeerGroupReport::raise(this, packet, _mapStream2PeerMedia.empty()); // no PeerMedia => no Group Media received for now, we can send the group media subscription message
	};
	onGroupBegin = [this](UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("NetGroup Begin message received from ", peerId)
		OnPeerGroupBegin::raise(this);
	};
	onGroupPlayPush = [this](PacketReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		DEBUG("Group Push Out mode received from peer ", peerId, " : ", Format<UInt8>("%.2x", *packet.current()))

		auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end())
			itPeerMedia->second->setPushMode(packet.read8());
	};
	onGroupPlayPull = [this](PacketReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		UInt64 fragment = packet.read7BitLongValue();
		TRACE("Group Pull message received from peer ", peerId, " - fragment : ", fragment)

		auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end())
			itPeerMedia->second->onPlayPull(fragment);
	};
	onFragmentsMap = [this](PacketReader& packet, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		UInt64 counter = packet.read7BitLongValue();
		DEBUG("Group Fragments map (type 22) received from ", peerId, " : ", counter)

		auto itPeerMedia = _mapFlow2PeerMedia.find(flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end())
			itPeerMedia->second->onFragmentsMap(counter, packet.current(), packet.available());

		packet.next(packet.available());
	};
	onFragment = [this](UInt8 marker, UInt64 id, UInt8 splitedNumber, UInt8 mediaType, UInt32 time, PacketReader& packet, double lostRate, UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		auto itPeerMedia = _mapWriter2PeerMedia.find(writerId);
		if (itPeerMedia != _mapWriter2PeerMedia.end()) {
			// save the media flow id if new
			if (!itPeerMedia->second->idFlowMedia)
				itPeerMedia->second->idFlowMedia = flowId;
			itPeerMedia->second->onFragment(marker, id, splitedNumber, mediaType, time, packet, lostRate);
		}
	};
	onGroupAskClose = [this](UInt16 streamId, UInt64 flowId, UInt64 writerId) {
		bool res = OnPeerGroupAskClose::raise<false>(peerId);
		DEBUG("NetGroup close message received from peer ", peerId, " : ", res ? "refused" : "accepted")
		return res;
	};

	_sessionId = P2PSessionCounter++;
	Util::UnformatHex(BIN peerId.data(), peerId.size(), rawId, true);

	_pMainStream->OnGroupMedia::subscribe(onGroupMedia);
	_pMainStream->OnGroupReport::subscribe(onGroupReport);
	_pMainStream->OnGroupPlayPush::subscribe(onGroupPlayPush);
	_pMainStream->OnGroupPlayPull::subscribe(onGroupPlayPull);
	_pMainStream->OnFragmentsMap::subscribe(onFragmentsMap);
	_pMainStream->OnGroupBegin::subscribe(onGroupBegin);
	_pMainStream->OnFragment::subscribe(onFragment);
	_pMainStream->OnGroupHandshake::subscribe(onGroupHandshake);
	_pMainStream->OnGroupAskClose::subscribe(onGroupAskClose);
}

P2PSession::~P2PSession() {
	DEBUG("Deletion of P2PSession ", peerId)
	close(true);

	_pMainStream->OnGroupMedia::unsubscribe(onGroupMedia);
	_pMainStream->OnGroupReport::unsubscribe(onGroupReport);
	_pMainStream->OnGroupPlayPush::unsubscribe(onGroupPlayPush);
	_pMainStream->OnGroupPlayPull::unsubscribe(onGroupPlayPull);
	_pMainStream->OnFragmentsMap::unsubscribe(onFragmentsMap);
	_pMainStream->OnGroupBegin::unsubscribe(onGroupBegin);
	_pMainStream->OnFragment::unsubscribe(onFragment);
	_pMainStream->OnGroupHandshake::unsubscribe(onGroupHandshake);
	_pMainStream->OnGroupAskClose::unsubscribe(onGroupAskClose);
	_parent = NULL;
}

void P2PSession::close(bool abrupt) {
	if (status == RTMFP::FAILED)
		return;

	_pLastWriter.reset();

	closeGroup(true);

	if (_pListener) {
		_parent->stopListening(peerId);
		_pListener = NULL;
	}

	FlowManager::close(abrupt);
}


void P2PSession::closeGroup(bool abrupt) {

	// Full close : we also close the NetGroup Report writer
	if (abrupt) {
		_groupConnectSent = _groupBeginSent = groupFirstReportSent = false;
		/*if (_pReportWriter)
		_pReportWriter->close();*/ // Do not send the close message, we are closing session
		_pReportWriter.reset();
	}

	for (auto itPeerMedia : _mapWriter2PeerMedia)
		itPeerMedia.second->close(abrupt);
	_mapStream2PeerMedia.clear();
	_mapWriter2PeerMedia.clear();
	_mapFlow2PeerMedia.clear();
	OnPeerClose::raise(peerId);
}

RTMFPFlow* P2PSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature, UInt64 idWriterRef) {

	if (signature.size()>6 && signature.compare(0, 6, "\x00\x54\x43\x04\xFA\x89", 6) == 0) { // Direct P2P NetStream
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 6, signature.length() - 6).read7BitValue());
		DEBUG("Creating new Flow (2) for P2PSession ", name())
		_pMainStream->addStream(idSession, pStream);
		return new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *this, idWriterRef);
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
		return new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *this, idWriterRef);
	}
	string tmp;
	ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	return NULL;
}


void P2PSession::handleWriterClosed(shared_ptr<RTMFPWriter>& pWriter) {
	// We reset the pointers before closure
	if (pWriter == _pReportWriter)
		_pReportWriter.reset();
	else if (pWriter == _pNetStreamWriter)
		_pNetStreamWriter.reset();
	else if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0) {
		auto itWriter = _mapWriter2PeerMedia.find(pWriter->id);
		if (itWriter != _mapWriter2PeerMedia.end()) {
			FATAL_ASSERT(itWriter->second->pStreamKey) // implementation error
				auto itStream = _mapStream2PeerMedia.find(*itWriter->second->pStreamKey);
			if (itStream != _mapStream2PeerMedia.end()) {
				if (itStream->second->idFlow)
					_mapFlow2PeerMedia.erase(itStream->second->idFlow);
				_mapStream2PeerMedia.erase(itStream);
			}
			_mapWriter2PeerMedia.erase(itWriter);
		}
	}
}

unsigned int P2PSession::callFunction(const char* function, int nbArgs, const char** args) {

	if (!_pNetStreamWriter) {
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		_pNetStreamWriter = createWriter(signature, 0);
	}

	AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation(function, true);
	for (int i = 0; i < nbArgs; i++) {
		if (args[i])
			amfWriter.writeString(args[i], strlen(args[i]));
	}
	_pNetStreamWriter->flush();
	return 0;
}

void P2PSession::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {

	_streamName = streamName;
}

// Only in responder mode
bool P2PSession::handlePlay(const string& streamName, UInt16 streamId, UInt64 flowId, double cbHandler) {
	DEBUG("The peer ", peerId, " is trying to play '", streamName, "'...")

	// Create the writers, signature is same as flow/stream and flowId must be set to flow id
	string signature("\x00\x54\x43\x04\xFA\x89", 6);
	RTMFP::Write7BitValue(signature, streamId);
	shared_ptr<RTMFPWriter> pDataWriter = createWriter(signature, flowId);
	shared_ptr<RTMFPWriter> pAudioWriter = createWriter(signature, flowId);
	shared_ptr<RTMFPWriter> pVideoWriter = createWriter(signature, flowId);

	Exception ex;
	if(!(_pListener = _parent->startListening<FlashListener, shared_ptr<RTMFPWriter>&>(ex, streamName, peerId, pDataWriter, pAudioWriter, pVideoWriter))) {
		// TODO : See if we can send a specific answer
		WARN(ex.error())
		return false;
	}
	INFO("Stream ", streamName, " found, sending start answer")

	// Write the stream play request to other end
	pDataWriter->setCallbackHandle(cbHandler);
	((shared_ptr<FlashWriter>)pDataWriter)->writeRaw().write16(0).write32(2000000 + streamId); // stream begin
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
		WARN("Unexpected group ID received : ", groupId, "\nExpected : ", _parent->groupIdHex())
	else if (memcmp(id.data(), _parent->rawId().data(), PEER_ID_SIZE+2) != 0)
		WARN("Our peer ID was expected but received : ", Util::FormatHex(BIN id.data()+2, PEER_ID_SIZE, LOG_BUFFER))
	else if (!_groupExpectedKey || memcmp(key.data(), _groupExpectedKey->data(), Crypto::HMAC::SIZE) != 0)
		WARN("Unexpected group key received, group connect request ignored")
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
		close();
		return;
	}
	else if (pWriter == _pNetStreamWriter)
		_pNetStreamWriter.reset();
	else if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0) {
		auto itWriter = _mapWriter2PeerMedia.find(pWriter->id);
		if (itWriter != _mapWriter2PeerMedia.end()) {
			DEBUG(peerId, " want to close the subscription media report writer ", pWriter->id)
			itWriter->second->close(false);
			return;
		}
	}
	else if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0) {
		auto itPeerMedia = _mapFlow2PeerMedia.find(pWriter->flowId);
		if (itPeerMedia != _mapFlow2PeerMedia.end()) {
			DEBUG(peerId, " want to close the media writer ", pWriter->id)

			// Inform the PeerMedia
			itPeerMedia->second->closeMediaWriter(false);
			return;
		}
	}

	Exception ex;
	pWriter->fail(ex, "Writer terminated on connection ", peerId);
	if (ex)
		WARN(ex.error())
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
		string signature("\x00\x47\x52\x11", 4);
		shared_ptr<RTMFPWriter> pWriter = createWriter(signature, _mainFlowId);
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
	if (!_pReportWriter) {
		string signature("\x00\x47\x52\x1C", 4);
		_pReportWriter = createWriter(signature, _mainFlowId);
	}

	DEBUG("Sending group connection request to peer ", peerId)
	_pReportWriter->writePeerGroup(_parent->groupIdHex(), _groupConnectKey->data(), rawId.c_str());
	_pReportWriter->flush();
	_groupConnectSent = true;
	sendGroupBegin();
}

bool P2PSession::createMediaWriter(shared_ptr<RTMFPWriter>& pWriter, UInt64 flowIdRef) {

	string signature("\x00\x47\x52\x12", 4);
	pWriter = createWriter(signature, flowIdRef);
	return true;
}

void P2PSession::closeFlow(UInt64 id) {
	auto itFlow = _flows.find(id);
	if (itFlow != _flows.end())
		itFlow->second->close();
}

void P2PSession::onConnection() {
	INFO("P2P Connection is now connected to ", name(), (_responder)? " (responder)" : " (initiator)")

	if (_isGroup) {
		if (_parent->addPeer2Group(peerId)) {
			buildGroupKey();
			if (!_responder) // TODO: not sure, I think initiator must do the peer connect first
				sendGroupPeerConnect();
		} else
			close();
	// Start playing
	} else if (!_parent->isPublisher()) {
		INFO("Sending play request to peer for stream '", _streamName, "'")
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		_pNetStreamWriter = createWriter(signature, _mainFlowId);
		AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation("play", true);
		amfWriter.amf0 = true; // Important for p2p unicast play
		amfWriter.writeString(_streamName.c_str(), _streamName.size());
		_pNetStreamWriter->flush();
		_parent->setP2PPlayReady();
	}
}

void P2PSession::handleDataAvailable(bool isAvailable) { 

	_parent->setDataAvailable(isAvailable); // only for P2P direct play
}

void P2PSession::askPeer2Disconnect() {
	if (_pReportWriter && _lastTryDisconnect.isElapsed(NETGROUP_DISCONNECT_DELAY)) {
		DEBUG("Best Peer - Asking ", peerId, " to close")
		_pReportWriter->writeRaw(BIN "\x0C", 1);
		_pReportWriter->flush(); // not sure
		_lastTryDisconnect.update();
	}
}

bool P2PSession::onHandshake38(const SocketAddress& address, shared_ptr<Handshake>& pHandshake) {
	// This is an existing peer, is it already connected?
	if (status > RTMFP::HANDSHAKE38) {
		DEBUG("Handshake 38 ignored, session is already in state ", status)
		return false;
	}
	// is it a concurrent connection ?
	else if (!_responder) {
		if (_parent->peerId() > peerId) {
			DEBUG("Concurrent handshake, our ID is bigger than peer, ignoring the handshake 38")
			return false;
		}
		DEBUG("Concurrent handshake, our ID is smaller than peer, continuing") // TODO: check how Flash manage concurrent connection
		// First remove the other handshake
		removeHandshake(_pHandshake);

		// Then reset parameters
		_pHandshake = pHandshake;
		_pHandshake->pSession = this;
		_responder = false;
		_address.set(address);
	}
	return true;
}

UDPSocket& P2PSession::socket(Mona::IPAddress::Family family) { 
	return _parent->socket(family); 
}

void P2PSession::removeHandshake(std::shared_ptr<Handshake>& pHandshake) {
	_parent->removeHandshake(pHandshake);
}

bool P2PSession::diffieHellman(DiffieHellman* &pDh) {
	return _parent->diffieHellman(pDh);
}

void P2PSession::buildGroupKey() {

	// Compile encrypted keys
	if (!_groupConnectKey) {
		_groupConnectKey.reset(new Buffer(Crypto::HMAC::SIZE));
		_groupExpectedKey.reset(new Buffer(Crypto::HMAC::SIZE));
		UInt8 mdp1[Crypto::HMAC::SIZE];
		Crypto::HMAC hmac;
		hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), _farNonce.data(), _farNonce.size(), mdp1);
		hmac.compute(EVP_sha256(), _parent->groupIdTxt().data(), _parent->groupIdTxt().size(), mdp1, Crypto::HMAC::SIZE, _groupConnectKey->data());
		hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), _nonce.data(), _nonce.size(), mdp1);
		hmac.compute(EVP_sha256(), _parent->groupIdTxt().data(), _parent->groupIdTxt().size(), mdp1, Crypto::HMAC::SIZE, _groupExpectedKey->data());
	}
}
