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

#include <retro_timers.h>
#include "common/Pcsx2Defs.h"
#include "TCP_Session.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#else
#include <unistd.h>
#endif

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	void TCP_Session::PushRecvBuff(TCP_Packet* tcp)
	{
		_recvBuff.Enqueue(tcp);
	}
	TCP_Packet* TCP_Session::PopRecvBuff()
	{
		TCP_Packet* ret;
		if (_recvBuff.Dequeue(&ret))
			return ret;
		else
			return nullptr;
	}

	void TCP_Session::IncrementMyNumber(u32 amount)
	{
		_OldMyNumbers.push_back(_MySequenceNumber);
		_OldMyNumbers.erase(_OldMyNumbers.begin());

		_MySequenceNumber += amount;
	}
	u32 TCP_Session::GetMyNumber()
	{
		return _MySequenceNumber;
	}
	std::tuple<u32, std::vector<u32>> TCP_Session::GetAllMyNumbers()
	{
		std::vector<u32> old;
		old.reserve(_OldMyNumbers.size());
		old.insert(old.end(), _OldMyNumbers.begin(), _OldMyNumbers.end());

		return {_MySequenceNumber, old};
	}
	void TCP_Session::ResetMyNumbers()
	{
		_MySequenceNumber = 1;
		_OldMyNumbers.clear();
		for (int i = 0; i < oldMyNumCount; i++)
			_OldMyNumbers.push_back(1);
	}

	TCP_Session::TCP_Session(ConnectionKey parKey, IP_Address parAdapterIP)
		: BaseSession(parKey, parAdapterIP)
	{
	}

	TCP_Packet* TCP_Session::CreateBasePacket(PayloadData* data)
	{
		//log_cb(RETRO_LOG_DEBUG, "Creating Base Packet\n");
		if (data == nullptr)
			data = new PayloadData(0);

		TCP_Packet* ret = new TCP_Packet(data);

		//and now to setup THE ENTIRE THING
		ret->sourcePort = destPort;
		ret->destinationPort = srcPort;

		ret->sequenceNumber = GetMyNumber();
		//log_cb(RETRO_LOG_DEBUG, "With MySeq: %d\n", ret->sequenceNumber);
		ret->acknowledgementNumber = GetExpectedSeq();
		//log_cb(RETRO_LOG_DEBUG, "With MyAck: %d\n", ret->acknowledgementNumber);

		ret->windowSize = 2 * maxSegmentSize;

		if (sendTimeStamps)
		{
			ret->options.push_back(new TCPopNOP());
			ret->options.push_back(new TCPopNOP());

			const auto timestampChrono = std::chrono::steady_clock::now() - timeStampStart;
			const u32 timestampSeconds = std::chrono::duration_cast<std::chrono::seconds>(timestampChrono).count() % UINT_MAX;

			ret->options.push_back(new TCPopTS(timestampSeconds, lastRecivedTimeStamp));
		}
		return ret;
	}

	void TCP_Session::CloseSocket()
	{
		if (client != INVALID_SOCKET)
		{
#ifdef _WIN32
			closesocket(client);
#elif defined(__POSIX__)
			::close(client);
#endif
			client = INVALID_SOCKET;
		}
	}

	void TCP_Session::Reset()
	{
		//CloseSocket();
		RaiseEventConnectionClosed();
	}

	TCP_Session::~TCP_Session()
	{
		CloseSocket();

		//Clear out _recvBuff
		while (!_recvBuff.IsQueueEmpty())
		{
			TCP_Packet* retPay;
			if (!_recvBuff.Dequeue(&retPay))
			{
				using namespace std::chrono_literals;
				retro_sleep(1);
				continue;
			}

			delete retPay;
		}
	}
} // namespace Sessions
