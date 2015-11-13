#include "RTMFPConnection.h"
#include "Mona/PacketWriter.h"
//#include "Mona/Crypto.h"
#include "AMFReader.h"
#include "Invoker.h"
#include "RTMFPWriter.h"
#include "Mona/Logs.h"

using namespace Mona;
using namespace std;

RTMFPConnection::RTMFPMediaPacket::RTMFPMediaPacket(const PoolBuffers& poolBuffers, const UInt8* data,UInt32 size,UInt32 time, bool audio): pBuffer(poolBuffers,size+15) {
	BinaryWriter writer(pBuffer->data(), size+15);

	writer.write8(audio? '\x08' : '\x09');
	// size on 3 bytes
	writer.write24(size);
	// time on 3 bytes
	writer.write24(time);
	// unknown 4 bytes set to 0
	writer.write32(0);
	// payload
	writer.write(data, size);
	// footer
	writer.write32(11+size);

}

const char RTMFPConnection::_FlvHeader[] = { 'F', 'L', 'V', 0x01,
  0x05,				/* 0x04 == audio, 0x01 == video */
  0x00, 0x00, 0x00, 0x09,
  0x00, 0x00, 0x00, 0x00
};

RTMFPConnection::RTMFPConnection(OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, bool audioReliable, bool videoReliable):
		_nextRTMFPWriterId(0),_firstRead(true), _firstWrite(true), _audioReliable(audioReliable), _videoReliable(videoReliable),
		_pOnStatusEvent(pOnStatusEvent), _pOnMedia(pOnMediaEvent), RTMFPHandshake(pOnSocketError) {
	onStatus = [this](const string& code, const string& description, FlashWriter& writer) {
		if (code == "NetConnection.Connect.Success") {
			connected = true;
			if (_isPublisher)
				sendCommand(CommandType::NETSTREAM_PUBLISH, _publication.c_str());
			else
				sendCommand(CommandType::NETSTREAM_PLAY, _publication.c_str());
		}
		else if (code == "NetStream.Publish.Start") {
			_pPublisher.reset(new Publisher(poolBuffers(), *_pInvoker, writer, _audioReliable, _videoReliable));
		} else if (code == "NetStream.Play.UnpublishNotify" || code == "NetConnection.Connect.Closed" || code == "NetStream.Publish.BadName") {
			if (code != "NetConnection.Connect.Closed")
				close();
			_died = true;
			_pPublisher.reset();
		}

		_pOnStatusEvent(code.c_str(), description.c_str());
	};
	onStreamCreated = [this](UInt16 idStream) {
		shared_ptr<FlashStream> pStream;
		_pMainStream->addStream(idStream, pStream);

		// Stream created, now we create the flow before sending another request
		string signature;
		signature.append("\x00\x54\x43\x04", 4);
		RTMFP::Write7BitValue(signature, idStream);
		UInt64 id = _flows.size();
		RTMFPFlow * pFlow = new RTMFPFlow(id, signature, pStream, _pInvoker->poolBuffers, *this);
		_waitingFlows[idStream] = pFlow;
		
		// Send createStream command and add command type to waiting commands
		if (_waitingCommands.empty()) {
			ERROR("created stream without command")
			return;
		}
		const StreamCommand command = _waitingCommands.back();
		switch(command.type) {
			case NETSTREAM_PLAY:
				pFlow->sendPlay(command.value);
				break;
			case NETSTREAM_PUBLISH:
				pFlow->sendPublish(command.value);
				break;
		}		
		_waitingCommands.pop_back();
	};
	onMedia = [this](UInt32 time,PacketReader& packet,double lostRate,bool audio) {
		if (_pOnMedia) // Synchronous read
			_pOnMedia(time,(const char*)packet.current(),packet.available(), audio);
		else { // Asynchronous read
			lock_guard<recursive_mutex> lock(_readMutex);
			_mediaPackets.emplace_back(new RTMFPMediaPacket(poolBuffers(), packet.current(), packet.available(), time, audio));
		}
	};

	_pMainStream->OnStatus::subscribe(onStatus);
	_pMainStream->OnStreamCreated::subscribe(onStreamCreated);
	_pMainStream->OnMedia::subscribe(onMedia);
}

