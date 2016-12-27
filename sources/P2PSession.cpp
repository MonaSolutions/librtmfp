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

using namespace Mona;
using namespace std;

UInt32 P2PSession::P2PSessionCounter = 2000000;

P2PSession::P2PSession(RTMFPSession* parent, string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, 
		const Mona::SocketAddress& host, bool responder, bool group) :
	_responder(responder), peerId(id), rawId("\x21\x0f"), hostAddress(host), _parent(parent), attempt(0), _rawResponse(false), _groupBeginSent(false), mediaSubscriptionSent(false),
	_lastIdSent(0), _pushOutMode(0), pushInMode(0), _fragmentsMap(MAX_FRAGMENT_MAP_SIZE), _idFragmentMap(0), groupReportInitiator(false), _groupConnectSent(false), _idMediaReportFlow(0), _isGroup(group),
	_isGroupDisconnected(false), groupFirstReportSent(false), mediaSubscriptionReceived(false), FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onGroupHandshake = [this](const string& groupId, const string& key, const string& peerId) {
		handleGroupHandshake(groupId, key, peerId);
	};
	onWriterClose = [this](shared_ptr<RTMFPWriter>& pWriter) {
		// We reset the pointers before closure
		if (pWriter == _pMediaReportWriter)
			_pMediaReportWriter.reset();
		else if (pWriter == _pReportWriter)
			_pReportWriter.reset(); 
		else if (pWriter == _pNetStreamWriter)
			_pNetStreamWriter.reset();
	};

	_sessionId = ++P2PSessionCounter;
	Util::UnformatHex(BIN peerId.data(), peerId.size(), rawId, true);

	_pMainStream->OnGroupMedia::subscribe((OnGroupMedia&)*this);
	_pMainStream->OnGroupReport::subscribe((OnGroupReport&)*this);
	_pMainStream->OnGroupPlayPush::subscribe((OnGroupPlayPush&)*this);
	_pMainStream->OnGroupPlayPull::subscribe((OnGroupPlayPull&)*this);
	_pMainStream->OnFragmentsMap::subscribe((OnFragmentsMap&)*this);
	_pMainStream->OnGroupBegin::subscribe((OnGroupBegin&)*this);
	_pMainStream->OnFragment::subscribe((OnFragment&)*this);
	_pMainStream->OnGroupHandshake::subscribe(onGroupHandshake);
}

P2PSession::~P2PSession() {
	DEBUG("Deletion of P2PSession ", peerId)
	close();
}

void P2PSession::close(bool full) {
	if (status == RTMFP::FAILED)
		return;

	closeGroup(full);

	if (_pListener) {
		_parent->stopListening(peerId);
		_pListener = NULL;
	}

	if (full) {
		_pMainStream->OnGroupMedia::unsubscribe((OnGroupMedia&)*this);
		_pMainStream->OnGroupReport::unsubscribe((OnGroupReport&)*this);
		_pMainStream->OnGroupPlayPush::unsubscribe((OnGroupPlayPush&)*this);
		_pMainStream->OnGroupPlayPull::unsubscribe((OnGroupPlayPull&)*this);
		_pMainStream->OnFragmentsMap::unsubscribe((OnFragmentsMap&)*this);
		_pMainStream->OnGroupBegin::unsubscribe((OnGroupBegin&)*this);
		_pMainStream->OnFragment::unsubscribe((OnFragment&)*this);
		_pMainStream->OnGroupHandshake::unsubscribe(onGroupHandshake);

		FlowManager::close();
		_parent = NULL;
	}
}

void P2PSession::subscribe(shared_ptr<RTMFPConnection>& pConnection) {
	_knownAddresses.emplace(pConnection->address(), RTMFP::ADDRESS_PUBLIC); // TODO: Calculate the address type?
	pConnection->setSession(this);

	FlowManager::subscribe(pConnection);
}

RTMFPFlow* P2PSession::createSpecialFlow(Exception& ex, UInt64 id, const string& signature) {

	if (signature.size()>6 && signature.compare(0, 6, "\x00\x54\x43\x04\xFA\x89", 6) == 0) { // Direct P2P NetStream
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 6, signature.length() - 6).read7BitValue());
		DEBUG("Creating new Flow (2) for P2PSession ", name())
		_pMainStream->addStream(idSession, pStream);
		RTMFPFlow* pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *_pConnection);
		pFlow->setPeerId(peerId);

		return pFlow;
	}
	else if ((signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)  // NetGroup Report stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x19", 4) == 0)  // NetGroup Data stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x1D", 4) == 0)  // NetGroup Message stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)  // NetGroup Media Report stream (fragments Map & Media Subscription)
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0)) {  // NetGroup Media stream
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true);

		DEBUG("Creating new flow (", id, ") for P2PSession ", peerId)
		RTMFPFlow* pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *_pConnection);
		pFlow->setPeerId(peerId);

		if (signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)
			_idMediaReportFlow = pFlow->id; // Record the NetGroup Media Report id
		return pFlow;
	}
	string tmp;
	ex.set(Exception::PROTOCOL, "Unhandled signature type : ", Util::FormatHex((const UInt8*)signature.data(), signature.size(), tmp), " , cannot create RTMFPFlow");
	return NULL;
}

