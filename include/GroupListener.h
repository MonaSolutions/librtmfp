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

#include "Listener.h"

struct GroupListener : Listener {
	typedef Base::Event<void(bool reliable, AMF::Type type, Base::UInt32 time, const Base::Packet& packet)> ON(Media);
	typedef Base::Event<void()>																				ON(Flush); // called at the end of the RTMFP_Write
	GroupListener(Publisher& publication, const std::string& identifier);
	virtual ~GroupListener();

	virtual void startPublishing();
	virtual void stopPublishing();

	virtual void pushAudio(Base::UInt32 time, const Base::Packet& packet, bool reliable);
	virtual void pushVideo(Base::UInt32 time, const Base::Packet& packet, bool reliable);

	virtual void flush();

private:

	bool	firstTime() { return !_dataInitialized; }

	bool	pushVideoInfos(Base::UInt32 time, const Base::Packet& packet);
	bool	pushAudioInfos(Base::UInt32 time);

	Base::UInt32 			_startTime;
	Base::UInt32			_lastTime;
	bool					_firstTime;
	Base::UInt32			_seekTime;
	bool					_codecInfosSent;
	Base::Time				_lastVideoCodecs; // last time video codecs have been sent
	Base::Time				_lastAACCodecs; // last time AAC audio codecs have been sent

	bool					_dataInitialized;
};
