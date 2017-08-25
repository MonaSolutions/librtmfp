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

#include "Base/Congestion.h"

using namespace std;

namespace Base {

bool Congestion::operator()(UInt64 queueing, UInt32 duration) {
	bool congested(queueing>=_lastQueueing);
	_lastQueueing = queueing;
	if (congested) {
		// congestion
		if (_congested) {
			UInt32 elapsed = UInt32(_congested.elapsed());
			if (elapsed > duration)
				return true;
		} else
			_congested.update();
	} else
		_congested = 0;
	return false;
}


} // namespace Mona