void P2PSession::handleNewWriter(shared_ptr<RTMFPWriter>& pWriter) {

	if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0) {
		(UInt64&)pWriter->flowId = _idMediaReportFlow; // new Media Writer of NetGroup will be associated to the Media Report Flow
		_pMediaWriter = pWriter;
		return;
	}

	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // other new Writer are associated to the P2PSession Report flow (first in _flow lists)

	if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)
		_pReportWriter = pWriter;
	else if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)
		_pMediaReportWriter = pWriter;
	else if (pWriter->signature.size() > 6 && pWriter->signature.compare(0, 7, "\x00\x54\x43\x04\xFA\x89\x01", 7) == 0)
		_pNetStreamWriter = pWriter; // TODO: maybe manage many streams
}

unsigned int P2PSession::callFunction(const char* function, int nbArgs, const char** args) {

	if (!_pNetStreamWriter) {
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection); // writer is automatically associated to _pNetStreamWriter
	}
	_pMainStream->callFunction(*_pNetStreamWriter, function, nbArgs, args);
	return 0;
}

void P2PSession::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {

	_streamName = streamName;
}

// Only in responder mode
bool P2PSession::handlePlay(const string& streamName, FlashWriter& writer) {
	DEBUG("The peer ", peerId, " is trying to play '", streamName, "'...")

	Exception ex;
	if(!(_pListener = _parent->startListening<FlashListener, FlashWriter&>(ex, streamName, peerId, writer))) {
		// TODO : See if we can send a specific answer
		WARN(ex.error())
		return false;
	}
	INFO("Stream ",streamName," found, sending start answer")

	// A peer is connected : unlock the possible blocking RTMFP_PublishP2P function
	_parent->setP2pPublisherReady();
	return true;
}

void P2PSession::handleProtocolFailed() {
	close();
}

void P2PSession::handleP2PAddressExchange(PacketReader& reader) {
	ERROR("Cannot handle P2P Address Exchange command on a P2P Connection") // target error (shouldn't happen)
}

void P2PSession::handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id) {
	if (!_isGroup)
		return;

	// Is it a reconnection? => add the peer to group
	if (_isGroupDisconnected && !_parent->addPeer2Group(peerId))
		return;

	if (String::ICompare(groupId, _parent->groupIdHex()) != 0) {
		ERROR("Unexpected group ID received : ", groupId, "\nExpected : ", _parent->groupIdHex())
		return;
	}
	string idReceived;
	Util::FormatHex(BIN id.data(), PEER_ID_SIZE, idReceived);
	if (String::ICompare(idReceived, _parent->peerId()) != 0) {
		ERROR("Our peer ID was expected but received : ", idReceived)
		return;
	}

	// Send the group connection request to peer if not already sent
	if (!_groupConnectSent)
		sendGroupPeerConnect();
}

void P2PSession::handleWriterFailed(shared_ptr<RTMFPWriter>& pWriter) {

	if (pWriter == _pMediaReportWriter) {
		DEBUG(peerId, " has closed the subscription media writer")
		pWriter->close();
		mediaSubscriptionSent = mediaSubscriptionReceived = false;
		return;
	}
	else if (pWriter == _pMediaWriter) {
		DEBUG(peerId, " has closed the media writer")
		pWriter->close();
		_pMediaWriter.reset();
		return;
	}
	else if (pWriter == _pReportWriter) {
		DEBUG(peerId, " has closed the report writer")
		_pReportWriter.reset();
	}
	else if (pWriter == _pNetStreamWriter)
		_pNetStreamWriter.reset();

	Exception ex;
	pWriter->fail(ex, "Writer terminated on connection ", peerId);
	if (ex)
		WARN(ex.error())
}

void P2PSession::sendGroupBegin() {
	if (!_groupBeginSent) {
		if (!_pReportWriter) {
			ERROR("Unable to find the Report flow (2) for NetGroup communication")
			return;
		}

		DEBUG("Sending Group Begin message")
		_pReportWriter->writeGroupBegin();
		_pReportWriter->flush();
		_groupBeginSent = true;
	}
}

