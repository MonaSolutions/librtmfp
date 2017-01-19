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

#include "FlowManager.h"
#include "Mona/PacketWriter.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"
#include "FlashConnection.h"
#include "RTMFPFlow.h"
#include "SocketHandler.h"

using namespace Mona;
using namespace std;

FlowManager::RTMFPMediaPacket::RTMFPMediaPacket(const PoolBuffers& poolBuffers, const UInt8* data, UInt32 size, UInt32 time, bool audio) : pBuffer(poolBuffers, size + 15), pos(0) {
	BinaryWriter writer(pBuffer->data(), size + 15);

	writer.write8(audio ? '\x08' : '\x09');
	// size on 3 bytes
	writer.write24(size);
	// time on 3 bytes
	writer.write24(time);
	// unknown 4 bytes set to 0
	writer.write32(0);
	// payload
	writer.write(data, size);
	// footer
	writer.write32(11 + size);

}

const char FlowManager::_FlvHeader[] = { 'F', 'L', 'V', 0x01,
0x05,				/* 0x04 == audio, 0x01 == video */
0x00, 0x00, 0x00, 0x09,
0x00, 0x00, 0x00, 0x00
};

FlowManager::FlowManager(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent) :
	_firstRead(true), _pInvoker(invoker), _firstMedia(true), _timeStart(0), _codecInfosRead(false), _pOnStatusEvent(pOnStatusEvent), _pOnMedia(pOnMediaEvent), _pOnSocketError(pOnSocketError),
	status(RTMFP::STOPPED), _tag(16, '0'), _sessionId(0), _pListener(NULL), _mainFlowId(0) {
	onStatus = [this](const string& code, const string& description, UInt16 streamId, UInt64 flowId, double cbHandler) {
		_pOnStatusEvent(code.c_str(), description.c_str());

		if (code == "NetConnection.Connect.Success")
			onConnect();
		else if (code == "NetStream.Publish.Start")
			onPublished(streamId);
		else if (code == "NetConnection.Connect.Closed" || code == "NetConnection.Connect.Rejected" || code == "NetStream.Publish.BadName") {
			close(false);
			return false; // close the flow
		}
		return true;
	};
	onPlay = [this](const string& streamName, UInt16 streamId, UInt64 flowId, double cbHandler) {
		return handlePlay(streamName, streamId, flowId, cbHandler);
	};
	onMedia = [this](const string& stream, UInt32 time, PacketReader& packet, double lostRate, bool audio) {

		if (!_codecInfosRead) {
			if (!audio && RTMFP::IsH264CodecInfos(packet.current(), packet.available())) {
				INFO("Video codec infos found, starting to read")
				_codecInfosRead = true;
			} else {
				if (!audio)
					DEBUG("Video frame dropped to wait first key frame");
				return;
			}
		}

		if(_firstMedia) {
			_firstMedia=false;
			_timeStart=time; // to set to 0 the first packets
		}
		else if (time < _timeStart) {
			DEBUG("Packet ignored because it is older (", time, ") than start time (", _timeStart, ")")
			return;
		}

		if (_pOnMedia) // Synchronous read
			_pOnMedia(name().c_str(), stream.c_str(), time-_timeStart, (const char*)packet.current(), packet.available(), audio);
		else { // Asynchronous read
			{
				lock_guard<recursive_mutex> lock(_readMutex); // TODO: use the 'stream' parameter
				_mediaPackets[name()].emplace_back(new RTMFPMediaPacket(_pInvoker->poolBuffers, packet.current(), packet.available(), time - _timeStart, audio));
			}
			handleDataAvailable(true);
		}
	};
	/*TODO: onError = [this](const Exception& ex) {
		string buffer;
		_pOnSocketError(String::Format(buffer, ex.error(), " on connection ", name()).c_str());
	};*/
	onMessage = [this](BinaryReader& reader) {
		receive(reader);
	};
	onNewWriter = [this](shared_ptr<RTMFPWriter>& pWriter) {
		handleNewWriter(pWriter);
	};
	onWriterException = [this](shared_ptr<RTMFPWriter>& pWriter) {
		handleWriterException(pWriter);
	};
	onWriterError = [this](const Exception& ex) {
		INFO("Closing session ", name(), " : ", ex.error())
		close(true);
	};

	Util::Random((UInt8*)_tag.data(), 16); // random serie of 16 bytes

	_pMainStream.reset(new FlashConnection());
	_pMainStream->OnStatus::subscribe(onStatus);
	_pMainStream->OnMedia::subscribe(onMedia);
	_pMainStream->OnPlay::subscribe(onPlay);
}

FlowManager::~FlowManager() {

	// remove the flows
	for (auto& it : _flows)
		delete it.second;
	_flows.clear();

	close(true);

	// delete media packets
	lock_guard<recursive_mutex> lock(_readMutex);
	_mediaPackets.clear();

	if (_pMainStream) {
		_pMainStream->OnStatus::unsubscribe(onStatus);
		_pMainStream->OnMedia::unsubscribe(onMedia);
		_pMainStream->OnPlay::unsubscribe(onPlay);
	}
}

