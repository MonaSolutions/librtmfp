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

#include "Connection.h"
#include "RTMFPSender.h"
#include "SocketHandler.h"
//#include "FlowManager.h"

using namespace Mona;
using namespace std;

Connection::Connection(SocketHandler* pHandler) : _pParent(pHandler), _status(RTMFP::STOPPED), _farId(0), _pThread(NULL), _nextRTMFPWriterId(1), _ping(0), _timeReceived(0),
 _pEncoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::ENCRYPT)),
 _pDecoder(new RTMFPEngine((const Mona::UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)),
 _pDefaultDecoder(new RTMFPEngine((const UInt8*)RTMFP_DEFAULT_KEY, RTMFPEngine::DECRYPT)) {

}

Connection::~Connection() {
	close();

	_flowWriters.clear();
	_pParent = NULL;
}

void Connection::close(bool abrupt) {

	if (_status == RTMFP::CONNECTED) {
		writeMessage(abrupt? 0x4C : 0x0C, 0); // Close message
		flush(false, 0x89);
	}

	// Here no new sending must happen except "failSignal"
	for (auto& it : _flowWriters) {
		OnWriterClose::raise(it.second);
		it.second->clear();
		if (!abrupt)
			it.second->close();
	}

	_status = abrupt? RTMFP::FAILED : RTMFP::NEAR_CLOSED;
	_closeTime.update();
}

void Connection::process(PoolBuffer& pBuffer) {
	if (_status >= RTMFP::NEAR_CLOSED)
		return;

	// Decode the RTMFP data
	if (pBuffer->size() < RTMFP_MIN_PACKET_SIZE) {
		ERROR("Invalid RTMFP packet on connection to ", _address.toString())
		return;
	}

	BinaryReader reader(pBuffer.data(), pBuffer.size());
	UInt32 idStream = RTMFP::Unpack(reader);
	pBuffer->clip(reader.position());

	// Handshake or session decoder?
	RTMFPEngine* pDecoder = (idStream == 0) ? _pDefaultDecoder.get() : _pDecoder.get();

#if defined(_DEBUG)
	Buffer copy(pBuffer.size());
	memcpy(copy.data(), pBuffer.data(), pBuffer.size());
#endif
	if (!pDecoder->process(BIN pBuffer.data(), pBuffer.size())) {
		WARN("Bad RTMFP CRC sum computing (idstream: ", idStream, ", address : ", _address.toString(), ")")
#if defined(_DEBUG)
		DUMP("RTMFP", copy.data(), copy.size(), "Raw request : ")
#endif
		return;
	}
	else
		handleMessage(pBuffer);
}

const PoolBuffers& Connection::poolBuffers() {
	return _pParent->poolBuffers();
}

void Connection::setPing(UInt16 time, UInt16 timeEcho) {
	if (timeEcho > time) {
		if (timeEcho - time < 30)
			time = 0;
		else
			time += 0xFFFF - timeEcho;
		timeEcho = 0;
	}
	UInt16 value = (time - timeEcho) * RTMFP_TIMESTAMP_SCALE;
	_ping = (value == 0 ? 1 : value);
}

BinaryWriter& Connection::writeMessage(UInt8 type, UInt16 length, RTMFPWriter* pWriter) {

	_pLastWriter = pWriter;

	UInt16 size = length + 3; // for type and size

	if (size>availableToWrite()) {
		flush(false, 0x89); // send packet (and without time echo)

		if (size > availableToWrite()) {
			ERROR("RTMFPMessage truncated because exceeds maximum UDP packet size on connection");
			size = availableToWrite();
		}
		_pLastWriter = NULL;
	}

	if (!_pSender)
		_pSender.reset(new RTMFPSender(poolBuffers(), _pEncoder));
	return _pSender->packet.write8(type).write16(length);
}

UInt8* Connection::packet() {
	if (!_pSender)
		_pSender.reset(new RTMFPSender(poolBuffers(), _pEncoder));
	_pSender->packet.resize(RTMFP_MAX_PACKET_SIZE, false);
	return _pSender->packet.data();
}

void Connection::handleWriterFailed(UInt64 id) {

	shared_ptr<RTMFPWriter> pWriter;
	if (writer(id, pWriter))
		OnWriterFailed::raise(pWriter);
	else
		WARN("RTMFPWriter ", id, " unfound for failed signal on connection ", name());
	
}

void Connection::handleAcknowledgment(UInt64 id, Mona::PacketReader& message) {

	shared_ptr<RTMFPWriter> pWriter;
	if (writer(id, pWriter)) {
		Exception ex;
		if (!pWriter->acknowledgment(ex, message))
			WARN(ex.error(), " on connection ", name())
	}
	else
		WARN("RTMFPWriter ", id, " unfound for acknowledgment on connection ", name())
}

void Connection::flush(UInt8 marker, UInt32 size) {
	if (!_pSender)
		return;
	_pSender->packet.clear(size);
	flush(false, marker);
}

void Connection::flush(bool echoTime, UInt8 marker) {
	_pLastWriter = NULL;
	if (!_pSender)
		return;
	if (_status < RTMFP::NEAR_CLOSED && _pSender->available()) {
		BinaryWriter& packet(_pSender->packet);

		// After 30 sec, send packet without echo time
		if (_lastReceptionTime.isElapsed(30000))
			echoTime = false;

		if (echoTime)
			marker += 4;
		else
			packet.clip(2);

		BinaryWriter writer(packet.data() + 6, 5);
		writer.write8(marker).write16(RTMFP::TimeNow());
		if (echoTime)
			writer.write16(_timeReceived + RTMFP::Time(_lastReceptionTime.elapsed()));

		_pSender->farId = _farId;
		_pSender->address.set(_address); // set the right address for sending

		if (packet.size() > RTMFP_MAX_PACKET_SIZE)
			ERROR(Exception::PROTOCOL, "Message exceeds max RTMFP packet size on connection (", packet.size(), ">", RTMFP_MAX_PACKET_SIZE, ")");

		// executed just in debug mode, or in dump mode
		if (Logs::GetLevel() >= 7)
			DUMP("RTMFP", packet.data() + 6, packet.size() - 6, "Response to ", _address.toString(), " (farId : ", _farId, ")")

		Exception ex;
		_pThread = _pParent->socket().send<RTMFPSender>(ex, _pSender, _pThread);

		if (ex)
			ERROR("RTMFP flush, ", ex.error());
	}
	_pSender.reset();
}

shared_ptr<RTMFPWriter>& Connection::writer(UInt64 id, shared_ptr<RTMFPWriter>& pWriter) {
	auto it = _flowWriters.find(id);
	if (it != _flowWriters.end())
		pWriter = it->second;
	return pWriter;
}

void Connection::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	while (++_nextRTMFPWriterId == 0 || !_flowWriters.emplace(_nextRTMFPWriterId, pWriter).second);
	(UInt64&)pWriter->id = _nextRTMFPWriterId;
	pWriter->amf0 = false;

	if (!pWriter->signature.empty())
		DEBUG("New writer ", pWriter->id, " on connection ", name());
	OnNewWriter::raise(_flowWriters.find(pWriter->id)->second);
}

