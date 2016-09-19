#include "P2PConnection.h"
#include "Invoker.h"
#include "RTMFPFlow.h"
#include "NetGroup.h"
#include "Mona/Logs.h"
#include "Listener.h"
#include "RTMFPConnection.h"

using namespace Mona;
using namespace std;

UInt32 P2PConnection::P2PSessionCounter = 2000000;

P2PConnection::P2PConnection(RTMFPConnection* parent, string id, Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, const SocketAddress& hostAddress, const Buffer& pubKey, bool responder) :
	_responder(responder), peerId(id), rawId("\x21\x0f"), _parent(parent), _sessionId(++P2PSessionCounter), attempt(0), _rawResponse(false), _pListener(NULL), _groupBeginSent(false), publicationInfosSent(false),
	_lastIdSent(0), _pushOutMode(0), pushInMode(0), _pMediaFlow(NULL), _pFragmentsFlow(NULL), _pReportFlow(NULL), _fragmentsMap(MAX_FRAGMENT_MAP_SIZE), _idFragmentMap(0), groupReportInitiator(false), _groupConnectSent(false),
	FlowManager(invoker, pOnSocketError, pOnStatusEvent, pOnMediaEvent) {
	onGroupHandshake = [this](const string& groupId, const string& key, const string& peerId) {
		handleGroupHandshake(groupId, key, peerId);
	};

	_outAddress = _targetAddress = hostAddress;

	_tag.resize(16);
	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes

	BinaryWriter writer2(_pubKey.data(), _pubKey.size());
	writer2.write(pubKey.data(), pubKey.size()); // copy parent public key

	if (peerId != "unknown")
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

P2PConnection::~P2PConnection() {
	DEBUG("Deletion of P2PConnection ", peerId)

	_pMainStream->OnGroupMedia::unsubscribe((OnGroupMedia&)*this);
	_pMainStream->OnGroupReport::unsubscribe((OnGroupReport&)*this);
	_pMainStream->OnGroupPlayPush::unsubscribe((OnGroupPlayPush&)*this);
	_pMainStream->OnGroupPlayPull::unsubscribe((OnGroupPlayPull&)*this);
	_pMainStream->OnFragmentsMap::unsubscribe((OnFragmentsMap&)*this);
	_pMainStream->OnGroupBegin::unsubscribe((OnGroupBegin&)*this);
	_pMainStream->OnFragment::unsubscribe((OnFragment&)*this);
	_pMainStream->OnGroupHandshake::unsubscribe(onGroupHandshake);
	close(true);
	_pMediaFlow = _pReportFlow = _pFragmentsFlow = NULL;
	_parent = NULL;
}

void P2PConnection::handleFlowClosed(UInt64 idFlow) {
	if (_pMediaFlow && idFlow == _pMediaFlow->id)
		_pMediaFlow = NULL;
	else if (_pReportFlow && idFlow == _pReportFlow->id) {
		_pReportFlow = NULL;
		INFO("Far peer has closed the NetGroup main writer, closing the connection...")
		close(true);
	} 
	else if (_pFragmentsFlow && idFlow == _pFragmentsFlow->id)
		_pFragmentsFlow = NULL;
}

UDPSocket&	P2PConnection::socket() { 
	return _parent->socket(); 
}

RTMFPFlow* P2PConnection::createSpecialFlow(UInt64 id, const string& signature) {
	if ((signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)  // NetGroup Report stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x19", 4) == 0)  // NetGroup Data stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x1D", 4) == 0)  // NetGroup Message stream
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)  // NetGroup Media Report stream (fragments Map)
		|| (signature.size() > 3 && signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0)) {  // NetGroup Media stream
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(pStream, true);
		RTMFPFlow* pFlow = NULL;
		if (signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0 && _pFragmentsFlow)
			pFlow = _pFragmentsFlow; // if Fragements flow exists already we keep the 1st one
		else {
			INFO("Creating new flow (", id, ") for P2PConnection ", peerId)
			pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
			pFlow->setPeerId(peerId);
			if (signature.compare(0, 4, "\x00\x47\x52\x1C", 4) == 0)
				_pReportFlow = pFlow; // Record the NetGroup stream
			else if (signature.compare(0, 4, "\x00\x47\x52\x11", 4) == 0)
				_pFragmentsFlow = pFlow; // Record the NetGroup Media Report stream
		}
		return pFlow;
	}
	return NULL;
}

