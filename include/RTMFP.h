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

#include "Mona/Mona.h"
#include "Mona/SocketAddress.h"
#include "Mona/BinaryReader.h"
#include "Mona/BinaryWriter.h"
#include "Mona/Time.h"
#include "Mona/Crypto.h"
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "Mona/Logs.h"

#define RTMFP_LIB_VERSION	0x01010000	// (1.1.0)

#define RTMFP_DEFAULT_KEY	(UInt8*)"Adobe Systems 02"
#define RTMFP_KEY_SIZE		0x10

#define RTMFP_HEADER_SIZE		11
#define RTMFP_MIN_PACKET_SIZE	(RTMFP_HEADER_SIZE+1)
#define RTMFP_MAX_PACKET_SIZE	1192
#define RTMFP_TIMESTAMP_SCALE	4

#define PEER_ID_SIZE			0x20

class RTMFPEngine : public virtual Mona::Object {
public:
	enum Direction {
		DECRYPT=0,
		ENCRYPT
	};
	RTMFPEngine(const Mona::UInt8* key, Direction direction) : _direction(direction) {
		memcpy(_key, key, RTMFP_KEY_SIZE);
		EVP_CIPHER_CTX_init(&_context);
	}
	virtual ~RTMFPEngine() {
		EVP_CIPHER_CTX_cleanup(&_context);
	}

	bool process(Mona::UInt8* data, int size) {
		int newSize(size);
		static Mona::UInt8 IV[RTMFP_KEY_SIZE];
		EVP_CipherInit_ex(&_context, EVP_aes_128_cbc(), NULL, _key, IV,_direction);
		EVP_CipherUpdate(&_context, data, &newSize, data, size);

		if (_direction == DECRYPT) { // check CRC
			Mona::BinaryReader reader(data, size);
			Mona::UInt16 crc(reader.read16());
			// TODO: return the binary reader object
			return (Mona::Crypto::ComputeCRC(reader) == crc);
		}
		return true;
	}

private:
	Direction				_direction;
	Mona::UInt8				_key[RTMFP_KEY_SIZE];
	EVP_CIPHER_CTX			_context;
};

class RTMFP : virtual Mona::Static {
public:
	enum AddressType {
		ADDRESS_UNSPECIFIED=0,
		ADDRESS_LOCAL=1,
		ADDRESS_PUBLIC=2,
		ADDRESS_REDIRECTION=3
	};

	static bool						ReadAddress(Mona::BinaryReader& reader, Mona::SocketAddress& address, Mona::UInt8 addressType);
	static Mona::BinaryWriter&		WriteAddress(Mona::BinaryWriter& writer, const Mona::SocketAddress& address, AddressType type=ADDRESS_UNSPECIFIED);

	static Mona::UInt32				Unpack(Mona::BinaryReader& reader);
	static void						Pack(Mona::BinaryWriter& writer,Mona::UInt32 farId);

	static void						ComputeAsymetricKeys(const Mona::Buffer& sharedSecret,
														const Mona::UInt8* initiatorNonce,Mona::UInt16 initNonceSize,
														const Mona::UInt8* responderNonce,Mona::UInt16 respNonceSize,
														 Mona::UInt8* requestKey,
														 Mona::UInt8* responseKey);

	static Mona::UInt16				TimeNow() { return Time(Mona::Time::Now()); }
	static Mona::UInt16				Time(Mona::Int64 timeVal) { return (timeVal / RTMFP_TIMESTAMP_SCALE)&0xFFFF; }

	static void						Write7BitValue(std::string& buff,Mona::UInt64 value);

	static bool						IsKeyFrame(const Mona::UInt8* data, Mona::UInt32 size) { return size>0 && (*data & 0xF0) == 0x10; }

	static bool						IsAACCodecInfos(const Mona::UInt8* data, Mona::UInt32 size) { return size>1 && (*data >> 4) == 0x0A && data[1] == 0; }

	static bool						IsH264CodecInfos(const Mona::UInt8* data, Mona::UInt32 size) { return size>1 && *data == 0x17 && data[1] == 0; }

	// Return a random iterator from a container which respect the isAllowed condition
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