void FlowManager::close(bool abrupt) {
	if (status == RTMFP::FAILED)
		return;

	for (auto& itConnection : _mapConnections) {
		itConnection.second->close(abrupt);
		if (abrupt)
			unsubscribeConnection(itConnection.second);
	}

	if (abrupt) {
		_pConnection.reset();
		_mapConnections.clear();
	}

	if (abrupt)
		status = RTMFP::FAILED;
	else {
		status = RTMFP::NEAR_CLOSED;
		_closeTime.update(); // To wait 90s before deleting session
	}
}

UInt16 FlowManager::latency() {
	return (_pConnection) ? (_pConnection->ping() >> 1) : 0;
}

void FlowManager::subscribe(shared_ptr<RTMFPConnection>& pConnection) {

	pConnection->OnMessage::subscribe(onMessage);
	pConnection->OnNewWriter::subscribe(onNewWriter);
	pConnection->OnWriterException::subscribe(onWriterException);
	pConnection->OnWriterClose::subscribe(onWriterClose);
	pConnection->OnWriterError::subscribe(onWriterError);
	_mapConnections.emplace(pConnection->address(), pConnection);
}

void FlowManager::unsubscribeConnection(const Mona::SocketAddress& address) {
	auto it = _mapConnections.find(address);
	if (it != _mapConnections.end()) {
		unsubscribeConnection(it->second);
		_mapConnections.erase(it);
	} else
		WARN("Unable to find the connection ", address.toString(), " for unbscribing")

	if (_mapConnections.empty()) {
		DEBUG("No more connection available, session ", name(), " is closing...")
		close(false);
	}
}

void FlowManager::unsubscribeConnection(shared_ptr<RTMFPConnection>& pConnection) {
	TRACE("Unsubscribing events of the connection ", pConnection->address().toString())

	pConnection->OnMessage::unsubscribe(onMessage);
	pConnection->OnNewWriter::unsubscribe(onNewWriter);
	pConnection->OnWriterException::unsubscribe(onWriterException);
	pConnection->OnWriterClose::unsubscribe(onWriterClose);
	pConnection->OnWriterError::unsubscribe(onWriterError);
}

bool FlowManager::readAsync(UInt8* buf, UInt32 size, int& nbRead) {
	if (nbRead != 0)
		ERROR("Parameter nbRead must equal zero in readAsync()")
	else if (status == RTMFP::CONNECTED) {

		bool available = false;
		lock_guard<recursive_mutex> lock(_readMutex);
		if (!_mediaPackets[name()].empty()) {
			// First read => send header
			if (_firstRead && size > sizeof(_FlvHeader)) { // TODO: make a real context with a recorded position
				memcpy(buf, _FlvHeader, sizeof(_FlvHeader));
				_firstRead = false;
				size -= sizeof(_FlvHeader);
				nbRead += sizeof(_FlvHeader);
			}

			UInt32 bufferSize = 0, toRead = 0;
			auto& queue = _mediaPackets[name()];
			while (!queue.empty() && (nbRead < size)) {

				// Read next packet
				std::shared_ptr<RTMFPMediaPacket>& packet = queue.front();
				bufferSize = packet->pBuffer.size() - packet->pos;
				toRead = (bufferSize > (size - nbRead)) ? size - nbRead : bufferSize;
				memcpy(buf + nbRead, packet->pBuffer.data() + packet->pos, toRead);
				nbRead += toRead;

				// If packet too big : save position and exit
				if (bufferSize > toRead) {
					packet->pos += toRead;
					break;
				}
				queue.pop_front();
			}
			available = !queue.empty();
			}
		if (!available)
			handleDataAvailable(false); // change the available status
		return true;
	} 

	return false;
}

