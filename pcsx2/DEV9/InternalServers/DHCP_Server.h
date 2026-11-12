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


#include "DEV9/SimpleQueue.h"
#include "DEV9/PacketReader/IP/IP_Address.h"
#include "DEV9/PacketReader/IP/UDP/UDP_Packet.h"
#include "DEV9/AdapterUtils.h"

namespace InternalServers
{
	class DHCP_Server
	{
	public:
		PacketReader::IP::IP_Address ps2IP;
		PacketReader::IP::IP_Address gateway;
		PacketReader::IP::IP_Address broadcastIP;

	private:

		PacketReader::IP::IP_Address dns1{};
		PacketReader::IP::IP_Address dns2{};
		PacketReader::IP::IP_Address netmask{};

		SimpleQueue<PacketReader::IP::UDP::UDP_Packet*> recvBuff;

		u16 maxMs = 576;

	public:
		DHCP_Server();

		void Init(const net_adapter* adapter, PacketReader::IP::IP_Address ipOverride = {}, PacketReader::IP::IP_Address subnetOverride = {}, PacketReader::IP::IP_Address gatewayOvveride = {});

		PacketReader::IP::UDP::UDP_Packet* Recv();
		bool Send(PacketReader::IP::UDP::UDP_Packet* payload);

		~DHCP_Server();

	private:
		void AutoDNS(const net_adapter* adapter, bool autoDNS1, bool autoDNS2);
		void AutoBroadcast(PacketReader::IP::IP_Address parPS2IP, PacketReader::IP::IP_Address parNetmask);
	};
} // namespace InternalServers
