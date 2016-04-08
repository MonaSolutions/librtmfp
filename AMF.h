#include "Mona/Mona.h"

#pragma once

#define AMF_NULL			0x05
#define AMF_UNDEFINED		0x06
#define AMF_UNSUPPORTED		0x0D
#define	AMF_AVMPLUS_OBJECT	0x11

#define AMF_NUMBER			0x00
#define AMF_BOOLEAN			0x01
#define AMF_STRING			0x02
#define AMF_DATE			0x0B

#define AMF_BEGIN_OBJECT		0x03
#define AMF_BEGIN_TYPED_OBJECT	0x10
#define AMF_END_OBJECT			0x09
#define AMF_REFERENCE			0x07

#define AMF_MIXED_ARRAY	    0x08 
#define	AMF_STRICT_ARRAY	0x0A

#define	AMF_LONG_STRING		0x0C


#define AMF3_UNDEFINED		0x00
#define AMF3_NULL			0x01
#define AMF3_FALSE			0x02
#define AMF3_TRUE			0x03
#define AMF3_INTEGER		0x04
#define AMF3_NUMBER			0x05
#define AMF3_STRING			0x06
#define AMF3_DATE			0x08
#define AMF3_ARRAY			0x09
#define AMF3_OBJECT			0x0A
#define AMF3_BYTEARRAY		0x0C
#define AMF3_DICTIONARY		0x11

#define	AMF_END				0xFF

#define AMF_MAX_INTEGER		268435455


class AMF : virtual Mona::Static {
public:
	enum ContentType {
		EMPTY				=0x00, // End of a NetGroup splitted media data
		CHUNKSIZE			=0x01,
		ABORT				=0x02, // unknown NetGroup type 1
		ACK					=0x03,
		RAW					=0x04,
		WIN_ACKSIZE			=0x05,
		BANDWITH			=0x06,
		AUDIO				=0x08,
		VIDEO				=0x09,
		GROUP_REPORT		=0x0A, // NetGroup Report
		MEMBER				=0x0B, // added for NetGroup
		GROUP_NKNOWN2		=0x0E, // unknown NetGroup type 2
		DATA_AMF3			=0x0F,
		GROUP_MEDIA_NEXT	=0x10, // Continuation of a NetGroup splitted media data
		INVOCATION_AMF3		=0x11,
		DATA				=0x12,
		INVOCATION			=0x14,
		GROUP_MEDIA_DATA    =0x20, // Audio/Video data
		GROUP_INFOS			=0x21, // Media stream infos
		GROUP_FRAGMENTS_MAP	=0x22, // Map of media fragments availables for the peer
		GROUP_PLAY			=0x23, // NetGroup Play request
		GROUP_MEDIA_START   =0x30, // Beginning of a NetGroup splitted media data
	};
};
