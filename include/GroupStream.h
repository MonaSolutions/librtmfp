
#pragma once

#include "FlashStream.h"

/**************************************************************
GroupStream is a Group NetStream
*/
class GroupStream : public FlashStream {
public:
	enum ContentType {
		GROUP_MEDIA_END		= 0x00, // End of a NetGroup splitted media data
		GROUP_INIT			= 0x01, // Init a Group session with a peer
		GROUP_DATA			= 0x02, // NetGroup data message
		GROUP_REPORT		= 0x0A, // NetGroup Report
		GROUP_MEMBER		= 0x0B, // NetGroup member
		GROUP_NKNOWN		= 0x0C, // UNknown NetGroup type (close message?)
		GROUP_BEGIN			= 0x0E, // Group Begin message
		GROUP_BEGIN_NEAREST	= 0x0F, // Group Begin message in mode NetGroupReceiveMode.NEAREST
		GROUP_MEDIA_NEXT	= 0x10, // Continuation of a NetGroup splitted media data
		GROUP_MEDIA_DATA	= 0x20, // Audio/Video data
		GROUP_INFOS			= 0x21, // Media stream infos
		GROUP_FRAGMENTS_MAP = 0x22, // Map of media fragments availables for the peer
		GROUP_PLAY_PUSH		= 0x23, // NetGroup Push request
		GROUP_PLAY_PULL		= 0x2B, // NetGroup Pull request
		GROUP_MEDIA_START	= 0x30, // Beginning of a NetGroup splitted media data
	};

	GroupStream(Mona::UInt16 id);
	virtual ~GroupStream();

	// return flase if writer is closed!
	virtual bool	process(Mona::PacketReader& packet,FlashWriter& writer, double lostRate=0);

protected:
	virtual void	messageHandler(const std::string& name, AMFReader& message, FlashWriter& writer);
};