void P2PConnection::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	while (++_nextRTMFPWriterId == 0 || !_flowWriters.emplace(_nextRTMFPWriterId, pWriter).second);
	(UInt64&)pWriter->id = _nextRTMFPWriterId;
	pWriter->amf0 = false;
	if (!_flows.empty()) {
		if (pWriter->signature.size() > 3 && pWriter->signature.compare(0, 4, "\x00\x47\x52\x12", 4) == 0 && _pFragmentsFlow)
			(UInt64&)pWriter->flowId = _pFragmentsFlow->id; // new Media Writer of NetGroup will be associated to the Report Flow
		else
			(UInt64&)pWriter->flowId = _flows.begin()->second->id; // new Writer will be associated to the P2PConnection flow (first in _flow lists)
	}
	if (!pWriter->signature.empty())
		DEBUG("New writer ", pWriter->id, " on connection ");
}

void P2PConnection::manageHandshake(Exception& ex, BinaryReader& reader) {
	UInt8 type = reader.read8();
	UInt16 length = reader.read16();

	switch (type) {
	case 0x30:
		DEBUG("Handshake 30 has already been received, request ignored") // responderHandshake0 is called by RTMFPConnection
		break;
	case 0x38:
		responderHandshake1(ex, reader); break;
	case 0x78:
		initiatorHandshake2(ex, reader); break;
	case 0x70:
		DEBUG("Unexpected p2p handshake type 70 (possible repeated request)")
		break;
	default:
		ex.set(Exception::PROTOCOL, "Unexpected p2p handshake type : ", Format<UInt8>("%.2x", (UInt8)type));
		break;
	}
}

void P2PConnection::responderHandshake0(Exception& ex, string tag, const SocketAddress& address) {

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write8(16);
	writer.write(tag);

	UInt8 cookie[COOKIE_SIZE];
	Util::Random(cookie, COOKIE_SIZE);
	writer.write8(COOKIE_SIZE);
	writer.write(cookie, COOKIE_SIZE);

	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x70).write16(writer.size() - RTMFP_HEADER_SIZE - 3);

	// Before sending we set connection parameters
	_outAddress = _targetAddress = address;
	_farId = 0;

	FlowManager::flush(0x0B, writer.size());
	_handshakeStep = 1;
}

