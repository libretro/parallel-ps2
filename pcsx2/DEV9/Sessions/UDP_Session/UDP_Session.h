/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/Pcsx2Defs.h"
#include <retro_atomic.h>
#include <chrono>
#ifdef _WIN32
#include <winsock2.h>
#elif defined(__POSIX__)
#define INVALID_SOCKET -1
#include <sys/socket.h>
#endif

#include "UDP_BaseSession.h"

namespace Sessions
{
	class UDP_Session : public UDP_BaseSession
	{
	private:
		retro_atomic_int_t open = RETRO_ATOMIC_INT_INITIALIZER(0);

#ifdef _WIN32
		SOCKET client = INVALID_SOCKET;
#elif defined(__POSIX__)
		int client = INVALID_SOCKET;
#endif

		u16 srcPort = 0;
		u16 destPort = 0;
		//Broadcast
		const bool isBroadcast; // = false;
		bool isMulticast = false;
		const bool isFixedPort; // = false;
		//EndBroadcast

		/* Monotonic microsecond stamp (cpu_features_get_time_usec): a
		 * 64-bit atomic integer instead of an atomic chrono time_point,
		 * which is lock-based on several ABIs anyway. */
		retro_atomic_64_t deathClockStart;
		const static int64_t MAX_IDLE_USEC;

	public:
		//Normal Port
		UDP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP);

		virtual bool PollInterest(uintptr_t* fd, int* events);
		//Fixed Port
#ifdef _WIN32
		UDP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP, bool parIsBroadcast, bool parIsMulticast, SOCKET parClient);
#elif defined(__POSIX__)
		UDP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP, bool parIsBroadcast, bool parIsMulticast, int parClient);
#endif

		virtual PacketReader::IP::IP_Payload* Recv();
		virtual bool WillRecive(PacketReader::IP::IP_Address parDestIP);
		virtual bool Send(PacketReader::IP::IP_Payload* payload);
		virtual void Reset();

		virtual ~UDP_Session();

	private:
		void CloseSocket();
	};
} // namespace Sessions
