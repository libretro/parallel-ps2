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

#include <climits>
#include <retro_atomic.h>
#include <algorithm>

#include "common/Pcsx2Defs.h"

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#define SD_RECEIVE SHUT_RD
#endif

#include "TCP_Session.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::TCP;

namespace Sessions
{
	/* As Recv decides what to do: the queue first, then by the state. A
	 * window or an ACK that lets data be read again comes from the PS2,
	 * whose packets wake the receive thread (SocketAdapter::send). */
	bool TCP_Session::PollInterest(uintptr_t* fd, int* events)
	{
		*events = 0;
		if (!_recvBuff.IsQueueEmpty())
			return false;
		switch (GetState())
		{
			case TCP_State::SendingSYN_ACK:
				*fd     = (uintptr_t)client;
				*events = SESSION_POLL_WRITE;
				return true;
			case TCP_State::CloseCompletedFlushBuffer:
				return false;
			case TCP_State::Connected:
			case TCP_State::Closing_ClosedByPS2:
				if (retro_atomic_load_acquire_int(&windowSize) != 0 &&
					retro_atomic_load_acquire_int(&myNumberACKed))
				{
					*fd     = (uintptr_t)client;
					*events = SESSION_POLL_READ;
				}
				return true;
			default:
				return true;
		}
	}

