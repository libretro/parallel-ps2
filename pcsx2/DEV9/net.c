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
#include <string.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

#include "common/Pcsx2Defs.h"
#include "net.h"
#include "DEV9.h"
#include "PacketReader/netpkt.h"
#include "PacketReader/IP/UDP/DNS/DNS_Packet.h"

static netif* nif;
static sthread_t* rx_thread = NULL;
static void NetRxThread(void);
/* The receive thread raises its own priority at start: rthreads can only
 * raise the calling thread's, and this is the one place lrps2 wanted a
 * thread above normal. */
static void NetRxThreadEntry(void* arg)
{
	(void)arg;
	sthread_raise_current_priority();
	NetRxThread();
}

/* Packets from the host. The network thread receives them into this ring and
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

/* What the PS2 sends, the other way: the EE writes records of the same
 * form (tx_put) and the network thread sends them. A size of -1 is an
 * adapter reset (ad_reset), with no bytes. The network thread is the one
 * thread that touches the adapter, its sessions and the internal DHCP
 * and DNS servers once it runs, so none of them needs a lock. A full
 * ring drops the frame, as a full wire would: the EE never waits for
 * the host. */
#define NET_TX_RING_BYTES (256 * 1024)
#define NET_TX_RESET (-1)
static retro_spsc_t s_tx_ring;
static bool s_tx_ring_ok;
static unsigned s_tx_dropped; /* EE thread only */

/* The network thread loops on this; the control side flips it on
 * start/stop. */
static retro_atomic_int_t RxRunning;

/* The EE took records out of the receive ring (net_rx_deliver), put one
 * in the transmit ring, or the network thread is to stop (TermNet): the
 * network thread waits on it when the receive ring is full. */
static retro_eventcount_t s_rx_room_ec;
static bool s_rx_room_ec_ok;

static bool rx_ring_has_room(size_t bytes)
{
	return retro_spsc_write_avail(&s_rx_ring) >= bytes;
}

/* Network thread: send what the EE queued, in order, through the
 * thread's one packet buffer. */
static void net_tx_drain(NetPacket* pkt)
{
	const size_t head = offsetof(NetPacket, buffer);
	while (retro_spsc_read_avail(&s_tx_ring) >= head)
	{
		int    size;
		size_t bytes;
		retro_spsc_peek(&s_tx_ring, &size, sizeof(size));
		bytes = NET_RX_RECORD_BYTES(size < 0 ? 0 : size);
		/* Whole records only: the EE writes one in parts. */
		if (retro_spsc_read_avail(&s_tx_ring) < bytes)
			break;
		retro_spsc_read(&s_tx_ring, pkt, bytes);
		if (size == NET_TX_RESET)
			nif->ops->reset(nif);
		else
			nif->ops->send(nif, pkt);
	}
}

/* EE thread: queue a record -- the size field, then the frame's one or
 * two runs, padded to the record's length -- and wake the network
 * thread. */
static void net_tx_push(int size, const void* data, size_t len, const void* more, size_t more_len)
{
	static const char pad[8] = {0};
	const size_t head  = offsetof(NetPacket, buffer);
	const size_t bytes = NET_RX_RECORD_BYTES(len + more_len);
	char field[offsetof(NetPacket, buffer)];
	if (!s_tx_ring_ok || !retro_atomic_load_acquire_int(&RxRunning))
		return;
	if (retro_spsc_write_avail(&s_tx_ring) < bytes)
	{
		s_tx_dropped++;
		return;
	}
	memset(field, 0, sizeof(field));
	memcpy(field, &size, sizeof(size));
	retro_spsc_write(&s_tx_ring, field, head);
	if (len)
		retro_spsc_write(&s_tx_ring, data, len);
	if (more_len)
		retro_spsc_write(&s_tx_ring, more, more_len);
	if (bytes > head + len + more_len)
		retro_spsc_write(&s_tx_ring, pad, bytes - head - len - more_len);
	retro_eventcount_notify(&s_rx_room_ec);
	if (nif->ops->wake_recv)
		nif->ops->wake_recv(nif);
}

/* The adapter's wait, or a millisecond's poll where it has none. */
static void net_wait_recv(void)
{
	if (nif->ops->wait_recv)
		nif->ops->wait_recv(nif);
	else
		retro_sleep(1);
}

