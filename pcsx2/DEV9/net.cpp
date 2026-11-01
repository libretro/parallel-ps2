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

#include <stddef.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include "common/Pcsx2Defs.h"

#if defined(__POSIX__)
#include <pthread.h>
#endif

#include <retro_timers.h>
#include "net.h"
#include "DEV9.h"
#ifdef _WIN32
#endif
#ifdef HAVE_PCAP
#include "pcap_io.h"
#endif
#include "sockets.h"

#include "PacketReader/EthernetFrame.h"
#include "PacketReader/IP/IP_Packet.h"
#include "PacketReader/IP/UDP/UDP_Packet.h"
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

NetAdapter* nif;
static sthread_t* rx_thread = NULL;
void NetRxThread();
/* The receive thread raises its own priority at start: rthreads can only
 * raise the calling thread's, and this is the one place lrps2 wanted a
 * thread above normal. */
static void NetRxThreadEntry(void* arg)
{
	(void)arg;
	sthread_raise_current_priority();
	NetRxThread();
}

/* Packets from the host. The RX thread receives them into this ring and
 * net_rx_deliver() hands them to the SMAP on the EE thread, so the
 * emulated device is only ever touched by the thread that emulates it.
 * A record is a NetPacket's size field followed by its bytes, rounded up
 * to a word. The adapter receives straight into the ring and the SMAP
 * reads the record where it lies; only a record that would straddle the
 * end of a ring that is not mirrored goes through a copy. Records are
 * whole words, so the size field itself never straddles. */
#define NET_RX_RING_BYTES (64 * 1024)
#define NET_RX_RECORD_BYTES(size) (offsetof(NetPacket, buffer) + (((size_t)(size) + 3) & ~(size_t)3))
static retro_spsc_t s_rx_ring;
static bool s_rx_ring_ok;

/* RX pump thread loops on this; the control side flips it on
 * start/stop.  volatile gives neither atomicity nor ordering in
 * C++; use release/acquire atomics for the cross-thread handshake. */
static retro_atomic_int_t RxRunning;

/* The EE took records out of the ring (net_rx_deliver), or the RX thread
 * is to stop (TermNet): the RX thread waits on it for room. */
static retro_eventcount_t s_rx_room_ec;
static bool s_rx_room_ec_ok;

static bool rx_ring_has_room(size_t bytes)
{
	return retro_spsc_write_avail(&s_rx_ring) >= bytes;
}

void NetAdapter::wait_recv()
{
	retro_sleep(1);
}

//rx thread
void NetRxThread()
{
	NetPacket tmp;
	const size_t largest = NET_RX_RECORD_BYTES(sizeof(tmp.buffer));
	while (retro_atomic_load_acquire_int(&RxRunning))
	{
		bool full = false;

		/* Receive only with room for the largest record, so a packet
		 * taken from the host always fits; the rest wait in the host's
		 * own buffers while the EE catches up. */
		for (;;)
		{
			void* span;
			NetPacket* pkt;
			if (retro_spsc_write_begin(&s_rx_ring, &span) >= largest)
				pkt = (NetPacket*)span;
			else if (rx_ring_has_room(largest))
				pkt = &tmp; /* room, but split by the end of the ring */
			else
			{
				full = true;
				break;
			}
			if (!nif->recv(pkt))
				break;
			if (pkt->size <= 0 || pkt->size > (int)sizeof(pkt->buffer))
				continue;
			if (pkt == &tmp)
				retro_spsc_write(&s_rx_ring, &tmp, NET_RX_RECORD_BYTES(tmp.size));
			else
				retro_spsc_write_end(&s_rx_ring, NET_RX_RECORD_BYTES(pkt->size));
		}

		if (!full)
		{
			/* The host has nothing: until the adapter may have. */
			nif->wait_recv();
			continue;
		}

		/* No room: asleep until the EE takes records out. */
		{
			const int key = retro_eventcount_prepare_wait(&s_rx_room_ec);
			if (rx_ring_has_room(largest) || !retro_atomic_load_acquire_int(&RxRunning))
				retro_eventcount_cancel_wait(&s_rx_room_ec);
			else
				retro_eventcount_commit_wait(&s_rx_room_ec, key);
		}
	}
}