	PacketReader::IP::IP_Payload* TCP_Session::Recv()
	{
		TCP_Packet* ret = PopRecvBuff();
		if (ret != nullptr)
			return ret;

		switch (GetState())
		{
			case TCP_State::SendingSYN_ACK:
			{
				fd_set writeSet;
				fd_set exceptSet;

				FD_ZERO(&writeSet);
				FD_ZERO(&exceptSet);

				FD_SET(client, &writeSet);
				FD_SET(client, &exceptSet);

				timeval nowait{0};
				select(client + 1, nullptr, &writeSet, &exceptSet, &nowait);

				if (FD_ISSET(client, &writeSet))
					return ConnectTCPComplete(true);
				if (FD_ISSET(client, &exceptSet))
					return ConnectTCPComplete(false);

				return nullptr;
			}
			case TCP_State::SentSYN_ACK:
				//Don't read data untill PS2 ACKs connection
				return nullptr;
			case TCP_State::CloseCompletedFlushBuffer:
				//When TCP connection is closed by the server
				//the server is the last to send a packet
				//so the event must be raised here
				SetState(TCP_State::CloseCompleted);
				RaiseEventConnectionClosed();
				return nullptr;
			case TCP_State::Connected:
			case TCP_State::Closing_ClosedByPS2:
				//Only accept data in above two states
				break;
			default:
				return nullptr;
		}

		uint maxSize = 0;
		if (sendTimeStamps)
			maxSize = std::min<uint>(maxSegmentSize - 12, retro_atomic_load_acquire_int(&windowSize));
		else
			maxSize = std::min<uint>(maxSegmentSize, retro_atomic_load_acquire_int(&windowSize));

		if (maxSize != 0 &&
			retro_atomic_load_acquire_int(&myNumberACKed))
		{
			std::unique_ptr<u8[]> buffer;
			int err = 0;
			int recived;

			u_long available;
#ifdef _WIN32
			err = ioctlsocket(client, FIONREAD, &available);
#elif defined(__POSIX__)
			err = ioctl(client, FIONREAD, &available);
#endif
			if (err != SOCKET_ERROR)
			{
				if (available > maxSize)
					log_cb(RETRO_LOG_INFO, "DEV9: TCP: Got a lot of data: %lu Using: %d\n", available, maxSize);

				buffer = std::make_unique<u8[]>(maxSize);
				recived = recv(client, (char*)buffer.get(), maxSize, 0);
				if (recived == -1)
#ifdef _WIN32
					err = WSAGetLastError();
#elif defined(__POSIX__)
					err = errno;
#endif

				switch (err)
				{
#ifdef _WIN32
					case WSAEINVAL:
					case WSAESHUTDOWN:
						//In theory, this should only occur when the PS2 has RST the connection
						//and the call to TCPSession.Recv() occurs at just the right time.

						//log_cb(RETRO_LOG_INFO, "DEV9: TCP: Recv() on shutdown socket\n");
						return nullptr;
					case WSAEWOULDBLOCK:
						return nullptr;
#elif defined(__POSIX__)
					case EINVAL:
					case ESHUTDOWN:
						//See WSAESHUTDOWN
						//log_cb(RETRO_LOG_INFO, "DEV9: TCP: Recv() on shutdown socket\n");
						return nullptr;
					case EWOULDBLOCK:
						return nullptr;
#endif
					case 0:
						break;
					default:
						CloseByRemoteRST();
						log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Recv Error: %d\n", err);
						return nullptr;
				}

				//Server Closed Socket
				if (recived == 0)
				{
					int result = shutdown(client, SD_RECEIVE);
					if (result == SOCKET_ERROR)
						log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Shutdown SD_RECEIVE Error: %d\n",
#ifdef _WIN32
							WSAGetLastError());
#elif defined(__POSIX__)
							errno);
#endif

					switch (GetState())
					{
						case TCP_State::Connected:
							return CloseByRemoteStage1();
						case TCP_State::Closing_ClosedByPS2:
							return CloseByPS2Stage3();
						default:
							CloseByRemoteRST();
							log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Remote Close In Invalid State\n");
							break;
					}
					return nullptr;
				}
				log_cb(RETRO_LOG_DEBUG, "DEV9: TCP: [SRV] Sending %d bytes\n", recived);

				PayloadData* recivedData = new PayloadData(recived);
				memcpy(recivedData->data.get(), buffer.get(), recived);

				TCP_Packet* iRet = CreateBasePacket(recivedData);
				IncrementMyNumber((u32)recived);

				iRet->SetACK(true);
				iRet->SetPSH(true);

				retro_atomic_store_release_int(&myNumberACKed, 0);
				//log_cb(RETRO_LOG_DEBUG, "DEV9: TCP: myNumberACKed Reset\n");
				return iRet;
			}
		}

		return nullptr;
	}

	TCP_Packet* TCP_Session::ConnectTCPComplete(bool success)
	{
		if (success)
		{
			SetState(TCP_State::SentSYN_ACK);

			TCP_Packet* ret = new TCP_Packet(new PayloadData(0));
			//Return the fact we connected
			ret->sourcePort = destPort;
			ret->destinationPort = srcPort;

			ret->sequenceNumber = GetMyNumber();
			IncrementMyNumber(1);

			ret->acknowledgementNumber = GetExpectedSeq();

			ret->SetSYN(true);
			ret->SetACK(true);
			ret->windowSize = 2 * maxSegmentSize;
			ret->options.push_back(new TCPopMSS(maxSegmentSize));

			ret->options.push_back(new TCPopNOP());
			ret->options.push_back(new TCPopWS(0));

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
		else
		{
			int error = 0;
#ifdef _WIN32
			int len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Unkown TCP Connection Error (getsockopt Error: %d)\n", WSAGetLastError());
#elif defined(__POSIX__)
			socklen_t len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Unkown TCP Connection Error (getsockopt Error: %d)\n", errno);
#endif
			else
				log_cb(RETRO_LOG_ERROR, "DEV9: TCP: Send Error: %d\n", error);

			SetState(TCP_State::CloseCompleted);
			RaiseEventConnectionClosed();
			return nullptr;
		}
	}

	PacketReader::IP::TCP::TCP_Packet* TCP_Session::CloseByPS2Stage3()
	{
		//log_cb(RETRO_LOG_INFO, "DEV9: TCP: Remote has closed connection after PS2\n");

		TCP_Packet* ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetACK(true);
		ret->SetFIN(true);

		retro_atomic_store_release_int(&myNumberACKed, 0);
		//log_cb(RETRO_LOG_DEBUG, "myNumberACKed Reset\n");

		SetState(TCP_State::Closing_ClosedByPS2ThenRemote_WaitingForAck);
		return ret;
	}

	PacketReader::IP::TCP::TCP_Packet* TCP_Session::CloseByRemoteStage1()
	{
		//log_cb(RETRO_LOG_INFO, "DEV9: TCP: Remote has closed connection\n");

		TCP_Packet* ret = CreateBasePacket();
		IncrementMyNumber(1);

		ret->SetACK(true);
		ret->SetFIN(true);

		retro_atomic_store_release_int(&myNumberACKed, 0);
		//log_cb(RETRO_LOG_DEBUG, "myNumberACKed Reset\n");

		SetState(TCP_State::Closing_ClosedByRemote);
		return ret;
	}
} // namespace Sessions
