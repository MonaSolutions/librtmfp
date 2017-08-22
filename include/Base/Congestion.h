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

#include "Base/Mona.h"
#include "Base/Time.h"
#include "Base/Net.h"

namespace Base {

/*!
Tool to compute queue congestion */
struct Congestion {
	Congestion() : _lastQueueing(0), _congested(0) {}

	// Wait RTO time by default (3 sec) => sounds right with socket and file
	bool operator()(UInt64 queueing, UInt32 duration = Net::RTO_INIT);
private:
	UInt64	_lastQueueing;
	Time	_congested;
};

} // namespace Mona

