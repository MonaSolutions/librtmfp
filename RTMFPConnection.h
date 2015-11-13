
#pragma once

#include "RTMFPHandshake.h"
#include "Publisher.h"

// Callback typedef definitions
typedef void(*OnStatusEvent)(const char*, const char*);
typedef void(*OnMediaEvent)(unsigned int, const char*, unsigned int, int);

class Invoker;
/**************************************************
RTMFPConnection represents a connection to the
RTMFP Server
*/
class RTMFPConnection : public RTMFPHandshake {
public:
	RTMFPConnection(OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, bool audioReliable=true, bool videoReliable=true);

	~RTMFPConnection();

	enum CommandType {
		NETSTREAM_PLAY = 1,
		NETSTREAM_PUBLISH
	};

	// Send a command to the main stream (play/publish)
	// TODO: See if we should add a createStream function
	void sendCommand(CommandType command, const char* streamName);

	// Asynchronous read (buffered)
	// return false if end of buf has been reached
	bool read(Mona::UInt8* buf, Mona::UInt32 size, int& nbRead);

	// Write media (netstream must be published)
	// return false if the client is not ready to publish, otherwise true
	bool write(const Mona::UInt8* buf, Mona::UInt32 size, int& pos);

	// Called by Invoker every second to manage connection (flush and ping)
	void manage();

	/******* Internal functions for writers *******/
	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter=NULL);
	
	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

private:
	
	// Close the connection properly
	void close();

	// Send the connection message (after the answer of handshake1)
	virtual bool sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Analyze packets received from the server (must be connected)
	virtual void receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	RTMFPWriter*	writer(Mona::UInt64 id);
	RTMFPFlow*		createFlow(Mona::UInt64 id,const std::string& signature);

	Mona::Time							_lastPing;
	Mona::UInt64						_nextRTMFPWriterId;
	Mona::Time							_lastKeepAlive; // last time a keepalive request has been received

	// Connection parameters
	bool								_videoReliable; // buffered/unbuffered video mode
	bool								_audioReliable; // buffered/unbuffered audio mode

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
	std::unique_ptr<Publisher>								_pPublisher;
	std::map<Mona::UInt64,RTMFPFlow*>						_flows;
	std::map<Mona::UInt64,std::shared_ptr<RTMFPWriter> >	_flowWriters;
	std::map<Mona::UInt16,RTMFPFlow*>						_waitingFlows; // Map of id streams to new RTMFP flows (before knowing the 

	// Asynchronous read
	struct RTMFPMediaPacket {

		RTMFPMediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data,Mona::UInt32 size,Mona::UInt32 time,bool audio);

		Mona::PoolBuffer	pBuffer;
	};
	std::deque<std::shared_ptr<RTMFPMediaPacket>>			_mediaPackets;
	std::recursive_mutex									_readMutex;
	bool													_firstRead;
	static const char										_FlvHeader[];

	bool													_firstWrite; // True if the input file has already been readed
};