void P2PSession::sendGroupMedia(const string& stream, const UInt8* data, UInt32 size, RTMFPGroupConfig* groupConfig) {

	DEBUG("Sending the Media Subscription for stream '", stream, "' to peer ", peerId)
	if (!_pMediaReportWriter) {
		string signature("\x00\x47\x52\x11", 4);
		new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection); // writer is automatically associated to _pMediaReportWriter
	}
	_pMediaReportWriter->writeGroupMedia(stream, data, size, groupConfig);
	mediaSubscriptionSent = true;
}

void P2PSession::sendGroupReport(const UInt8* data, UInt32 size) {

	if (!_pReportWriter) {
		ERROR("Unable to find the Report flow (2) for NetGroup communication")
		return;
	}
	_pReportWriter->writeRaw(data, size);
	if (!groupFirstReportSent)
		groupFirstReportSent = true;
	sendGroupBegin(); // (if not already sent)
}

bool P2PSession::sendMedia(const UInt8* data, UInt32 size, UInt64 fragment, bool pull) {
	if ((!pull && !isPushable((UInt8)fragment%8)))
		return false;

	if (!_pMediaWriter) {
		string signature("\x00\x47\x52\x12", 4);
		new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection); // writer is automatically associated to _pMediaWriter
	}
	_pMediaWriter->writeRaw(data, size);
	_pMediaWriter->flush();
	return true;
}

void P2PSession::sendFragmentsMap(UInt64 lastFragment, const UInt8* data, UInt32 size) {
	if (_pMediaReportWriter && lastFragment != _lastIdSent) {
		DEBUG("Sending Fragments Map message (type 22) to peer ", peerId, " (", lastFragment,")")
		_pMediaReportWriter->writeRaw(data, size);
		_pMediaReportWriter->flush();
		_lastIdSent = lastFragment;
	}
}

void P2PSession::setPushMode(UInt8 mode) {
	_pushOutMode = mode;
}

bool P2PSession::isPushable(UInt8 rest) {
	return (_pushOutMode & (1 << rest)) > 0;
}

void P2PSession::sendPushMode(UInt8 mode) {
	if (_pMediaReportWriter && pushInMode != mode) {
		string masks;
		if (mode > 0) {
			for (int i = 0; i < 8; i++) {
				if ((mode & (1 << i)) > 0)
					String::Append(masks, (masks.empty() ? "" : ", "), i, ", ", Format<UInt8>("%.1X", i + 8));
			}
		}

		DEBUG("Setting Group Push In mode to ", Format<UInt8>("%.2x", mode), " (", masks,") for peer ", peerId, " - last fragment : ", _idFragmentMap)
		_pMediaReportWriter->writeGroupPlay(mode);
		_pMediaReportWriter->flush();
		pushInMode = mode;
	}
}

void P2PSession::updateFragmentsMap(UInt64 id, const UInt8* data, UInt32 size) {
	if (id <= _idFragmentMap) {
		DEBUG("Wrong Group Fragments map received from peer ", peerId, " : ", id, " <= ", _idFragmentMap)
		return;
	}

	_idFragmentMap = id;
	if (!size)
		return; // 0 size protection

	if (size > MAX_FRAGMENT_MAP_SIZE)
		WARN("Size of fragment map > max size : ", size)
	_fragmentsMap.resize(size);
	BinaryWriter writer(_fragmentsMap.data(), size);
	writer.write(data, size);
}

bool P2PSession::checkMask(UInt8 bitNumber) {
	if (!_idFragmentMap)
		return false;

	if (_idFragmentMap % 8 == bitNumber)
		return true;

	// Determine the last fragment with bit mask
	UInt64 lastFragment = _idFragmentMap - (_idFragmentMap % 8);
	lastFragment += ((_idFragmentMap % 8) > bitNumber) ? bitNumber : bitNumber - 8;

	DEBUG("Searching ", lastFragment, " into ", Format<UInt8>("%.2x", *_fragmentsMap.data()), " ; (current id : ", _idFragmentMap, ") ; result = ",
		((*_fragmentsMap.data()) & (1 << (8 - _idFragmentMap + lastFragment))) > 0, " ; bit : ", bitNumber, " ; address : ", name(), " ; latency : ", latency())

	return ((*_fragmentsMap.data()) & (1 << (8 - _idFragmentMap + lastFragment))) > 0;
}

