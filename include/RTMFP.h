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

#include "Base/SocketAddress.h"
#include "Base/BinaryReader.h"
#include "Base/BinaryWriter.h"
#include "Base/Time.h"
#include "Base/Crypto.h"
#include "Base/Util.h"
#include "Base/Packet.h"
#include "Base/Socket.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "AMFWriter.h"
#include "Base/Logs.h"
#include <map>

#define RTMFP_LIB_VERSION	0x02040002	// (2.4.2)

#define RTMFP_DEFAULT_KEY	(Base::UInt8*)"Adobe Systems 02"
#define RTMFP_KEY_SIZE		0x10

#define RTMFP_HEADER_SIZE		11
#define RTMFP_MIN_PACKET_SIZE	(RTMFP_HEADER_SIZE+1)
#define RTMFP_MAX_PACKET_SIZE	1192

#define PEER_ID_SIZE			0x20
#define COOKIE_SIZE				0x40

#define PEER_LIST_ADDRESS_TYPE	std::map<Base::SocketAddress, RTMFP::AddressType>

struct RTMFPSender;
struct RTMFP : virtual Base::Static {
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

	struct Engine : virtual Base::Object {
		Engine(const Base::UInt8* key) {
			_context = EVP_CIPHER_CTX_new();
			memcpy(_key, key, KEY_SIZE); EVP_CIPHER_CTX_init(_context);
		}
		Engine(const Engine& engine) {
			_context = EVP_CIPHER_CTX_new();
			memcpy(_key, engine._key, KEY_SIZE); EVP_CIPHER_CTX_init(_context);
		}
		virtual ~Engine() {
			EVP_CIPHER_CTX_cleanup(_context);
			EVP_CIPHER_CTX_free(_context);
		}

		bool							decode(Base::Exception& ex, Base::Buffer& buffer, const Base::SocketAddress& address);
		std::shared_ptr<Base::Buffer>&	encode(std::shared_ptr<Base::Buffer>& pBuffer, Base::UInt32 farId, const Base::SocketAddress& address);

		static bool				Decode(Base::Exception& ex, Base::Buffer& buffer, const Base::SocketAddress& address) { return Default().decode(ex, buffer, address); }
		static std::shared_ptr<Base::Buffer>&	Encode(std::shared_ptr<Base::Buffer>& pBuffer, Base::UInt32 farId, const Base::SocketAddress& address) { return Default().encode(pBuffer, farId, address); }

	private:
		static Engine& Default() { thread_local Engine Engine(BIN "Adobe Systems 02"); return Engine; }

		enum { KEY_SIZE = 0x10 };
		Base::UInt8						_key[KEY_SIZE];
		EVP_CIPHER_CTX*					_context;
	};

	struct Message : virtual Base::Object, Base::Packet {
		Message(Base::UInt64 flowId, Base::UInt32 lost, const Base::Packet& packet) : lost(lost), flowId(flowId), Base::Packet(std::move(packet)) {}
		const Base::UInt64 flowId;
		const Base::UInt32 lost;
	};
	struct Flush : virtual Base::Object {
		Flush(Base::Int32 ping, bool keepalive, bool died, std::map<Base::UInt64, Base::Packet>& acks) :
			ping(ping), acks(std::move(acks)), keepalive(keepalive), died(died) {}
		const Base::Int32				ping; // if died, ping takes error
		const bool						keepalive;
		const bool						died;
		const std::map<Base::UInt64, Base::Packet>	acks; // ack + fails
	};

	struct Output : virtual Base::Object {

		virtual Base::UInt32	rto() const = 0;
		virtual void			send(const std::shared_ptr<RTMFPSender>& pSender) = 0;
		virtual Base::UInt64	queueing() const = 0;
	};

	static bool						ReadAddress(Base::BinaryReader& reader, Base::SocketAddress& address, AddressType& addressType);
	static Base::BinaryWriter&		WriteAddress(Base::BinaryWriter& writer, const Base::SocketAddress& address, AddressType type=ADDRESS_UNSPECIFIED);

	static Base::UInt32				Unpack(Base::BinaryReader& reader);
	static void						Pack(Base::Buffer& buffer,Base::UInt32 farId);

	static bool						Send(Base::Socket& socket, const Base::Packet& packet, const Base::SocketAddress& address);
	static Base::Buffer&			InitBuffer(std::shared_ptr<Base::Buffer>& pBuffer, Base::UInt8 marker);
	static Base::Buffer&			InitBuffer(std::shared_ptr<Base::Buffer>& pBuffer, std::atomic<Base::Int64>& initiatorTime, Base::UInt8 marker);
	static void						ComputeAsymetricKeys(const Base::Binary& sharedSecret, const Base::UInt8* initiatorNonce,Base::UInt32 initNonceSize, const Base::UInt8* responderNonce,Base::UInt32 respNonceSize, Base::UInt8* requestKey, Base::UInt8* responseKey);

	static Base::UInt16				TimeNow() { return Time(Base::Time::Now()); }
	static Base::UInt16				Time(Base::Int64 timeVal) { return (timeVal / RTMFP::TIMESTAMP_SCALE)&0xFFFF; }

	static bool						IsKeyFrame(const Base::UInt8* data, Base::UInt32 size) { return size>0 && (*data & 0xF0) == 0x10; }

	static bool						IsAACCodecInfos(const Base::UInt8* data, Base::UInt32 size) { return size>1 && (*data >> 4) == 0x0A && data[1] == 0; }

	static bool						IsVideoCodecInfos(const Base::UInt8* data, Base::UInt32 size) { return size>1 && ((*data|0x0F) == 0x1F) && data[1] == 0; }

	// Read addresses from the buffer reader
	// return : True if at least an address has been read
	static bool	ReadAddresses(Base::BinaryReader& reader, PEER_LIST_ADDRESS_TYPE& addresses, Base::SocketAddress& hostAddress, std::function<void(const Base::SocketAddress&, AddressType)> onNewAddress);

	/* AMF Utility functions */
	static void WriteInvocation(AMFWriter& writer, const char* name, double callback, bool amf3);
	static void WriteAMFState(AMFWriter& writer, const char* name, const char* code, const std::string& description, bool amf0, bool withoutClosing = false);

	// Return a random iterator which respect the isAllowed condition
	template<class ContainerType, typename Iterator>
	static bool GetRandomIt(ContainerType& container, Iterator& itResult, std::function<bool(const Iterator&)> isAllowed) {
		if (container.empty())
			return false;

		auto itRandom = container.begin();
		advance(itRandom, Base::Util::Random<Base::UInt32>() % container.size());

		itResult = itRandom;
		while (!isAllowed(itResult)) {
			if (++itResult == container.end())
				itResult = container.begin();

			if (itResult == itRandom) // No match
				return false;
		}
		return true;
	}

	// Return the previous iterator in an ordered container, if the iterator is begin() return the last iterator
	template<class ContainerType, typename Iterator>
	static Iterator& GetPreviousIt(ContainerType& container, Iterator& it) {
		if (it == container.begin())
			it = --(container.end());
		else
			--it;
		return it;
	}

	// Return the previous iterator in an ordered container, if the next iterator is end() return the first iterator
	template<class ContainerType, typename Iterator>
	static Iterator& GetNextIt(ContainerType& container, Iterator& it) {
		if (it == container.end() || ++it == container.end())
			it = container.begin();

		return it;
	}
};