RTMFPConnection::~RTMFPConnection() {
	close();

	// Here no new sending must happen except "failSignal"
	for (auto& it : _flowWriters)
		it.second->clear();

	// delete waiting flows
	for(auto& it : _waitingFlows)
		delete it.second;
	_waitingFlows.clear();

	// delete flows
	for(auto& it : _flows)
		delete it.second;
	_flows.clear();

	// delete media packets
	lock_guard<recursive_mutex> lock(_readMutex);
	_mediaPackets.clear();
	
	// delete flowWriters
	_flowWriters.clear();

	if (_pMainStream) {
		_pMainStream->OnStatus::unsubscribe(onStatus);
		_pMainStream->OnStreamCreated::unsubscribe(onStreamCreated);
		_pMainStream->OnMedia::unsubscribe(onMedia);
	}
}

void RTMFPConnection::close() {

	if (connected) {
		if (_pPublisher)
			_pPublisher.reset();

		writeMessage(0x4C, 0); // Close message
		flush(false);
		connected = false;
	}
}

// TODO: see if we always need to manage a list of commands
void RTMFPConnection::sendCommand(CommandType command, const char* streamName) {
	if(!_pInvoker) {
		ERROR("Can't play stream because RTMFPConnection is not initialized");
		return;
	}
	if(!connected) {
		ERROR("Can't play stream because RTMFPConnection is not connected");
		return;
	}

	map<UInt64,RTMFPFlow*>::const_iterator it = _flows.find(2);
	RTMFPFlow* pFlow = it==_flows.end() ? NULL : it->second;
	if(pFlow != NULL) {
		_waitingCommands.emplace_front(command, streamName);
		pFlow->createStream();
	} else
		ERROR("Unable to find the main flow");
}

bool RTMFPConnection::read(UInt8* buf,UInt32 size, int& nbRead) {
	nbRead = 0;
	if (_died)
		return false; // to stop the parent loop

	lock_guard<recursive_mutex> lock(_readMutex);
	if (!_mediaPackets.empty()) {
		// First read => send header
		if (_firstRead && size > sizeof(_FlvHeader)) { // TODO: make a real context with a recorded position
			memcpy(buf, _FlvHeader, sizeof(_FlvHeader));
			_firstRead = false;
			size -= sizeof(_FlvHeader);
			nbRead += sizeof(_FlvHeader);
		}

		UInt32 bufferSize = 0;
		while (!_mediaPackets.empty() && (nbRead < size)) {

			std::shared_ptr<RTMFPMediaPacket> packet = _mediaPackets.front();
			bufferSize = packet->pBuffer.size();
			if (bufferSize > (size - nbRead))
				return false;

			memcpy(buf + nbRead, packet->pBuffer.data(), bufferSize);
			_mediaPackets.pop_front();
			nbRead += bufferSize;
		}
	}

	return true;
}

bool RTMFPConnection::write(const UInt8* buf, UInt32 size, int& pos) {
	pos = 0;
	if (_died) {
		pos = -1;
		return false; // to stop the parent loop
	}

	if(!_pPublisher) {
		DEBUG("Can't write data because NetStream is not published")
		return true;
	}

	return _pPublisher->publish(buf, size, pos);
}

bool RTMFPConnection::sendConnect(Exception& ex, BinaryReader& reader) {

	// Analyze the response before connecting
	if (!RTMFPHandshake::sendConnect(ex, reader))
		return false;

	string signature("\x00\x54\x43\x04\x00", 5);
	RTMFPFlow* pFlow = createFlow(2, signature);
	if(!pFlow)
		return false;
	
	pFlow->sendConnect(_url, _pSocket->address().port());
	return true;
}

