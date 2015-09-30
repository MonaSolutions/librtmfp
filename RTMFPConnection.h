#include "Mona/UDPSocket.h"
#include "Invoker.h"
#include "RTMFPSender.h"


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

	////////////////// To implement on client side ////////////////////

	// Called when an error occurs on the socket
	virtual void onSocketError(const Mona::Exception& ex)=0;

	// Called when the socket is connected (before handshake)
	virtual void onSocketConnected()=0;

private:

	// Send the next handshake
	bool sendNextHandshake(Mona::Exception& ex, const Mona::UInt8* data=NULL, Mona::UInt32 size=0);

	// Send the first handshake message (with rtmfp url + tag)
	Mona::UInt8 sendHandshake0(Mona::BinaryWriter& writer);

	// Send the second handshake message
	Mona::UInt8 sendHandshake1(Mona::Exception& ex, Mona::BinaryWriter& writer, Mona::BinaryReader& reader);

	// Initialize the packet in the RTMFPSender
	Mona::UInt8* packet();

	// Flush the connection
	void flush(Mona::Exception& ex, Mona::UInt8 marker, Mona::UInt32 size);

	// Finalize and send the current packet
	void flush(Mona::Exception& ex,/*bool echoTime,*/Mona::UInt8 marker);

	Mona::UInt8							_step;
	Mona::Buffer						_tag;
	Mona::Buffer						_pubKey;
	Mona::Buffer						_nonce;

	std::string							_url;

	Mona::UDPSocket::OnError::Type		onError;
	Mona::UDPSocket::OnPacket::Type		onPacket;
	
	Mona::SocketAddress					_address;
	std::shared_ptr<Mona::UDPSocket>	_pSocket;
	std::shared_ptr<RTMFPSender>		_pSender;

	std::shared_ptr<RTMFPEngine>		_pEncoder;
	std::shared_ptr<RTMFPEngine>		_pDecoder;

	const Invoker*						_pInvoker;
	Mona::PoolThread*					_pThread;
};