void P2PConnection::responderHandshake1(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep > 1) {
		ex.set(Exception::PROTOCOL, "Unexpected handshake type 38 (step : ", _handshakeStep, ")");
		return;
	}

	_farId = reader.read32();

	string cookie;
	if (reader.read8() != 0x40) {
		ex.set(Exception::PROTOCOL, "Cookie size should be 64 bytes but found : ", *(reader.current() - 1));
		return;
	}
	reader.read(0x40, cookie);

	UInt32 publicKeySize = reader.read7BitValue();
	if (publicKeySize != 0x84)
		DEBUG("Public key size should be 132 bytes but found : ", publicKeySize);
	UInt32 idPos = reader.position(); // record position for peer ID determination
	if ((publicKeySize = reader.read7BitValue()) != 0x82)
		DEBUG("Public key size should be 130 bytes but found : ", publicKeySize);
	UInt16 signature = reader.read16();
	if (signature != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Expected signature 1D02 but found : ", Format<UInt16>("%.4x", signature));
		return;
	}
	reader.read(publicKeySize-2, _farKey);

	// Read peerId and update the parent
	UInt8 id[PEER_ID_SIZE];
	EVP_Digest(reader.data() + idPos, 0x84, id, NULL, EVP_sha256(), NULL);
	if (peerId == "unknown")
		rawId.append(STR id, PEER_ID_SIZE);
	INFO("peer ID calculated from public key : ", Util::FormatHex(id, PEER_ID_SIZE, peerId))
	_parent->addPeer2HeardList(_outAddress, peerId, rawId.data());

	UInt32 nonceSize = reader.read7BitValue();
	if (nonceSize != 0x4C) {
		ex.set(Exception::PROTOCOL, "Responder Nonce size should be 76 bytes but found : ", nonceSize);
		return;
	}
	reader.read(nonceSize, _farNonce);
	_farNonce.resize(nonceSize);

	UInt8 endByte;
	if ((endByte = reader.read8()) != 0x58) {
		ex.set(Exception::PROTOCOL, "Unexpected end byte : ", endByte, " (expected 0x58)");
		return;
	}

	// Write Response
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(_sessionId); // TODO: see if we need to do a << 24
	writer.write8(0x49); // nonce is 73 bytes long
	BinaryWriter nonceWriter(_nonce.data(), 0x49);
	nonceWriter.write(EXPAND("\x03\x1A\x00\x00\x02\x1E\x00\x41\x0E"));
	Util::Random(_nonce.data()+9, 64); // nonce 64 random bytes
	writer.write(_nonce.data(), 0x49);
	writer.write8(0x58);

	// Important: send this before computing encoder key because we need the default encoder
	// TODO: ensure that the default encoder is used for handshake (in repeated messages)
	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x78).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	FlowManager::flush(0x0B, writer.size());

	// Compute P2P keys for decryption/encryption
	if (!_parent->computeKeys(ex, _farKey, _farNonce, _nonce.data(), 0x49, _sharedSecret, _pDecoder, _pEncoder))
		return;

	DEBUG("Initiator Nonce : ", Util::FormatHex(BIN _farNonce.data(), _farNonce.size(), LOG_BUFFER))
	DEBUG("Responder Nonce : ", Util::FormatHex(BIN _nonce.data(), 0x49, LOG_BUFFER))

	_handshakeStep = 2;
	_parent->addPeer2Group(_outAddress, peerId); // TODO: see if we must check the result
}

void P2PConnection::initiatorHandshake70(Exception& ex, BinaryReader& reader, const SocketAddress& address) {

	if (_handshakeStep > 1) {
		WARN("Handshake 70 already received, ignored")
		return;
	}

	string cookie;
	UInt8 cookieSize = reader.read8();
	if (cookieSize != 0x40) {
		ex.set(Exception::PROTOCOL, "Unexpected cookie size : ", cookieSize);
		return;
	}
	reader.read(cookieSize, cookie);

	UInt32 keySize = (UInt32)reader.read7BitLongValue();
	if (keySize != 0x82) {
		// TODO: support other size (0x81 can happen)
		ex.set(Exception::PROTOCOL, "Unexpected responder key size : ", keySize);
		return;
	}
	if (reader.read16() != 0x1D02) {
		ex.set(Exception::PROTOCOL, "Unexpected signature before responder key (expected 1D02)");
		return;
	}
	reader.read(0x80, _farKey);

	// Write handshake1
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write32(_sessionId); // id

	writer.write7BitLongValue(cookieSize);
	writer.write(cookie); // Resend cookie

	writer.write7BitLongValue(_pubKey.size() + 4);
	writer.write7BitValue(_pubKey.size() + 2);
	writer.write16(0x1D02); // (signature)
	writer.write(_pubKey);

	_nonce.resize(0x4C,false);
	BinaryWriter nonceWriter(_nonce.data(), 0x4C);
	nonceWriter.write(EXPAND("\x02\x1D\x02\x41\x0E"));
	Util::Random(_nonce.data()+5, 64); // nonce is a serie of 64 random bytes
	nonceWriter.next(64);
	nonceWriter.write(EXPAND("\x03\x1A\x02\x0A\x02\x1E\x02"));
	writer.write7BitValue(_nonce.size());
	writer.write(_nonce);
	writer.write8(0x58);

	// Before sending we set connection parameters
	_outAddress = _targetAddress = address;
	_farId = 0;
	_parent->addPeer2HeardList(_outAddress, peerId, rawId.data());

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x38).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	if (!ex) {
		FlowManager::flush(0x0B, writer.size());
		_handshakeStep = 2;
	}
}