void RTMFPConnection::receive(Exception& ex, BinaryReader& reader) {

	// Variables for request (0x10 and 0x11)
	UInt8 flags;
	RTMFPFlow* pFlow=NULL;
	UInt64 stage=0;
	UInt64 deltaNAck=0;

	UInt8 type = reader.available()>0 ? reader.read8() : 0xFF;
	bool answer = false;

	// Can have nested queries
	while(type!=0xFF) {

		UInt16 size = reader.read16();	

		PacketReader message(reader.current(),size);

		switch(type) {
			case 0x0c :
				ex.set(Exception::PROTOCOL, "Failed on server side");
				writeMessage(0x0C, 0);
				break;

			/*case 0x4c :
				/// Session death!
				_failed = true; // to avoid the fail signal!!
				kill();
				return;*/

			/// KeepAlive
			case 0x01:
				/*if(!peer.connected)
					fail("Timeout connection client");
				else*/
				writeMessage(0x41,0);
				break;
			case 0x41 :
				_lastKeepAlive.update();
				break;

			/*case 0x5e : {
				// RTMFPFlow exception!
				UInt64 id = message.read7BitLongValue();
				
				RTMFPWriter* pRTMFPWriter = writer(id);
				if(pRTMFPWriter)
					pRTMFPWriter->fail("Writer rejected on session ",name());
				else
					WARN("RTMFPWriter ", id, " unfound for failed signal on session ", name());
				break;

			}
			case 0x18 :
				/// This response is sent when we answer with a Acknowledgment negative
				// It contains the id flow
				// I don't unsertand the usefulness...
				//pFlow = &flow(message.read8());
				//stage = pFlow->stageSnd();
				// For the moment, we considerate it like a exception
				fail("ack negative from server"); // send fail message immediatly
				break;*/

			case 0x51 : {
				/// Acknowledgment
				UInt64 id = message.read7BitLongValue();
				RTMFPWriter* pRTMFPWriter = writer(id);
				if(pRTMFPWriter)
					pRTMFPWriter->acknowledgment(message);
				else
					WARN("RTMFPWriter ",id," unfound for acknowledgment on connection ");
				break;
			}
			/// Request
			// 0x10 normal request
			// 0x11 special request, in repeat case (following stage request)
			case 0x10 : {
				flags = message.read8();
				UInt64 idFlow = message.read7BitLongValue();
				stage = message.read7BitLongValue()-1;
				deltaNAck = message.read7BitLongValue()-1;
				
				/*if (_failed)
					break;*/

				map<UInt64,RTMFPFlow*>::const_iterator it = _flows.find(idFlow);
				pFlow = it==_flows.end() ? NULL : it->second;

				// Header part if present
				if(flags & MESSAGE_HEADER) {
					string signature;
					message.read(message.read8(),signature);

					if(!pFlow)
						pFlow = createFlow(idFlow, signature);

					if(message.read8()>0) {

						// Fullduplex header part
						if(message.read8()!=0x0A)
							WARN("Unknown fullduplex header part for the flow ",idFlow)
						else 
							message.read7BitLongValue(); // Fullduplex useless here! Because we are creating a new RTMFPFlow!

						// Useless header part 
						UInt8 length=message.read8();
						while(length>0 && message.available()) {
							WARN("Unknown message part on flow ",idFlow);
							message.next(length);
							length=message.read8();
						}
						if(length>0) {
							ex.set(Exception::PROTOCOL, "Bad header message part, finished before scheduled");
							return;
						}
					}
				}
				
				if(!pFlow) {
					WARN("RTMFPFlow ",idFlow," unfound");
					/*if (_pFlowNull)
						((UInt64&)_pFlowNull->id) = idFlow;
					pFlow = _pFlowNull;*/
				}

			}	
			case 0x11 : {
				++stage;
				++deltaNAck;

				// has Header?
				if(type==0x11)
					flags = message.read8();

				// Process request
				if (pFlow && !_died)
					pFlow->receive(stage, deltaNAck, message, flags);

				break;
			}
			default :
				ex.set(Exception::PROTOCOL,"RTMFPMessage type '", Format<UInt8>("%02x", type), "' unknown");
				return;
		}

		// Next
		reader.next(size); // TODO: maybe PacketReader was needed above to not move the cursor of "reader"
		type = reader.available()>0 ? reader.read8() : 0xFF;

		// Commit RTMFPFlow (pFlow means 0x11 or 0x10 message)
		if(pFlow && type!= 0x11) {
			pFlow->commit();
			if(pFlow->consumed()) {
				if (pFlow->critical()) {
					if (!connected) {
						// without connection, nothing must be sent!
						for (auto& it : _flowWriters)
							it.second->clear();
					}
					// TODO: commented because it replace other events (NetConnection.Connect.Rejected)
					// fail(); // If connection fails, log is already displayed, and so fail the whole session!
				}
				_flows.erase(pFlow->id);
				delete pFlow;
			}
			pFlow=NULL;
		}
	}
}

