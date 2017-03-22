/*
Copyright 2016 Thomas Jammet
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This file is part of Librtmfp.

Librtmfp is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Librtmfp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Librtmfp.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "Mona/SocketAddress.h"
#include "Mona/BinaryReader.h"
#include "Mona/BinaryWriter.h"
#include "Mona/Time.h"
#include "Mona/Crypto.h"
#include "Mona/Util.h"
#include "Mona/Packet.h"
#include "Mona/Socket.h"
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "Mona/Logs.h"

#include <map>

#define RTMFP_LIB_VERSION	0x02000008	// (2.0.8)

#define RTMFP_DEFAULT_KEY	(Mona::UInt8*)"Adobe Systems 02"
#define RTMFP_KEY_SIZE		0x10

#define RTMFP_HEADER_SIZE		11
#define RTMFP_MIN_PACKET_SIZE	(RTMFP_HEADER_SIZE+1)
#define RTMFP_MAX_PACKET_SIZE	1192
#define RTMFP_TIMESTAMP_SCALE	4

#define PEER_ID_SIZE			0x20
#define COOKIE_SIZE				0x40

#define PEER_LIST_ADDRESS_TYPE	std::map<Mona::SocketAddress, RTMFP::AddressType>

struct RTMFPSender;
struct RTMFP : virtual Mona::Static {
	enum AddressType {
		ADDRESS_UNSPECIFIED=0,
		ADDRESS_LOCAL=1,
		ADDRESS_PUBLIC=2,
		ADDRESS_REDIRECTION=3
	};

	enum DataType {
		TYPE_UNKNOWN = 0,
		TYPE_AMF,
		TYPE_AMF0,
		TYPE_JSON,
		TYPE_XMLRPC,
		TYPE_QUERY
	};

	enum { TIMESTAMP_SCALE = 4 };
	enum { SENDABLE_MAX = 6 }; // Number of packet max to send before receiving an ack

	enum {
		SIZE_HEADER = 11,
		SIZE_PACKET = 1192,
		SIZE_COOKIE = 0x40
	};

	enum {
		MESSAGE_OPTIONS = 0x80,
		MESSAGE_WITH_BEFOREPART = 0x20,
		MESSAGE_WITH_AFTERPART = 0x10,
		MESSAGE_RELIABLE = 0x04, // not a RTMFP spec., just for a RTMFPPacket need
		MESSAGE_ABANDON = 0x02,
		MESSAGE_END = 0x01
	};

	enum SessionStatus {
		STOPPED,
		HANDSHAKE30,
		HANDSHAKE70,
		HANDSHAKE38,
		HANDSHAKE78,
		CONNECTED,
		NEAR_CLOSED,
		FAILED
	};

	struct Engine : virtual Mona::Object {
		Engine(const Mona::UInt8* key) { memcpy(_key, key, KEY_SIZE); EVP_CIPHER_CTX_init(&_context); }
		Engine(const Engine& engine) { memcpy(_key, engine._key, KEY_SIZE); EVP_CIPHER_CTX_init(&_context); }
		virtual ~Engine() { EVP_CIPHER_CTX_cleanup(&_context); }

		bool							decode(Mona::Exception& ex, Mona::Buffer& buffer, const Mona::SocketAddress& address);
		std::shared_ptr<Mona::Buffer>&	encode(std::shared_ptr<Mona::Buffer>& pBuffer, Mona::UInt32 farId, const Mona::SocketAddress& address);

		static bool				Decode(Mona::Exception& ex, Mona::Buffer& buffer, const Mona::SocketAddress& address) { return Default().decode(ex, buffer, address); }
		static std::shared_ptr<Mona::Buffer>&	Encode(std::shared_ptr<Mona::Buffer>& pBuffer, Mona::UInt32 farId, const Mona::SocketAddress& address) { return Default().encode(pBuffer, farId, address); }

	private:
		static Engine& Default() { thread_local Engine Engine(BIN "Adobe Systems 02"); return Engine; }

		enum { KEY_SIZE = 0x10 };
		Mona::UInt8						_key[KEY_SIZE];
		EVP_CIPHER_CTX					_context;
	};

	struct Message : virtual Mona::Object, Mona::Packet {
		Message(Mona::UInt64 flowId, Mona::UInt32 lost, const Mona::Packet& packet) : lost(lost), flowId(flowId), Mona::Packet(std::move(packet)) {}
		const Mona::UInt64 flowId;
		const Mona::UInt32 lost;
	};
	struct Flush : virtual Mona::Object {
		Flush(Mona::Int32 ping, bool keepalive, bool died, std::map<Mona::UInt64, Mona::Packet>& acks) :
			ping(ping), acks(std::move(acks)), keepalive(keepalive), died(died) {}
		const Mona::Int32				ping; // if died, ping takes error
		const bool						keepalive;
		const bool						died;
		const std::map<Mona::UInt64, Mona::Packet>	acks; // ack + fails
	};

	struct Output : virtual Mona::Object {

		virtual Mona::UInt32	rto() const = 0;
		virtual void			send(const std::shared_ptr<RTMFPSender>& pSender) = 0;
		virtual Mona::UInt64	queueing() const = 0;
	};

	static bool						ReadAddress(Mona::BinaryReader& reader, Mona::SocketAddress& address, AddressType& addressType);
	static Mona::BinaryWriter&		WriteAddress(Mona::BinaryWriter& writer, const Mona::SocketAddress& address, AddressType type=ADDRESS_UNSPECIFIED);

	static Mona::UInt32				Unpack(Mona::BinaryReader& reader);
	static void						Pack(Mona::Buffer& buffer,Mona::UInt32 farId);

	static bool						Send(Mona::Socket& socket, const Mona::Packet& packet, const Mona::SocketAddress& address);
	static Mona::Buffer&			InitBuffer(std::shared_ptr<Mona::Buffer>& pBuffer, Mona::UInt8 marker);
	static Mona::Buffer&			InitBuffer(std::shared_ptr<Mona::Buffer>& pBuffer, std::atomic<Mona::Int64>& initiatorTime, Mona::UInt8 marker);
	static void						ComputeAsymetricKeys(const Mona::Binary& sharedSecret, const Mona::UInt8* initiatorNonce,Mona::UInt32 initNonceSize, const Mona::UInt8* responderNonce,Mona::UInt32 respNonceSize, Mona::UInt8* requestKey, Mona::UInt8* responseKey);

	static Mona::UInt16				TimeNow() { return Time(Mona::Time::Now()); }
	static Mona::UInt16				Time(Mona::Int64 timeVal) { return (timeVal / RTMFP_TIMESTAMP_SCALE)&0xFFFF; }

	static bool						IsKeyFrame(const Mona::UInt8* data, Mona::UInt32 size) { return size>0 && (*data & 0xF0) == 0x10; }

	static bool						IsAACCodecInfos(const Mona::UInt8* data, Mona::UInt32 size) { return size>1 && (*data >> 4) == 0x0A && data[1] == 0; }

	static bool						IsVideoCodecInfos(const Mona::UInt8* data, Mona::UInt32 size) { return size>1 && ((*data|0x0F) == 0x1F) && data[1] == 0; }

	// Read addresses from the buffer reader
	// return : True if at least an address has been read
	static bool	ReadAddresses(Mona::BinaryReader& reader, PEER_LIST_ADDRESS_TYPE& addresses, Mona::SocketAddress& hostAddress, std::function<void(const Mona::SocketAddress&, AddressType)> onNewAddress);

	// Return a random iterator which respect the isAllowed condition
	template<class ContainerType, typename Iterator>
	static bool getRandomIt(ContainerType& container, Iterator& itResult, std::function<bool(const Iterator&)> isAllowed) {
		if (container.empty())
			return false;

		auto itRandom = container.begin();
		advance(itRandom, Mona::Util::Random<Mona::UInt32>() % container.size());

		itResult = itRandom;
		while (!isAllowed(itResult)) {
			if (++itResult == container.end())
				itResult = container.begin();

			if (itResult == itRandom) // No match
				return false;
		}
		return true;
	}
};

