/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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
#include <stdlib.h>
#include <string>
#include <cstring>


#include "Config.h"
#include "netio.h"

#include "PacketReader/MAC_Address.h"
#include "PacketReader/IP/IP_Address.h"
#include "AdapterUtils.h"
#include "InternalServers/DHCP_Server.h"
#include "InternalServers/DNS_Server.h"

struct ConfigDEV9;

// first three recognized by Xlink as Sony PS2
const PacketReader::MAC_Address defaultMAC = {{{0x00, 0x04, 0x1F, 0x82, 0x30, 0x31}}};


struct AdapterEntry
{
	enum EmuNetApi type;
	//UTF8
	std::string name;
	std::string guid;
};

enum struct AdapterOptions : int
{
	None = 0,
	DHCP_ForcedOn = 1 << 0,
	DHCP_OverrideIP = 1 << 1,
	DHCP_OverideSubnet = 1 << 2,
	DHCP_OverideGateway = 1 << 3,
};

constexpr enum AdapterOptions operator|(const enum AdapterOptions selfValue, const enum AdapterOptions inValue)
{
	return (enum AdapterOptions)(int(selfValue) | int(inValue));
}
constexpr enum AdapterOptions operator&(const enum AdapterOptions selfValue, const enum AdapterOptions inValue)
{
	return (enum AdapterOptions)(int(selfValue) & int(inValue));
}

class NetAdapter
{
public:
	static const PacketReader::IP::IP_Address internalIP;

protected:
	PacketReader::MAC_Address ps2MAC;
	static const PacketReader::MAC_Address broadcastMAC;
	static const PacketReader::MAC_Address internalMAC;

private:
	//Only set if packet sent to the internal IP address
	PacketReader::IP::IP_Address ps2IP{};

	bool dhcpOn = false;

protected:
	InternalServers::DHCP_Server dhcpServer;
	dns_server* dnsServer;

public:
	NetAdapter();
	/* Once the network thread runs (net.cpp), everything below but
	 * wake_recv is called on it alone, and on the control side only
	 * while it is stopped: nothing here takes a lock. */
	virtual bool isInitialised() = 0;
	virtual bool recv(NetPacket* pkt) = 0; /* a packet from the host */
	/* Once recv has nothing: return when it may have
	 * something, or after a bound. This default polls, every millisecond;
	 * an adapter that can sleep on its host handles does. */
	virtual void wait_recv();
	/* Ends a wait_recv early, from any thread: what recv would find, or
	 * whether it is called at all, may have changed. */
	virtual void wake_recv() {}
	virtual bool send(NetPacket* pkt); /* a packet the PS2 sent, to the host */
	virtual void reset(){};
	virtual void reloadSettings() = 0;
	virtual void close(){};
	virtual ~NetAdapter();

protected:
	void SetMACAddress(PacketReader::MAC_Address* mac);
	bool VerifyPkt(NetPacket* pkt, int read_size);

	void InspectRecv(NetPacket* pkt);
	void InspectSend(NetPacket* pkt);

	void InitInternalServer(const net_adapter* adapter, bool dhcpForceEnable = false, PacketReader::IP::IP_Address ipOverride = {}, PacketReader::IP::IP_Address subnetOverride = {}, PacketReader::IP::IP_Address gatewayOveride = {});
	void ReloadInternalServer(const net_adapter* adapter, bool dhcpForceEnable = false, PacketReader::IP::IP_Address ipOverride = {}, PacketReader::IP::IP_Address subnetOverride = {}, PacketReader::IP::IP_Address gatewayOveride = {});

public:
	/* The internal DHCP and DNS servers' replies, polled by the network
	 * thread, which is their queues' one consumer. */
	bool InternalServerRecv(NetPacket* pkt);

private:
	bool InternalServerSend(NetPacket* pkt);
};