/* The network thread: sends what the EE queued, receives from the host
 * and from the internal servers into the receive ring, and sleeps in the
 * adapter's wait when there is neither. */
static void NetRxThread(void)
{
	NetPacket tmp;
	const size_t largest = NET_RX_RECORD_BYTES(sizeof(tmp.buffer));
	while (retro_atomic_load_acquire_int(&RxRunning))
	{
		bool full = false;

		net_tx_drain(&tmp);

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
			if (!nif->ops->recv(nif, pkt) && !netif_internal_recv(nif, pkt))
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
			/* The host has nothing: until the adapter may have, or the
			 * EE sends (net_tx_push wakes the wait). */
			if (!retro_spsc_read_avail(&s_tx_ring))
				net_wait_recv();
			continue;
		}

		/* No room: asleep until the EE takes records out or sends. */
		{
			const int key = retro_eventcount_prepare_wait(&s_rx_room_ec);
			if (rx_ring_has_room(largest) || retro_spsc_read_avail(&s_tx_ring)
			 || !retro_atomic_load_acquire_int(&RxRunning))
				retro_eventcount_cancel_wait(&s_rx_room_ec);
			else
				retro_eventcount_commit_wait(&s_rx_room_ec, key);
		}
	}
}

bool net_rx_deliver(void)
{
	NetPacket tmp;
	bool delivered = false;
	bool taken = false;

	if (nif == NULL)
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
				delivered |= rx_process((NetPacket*)(void*)span);
				retro_spsc_read_end(&s_rx_ring, bytes);
				continue;
			}
			retro_spsc_read(&s_rx_ring, &tmp, bytes);
		}
		else
			break;
		delivered |= rx_process(&tmp);
	}
	/* Room for a network thread waiting for it. */
	if (taken)
		retro_eventcount_notify(&s_rx_room_ec);
	return delivered;
}

void tx_put(const void* head, int head_len, const void* tail, int tail_len)
{
	const int size = head_len + tail_len;
	if (nif != NULL && head_len >= 0 && tail_len >= 0 && size > 0 && size <= (int)sizeof(((NetPacket*)0)->buffer))
		net_tx_push(size, head, (size_t)head_len, tail, (size_t)tail_len);
}

void ad_reset(void)
{
	if (nif != NULL)
		net_tx_push(NET_TX_RESET, NULL, 0, NULL, 0);
}

/* The control side: start and stop the network thread. While it is
 * stopped the adapter is the caller's to touch. */
static void net_thread_start(void)
{
	retro_atomic_store_release_int(&RxRunning, 1);
	rx_thread = sthread_create(NetRxThreadEntry, NULL);
}

static void net_thread_stop(void)
{
	retro_atomic_store_release_int(&RxRunning, 0);
	retro_eventcount_notify(&s_rx_room_ec);
	if (nif->ops->wake_recv)
		nif->ops->wake_recv(nif);
	log_cb(RETRO_LOG_INFO, "DEV9: Waiting for the network thread to terminate..\n");
	sthread_join(rx_thread);
	rx_thread = NULL;
	if (s_tx_dropped)
		log_cb(RETRO_LOG_WARN, "DEV9: %u frames dropped: the network thread fell behind\n", s_tx_dropped);
	s_tx_dropped = 0;
}

static netif* GetNetAdapter(void)
{
	switch (EmuConfig.DEV9.EthApi)
	{
#ifdef _WIN32
		case EMU_NETAPI_TAP:
			/* No TAP implementation is in the tree. */
			log_cb(RETRO_LOG_ERROR, "DEV9: TAP backend not present in this core, use the Sockets api\n");
			return NULL;
#endif
		case EMU_NETAPI_PCAP_BRIDGED:
		case EMU_NETAPI_PCAP_SWITCHED:
#ifdef HAVE_PCAP
			return net_pcap_new();
#else
			log_cb(RETRO_LOG_ERROR, "DEV9: PCAP backend not built into this core, use the Sockets api\n");
			return NULL;
#endif
		case EMU_NETAPI_SOCKETS:
			return net_sockets_new();
		default:
			return NULL;
	}
}

static void net_init_failed(netif* na, const char* what)
{
	log_cb(RETRO_LOG_ERROR, "DEV9: %s\n", what);
	if (na)
		na->ops->destroy(na);
	EmuConfig.DEV9.EthEnable = false;
}

