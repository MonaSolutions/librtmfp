
#pragma once

#include "Mona/UDPSocket.h"
#include "RTMFPSender.h"
#include "AMFWriter.h"
#include "AMFReader.h"
#include "Mona/DiffieHellman.h"
#include "RTMFPWriter.h"
#include "RTMFPFlow.h"
#include "BandWriter.h"

class Invoker;
class RTMFPConnection : public BandWriter {
public:
	RTMFPConnection(void (*onSocketError)(const char*), void (*onStatusEvent)(const char*,const char*), void (*onMediaEvent)(unsigned int, const char*, unsigned int,int));

	~RTMFPConnection();
	
	bool connect(Mona::Exception& ex, Invoker* invoker, const char* host, int port, const char* url);

	void playStream(Mona::Exception& ex, const char* streamName);

	Mona::UInt32 read(Mona::UInt8* buf, Mona::UInt32 size);

	void close();

	// Called by Invoker every second to manage connection (flush and ping)
	void manage();

	/******* Internal functions for writers *******/

	void									flush() { flush(true); }

	virtual const Mona::PoolBuffers&		poolBuffers();

	virtual Mona::BinaryWriter&				writeMessage(Mona::UInt8 type, Mona::UInt16 length, RTMFPWriter* pWriter=NULL);
	
	virtual void							initWriter(const std::shared_ptr<RTMFPWriter>& pWriter);

	// Return the size available in the current sender (or max size if there is no current sender)
	virtual Mona::UInt32					availableToWrite() { return RTMFP_MAX_PACKET_SIZE - (_pSender ? _pSender->packet.size() : RTMFP_HEADER_SIZE); }
	
	virtual bool canWriteFollowing(RTMFPWriter& writer) { return _pLastWriter == &writer; }

	virtual bool				failed() const { return false; /* return _failed; */ }

private:

	// External Callbacks to link with parent
	void (* _onSocketError)(const char*);
	void (* _onStatusEvent)(const char*,const char*);
	void (* _onMedia)(unsigned int, const char*, unsigned int,int);

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
	//double							_idStreamPlayed; // Id of playing stream
	//Mona::UInt64						_writerId; // writer Id received in acknowledgment
	//Mona::UInt64						_stage;
	Mona::UInt64						_nextRTMFPWriterId;

	Mona::UInt64						_bytesReceived; // Number of bytes received
	Mona::Time							_lastKeepAlive; // last time a keepalive request has been received

	std::string							_url; // RTMFP url of the application
	std::string							_streamPlayed; // Stream name of the stream to be played

	FlashConnection::OnStatus::Type						onStatus;
	FlashConnection::OnStreamCreated::Type				onStreamCreated;
	FlashConnection::OnMedia::Type						onMedia;

	/*FlashStream::OnStop::Type						onStreamStop;*/

	Mona::UDPSocket::OnError::Type		onError;
	Mona::UDPSocket::OnPacket::Type		onPacket;

	std::shared_ptr<FlashConnection>						_pMainStream;
	std::map<Mona::UInt64,RTMFPFlow*>						_flows;
	std::map<Mona::UInt64,std::shared_ptr<RTMFPWriter> >	_flowWriters;
	std::map<Mona::UInt16,RTMFPFlow*>						_waitingFlows; // Map of id streams to new RTMFP flows (before knowing the 
	RTMFPWriter*											_pLastWriter;
	
	Mona::SocketAddress						_address;
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
};