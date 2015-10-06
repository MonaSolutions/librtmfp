#include "Mona/UDPSocket.h"
#include "Invoker.h"
#include "RTMFPSender.h"
#include "AMFWriter.h"
#include "AMFReader.h"
#include "Mona/DiffieHellman.h"

#define MESSAGE_HEADER			0x80

class RTMFPConnection {
public:
	RTMFPConnection();

	~RTMFPConnection() {

		if(_pSocket) {
			_pSocket->OnPacket::unsubscribe(onPacket);
			_pSocket->OnError::unsubscribe(onError);
		}
	}
	
	bool connect(Mona::Exception& ex, Invoker* invoker, const char* host, int port, const char* url);

	void close();

protected:

	////////////////// To implement on client side ////////////////////

	// Called when an error occurs on the socket
	virtual void onSocketError(const Mona::Exception& ex)=0;

	// Called when the socket is connected (before handshake)
	virtual void onSocketConnected()=0;

	virtual void onStatusEvent(const char* code, const char* description)=0;

private:

	// Send the next handshake
	void sendNextHandshake(Mona::Exception& ex, const Mona::UInt8* data=NULL, Mona::UInt32 size=0);

	// Handle message (after hanshake is done)
	void handleMessage(Mona::Exception& ex, const Mona::PoolBuffer& pBuffer);

	// Send the first handshake message (with rtmfp url + tag)
	Mona::UInt8 sendHandshake0(Mona::BinaryWriter& writer);

	// Send the second handshake message
	Mona::UInt8 sendHandshake1(Mona::Exception& ex, Mona::BinaryWriter& writer, Mona::BinaryReader& reader);

	// Send the third handshake message
	void sendConnect(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Analyze packets received from the server
	void receive(Mona::Exception& ex, Mona::BinaryReader& reader);

	// Treat audio received
	void audioHandler(Mona::UInt32 time, Mona::PacketReader& message);

	// Treat video received 
	void videoHandler(Mona::UInt32 time, Mona::PacketReader& message);

	// Treat message received in an invocation packet
	void invocationHandler(Mona::Exception& ex, const std::string& name, AMFReader& message);

	// Initialize the packet in the RTMFPSender
	Mona::UInt8* packet();

	// Send a message (must be connected)
	void sendMessage(Mona::Exception& ex, Mona::UInt8 marker, Mona::UInt8 idResponse, AMFWriter& writer, bool echotime);

	// Flush the connection
	void flush(Mona::Exception& ex, Mona::UInt8 marker, Mona::UInt32 size,bool echoTime);

	// Finalize and send the current packet
	void flush(Mona::Exception& ex,bool echoTime,Mona::UInt8 marker);

	// Compute keys for encryption/decryption of the session (after handshake ok)
	bool computeKeys(Mona::Exception& ex, const std::string& farPubKey, const std::string& nonce);

	Mona::UInt8							_step;
	Mona::Buffer						_tag;
	Mona::Buffer						_pubKey;
	Mona::Buffer						_nonce;

	Mona::UInt16						_timeReceived;
	Mona::UInt32						_farId;

	Mona::UInt64						_writerId; // writer Id received in acknowledgment
	Mona::UInt64						_bytesReceived;

	std::string							_url;

	Mona::UDPSocket::OnError::Type		onError;
	Mona::UDPSocket::OnPacket::Type		onPacket;
	
	Mona::SocketAddress					_address;
	std::shared_ptr<Mona::UDPSocket>	_pSocket;
	std::shared_ptr<RTMFPSender>		_pSender;

	// Encryption/Decription
	std::shared_ptr<RTMFPEngine>		_pEncoder;
	std::shared_ptr<RTMFPEngine>		_pDecoder;
	Mona::DiffieHellman					_diffieHellman;
	Mona::Buffer						_sharedSecret;

	const Invoker*						_pInvoker;
	Mona::PoolThread*					_pThread;
};