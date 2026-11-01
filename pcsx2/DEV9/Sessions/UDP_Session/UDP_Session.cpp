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

#include "common/Pcsx2Defs.h"

#ifdef __POSIX__
#define SOCKET_ERROR -1
#include <errno.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#endif

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#include <retro_atomic.h>
#include <features/features_cpu.h>
#include "UDP_Session.h"
#include "DEV9/PacketReader/IP/UDP/UDP_Packet.h"

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::UDP;


namespace Sessions
{
	const int64_t UDP_Session::MAX_IDLE_USEC = INT64_C(120) * 1000000; //See RFC 4787 Section 4.3

	//TODO, figure out handling of multicast

	UDP_Session::UDP_Session(ConnectionKey parKey, IP_Address parAdapterIP)
		: UDP_BaseSession(parKey, parAdapterIP)
		, isBroadcast(false)
		, isFixedPort(false)
		, deathClockStart{(int64_t)cpu_features_get_time_usec()}
	{
	}

#ifdef _WIN32
	UDP_Session::UDP_Session(ConnectionKey parKey, IP_Address parAdapterIP, bool parIsBroadcast, bool parIsMulticast, SOCKET parClient)
#elif defined(__POSIX__)
	UDP_Session::UDP_Session(ConnectionKey parKey, IP_Address parAdapterIP, bool parIsBroadcast, bool parIsMulticast, int parClient)
#endif
		: UDP_BaseSession(parKey, parAdapterIP)
		, open(true)
		, client(parClient)
		, srcPort(parKey.ps2Port)
		, destPort(parKey.srvPort)
		, isBroadcast(parIsBroadcast)
		, isMulticast(parIsMulticast)
		, isFixedPort(true)
		, deathClockStart{(int64_t)cpu_features_get_time_usec()}
	{
	}

	/* A fixed port's session receives through the port; its idle clock
	 * is minutes, which the receive thread's bounded wait covers. */
	bool UDP_Session::PollInterest(uintptr_t* fd, int* events)
	{
		*events = 0;
		if (!open || isFixedPort)
			return true;
		*fd     = (uintptr_t)client;
		*events = SESSION_POLL_READ;
		return true;
	}

	IP_Payload* UDP_Session::Recv()
	{
		if (!open)
			return nullptr;

		if (isFixedPort)
		{
			if (cpu_features_get_time_usec() - retro_atomic_load_acquire_64(&deathClockStart) > MAX_IDLE_USEC)
			{
				CloseSocket();
				log_cb(RETRO_LOG_INFO, "DEV9: UDP: UDPFixed Max Idle Reached\n");
				RaiseEventConnectionClosed();
			}
			return nullptr;
		}

		int ret;
		fd_set sReady;
		fd_set sExcept;

		timeval nowait{0};
		FD_ZERO(&sReady);
		FD_ZERO(&sExcept);
		FD_SET(client, &sReady);
		FD_SET(client, &sExcept);
		ret = select(client + 1, &sReady, nullptr, &sExcept, &nowait);

		bool hasData;
		if (ret == SOCKET_ERROR)
		{
			hasData = false;
			log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Select Failed. Error Code: %d\n",
#ifdef _WIN32
				WSAGetLastError());
#elif defined(__POSIX__)
				errno);
#endif
		}
		else if (FD_ISSET(client, &sExcept))
		{
			hasData = false;

			int error = 0;
#ifdef _WIN32
			int len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Unkown UDP Connection Error (getsockopt Error: %d)\n", WSAGetLastError());
#elif defined(__POSIX__)
			socklen_t len = sizeof(error);
			if (getsockopt(client, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Unkown UDP Connection Error (getsockopt Error: %d)\n", errno);
#endif
			else
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Recv Error: %d\n", error);
		}
		else
			hasData = FD_ISSET(client, &sReady);

		if (hasData)
		{
			u_long available = 0;
			PayloadData* recived = nullptr;
			std::unique_ptr<u8[]> buffer;
			sockaddr endpoint{0};

			//FIONREAD returns total size of all available messages
			//but we will read one message at a time
#ifdef _WIN32
			ret = ioctlsocket(client, FIONREAD, &available);
#elif defined(__POSIX__)
			ret = ioctl(client, FIONREAD, &available);
#endif
			if (ret != SOCKET_ERROR)
			{
				buffer = std::make_unique<u8[]>(available);

#ifdef _WIN32
				int fromlen = sizeof(endpoint);
#elif defined(__POSIX__)
				socklen_t fromlen = sizeof(endpoint);
#endif
				ret = recvfrom(client, (char*)buffer.get(), available, 0, &endpoint, &fromlen);
			}

			if (ret == SOCKET_ERROR)
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Recv Error: %d\n",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif
				RaiseEventConnectionClosed();
				return nullptr;
			}

			recived = new PayloadData(ret);
			memcpy(recived->data.get(), buffer.get(), ret);

			UDP_Packet* iRet = new UDP_Packet(recived);
			iRet->destinationPort = srcPort;
			iRet->sourcePort = destPort;

			retro_atomic_store_release_64(&deathClockStart, (int64_t)cpu_features_get_time_usec());

			return iRet;
		}

		if (cpu_features_get_time_usec() - retro_atomic_load_acquire_64(&deathClockStart) > MAX_IDLE_USEC)
		{
			//CloseSocket();
			log_cb(RETRO_LOG_INFO, "DEV9: UDP: Max Idle Reached\n");
			RaiseEventConnectionClosed();
		}

		return nullptr;
	}

	bool UDP_Session::WillRecive(IP_Address parDestIP)
	{
		if (!open)
			return false;

		if (isBroadcast || (parDestIP == destIP))
		{
			retro_atomic_store_release_64(&deathClockStart, (int64_t)cpu_features_get_time_usec());
			return true;
		}
		return false;
	}

	bool UDP_Session::Send(PacketReader::IP::IP_Payload* payload)
	{
		retro_atomic_store_release_64(&deathClockStart, (int64_t)cpu_features_get_time_usec());

		IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(payload);
		UDP_Packet udp(ipPayload->data, ipPayload->GetLength());

		if (destPort != 0)
		{
			//client already created
			if (!(udp.destinationPort == destPort && udp.sourcePort == srcPort))
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Packet invalid for current session (Duplicate key?)\n");
				return false;
			}
		}
		else
		{
			//create client
			destPort = udp.destinationPort;
			srcPort = udp.sourcePort;

			//Multicast address start with 0b1110
			if ((destIP.bytes[0] & 0xF0) == 0xE0)
			{
				isMulticast = true;
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Unexpected Multicast Connection\n");
			}

			int ret;
			client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			if (client == INVALID_SOCKET)
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to open socket. Error: %d\n",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif
				RaiseEventConnectionClosed();
				return false;
			}

			const int reuseAddress = true; //BOOL
			ret = setsockopt(client, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddress, sizeof(reuseAddress));

			if (ret == SOCKET_ERROR)
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to set SO_REUSEADDR. Error: %d\n",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif

			if (adapterIP.integer != 0)
			{
				sockaddr_in endpoint{0};
				endpoint.sin_family = AF_INET;
				*(IP_Address*)&endpoint.sin_addr = adapterIP;

				ret = bind(client, (const sockaddr*)&endpoint, sizeof(endpoint));

				if (ret == SOCKET_ERROR)
					log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to bind socket. Error: %d\n",
#ifdef _WIN32
						WSAGetLastError());
