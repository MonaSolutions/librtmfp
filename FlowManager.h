#pragma once

#include "Publisher.h"
#include "BandWriter.h"
#include "RTMFP.h"
#include "FlashConnection.h"
#include "Mona/Signal.h"

// Callback typedef definitions
typedef void(*OnStatusEvent)(const char*, const char*);
typedef void(*OnMediaEvent)(unsigned int, const char*, unsigned int, int);

class Invoker;
class RTMFPFlow;
/**************************************************
FlowManager is an abstract class used to manage 
lists of RTMFPFlow and RTMFPWriter
It is the base class of RTMFPConnection and P2PConnection
*/
class FlowManager : public BandWriter {
public:
	FlowManager(Invoker* invoker, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

	~FlowManager();

	enum CommandType {
		NETSTREAM_PLAY = 1,
		NETSTREAM_PUBLISH
	};

	// Send a command to the main stream (play/publish)
	// return : True if the request succeed, false otherwise
	// TODO: See if we should add a createStream function
	bool sendCommand(CommandType command, const char* streamName, bool audioReliable=false, bool videoReliable=false);

	// Asynchronous read (buffered)
	// return false if end of buf has been reached
	bool read(Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Write media (netstream must be published)
	// return false if the client is not ready to publish, otherwise true
	bool write(const Mona::UInt8* buf, Mona::UInt32 size, int& pos);

	// Called by Invoker every second to manage connection (flush and ping)
	//void manage();

	// Analyze packets received from the server (must be connected)
	void receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	/******* Internal functions for writers *******/
	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

	virtual const Mona::PoolBuffers&		poolBuffers();

	virtual bool							failed() const { return _died; }

	virtual bool							canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	std::shared_ptr<RTMFPEngine>							pEncoder;
	std::shared_ptr<RTMFPEngine>							pDecoder;

	Mona::Signal											connectSignal; // signal to wait connection

protected:

	// Close the connection properly
	virtual void close();

	RTMFPWriter*	writer(Mona::UInt64 id);
	RTMFPFlow*		createFlow(Mona::UInt64 id, const std::string& signature);

	Mona::Time							_lastPing;
	Mona::UInt64						_nextRTMFPWriterId;
	Mona::Time							_lastKeepAlive; // last time a keepalive request has been received

	bool								_died; // True if is the connection is died

	// Pool of stream commands
	struct StreamCommand {
		StreamCommand(CommandType t, const char* v) : type(t), value(v) {}

		CommandType		type;
		std::string		value;
	};
	std::deque<StreamCommand> _waitingCommands;

	// External Callbacks to link with parent
	OnStatusEvent	_pOnStatusEvent;
	OnMediaEvent	_pOnMedia;

	// Events
	FlashConnection::OnStatus::Type						onStatus; // NetConnection or NetStream status event
	FlashConnection::OnStreamCreated::Type				onStreamCreated; // Received when stream has been created and is waiting for a command
	FlashConnection::OnMedia::Type						onMedia; // Received when we receive media (audio/video)

	// Job Members
	std::shared_ptr<FlashConnection>						_pMainStream; // Main Stream (NetConnection or P2P Connection Handler)
	std::unique_ptr<Publisher>								_pPublisher;
	std::map<Mona::UInt64, RTMFPFlow*>						_flows;
	std::map<Mona::UInt64, std::shared_ptr<RTMFPWriter> >	_flowWriters;
	std::map<Mona::UInt16, RTMFPFlow*>						_waitingFlows; // Map of id streams to new RTMFP flows (before knowing the flow id)
	RTMFPWriter*											_pLastWriter; // Write pointer used to check if it is possible to write

	// Asynchronous read
	struct RTMFPMediaPacket {

		RTMFPMediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 time, bool audio);

		Mona::PoolBuffer	pBuffer;
	};
	std::deque<std::shared_ptr<RTMFPMediaPacket>>			_mediaPackets;
	std::recursive_mutex									_readMutex;
	bool													_firstRead;
	static const char										_FlvHeader[];

	bool													_firstWrite; // True if the input file has already been read

	Invoker*												_pInvoker;

	Mona::SocketAddress										_hostAddress; // host address

private:
	// Publishing members for future publisher
	bool													_audioReliable;
	bool													_videoReliable;
};