void Connection::flushWriters() {
	// Every 25s : ping
	if (_lastPing.isElapsed(25000) && connected()) {
		writeMessage(0x01, 0);
		flush(false, 0x89);
		_lastPing.update();
	}

	// Raise RTMFPWriter
	auto it = _flowWriters.begin();
	while (it != _flowWriters.end()) {
		shared_ptr<RTMFPWriter>& pWriter(it->second);
		Exception ex;
		pWriter->manage(ex);
		if (ex) {
			/* TODO: if (pWriter->critical) {
				fail(ex.error());
				break;
			}*/
			continue;
		}
		if (pWriter->consumed()) {
			OnWriterClose::raise(pWriter);
			DEBUG("Connection ", name(), " - RTMFPWriter ", pWriter->id, " consumed");
			_flowWriters.erase(it++);
			continue;
		}
		++it;
	}
}

shared_ptr<RTMFPWriter> Connection::changeWriter(RTMFPWriter& writer) {
	auto it = _flowWriters.find(writer.id);
	if (it == _flowWriters.end()) {
		ERROR("RTMFPWriter ", writer.id, " change impossible on connection")
		return shared_ptr<RTMFPWriter>(&writer);
	}
	shared_ptr<RTMFPWriter> pWriter(it->second);
	it->second.reset(&writer);
	return pWriter;
}

void Connection::clearWriters() {

	for (auto& it : _flowWriters)
		it.second->clear();
}

void Connection::manage() {

	// Flush writers
	flushWriters();
}

void Connection::sendHandshake30(const string& epd, const string& tag) {
	// (First packets are encoded with default key)
	BinaryWriter writer(packet(), RTMFP_MAX_PACKET_SIZE);
	writer.clear(RTMFP_HEADER_SIZE + 3); // header + type and size

	writer.write7BitLongValue(epd.size());
	writer.write(epd.data(), epd.size());

	writer.write(tag);

	BinaryWriter(writer.data() + RTMFP_HEADER_SIZE, 3).write8(0x30).write16(writer.size() - RTMFP_HEADER_SIZE - 3);
	flush(0x0B, writer.size());
	_status = RTMFP::HANDSHAKE30;
}

void Connection::handleP2pAddresses(BinaryReader& reader) {
	DEBUG("Server has sent to us the peer addresses of responders") // (here we are the initiator)

	// Read & check handshake0's response
	UInt8 tagSize = reader.read8();
	if (tagSize != 16) {
		ERROR("Unexpected tag size : ", tagSize)
		return;
	}
	
	string tagReceived;
	reader.read(16, tagReceived);

	// Send handshake 30 to peer addresses found
	SocketAddress hostAddress;
	PEER_LIST_ADDRESS_TYPE addresses;
	if (RTMFP::ReadAddresses(reader, addresses, hostAddress))
		_pParent->onP2PAddresses(tagReceived, addresses, hostAddress);
}
