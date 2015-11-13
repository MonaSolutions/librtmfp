
#pragma once

#include "Mona/Mona.h"
#include "FlashStream.h"

namespace FlashEvents {
	struct OnStreamCreated: Mona::Event<void(Mona::UInt16 idStream)> {};
};

/**************************************************************
FlashConnection is linked to an as3 NetConnection
It creates FlashStream (NetStream) and handle messages on the
connection
*/
class FlashConnection : public FlashStream, public virtual Mona::Object,
	public FlashEvents::OnStreamCreated {
public:
	FlashConnection(/*Invoker& invoker,Peer& peer*/);
	virtual ~FlashConnection();

	void	disengage(FlashWriter* pWriter = NULL);

	FlashStream* addStream(Mona::UInt16 id, std::shared_ptr<FlashStream>& pStream);

	FlashStream* getStream(Mona::UInt16 id, std::shared_ptr<FlashStream>& pStream);

	void flush() {for(auto& it : _streams) it.second->flush(); }

	void setPort(Mona::UInt16 port) { _port = port; }

	// Send the connect request to the RTMFP server
	virtual void connect(FlashWriter& writer, const std::string& url, Mona::UInt16 port);

	virtual void createStream(FlashWriter& writer);
	
private:
	void	messageHandler(const std::string& name, AMFReader& message, FlashWriter& writer);
	void	rawHandler(Mona::UInt16 type, Mona::PacketReader& packet, FlashWriter& writer);

	std::map<Mona::UInt16,std::shared_ptr<FlashStream>>	_streams;
	std::string											_buffer;
	
	Mona::UInt16	_port;
	bool			_creatingStream; // If we are waiting for a stream to be created
	std::string		_streamToPlay;
};