bool net_rx_deliver()
{
	NetPacket tmp;
	bool delivered = false;
	bool taken = false;

	if (nif == nullptr)
		return false;

	while (rx_fifo_can_rx())
	{
		const void* span;
		const size_t contiguous = retro_spsc_read_begin(&s_rx_ring, &span);
		if (contiguous)
		{
			int size;
			size_t bytes;
			memcpy(&size, span, sizeof(size));
			bytes = NET_RX_RECORD_BYTES(size);
			taken = true;
			if (contiguous >= bytes)
			{
				/* rx_process only reads the packet. */
				delivered |= rx_process((NetPacket*)const_cast<void*>(span));
				retro_spsc_read_end(&s_rx_ring, bytes);
				continue;
			}
			retro_spsc_read(&s_rx_ring, &tmp, bytes);
		}
		else if (!nif->InternalServerRecv(&tmp))
			break;
		delivered |= rx_process(&tmp);
	}
	/* Room for an RX thread waiting for it. */
	if (taken)
		retro_eventcount_notify(&s_rx_room_ec);
	return delivered;
}

void tx_put(NetPacket* pkt)
{
	if (nif != nullptr)
		nif->send(pkt);
	//pkt must be copied if its not processed by here, since it can be allocated on the callers stack
}

void ad_reset()
{
	if (nif != nullptr)
		nif->reset();
}

NetAdapter* GetNetAdapter()
{
	NetAdapter* na = nullptr;

	switch (EmuConfig.DEV9.EthApi)
	{
#ifdef _WIN32
		case Pcsx2Config::DEV9Options::NetApi::TAP:
			/* No TAP implementation is in the tree. */
			log_cb(RETRO_LOG_ERROR, "DEV9: TAP backend not present in this core, use the Sockets api\n");
			return 0;
#endif
		case Pcsx2Config::DEV9Options::NetApi::PCAP_Bridged:
		case Pcsx2Config::DEV9Options::NetApi::PCAP_Switched:
#ifdef HAVE_PCAP
			na = static_cast<NetAdapter*>(new PCAPAdapter());
			break;
#else
			log_cb(RETRO_LOG_ERROR, "DEV9: PCAP backend not built into this core, use the Sockets api\n");
			return 0;
#endif
		case Pcsx2Config::DEV9Options::NetApi::Sockets:
			na = static_cast<NetAdapter*>(new SocketAdapter());
			break;
		default:
			return 0;
	}

	if (!na->isInitialised())
	{
		delete na;
		return 0;
	}
	return na;
}

void InitNet()
{
	NetAdapter* na = GetNetAdapter();

	if (!na)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to GetNetAdapter()\n");
		EmuConfig.DEV9.EthEnable = false;
		return;
	}

	if (!s_rx_ring_ok)
		s_rx_ring_ok = retro_spsc_init_mirrored(&s_rx_ring, NET_RX_RING_BYTES);
	if (!s_rx_ring_ok)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to allocate the receive ring\n");
		delete na;
		EmuConfig.DEV9.EthEnable = false;
		return;
	}
	retro_spsc_clear(&s_rx_ring);
	if (!s_rx_room_ec_ok)
		s_rx_room_ec_ok = retro_eventcount_init(&s_rx_room_ec);
	if (!s_rx_room_ec_ok)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to set up the receive thread's wait\n");
		delete na;
		EmuConfig.DEV9.EthEnable = false;
		return;
	}

	nif = na;
	retro_atomic_store_release_int(&RxRunning, 1);

	rx_thread = sthread_create(NetRxThreadEntry, NULL);

}

void ReconfigureLiveNet(const Pcsx2Config& old_config)
{
	//Eth
	if (EmuConfig.DEV9.EthEnable)
	{
		if (old_config.DEV9.EthEnable)
		{
			//Reload Net if adapter changed
			if (EmuConfig.DEV9.EthDevice != old_config.DEV9.EthDevice ||
				EmuConfig.DEV9.EthApi != old_config.DEV9.EthApi)
			{
				TermNet();
				InitNet();
				return;
			}
			else
				nif->reloadSettings();
		}
		else
			InitNet();
	}
	else if (old_config.DEV9.EthEnable)
		TermNet();
}

