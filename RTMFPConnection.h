
#pragma once

#include "Mona/UDPSocket.h"
#include "RTMFPSender.h"
#include "AMFWriter.h"
#include "AMFReader.h"
#include "Mona/DiffieHellman.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "BandWriter.h"
#include "Publisher.h"

// Callback typedef definitions
typedef void(*OnSocketError)(const char*);
typedef void(*OnStatusEvent)(const char*, const char*);
typedef void(*OnMediaEvent)(unsigned int, const char*, unsigned int, int);

class Invoker;
class RTMFPConnection : public BandWriter {
public:
	RTMFPConnection(OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent, bool audioReliable=true, bool videoReliable=true);

	~RTMFPConnection();

	enum CommandType {
		NETSTREAM_PLAY = 1,
		NETSTREAM_PUBLISH
	};
	
	// Connect to the specified url, return true if the command succeed
	bool connect(Mona::Exception& ex, Invoker* invoker, const char* url, const char* host, const char* publication, bool isPublisher);

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

	bool	died; // connection is died

	/******* Internal functions for writers *******/

	void									flush() { flush(true); }

	virtual const Mona::PoolBuffers&		poolBuffers();

	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter=NULL);
	
	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->packet.size() : RTMFP_HEADER_SIZE); }
	
	virtual bool							canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	virtual bool							failed() const { return false; /* return _failed; */ }

private:
	
	// Close the connection properly
	void close();

	// External Callbacks to link with parent
	OnSocketError	_pOnSocketError;
	OnStatusEvent	_pOnStatusEvent;
	OnMediaEvent	_pOnMedia;

	// Handle message (after hanshake is done)
	void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer);

	// Send the first handshake message (with rtmfp url + tag)
	void sendHandshake0();

	// Send the second handshake message
	void sendHandshake1(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Send the third handshake message
	void sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Analyze packets received from the server
	void receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Initialize the packet in the RTMFPSender
	Mona::UInt8* packet();

	// Write only a type and send it directly
	void writeType(Mona::UInt8 type, Mona::UInt8 length, bool echoTime);

	// Send a message (must be connected)
	void sendMessage(Mona::UInt8 marker, Mona::UInt8 idResponse, AMFWriter& writer, bool echoTime);

	// Finalize and send the current packet
	void flush(bool echoTime, Mona::UInt8 marker=0x89);

	// Flush the connection
	void flush(Mona::UInt8 marker, Mona::UInt32 size,bool echoTime);

	RTMFPWriter*	writer(Mona::UInt64 id);
	RTMFPFlow*		createFlow(Mona::UInt64 id,const std::string& signature);

	// Dump
	void DumpResponse(const Mona::UInt8* data, Mona::UInt32 size);

	// Compute keys for encryption/decryption of the session (after handshake ok)
	// TODO: Create a Startable object for this
	bool computeKeys(Mona::Exception& ex, const std::string& farPubKey, const std::string& nonce);

	Mona::UInt8							_handshakeStep; // Handshake step (3 possible states)
	Mona::Time							_lastPing;

	Mona::UInt16						_timeReceived; // last time received
	Mona::UInt32						_farId;
	Mona::UInt64						_nextRTMFPWriterId;

	Mona::Time							_lastKeepAlive; // last time a keepalive request has been received

	// Connection parameters
	Mona::SocketAddress					_address; // host address
	std::string							_url; // RTMFP url of the application
	std::string							_publication; // Stream name
	bool								_isPublisher; // Publisher or Player?
	bool								_videoReliable; // buffered/unbuffered video mode
	bool								_audioReliable; // buffered/unbuffered audio mode

	// Pool of stream commands
	struct StreamCommand {
		StreamCommand(CommandType t, const char* v) : type(t), value(v) {}

		CommandType		type;
		std::string		value;
	};
	std::deque<StreamCommand> _waitingCommands;

	// Events
	FlashConnection::OnStatus::Type						onStatus; // NetConnection or NetStream status event
	FlashConnection::OnStreamCreated::Type				onStreamCreated; // Received when stream has been created and is waiting for a command
	FlashConnection::OnMedia::Type						onMedia; // Received when we receive media (audio/video)
	Mona::UDPSocket::OnError::Type						onError; // TODO: delete this if not needed
	Mona::UDPSocket::OnPacket::Type						onPacket; // Main input event, received on each raw packet

	std::unique_ptr<Publisher>								_pPublisher;
	std::shared_ptr<FlashConnection>						_pMainStream;
	std::map<Mona::UInt64,RTMFPFlow*>						_flows;
	std::map<Mona::UInt64,std::shared_ptr<RTMFPWriter> >	_flowWriters;
	std::map<Mona::UInt16,RTMFPFlow*>						_waitingFlows; // Map of id streams to new RTMFP flows (before knowing the 
	RTMFPWriter*											_pLastWriter;
	
	std::shared_ptr<Mona::UDPSocket>		_pSocket;
	std::shared_ptr<RTMFPSender>			_pSender;
	const Invoker*							_pInvoker;
	Mona::PoolThread*						_pThread;

	// Encryption/Decription
	std::shared_ptr<RTMFPEngine>			_pEncoder;
	std::shared_ptr<RTMFPEngine>			_pDecoder;
	Mona::DiffieHellman						_diffieHellman;
	Mona::Buffer							_sharedSecret;
	Mona::Buffer							_tag;
	Mona::Buffer							_pubKey;
	Mona::Buffer							_nonce;

	// Asynchronous read
	struct RTMFPMediaPacket {

		RTMFPMediaPacket(const Mona::PoolBuffers& poolBuffers,const Mona::UInt8* data,Mona::UInt32 size,Mona::UInt32 time,bool audio);

		Mona::Buffer	pBuffer;
	};
	std::deque<std::shared_ptr<RTMFPMediaPacket>>			_mediaPackets;
	std::recursive_mutex									_readMutex;
	bool													_firstRead;
	static const char										_FlvHeader[];

	bool													_firstWrite; // True if the input file as already been readed
};