void InitNet(void)
{
	netif* na = GetNetAdapter();
	if (!na)
	{
		net_init_failed(NULL, "Failed to GetNetAdapter()");
		return;
	}
	if (!s_rx_ring_ok)
		s_rx_ring_ok = retro_spsc_init_mirrored(&s_rx_ring, NET_RX_RING_BYTES);
	if (!s_rx_ring_ok)
	{
		net_init_failed(na, "Failed to allocate the receive ring");
		return;
	}
	if (!s_tx_ring_ok)
		s_tx_ring_ok = retro_spsc_init(&s_tx_ring, NET_TX_RING_BYTES);
	if (!s_tx_ring_ok)
	{
		net_init_failed(na, "Failed to allocate the transmit ring");
		return;
	}
	if (!s_rx_room_ec_ok)
		s_rx_room_ec_ok = retro_eventcount_init(&s_rx_room_ec);
	if (!s_rx_room_ec_ok)
	{
		net_init_failed(na, "Failed to set up the receive thread's wait");
		return;
	}
	retro_spsc_clear(&s_rx_ring);
	retro_spsc_clear(&s_tx_ring);
	nif = na;
	net_thread_start();
}

void ReconfigureLiveNet(const Pcsx2Config* old_config)
{
	if (EmuConfig.DEV9.EthEnable)
	{
		if (old_config->DEV9.EthEnable)
		{
			/* Reload the net if the adapter changed */
			if (strcmp(EmuConfig.DEV9.EthDevice, old_config->DEV9.EthDevice) != 0 ||
				EmuConfig.DEV9.EthApi != old_config->DEV9.EthApi)
			{
				TermNet();
				InitNet();
				return;
			}
			/* The adapter belongs to the network thread while it runs. */
			net_thread_stop();
			nif->ops->reload(nif);
			net_thread_start();
		}
		else
			InitNet();
	}
	else if (old_config->DEV9.EthEnable)
		TermNet();
}

void TermNet(void)
{
	if (retro_atomic_load_acquire_int(&RxRunning))
	{
		/* The thread first: then the adapter is this thread's to close. */
		net_thread_stop();
		nif->ops->close(nif);
		log_cb(RETRO_LOG_INFO, "DEV9: Done\n");
		nif->ops->destroy(nif);
		nif = NULL;
	}
}

const u8 net_internal_ip[4] = {192, 0, 2, 1};
const u8 net_internal_mac[6] = {0x76, 0x6D, 0xF4, 0x63, 0x30, 0x31};
const u8 net_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const u8 net_default_mac[6] = {0x00, 0x04, 0x1F, 0x82, 0x30, 0x31};
static const u8 limited_broadcast[4] = {255, 255, 255, 255};

void netif_init(netif* n, const netif_ops* ops)
{
	memset(n, 0, sizeof(*n));
	n->ops = ops;
	n->dns = dns_server_new();
	/* The EEPROM matches the default until the adapter sets one */
	netif_set_mac(n, NULL);
}

void netif_deinit(netif* n)
{
	dns_server_free(n->dns);
	n->dns = NULL;
}

void netif_set_mac(netif* n, const u8* mac)
{
	memcpy(n->ps2_mac, mac ? mac : net_default_mac, 6);
	if (!dev9.eeprom)
		return;
	memcpy(dev9.eeprom, n->ps2_mac, 6);
	/* The checksum: the MAC's three 16-bit words added up */
	dev9.eeprom[3] = (u16)((dev9.eeprom[0] + dev9.eeprom[1] + dev9.eeprom[2]) & 0xffff);
}

bool netif_verify(netif* n, NetPacket* pkt, int size)
{
	const u8* f = (const u8*)pkt->buffer;
	if (size < ETH_HEADER)
		return false;
	/* Strange packets, not for the PS2 */
	if (memcmp(f, n->ps2_mac, 6) != 0 && memcmp(f, net_broadcast_mac, 6) != 0)
		return false;
	/* pcap looping the PS2's own packets back */
	if (memcmp(f + 6, n->ps2_mac, 6) == 0)
		return false;
	pkt->size = size;
	return true;
}