bool P2PConnection::initiatorHandshake2(Exception& ex, BinaryReader& reader) {

	if (_handshakeStep > 2) {
		WARN("Handshake 78 already received, ignored")
		return true;
	}

	_farId = reader.read32(); // session Id
	UInt8 nonceSize = reader.read8();
	if (nonceSize != 0x49) {
		ex.set(Exception::PROTOCOL, "Unexpected nonce size : ", nonceSize, " (expected 73)");
		return false;
	}
	reader.read(nonceSize, _farNonce);
	_farNonce.resize(nonceSize);
	if (String::ICompare(_farNonce, "\x03\x1A\x00\x00\x02\x1E\x00\x41\0E", 9) != 0) {
		ex.set(Exception::PROTOCOL, "Incorrect responder nonce : ", _farNonce);
		return false;
	}

	UInt8 endByte = reader.read8();
	if (endByte != 0x58) {
		ex.set(Exception::PROTOCOL, "Unexpected end of handshake 2 : ", endByte);
		return false;
	}

	// Compute P2P keys for decryption/encryption (TODO: refactorize key computing)
	string initiatorNonce((const char*)_nonce.data(), _nonce.size());
	if (!_parent->computeKeys(ex, _farKey, initiatorNonce, BIN _farNonce.data(), nonceSize, _sharedSecret, _pDecoder, _pEncoder, false))
		return false;

	DEBUG("Initiator Nonce : ", Util::FormatHex(_nonce.data(), _nonce.size(), LOG_BUFFER))
	DEBUG("Responder Nonce : ", Util::FormatHex(BIN _farNonce.data(), _farNonce.size(), LOG_BUFFER))

	_handshakeStep = 3;
	connected = true;
	_parent->addPeer2Group(_outAddress, peerId);
	NOTE("P2P Connection ", _sessionId, " is now connected to ", peerId)

	if (_group) {
		// Send group connection request
		// Create 1st Netstream and flow
		string signature("\x00\x47\x52\x1C", 4);
		_pReportFlow = createFlow(2, signature);
		_pReportFlow->setPeerId(peerId);

		// Compile encrypted key
		UInt8 mdp1[Crypto::HMAC::SIZE];
		UInt8 mdp2[Crypto::HMAC::SIZE];
		Crypto::HMAC hmac;
		hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), BIN _farNonce.data(), _farNonce.size(), mdp1);
		hmac.compute(EVP_sha256(), _group->idTxt.data(), _group->idTxt.size(), mdp1, Crypto::HMAC::SIZE, mdp2);

		INFO("Sending group connection request to peer")
		_rawResponse = true;
		_pReportFlow->sendGroupPeerConnect(_group->idHex, mdp2, rawId.c_str());
		_groupConnectSent = true;
		sendGroupBegin();
	}
	else {
		// Start playing
		// Create 1st NetStream and flow
		string signature("\x00\x54\x43\x04\xFA\x89\x01", 7); // stream id = 1
		RTMFPFlow* pFlow = createFlow(2, signature);
		pFlow->setPeerId(peerId);

		INFO("Sending play request to peer for stream '", _streamName, "'")
		pFlow->sendPlay(_streamName);
	}	
	return true;
}

unsigned int P2PConnection::callFunction(const char* function, int nbArgs, const char** args) {
	map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it == _flows.end() ? NULL : it->second;
	if (pFlow) {
		pFlow->call(function, nbArgs, args);
		return 1;
	}
	return 0;
}

void P2PConnection::flush(bool echoTime, UInt8 marker) {
	if (_rawResponse && marker != 0x0B)
		marker = 0x09;
	if (_responder)
		FlowManager::flush(echoTime, (marker == 0x0B) ? 0x0B : marker + 1);
	else
		FlowManager::flush(echoTime, marker);
}

void P2PConnection::addCommand(CommandType command, const char* streamName, bool audioReliable, bool videoReliable) {

	_streamName = streamName;
}