void FlowManager::receive(BinaryReader& reader) {

	// Variables for request (0x10 and 0x11)
	UInt8 flags;
	RTMFPFlow* pFlow = NULL;
	UInt64 stage = 0;
	UInt64 deltaNAck = 0;

	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF;
	bool answer = false;

	// Can have nested queries
	while (type != 0xFF) {

		UInt16 size = reader.read16();

		PacketReader message(reader.current(), size);

		switch (type) {
		case 0x0f: // P2P address destinator exchange
			handleP2PAddressExchange(message);
			break;
		case 0xcc:
			INFO("CC message received (unknown for now) from connection ", name())
#if defined(_DEBUG)
			Logs::Dump(reader.current(), size);
#endif
			break;
		case 0x0c:
			INFO("Session ", name(), " is closing");
			close(true);
			break;
		case 0x4c : // P2P closing session (only for p2p I think)
			INFO("P2P Session ", name(), " is closing abruptly")
			close(true);
			return;
		case 0x01: // KeepAlive
			/*if(!peer.connected)
			fail("Timeout connection client");
			else*/
			_pConnection->writeMessage(0x41, 0);
			break;
		case 0x41:
			_lastKeepAlive.update();
			break;

		case 0x5e : {  // P2P closing flow (RTMFPFlow exception, only for p2p)
			UInt64 id = message.read7BitLongValue();
			_pConnection->handleWriterException(id);
			break;
		}
			/*case 0x18 :
			/// This response is sent when we answer with a Acknowledgment negative
			// It contains the id flow
			// I don't unsertand the usefulness...
			//pFlow = &flow(message.read8());
			//stage = pFlow->stageSnd();
			// For the moment, we considerate it like a exception
			fail("ack negative from server"); // send fail message immediatly
			break;*/

		case 0x51: {
			/// Acknowledgment
			UInt64 id = message.read7BitLongValue();
			_pConnection->handleAcknowledgment(id, message);
			break;
		}
		/// Request
		// 0x10 normal request
		// 0x11 special request, in repeat case (following stage request)
		case 0x10: {
			flags = message.read8();
			UInt64 idFlow = message.read7BitLongValue();
			stage = message.read7BitLongValue() - 1;
			deltaNAck = message.read7BitLongValue() - 1;

			if (status == RTMFP::FAILED)
				break;

			map<UInt64, RTMFPFlow*>::const_iterator it = _flows.find(idFlow);
			pFlow = it == _flows.end() ? NULL : it->second;

			// Header part if present
			if (flags & MESSAGE_HEADER) {
				string signature;
				message.read(message.read8(), signature);

				UInt64 idWriterRef = 0;
				if (message.read8()>0) {

					// Fullduplex header part
					if (message.read8() != 0x0A)
						WARN("Unknown fullduplex header part for the flow ", idFlow)
					else
						idWriterRef = message.read7BitLongValue(); // RTMFPWriter ID related to this flow

					// Useless header part 
					UInt8 length = message.read8();
					while (length>0 && message.available()) {
						WARN("Unknown message part on flow ", idFlow);
						message.next(length);
						length = message.read8();
					}
					if (length>0) {
						ERROR("Bad header message part, finished before scheduled");
						return;
					}
				}

				if (!pFlow)
					pFlow = createFlow(idFlow, signature, idWriterRef);
			}

			if (!pFlow) {
				WARN("RTMFPFlow ", idFlow, " unfound for connection ", name());
				break;
			}

		}
		case 0x11: {
			++stage;
			++deltaNAck;

			// has Header?
			if (type == 0x11)
				flags = message.read8();

			// Process request
			if (pFlow && (status != RTMFP::FAILED))
				pFlow->receive(stage, deltaNAck, message, flags);

			break;
		}
		default:
			ERROR("RTMFPMessage type '", Format<UInt8>("%02x", type), "' unknown on connection ", name());
			return;
		}

		// Next
		reader.next(size);
		type = reader.available()>0 ? reader.read8() : 0xFF;

		// Commit RTMFPFlow (pFlow means 0x11 or 0x10 message)
		if (pFlow && (status != RTMFP::FAILED) && type != 0x11) {
			pFlow->commit();
			if (pFlow->consumed())
				removeFlow(pFlow);
			pFlow = NULL;
		}
	}
}

RTMFPFlow* FlowManager::createFlow(UInt64 id, const string& signature, UInt64 idWriterRef) {
	if (status == RTMFP::FAILED) {
		ERROR("Connection is died, no more RTMFPFlow creation possible");
		return NULL;
	}
	if (!_pMainStream)
		return NULL; // has failed! use FlowNull rather

	map<UInt64, RTMFPFlow*>::iterator it = _flows.lower_bound(id);
	if (it != _flows.end() && it->first == id) {
		WARN("RTMFPFlow ", id, " has already been created on connection")
		return it->second;
	}

	// Get flash stream process engine related by signature
	Exception ex;
	RTMFPFlow* pFlow = createSpecialFlow(ex, id, signature, idWriterRef);
	if (!pFlow && signature.size()>3 && signature.compare(0, 4, "\x00\x54\x43\x04", 4) == 0) { // NetStream (P2P or normal)
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		DEBUG("Creating new Flow (", id, ") for NetStream ", idSession)

		// Search in mainstream
		if (_pMainStream->getStream(idSession, pStream))
			pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *_pConnection, idWriterRef);
		else
			ex.set(Exception::PROTOCOL, "RTMFPFlow ", id, " indicates a non-existent ", idSession, " NetStream on connection ", name());
	}
	if (!pFlow) {
		ERROR(ex.error())
		return NULL;
	}

	return _flows.emplace_hint(it, piecewise_construct, forward_as_tuple(id), forward_as_tuple(pFlow))->second;
}

void FlowManager::manage() {

	auto itFlow = _flows.begin();
	while (itFlow != _flows.end()) {
		if (itFlow->second->consumed())
			removeFlow((itFlow++)->second);
		else
			++itFlow;
	}
}

void FlowManager::removeFlow(RTMFPFlow* pFlow) {

	if (pFlow->id == _mainFlowId) {
		DEBUG("Main flow is closing, session ", name(), " will close")
		if (status != RTMFP::CONNECTED) {
			// without connection, nothing must be sent!
			_pConnection->clearWriters();
		}
		_mainFlowId = 0;
		close(false);
	}
	DEBUG("Session ", name(), " - RTMFPFlow ", pFlow->id, " consumed");
	_flows.erase(pFlow->id);
	delete pFlow;
}