BinaryWriter& RTMFPConnection::writeMessage(UInt8 type, UInt16 length, RTMFPWriter* pWriter) {

	// No sending formated message for a failed session!
	/*if (_failed)
		return DataWriter::Null.packet;*/

	_pLastWriter=pWriter;

	UInt16 size = length + 3; // for type and size

	if(size>availableToWrite()) {
		flush(false); // send packet (and without time echo)
		
		if(size > availableToWrite()) {
			ERROR("RTMFPMessage truncated because exceeds maximum UDP packet size on connection");
			size = availableToWrite();
		}
		_pLastWriter=NULL;
	}

	if (!_pSender)
		_pSender.reset(new RTMFPSender(_pInvoker->poolBuffers, _pEncoder));
	return _pSender->packet.write8(type).write16(length);
}

RTMFPWriter* RTMFPConnection::writer(UInt64 id) {
	auto it = _flowWriters.find(id);
	if(it==_flowWriters.end())
		return NULL;
	return it->second.get();
}

RTMFPFlow* RTMFPConnection::createFlow(UInt64 id,const string& signature) {
	if(_died) {
		ERROR("Connection is died, no more RTMFPFlow creation possible");
		return NULL;
	}
	if (!_pMainStream)
		return NULL; // has failed! use FlowNull rather

	map<UInt64,RTMFPFlow*>::iterator it = _flows.lower_bound(id);
	if(it!=_flows.end() && it->first==id) {
		WARN("RTMFPFlow ",id," has already been created on connection")
		return it->second;
	}

	RTMFPFlow* pFlow;

	// get flash stream process engine related by signature
	if (signature.size() > 4 && signature.compare(0, 5, "\x00\x54\x43\x04\x00", 5) == 0) { // NetConnection
		INFO("Creating new Flow (",id,") for NetConnection")
		pFlow = new RTMFPFlow(id, signature, _pInvoker->poolBuffers, *this, _pMainStream);
	} else if (signature.size()>3 && signature.compare(0, 4, "\x00\x54\x43\x04", 4) == 0) { // NetStream
		shared_ptr<FlashStream> pStream;
		UInt32 idSession(BinaryReader((const UInt8*)signature.c_str() + 4, signature.length() - 4).read7BitValue());
		INFO("Creating new Flow (",id,") for NetStream ", idSession)

		// First : search in waiting flows
		auto it = _waitingFlows.find(idSession);
		if(it!=_waitingFlows.end()) {
			pFlow = it->second;
			pFlow->setId(id);
			_waitingFlows.erase(it);
		}
		// 2nd : search in mainstream
		else if (_pMainStream->getStream(idSession,pStream))
			pFlow = new RTMFPFlow(id, signature, pStream, poolBuffers(), *this);
		else {
			ERROR("RTMFPFlow ",id," indicates a non-existent ",idSession," NetStream on connection ");
			return NULL;
		}
		
	} 
	else {
		ERROR("Unhandled signature type : ", signature, " , cannot create RTMFPFlow")
		return NULL;
	}
		/*else if(signature.size()>2 && signature.compare(0,3,"\x00\x47\x43",3)==0)  // NetGroup
		pFlow = new RTMFPFlow(id, signature, _pInvoker->poolBuffers, *this, _pMainStream);*/

	return _flows.emplace_hint(it, piecewise_construct, forward_as_tuple(id), forward_as_tuple(pFlow))->second;
}

void RTMFPConnection::initWriter(const shared_ptr<RTMFPWriter>& pWriter) {
	while (++_nextRTMFPWriterId == 0 || !_flowWriters.emplace(_nextRTMFPWriterId, pWriter).second);
	(UInt64&)pWriter->id = _nextRTMFPWriterId;
	if (!_flows.empty())
		(UInt64&)pWriter->flowId = _flows.begin()->second->id; // newWriter will be associated to the NetConnection flow (first in _flow lists)
	if (!pWriter->signature.empty())
		DEBUG("New writer ", pWriter->id, " on connection ");
}

void RTMFPConnection::manage() {
	if (!_pMainStream)
		return;

	// Every 25s : ping
	if (_lastPing.isElapsed(25000) && connected) {
		writeMessage(0x01, 0); // TODO: send only if needed
		flush(false);
		_lastPing.update();
	}

	// Raise RTMFPWriter
	auto it=_flowWriters.begin();
	while (it != _flowWriters.end()) {
		shared_ptr<RTMFPWriter>& pWriter(it->second);
		Exception ex;
		pWriter->manage(ex);
		if (ex) {
			if (pWriter->critical) {
				// TODO: fail(ex.error());
				break;
			}
			continue;
		}
		if (pWriter->consumed()) {
			_flowWriters.erase(it++);
			continue;
		}
		++it;
	}
}