// Only in responder mode
bool P2PConnection::handlePlay(const string& streamName, FlashWriter& writer) {
	INFO("The peer ", peerId, " is trying to play '", streamName, "'...")

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

void P2PConnection::handleP2PAddressExchange(Exception& ex, PacketReader& reader) {
	ERROR("Cannot handle P2P Address Exchange command on a P2P Connection") // target error (shouldn't happen)
}

void P2PConnection::handleGroupHandshake(const std::string& groupId, const std::string& key, const std::string& id) {

	if (!_group) {
			ERROR("Group Handshake on a normal p2p connection")
			return;
		}

	if (String::ICompare(groupId, _group->idHex) != 0) {
		ERROR("Unexpected group ID received : ", groupId, "\nExpected : ", _group->idHex)
		return;
	}
	string idReceived;
	Util::FormatHex(BIN id.data(), PEER_ID_SIZE, idReceived);
	if (String::ICompare(idReceived, _parent->peerId()) != 0) {
		ERROR("Our peer ID was expected but received : ", idReceived)
		return;
	}

	// Send the group connection request to peer if not already sent
	if (!_groupConnectSent) {

		// Compile encrypted key
		UInt8 mdp1[Crypto::HMAC::SIZE];
		UInt8 mdp2[Crypto::HMAC::SIZE];
		Crypto::HMAC hmac;
		hmac.compute(EVP_sha256(), _sharedSecret.data(), _sharedSecret.size(), BIN _farNonce.data(), _farNonce.size(), mdp1);
		hmac.compute(EVP_sha256(), _group->idTxt.data(), _group->idTxt.size(), mdp1, Crypto::HMAC::SIZE, mdp2);

		if (!_pReportFlow) {
			ERROR("Unable to find the Report flow (2) for NetGroup communication")
			return;
		}

		INFO("Sending group connection answer to peer")
		_pReportFlow->sendGroupPeerConnect(_group->idHex, mdp2, rawId.c_str()/*, false*/);
		_groupConnectSent = true;
		sendGroupBegin();
	}
}

void P2PConnection::handleWriterFailed(RTMFPWriter* pWriter) {
	if (_pReportFlow && pWriter->flowId == _pReportFlow->id) {
		INFO("Far peer has closed the NetGroup main writer, closing the connection...")
		close(true);
	}
}

void P2PConnection::close(bool full) {
	_group.reset();

	if (connected) {
		closeGroup();
		connected = false;
		_handshakeStep = 0;
		_groupConnectSent = false;
	}

	if (_pListener) {
		_parent->stopListening(peerId);
		_pListener = NULL;
	}

	if (!full)
		return;

	// TODO: check if necessary, it could send a 5C message which has nothing to do with P2P
	FlowManager::close();
}

void P2PConnection::sendGroupBegin() {
	if (!_groupBeginSent) {
		if (!_pReportFlow) {
			ERROR("Unable to find the Report flow (2) for NetGroup communication")
				return;
		}

		INFO("Sending Group Begin message")
		_pReportFlow->sendGroupBegin();
		_groupBeginSent = true;
	}
}

void P2PConnection::sendGroupMedia(const string& stream, const UInt8* data, UInt32 size, UInt64 updatePeriod, UInt16 windowDuration) {

	INFO("Sending the stream infos for stream '", stream, "'")
	string signature("\x00\x47\x52\x11", 4);
	_pFragmentsFlow = createFlow(signature);
	_pFragmentsFlow->setPeerId(peerId);
	_pFragmentsFlow->sendGroupMediaInfos(stream, data, size, updatePeriod, windowDuration);
	publicationInfosSent = true;
}

// TODO: see if necessary (seems just a sendRaw)
void P2PConnection::sendGroupReport(const UInt8* data, UInt32 size) {

	if (!_pReportFlow) {
		ERROR("Unable to find the Report flow (2) for NetGroup communication")
		return;
	}
	_pReportFlow->sendRaw(data, size, false);
	sendGroupBegin(); // (if not already sent)
}

void P2PConnection::sendMedia(const UInt8* data, UInt32 size, UInt64 fragment, bool pull) {
	if ((!pull && !isPushable((UInt8)fragment%8)))
		return;

	if (!_pMediaFlow) {
		string signature("\x00\x47\x52\x12", 4);
		if (!(_pMediaFlow = createFlow(signature)))
			return;
		_pMediaFlow->setPeerId(peerId);
	}
	_pMediaFlow->sendRaw(data, size, true);
}

void P2PConnection::sendFragmentsMap(UInt64 lastFragment, const UInt8* data, UInt32 size) {
	if (_pFragmentsFlow && lastFragment != _lastIdSent) {
		DEBUG("Sending Fragments Map message (type 22) to peer ", peerId)
		_pFragmentsFlow->sendRaw(data, size, true);
		_lastIdSent = lastFragment;
	}
}

void P2PConnection::setPushMode(UInt8 mode) {
	INFO("Setting Group Push Out mode to ", Format<UInt8>("%.2x", mode), " for neighbor at address ", _outAddress.toString());
	_pushOutMode = mode;
}

bool P2PConnection::isPushable(UInt8 rest) {
	return (_pushOutMode & (1 << rest)) > 0;
}

void P2PConnection::sendPushMode(UInt8 mode) {
	if (_pFragmentsFlow && pushInMode != mode) {
		INFO("Setting Group Push In mode to ", Format<UInt8>("%.2x", mode), " for neighbor at address ", _outAddress.toString());

		_pFragmentsFlow->sendGroupPlay(mode);
		pushInMode = mode;
	}
}

void P2PConnection::updateFragmentsMap(UInt64 id, const UInt8* data, UInt32 size) {
	_idFragmentMap = id;
	if (!size)
		return; // 0 size protection

	if (size > MAX_FRAGMENT_MAP_SIZE)
		WARN("Size of fragment map > max size : ", size)
	_fragmentsMap.resize(size);
	BinaryWriter writer(_fragmentsMap.data(), size);
	writer.write(data, size);
}

bool P2PConnection::checkMask(UInt8 bitNumber) {
	if (!_idFragmentMap)
		return false;

	if (_idFragmentMap % 8 == bitNumber)
		return true;

	// Determine the last fragment with bit mask
	UInt64 lastFragment = _idFragmentMap - (_idFragmentMap % 8);
	lastFragment += ((_idFragmentMap % 8) > bitNumber) ? bitNumber : bitNumber - 8;

	DEBUG("Searching ", lastFragment, " into ", Format<UInt8>("%.2x", *_fragmentsMap.data()), " ; (current id : ", _idFragmentMap, ") ; result = ",
		((*_fragmentsMap.data()) & (1 << (8 - _idFragmentMap + lastFragment))) > 0, " ; bit : ", bitNumber, " ; address : ", _targetAddress.toString(), " ; latency : ", latency())

	return ((*_fragmentsMap.data()) & (1 << (8 - _idFragmentMap + lastFragment))) > 0;
}

bool P2PConnection::hasFragment(UInt64 index) {
	if (!_idFragmentMap || _idFragmentMap < index) {
		DEBUG("Searching ", index, " impossible, current id : ", _idFragmentMap)
		return false; // No Fragment or index too recent
	}

	if (_idFragmentMap == index)
		return true;

	UInt32 offset = (UInt32)((_idFragmentMap - index) / 8);
	UInt32 rest = 7 - ((_idFragmentMap - index) % 8);
	if (offset > _fragmentsMap.size())
		return false; // Fragment deleted from buffer

	DEBUG("Searching ", index, " into ", Format<UInt8>("%.2x", *(_fragmentsMap.data() + offset)), " ; (current id : ", _idFragmentMap, ", offset : ", offset, ") ; result = ",
		(*(_fragmentsMap.data() + offset) & (1 << rest)) > 0)

	return (*(_fragmentsMap.data() + offset) & (1 << rest)) > 0;
}

void P2PConnection::sendPull(UInt64 index) {
	if (_pFragmentsFlow) {
		INFO("Sending pull request for fragment ", index, " to peer ", _targetAddress.toString());

		_pFragmentsFlow->sendGroupPull(index);
	}
}

void P2PConnection::closeGroup() {

	if (_pReportFlow)
		_pReportFlow->close();
	if (_pFragmentsFlow)
		_pFragmentsFlow->close();
}
