#pragma once

#include "Publisher.h"
#include "BandWriter.h"
#include "RTMFP.h"
#include "FlashConnection.h"
#include "Mona/Signal.h"
#include "Mona/DiffieHellman.h"
#include "Mona/UDPSocket.h"
#include "RTMFPSender.h"

// Callback typedef definitions
typedef void(*OnStatusEvent)(const char*, const char*);
typedef void(*OnMediaEvent)(unsigned int, const char*, unsigned int, int);
typedef void(*OnSocketError)(const char*);

class Invoker;
class RTMFPFlow;
/**************************************************
FlowManager is an abstract class used to manage 
lists of RTMFPFlow and RTMFPWriter
It is the base class of RTMFPConnection and P2PConnection
*/
class FlowManager : public BandWriter {
public:
	FlowManager(Invoker* invoker, OnSocketError pOnSocketError, OnStatusEvent pOnStatusEvent, OnMediaEvent pOnMediaEvent);

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

	bool						computeKeys(Mona::Exception& ex, const std::string& farPubKey, const std::string& initiatorNonce, const Mona::UInt8* responderNonce, Mona::UInt32 responderNonceSize, std::shared_ptr<RTMFPEngine>& pDecoder, std::shared_ptr<RTMFPEngine>& pEncoder);

	virtual Mona::UDPSocket&				socket() { return *_pSocket; }

	/******* Internal functions for writers *******/
	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter = NULL);

	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

	virtual const Mona::PoolBuffers&		poolBuffers();

	virtual bool							failed() const { return _died; }

	virtual bool							canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	virtual void							flush() { flush(true); }

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->packet.size() : RTMFP_HEADER_SIZE); }

	Mona::Signal							connectSignal; // signal to wait connection

protected:

	// Analyze packets received from the server (must be connected)
	void						receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Handle message (after hanshake0)
	virtual void				handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer, const Mona::SocketAddress& address);

	// Close the socket, set the connected and died flags and wait for destruction
	virtual void				close();

	RTMFPWriter*				writer(Mona::UInt64 id);
	RTMFPFlow*					createFlow(Mona::UInt64 id, const std::string& signature);

	// Initialize the packet in the RTMFPSender
	Mona::UInt8*				packet();

	// Clear the packet and flush the connection
	void						flush(Mona::UInt8 marker, Mona::UInt32 size, bool echoTime);

	// Flush the connection
	// marker values can be :
	// - 0B for handshake
	// - 09 for raw request
	// - 89 for AMF request
	virtual void				flush(bool echoTime, Mona::UInt8 marker = 0x89);

	// Manage handshake messages (marker 0x0B)
	virtual void				manageHandshake(Mona::Exception& ex, Mona::BinaryReader& reader) = 0;

	enum HandshakeType {
		BASE_HANDSHAKE = 0x0A,
		P2P_HANDSHAKE = 0x0F
	};

	// Send the first handshake message (with rtmfp url + tag)
	void sendHandshake0(HandshakeType type, const std::string& epd);

	virtual RTMFPEngine*	getDecoder(Mona::UInt32 idStream, const Mona::SocketAddress& address) { return (idStream == 0) ? _pDefaultDecoder.get() : _pDecoder.get(); }

	void flushWriters();

	Mona::UInt8							_handshakeStep; // Handshake step (3 possible states)
	Mona::UInt16						_timeReceived; // last time received
	Mona::UInt32						_farId; // Session id

	Mona::SocketAddress					_outAddress; // current address used for sending
	Mona::SocketAddress					_hostAddress; // host address
	Mona::Time							_lastPing;
	Mona::UInt64						_nextRTMFPWriterId;
	Mona::Time							_lastKeepAlive; // last time a keepalive request has been received

	bool								_died; // True if is the connection is died

	// Encryption/Decryption
	std::shared_ptr<RTMFPEngine>		_pEncoder;
	std::shared_ptr<RTMFPEngine>		_pDecoder;
	std::shared_ptr<RTMFPEngine>		_pDefaultDecoder; // used for id stream 0

	Mona::DiffieHellman					_diffieHellman;
	Mona::Buffer						_sharedSecret; 
	Mona::Buffer						_tag;

	// External Callbacks to link with parent
	OnStatusEvent						_pOnStatusEvent;
	OnMediaEvent						_pOnMedia;
	OnSocketError						_pOnSocketError;

	// Events
	FlashConnection::OnStatus::Type						onStatus; // NetConnection or NetStream status event
	FlashConnection::OnStreamCreated::Type				onStreamCreated; // Received when stream has been created and is waiting for a command
	FlashConnection::OnMedia::Type						onMedia; // Received when we receive media (audio/video)
	Mona::UDPSocket::OnError::Type						onError; // TODO: delete this if not needed
	Mona::UDPSocket::OnPacket::Type						onPacket; // Main input event, received on each raw packet

	// Job Members
	std::shared_ptr<FlashConnection>						_pMainStream; // Main Stream (NetConnection or P2P Connection Handler)
	std::unique_ptr<Publisher>								_pPublisher;
	std::map<Mona::UInt64, RTMFPFlow*>						_flows;
	std::map<Mona::UInt64, std::shared_ptr<RTMFPWriter> >	_flowWriters;
	std::map<Mona::UInt16, RTMFPFlow*>						_waitingFlows; // Map of id streams to new RTMFP flows (before knowing the flow id)
	RTMFPWriter*											_pLastWriter; // Write pointer used to check if it is possible to write
	Invoker*												_pInvoker;

	std::unique_ptr<Mona::UDPSocket>						_pSocket; // Sending socket established with server
	std::shared_ptr<RTMFPSender>							_pSender; // Current sender object
	Mona::PoolThread*										_pThread; // Thread used to send last message

	// Asynchronous read
	struct RTMFPMediaPacket {

		RTMFPMediaPacket(const Mona::PoolBuffers& poolBuffers, const Mona::UInt8* data, Mona::UInt32 size, Mona::UInt32 time, bool audio);

		Mona::PoolBuffer	pBuffer;
	};
	std::deque<std::shared_ptr<RTMFPMediaPacket>>			_mediaPackets;
	std::recursive_mutex									_readMutex;
	bool													_firstRead;
	static const char										_FlvHeader[];

	// Write members
	bool													_firstWrite; // True if the input file has already been read

private:
	// Pool of stream commands
	struct StreamCommand {
		StreamCommand(CommandType t, const char* v, bool aReliable, bool vReliable) : type(t), value(v), audioReliable(aReliable), videoReliable(vReliable) {}

		CommandType		type;
		std::string		value;
		bool			audioReliable;
		bool			videoReliable;
	};
	std::deque<StreamCommand>	_waitingCommands;
	std::recursive_mutex		_mutexCommands;
	Mona::UInt16				_nbCreateStreams; // Number of streams to create

	// If there is at least one request of command : create the stream
	void						createWaitingStreams();
};