void TermNet()
{
	if (retro_atomic_load_acquire_int(&RxRunning))
	{
		retro_atomic_store_release_int(&RxRunning, 0);
		retro_eventcount_notify(&s_rx_room_ec);
		nif->wake_recv();
		nif->close();
		log_cb(RETRO_LOG_INFO, "DEV9: Waiting for RX-net thread to terminate..\n");
		sthread_join(rx_thread);
		rx_thread = NULL;
		log_cb(RETRO_LOG_INFO, "DEV9: Done\n");

		delete nif;
		nif = nullptr;
	}
}

using namespace PacketReader;
using namespace PacketReader::IP;
using namespace PacketReader::IP::UDP;

const IP_Address NetAdapter::internalIP{{{192, 0, 2, 1}}};
const MAC_Address NetAdapter::broadcastMAC{{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};
const MAC_Address NetAdapter::internalMAC{{{0x76, 0x6D, 0xF4, 0x63, 0x30, 0x31}}};

NetAdapter::NetAdapter()
{
	//Ensure eeprom matches our default
	SetMACAddress(nullptr);
}

bool NetAdapter::send(NetPacket* pkt)
{
	return InternalServerSend(pkt);
}

NetAdapter::~NetAdapter()
{
}

void NetAdapter::InspectSend(NetPacket* pkt)
{
	if (EmuConfig.DEV9.EthLogDNS)
	{
		EthernetFrame frame(pkt);
		if (frame.protocol == (u16)EtherType::IPv4)
		{
			PayloadPtr* payload = static_cast<PayloadPtr*>(frame.GetPayload());
			IP_Packet ippkt(payload->data, payload->GetLength());

			if (ippkt.protocol == (u16)IP_Type::UDP)
			{
				IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
				UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

				if (udppkt.destinationPort == 53)
				{
					log_cb(RETRO_LOG_INFO, "DEV9: DNS: Packet Sent To %i.%i.%i.%i\n",
						ippkt.destinationIP.bytes[0], ippkt.destinationIP.bytes[1], ippkt.destinationIP.bytes[2], ippkt.destinationIP.bytes[3]);
					dnsLogger.InspectSend(&udppkt);
				}
			}
		}
	}
}
void NetAdapter::InspectRecv(NetPacket* pkt)
{
	if (EmuConfig.DEV9.EthLogDNS)
	{
		EthernetFrame frame(pkt);
		if (frame.protocol == (u16)EtherType::IPv4)
		{
			PayloadPtr* payload = static_cast<PayloadPtr*>(frame.GetPayload());
			IP_Packet ippkt(payload->data, payload->GetLength());

			if (ippkt.protocol == (u16)IP_Type::UDP)
			{
				IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
				UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

				if (udppkt.sourcePort == 53)
				{
					log_cb(RETRO_LOG_INFO, "DEV9: DNS: Packet Sent From %i.%i.%i.%i\n",
						ippkt.sourceIP.bytes[0], ippkt.sourceIP.bytes[1], ippkt.sourceIP.bytes[2], ippkt.sourceIP.bytes[3]);
					dnsLogger.InspectRecv(&udppkt);
				}
			}
		}
	}
}

void NetAdapter::SetMACAddress(MAC_Address* mac)
{
	if (mac == nullptr)
		ps2MAC = defaultMAC;
	else
		ps2MAC = *mac;

	*(MAC_Address*)&dev9.eeprom[0] = ps2MAC;

	//The checksum seems to be all the values of the mac added up in 16bit chunks
	dev9.eeprom[3] = (dev9.eeprom[0] + dev9.eeprom[1] + dev9.eeprom[2]) & 0xffff;
}

bool NetAdapter::VerifyPkt(NetPacket* pkt, int read_size)
{
	if ((*(MAC_Address*)&pkt->buffer[0] != ps2MAC) && (*(MAC_Address*)&pkt->buffer[0] != broadcastMAC))
	{
		//ignore strange packets
		return false;
	}

	if (*(MAC_Address*)&pkt->buffer[6] == ps2MAC)
	{
		//avoid pcap looping packets
		return false;
	}
	pkt->size = read_size;
	return true;
}

#ifdef _WIN32
void NetAdapter::InitInternalServer(PIP_ADAPTER_ADDRESSES adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOvveride)
#elif defined(__POSIX__)
void NetAdapter::InitInternalServer(ifaddrs* adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOvveride)
#endif
{
	if (adapter == nullptr)
		log_cb(RETRO_LOG_ERROR, "DEV9: InitInternalServer() got nullptr for adapter\n");

	dhcpOn = EmuConfig.DEV9.InterceptDHCP || dhcpForceEnable;
	if (dhcpOn)
		dhcpServer.Init(adapter, ipOverride, subnetOverride, gatewayOvveride);

	dnsServer.Init(adapter);
}

#ifdef _WIN32
void NetAdapter::ReloadInternalServer(PIP_ADAPTER_ADDRESSES adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOveride)
#elif defined(__POSIX__)
void NetAdapter::ReloadInternalServer(ifaddrs* adapter, bool dhcpForceEnable, IP_Address ipOverride, IP_Address subnetOverride, IP_Address gatewayOveride)
#endif
{
	if (adapter == nullptr)
		log_cb(RETRO_LOG_ERROR, "DEV9: ReloadInternalServer() got nullptr for adapter\n");

	dhcpOn = EmuConfig.DEV9.InterceptDHCP || dhcpForceEnable;
	if (dhcpOn)
		dhcpServer.Init(adapter, ipOverride, subnetOverride, gatewayOveride);

	dnsServer.Init(adapter);
}

bool NetAdapter::InternalServerRecv(NetPacket* pkt)
{
	IP_Payload* ippay;
	ippay = dhcpServer.Recv();
	if (ippay != nullptr)
	{
		IP_Packet* ippkt = new IP_Packet(ippay);
		ippkt->destinationIP = {{{255, 255, 255, 255}}};
		ippkt->sourceIP = internalIP;
		EthernetFrame frame(ippkt);
		frame.sourceMAC = internalMAC;
		frame.destinationMAC = ps2MAC;
		frame.protocol = (u16)EtherType::IPv4;
		frame.WritePacket(pkt);
		return true;
	}

	ippay = dnsServer.Recv();
	if (ippay != nullptr)
	{
		IP_Packet* ippkt = new IP_Packet(ippay);
		ippkt->destinationIP = ps2IP;
		ippkt->sourceIP = internalIP;
		EthernetFrame frame(ippkt);
		frame.sourceMAC = internalMAC;
		frame.destinationMAC = ps2MAC;
		frame.protocol = (u16)EtherType::IPv4;
		frame.WritePacket(pkt);
		InspectRecv(pkt);
		return true;
	}

	return false;
}

bool NetAdapter::InternalServerSend(NetPacket* pkt)
{
	EthernetFrame frame(pkt);
	if (frame.protocol == (u16)EtherType::IPv4)
	{
		PayloadPtr* payload = static_cast<PayloadPtr*>(frame.GetPayload());
		IP_Packet ippkt(payload->data, payload->GetLength());

		if (ippkt.protocol == (u16)IP_Type::UDP)
		{
			IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
			UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

			if (udppkt.destinationPort == 67)
			{
				//Send DHCP
				if (dhcpOn)
					return dhcpServer.Send(&udppkt);
			}
		}

		if (ippkt.destinationIP == internalIP)
		{
			if (ippkt.protocol == (u16)IP_Type::UDP)
			{
				ps2IP = ippkt.sourceIP;

				IP_PayloadPtr* ipPayload = static_cast<IP_PayloadPtr*>(ippkt.GetPayload());
				UDP_Packet udppkt(ipPayload->data, ipPayload->GetLength());

				if (udppkt.destinationPort == 53)
				{
					//Send DNS
					return dnsServer.Send(&udppkt);
				}
			}
			return true;
		}
	}
	return false;
}