bool P2PSession::hasFragment(UInt64 index) {
	if (!_idFragmentMap || (_idFragmentMap < index)) {
		TRACE("Searching ", index, " impossible into ", peerId, ", current id : ", _idFragmentMap)
		return false; // No Fragment or index too recent
	}
	else if (_idFragmentMap == index) {
		TRACE("Searching ", index, " OK into ", peerId, ", current id : ", _idFragmentMap)
		return true; // Fragment is the last one or peer has all fragments
	}
	else if (_setPullBlacklist.find(index) != _setPullBlacklist.end()) {
		TRACE("Searching ", index, " impossible into ", peerId, " a request has already failed")
		return false;
	}

	UInt32 offset = (UInt32)((_idFragmentMap - index - 1) / 8);
	UInt32 rest = ((_idFragmentMap - index - 1) % 8);
	if (offset > _fragmentsMap.size()) {
		TRACE("Searching ", index, " impossible into ", peerId, ", out of buffer (", offset, "/", _fragmentsMap.size(), ")")
		return false; // Fragment deleted from buffer
	}

	TRACE("Searching ", index, " into ", Format<UInt8>("%.2x", *(_fragmentsMap.data() + offset)), " ; (current id : ", _idFragmentMap, ", offset : ", offset, ") ; result = ",
		(*(_fragmentsMap.data() + offset) & (1 << rest)) > 0)

	return (*(_fragmentsMap.data() + offset) & (1 << rest)) > 0;
}

void P2PSession::sendPull(UInt64 index) {
	if (_pMediaReportWriter) {
		TRACE("Sending pull request for fragment ", index, " to peer ", peerId);
		_pMediaReportWriter->writeGroupPull(index);
	}
}

void P2PSession::closeGroup(bool full) {

	// Full close : we also close the NetGroup Report writer
	if (full && _pReportWriter) {
		_groupConnectSent = false;
		_groupBeginSent = false;
		groupFirstReportSent = false;
		_pReportWriter->close();
	}

	_isGroupDisconnected = true;
	mediaSubscriptionSent = mediaSubscriptionReceived = false;
	pushInMode = 0;
	if (_pMediaReportWriter)
		_pMediaReportWriter->close();
	if (_pMediaWriter)
		_pMediaWriter->close();

	OnPeerClose::raise(peerId, pushInMode, full); // notify NetGroup to reset push masks
}

void P2PSession::sendGroupPeerConnect() {
	if (!_pReportWriter) {
		string signature("\x00\x47\x52\x1C", 4);
		new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection);  // writer is automatically associated to _pReportWriter
	}

	// Compile encrypted key
	if (!_groupConnectKey) {
		_groupConnectKey.reset(new Buffer(Crypto::HMAC::SIZE));
		UInt8 mdp1[Crypto::HMAC::SIZE];
		Crypto::HMAC hmac;
		hmac.compute(EVP_sha256(), _pConnection->sharedSecret().data(), _pConnection->sharedSecret().size(), _pConnection->farNonce().data(), _pConnection->farNonce().size(), mdp1);
		hmac.compute(EVP_sha256(), _parent->groupIdTxt().data(), _parent->groupIdTxt().size(), mdp1, Crypto::HMAC::SIZE, _groupConnectKey->data());
	}

	DEBUG("Sending group connection request to peer ", peerId)
	_pReportWriter->writePeerGroup(_parent->groupIdHex(), _groupConnectKey->data(), rawId.c_str());
	_pReportWriter->flush();
	_groupConnectSent = true;
	sendGroupBegin();
}

void P2PSession::addPullBlacklist(UInt64 idFragment) {
	// TODO: delete old blacklisted fragments
	_setPullBlacklist.emplace(idFragment);
}

void P2PSession::onConnection(shared_ptr<RTMFPConnection>& pConnection) {

	INFO("P2P Connection is now connected to ", name())

	status = RTMFP::CONNECTED;
	_pConnection = pConnection;
	if (!_pFlowNull)
		_pFlowNull.reset(new RTMFPFlow(0, String::Empty, _pMainStream, _pInvoker->poolBuffers, *pConnection));

	if (_isGroup && _parent->addPeer2Group(peerId))
		sendGroupPeerConnect();
	// Start playing
	else if (!_parent->isPublisher()) {
		INFO("Sending play request to peer for stream '", _streamName, "'")
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		new RTMFPWriter(FlashWriter::OPENED, signature, *_pConnection); // writer is automatically associated to _pNetStreamWriter
		AMFWriter& amfWriter = _pNetStreamWriter->writeInvocation("play", true);
		amfWriter.amf0 = true; // Important for p2p unicast play
		amfWriter.writeString(_streamName.c_str(), _streamName.size());
		_pNetStreamWriter->flush();
	}
}
