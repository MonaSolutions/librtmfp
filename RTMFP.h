
#pragma once

#include "Mona/Mona.h"
#include "Mona/SocketAddress.h"
#include "Mona/BinaryReader.h"
#include "Mona/BinaryWriter.h"
#include "Mona/Time.h"
#include "Mona/Crypto.h"
#include <openssl/evp.h>

#include "Mona/Logs.h"

#define RTMFP_DEFAULT_KEY	(UInt8*)"Adobe Systems 02"
#define RTMFP_KEY_SIZE		0x10

#define RTMFP_HEADER_SIZE		11
#define RTMFP_MIN_PACKET_SIZE	(RTMFP_HEADER_SIZE+1)
#define RTMFP_MAX_PACKET_SIZE	1192
#define RTMFP_TIMESTAMP_SCALE	4


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
};