#elif defined(__POSIX__)
						errno);
#endif
			}

			sockaddr_in endpoint{0};
			endpoint.sin_family = AF_INET;
			*(IP_Address*)&endpoint.sin_addr = destIP;
			endpoint.sin_port = htons(destPort);

			ret = connect(client, (const sockaddr*)&endpoint, sizeof(endpoint));

			if (ret == SOCKET_ERROR)
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to connect socket. Error: %d\n",
#ifdef _WIN32
					WSAGetLastError());
#elif defined(__POSIX__)
					errno);
#endif
				RaiseEventConnectionClosed();
				return false;
			}

			if (srcPort != 0)
				retro_atomic_store_release_int(&open, 1);
		}

		PayloadPtr* udpPayload = static_cast<PayloadPtr*>(udp.GetPayload());

		//Send
		int ret = SOCKET_ERROR;
		if (isBroadcast)
		{
			sockaddr_in endpoint{0};
			endpoint.sin_family = AF_INET;
			endpoint.sin_addr.s_addr = INADDR_BROADCAST;
			endpoint.sin_port = htons(destPort);

			ret = sendto(client, (const char*)udpPayload->data, udpPayload->GetLength(), 0, (const sockaddr*)&endpoint, sizeof(endpoint));
		}
		else if (isMulticast | isFixedPort)
		{
			sockaddr_in endpoint{0};
			endpoint.sin_family = AF_INET;
			*(IP_Address*)&endpoint.sin_addr = destIP;
			endpoint.sin_port = htons(destPort);

			ret = sendto(client, (const char*)udpPayload->data, udpPayload->GetLength(), 0, (const sockaddr*)&endpoint, sizeof(endpoint));
		}
		else
			ret = send(client, (const char*)udpPayload->data, udpPayload->GetLength(), 0);

		if (ret == SOCKET_ERROR)
		{
#ifdef _WIN32
			ret = WSAGetLastError();
#elif defined(__POSIX__)
			ret = errno;
#endif
			log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Send Error %d\n", ret);

			//We can recive an ICMP Port Unreacable error, which can get raised in send (and maybe sendto?)
			//On Windows this an WSAECONNRESET error, although I've not been able to reproduce in testing
			//On Linux this is an ECONNREFUSED error (Testing needed to confirm full behaviour)

			//The decision to ignore the error and retry was made to allow R&C Deadlock ressurection team to packet capture eveything
#ifdef _WIN32
			if (ret == WSAECONNRESET)
#elif defined(__POSIX__)
			if (ret == ECONNREFUSED)
#endif
			{
				if (isFixedPort)
				{
					sockaddr_in endpoint{0};
					endpoint.sin_family = AF_INET;
					*(IP_Address*)&endpoint.sin_addr = destIP;
					endpoint.sin_port = htons(destPort);

					ret = sendto(client, (const char*)udpPayload->data, udpPayload->GetLength(), 0, (const sockaddr*)&endpoint, sizeof(endpoint));
				}
				else
					//Do we need to clear the error somehow?
					ret = send(client, (const char*)udpPayload->data, udpPayload->GetLength(), 0);

				if (ret == SOCKET_ERROR)
				{
					log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Send Error (Second attempt) %d\n",
#ifdef _WIN32
						WSAGetLastError());
#elif defined(__POSIX__)
						errno);
#endif
					return false;
				}
			}
			else
			{
				RaiseEventConnectionClosed();
				return false;
			}
		}

		if (srcPort == 0)
			RaiseEventConnectionClosed();

		return true;
	}

	void UDP_Session::CloseSocket()
	{
		retro_atomic_store_release_int(&open, 0);
		if (!isFixedPort && client != INVALID_SOCKET)
		{
#ifdef _WIN32
			closesocket(client);
#elif defined(__POSIX__)
			::close(client);
#endif
			client = INVALID_SOCKET;
		}
	}

	void UDP_Session::Reset()
	{
		//CloseSocket();
		RaiseEventConnectionClosed();
	}

	UDP_Session::~UDP_Session()
	{
		CloseSocket();
	}
} // namespace Sessions
