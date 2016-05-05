
#pragma once

#include "Mona/Mona.h"
#include "FlashConnection.h"
#include "Mona/PoolBuffers.h"
#include "RTMFPWriter.h"
#include "BandWriter.h"

class RTMFPPacket;
class RTMFPFragment;
/**************************************************************
RTMFPFlow is the receiving class for one NetStream of a 
connection, it is associated to an RTMFPWriter for
sending RTMFP answers
It manages acknowledgments and lost count of messages received
*/
class RTMFPFlow : public virtual Mona::Object {
public:
	RTMFPFlow(Mona::UInt64 id,const std::string& signature,const Mona::PoolBuffers& poolBuffers, BandWriter& band, const std::shared_ptr<FlashConnection>& pMainStream);
	RTMFPFlow(Mona::UInt64 id,const std::string& signature,const std::shared_ptr<FlashStream>& pStream, const Mona::PoolBuffers& poolBuffers, BandWriter& band);
	virtual ~RTMFPFlow();

	const Mona::UInt64		id;

	// Update the Id of session (called when a new NetStream has been created)
	void	setId(Mona::UInt64 idFlow);

	bool	critical() const { return _pWriter->critical; }

	// Handle fragments received
	void	receive(Mona::UInt64 stage,Mona::UInt64 deltaNAck,Mona::PacketReader& fragment,Mona::UInt8 flags);
	
	// Send acknowledgment
	void	commit();

	void	fail(const std::string& error);

	bool	consumed() { return _completed; }

	// Record the far peer Id to identify the media source
	void	setPeerId(const std::string& peerId) { if (_pStream) _pStream->setPeerId(peerId); }

	// RTMFP Commands
	void	sendConnect(const std::string& url) { if (_pStream) _pStream->connect(*_pWriter, url); }
	void	sendPlay(const std::string& name) { if (_pStream) _pStream->play(*_pWriter, name); }
	void	sendPublish(const std::string& name) { if (_pStream) _pStream->publish(*_pWriter, name); }
	void	createStream() { if (_pStream) _pStream->createStream(*_pWriter); }
	void	sendPeerInfo(Mona::UInt16 port) { if (_pStream) _pStream->sendPeerInfo(*_pWriter, port); }
	void	sendGroupConnect(const std::string& netGroup) { if (_pStream) _pStream->sendGroupConnect(*_pWriter, netGroup); }
	void	sendGroupPeerConnect(const std::string& netGroup, const Mona::UInt8* key, const std::string& peerId/*, bool initiator*/) { if (_pStream) _pStream->sendGroupPeerConnect(*_pWriter, netGroup, key, peerId/*, initiator*/); }
	void	sendGroupBegin() { if (_pStream) _pStream->sendGroupBegin(*_pWriter); }
	void	sendGroupMediaInfos(const std::string& stream, const Mona::UInt8* data, Mona::UInt32 size) { if (_pStream) _pStream->sendGroupMediaInfos(*_pWriter, stream, data, size); }
	void	sendGroupReport(const std::string& peerId) { if (_pStream) _pStream->sendGroupReport(*_pWriter, peerId); }
	void	sendRaw(const Mona::UInt8* data, Mona::UInt32 size) { if (_pStream) _pStream->sendRaw(*_pWriter, data, size); }
	void	sendGroupPlay(Mona::UInt8 mode) { if (_pStream) _pStream->sendGroupPlay(*_pWriter, mode); }
	//void	closeGroupStream(Mona::UInt8 type, Mona::UInt64 fragmentCounter, Mona::UInt32 lastTime) { if (_pStream) _pStream->sendGroupCloseStream(type, fragmentCounter, lastTime); }

private:
	// Handle on fragment received
	void	onFragment(Mona::UInt64 stage,Mona::PacketReader& fragment,Mona::UInt8 flags);

	void	complete();

	bool							_completed; // Indicates that the flow is consumed
	BandWriter&						_band; // RTMFP connection to send messages
	std::shared_ptr<RTMFPWriter>	_pWriter; // Writer for sending AMF messages on RTMFP
	const Mona::UInt64				_stage; // Current stage (index) of messages received
	std::shared_ptr<FlashStream>	_pStream; // NetStream handler of the flow

	// Receiving
	RTMFPPacket*					_pPacket; // current packet/message containing 1 or more fragments (if chunked)
	std::map<Mona::UInt64,RTMFPFragment>	_fragments; // map of all fragments received and not handled for now
	Mona::UInt32					_numberLostFragments;
	const Mona::PoolBuffers&		_poolBuffers;
};