/* The DNS packet a frame carries, to or from port 53. */
static void inspect(const NetPacket* pkt, bool sent)
{
	net_eth eth;
	net_ip ip;
	net_udp udp;
	const u8* addr;
	if (!EmuConfig.DEV9.EthLogDNS)
		return;
	if (!net_eth_parse((const u8*)pkt->buffer, pkt->size, &eth) || eth.type != ETHERTYPE_IPV4 ||
		!net_ip_parse(eth.data, eth.len, &ip) || ip.proto != IP_PROTO_UDP ||
		!net_udp_parse(ip.data, ip.len, &udp) || (sent ? udp.dport : udp.sport) != DNS_PORT)
		return;
	addr = sent ? ip.dst : ip.src;
	log_cb(RETRO_LOG_INFO, "DEV9: DNS: Packet Sent %s %u.%u.%u.%u\n", sent ? "To" : "From",
		addr[0], addr[1], addr[2], addr[3]);
	dns_log(udp.data, udp.len);
}

void netif_inspect_send(netif* n, const NetPacket* pkt)
{
	(void)n;
	inspect(pkt, true);
}

void netif_inspect_recv(netif* n, const NetPacket* pkt)
{
	(void)n;
	inspect(pkt, false);
}

void netif_init_servers(netif* n, const net_adapter* adapter, bool dhcp_force,
	const u8* ip, const u8* subnet, const u8* gateway)
{
	if (adapter == NULL)
		log_cb(RETRO_LOG_ERROR, "DEV9: InitInternalServer() got nullptr for adapter\n");
	n->dhcp_on = EmuConfig.DEV9.InterceptDHCP || dhcp_force;
	if (n->dhcp_on)
		dhcp_server_init(&n->dhcp, adapter, net_internal_ip, ip, subnet, gateway);
	if (n->dns)
		dns_server_set_adapter(n->dns, adapter);
}

/* A UDP reply from the internal servers, its data already at
 * NET_UDP_DATA: the headers in front of it. */
static void internal_reply(netif* n, NetPacket* pkt, const u8 dst[4], u16 sport, u16 dport, int len)
{
	u8* f = (u8*)pkt->buffer;
	net_put_udp(f, net_internal_ip, dst, sport, dport, len);
	net_put_ip(f, net_internal_ip, dst, IP_PROTO_UDP, 64, 0, UDP_HEADER + len);
	net_put_eth(f, n->ps2_mac, net_internal_mac, ETHERTYPE_IPV4);
	pkt->size = NET_UDP_DATA + len;
}

bool netif_internal_recv(netif* n, NetPacket* pkt)
{
	u8* data = (u8*)pkt->buffer + NET_UDP_DATA;
	const int cap = ETH_FRAME_MAX - NET_UDP_DATA;
	u16 port;
	int len = n->dhcp_on ? dhcp_server_recv(&n->dhcp, data, cap) : 0;
	if (len > 0)
	{
		internal_reply(n, pkt, limited_broadcast, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, len);
		return true;
	}
	len = n->dns ? dns_server_recv(n->dns, data, cap, &port) : 0;
	if (len > 0)
	{
		internal_reply(n, pkt, n->ps2_ip, DNS_PORT, port, len);
		netif_inspect_recv(n, pkt);
		return true;
	}
	return false;
}

bool netif_internal_send(netif* n, NetPacket* pkt)
{
	net_eth eth;
	net_ip ip;
	net_udp udp;
	bool is_udp;
	if (!net_eth_parse((const u8*)pkt->buffer, pkt->size, &eth) || eth.type != ETHERTYPE_IPV4 ||
		!net_ip_parse(eth.data, eth.len, &ip))
		return false;
	is_udp = ip.proto == IP_PROTO_UDP && net_udp_parse(ip.data, ip.len, &udp);
	if (is_udp && udp.dport == DHCP_SERVER_PORT && n->dhcp_on)
		return dhcp_server_send(&n->dhcp, udp.data, udp.len);
	if (memcmp(ip.dst, net_internal_ip, 4) != 0)
		return false;
	if (is_udp)
	{
		memcpy(n->ps2_ip, ip.src, 4);
		if (udp.dport == DNS_PORT)
			return n->dns && dns_server_send(n->dns, udp.data, udp.len, udp.sport);
	}
	return true;
}
