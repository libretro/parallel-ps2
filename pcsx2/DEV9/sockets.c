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

#include <stdlib.h>
#include <string.h>

#include <retro_timers.h>
#include "common/Pcsx2Defs.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
typedef WSAPOLLFD rx_pollfd;
#define RX_POLL(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
typedef struct pollfd rx_pollfd;
#define RX_POLL(fds, n, ms) poll((fds), (nfds_t)(n), (ms))
#endif

#include "sockets.h"
#include "AdapterUtils.h"
#include "DEV9.h"

/* The receive thread's sleep, bounded: what no socket says, a session's
 * idle clock, is minutes. */
#define RX_WAIT_MS 100
/* Sockets one poll takes; past that, sessions are polled as before. */
#define RX_POLL_MAX 64

static const u8 any_ip[4] = {0, 0, 0, 0};
static const u8 limited_broadcast[4] = {255, 255, 255, 255};

static bool key_equal(const net_key* a, const net_key* b)
{
	return memcmp(a->ip, b->ip, 4) == 0 && a->proto == b->proto &&
		   a->ps2_port == b->ps2_port && a->srv_port == b->srv_port;
}

net_session* sockets_find(sockets_adapter* a, const net_key* key)
{
	int i;
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* s = &a->sessions[i];
		if (s->used && !s->closed && key_equal(&s->key, key))
			return s;
	}
	return NULL;
}

net_session* sockets_session_array(sockets_adapter* a)
{
	return a->sessions;
}

icmp_ping* sockets_ping_alloc(sockets_adapter* a, net_session* s)
{
	int i;
	for (i = 0; i < NET_PINGS; i++)
	{
		icmp_ping* p = &a->pings[i];
		if (p->used)
			continue;
		p->used = true;
		p->session = s;
#ifdef _WIN32
		p->file = INVALID_HANDLE_VALUE;
		p->event = NULL;
#else
		p->sock = -1;
#endif
		return p;
	}
	return NULL;
}

void sockets_ping_free(icmp_ping* p)
{
	p->used = false;
	p->session = NULL;
}

/* A slot for a new session, toward ip from the PS2. */
static net_session* session_new(sockets_adapter* a, const net_key* key, const u8 remote_ip[4])
{
	int i;
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* s = &a->sessions[i];
		if (s->used)
			continue;
		memset(s, 0, sizeof(*s));
		s->used = true;
		s->owner = a;
		s->key = *key;
		memcpy(s->remote_ip, remote_ip, 4);
		memcpy(s->ps2_ip, a->base.dhcp.ps2_ip, 4);
		memcpy(s->adapter_ip, a->adapter_ip, 4);
		return s;
	}
	log_cb(RETRO_LOG_ERROR, "DEV9: Socket: %d connections open, refusing another\n", NET_SESSIONS);
	return NULL;
}

/* Sessions that ended, freed now that nothing of theirs is running. */
static void reap(sockets_adapter* a)
{
	int i;
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* s = &a->sessions[i];
		if (!s->used || !s->closed)
			continue;
		s->ops->release(s);
		s->used = false;
		switch (s->key.proto)
		{
			case IP_PROTO_UDP:
				if (s->key.srv_port == 0 && memcmp(s->key.ip, any_ip, 4) == 0)
					log_cb(RETRO_LOG_INFO, "DEV9: Socket: Closed Dead UDP Fixed Port to %d\n", s->key.ps2_port);
				else
					log_cb(RETRO_LOG_INFO, "DEV9: Socket: Closed Dead UDP Connection to %d\n", s->key.srv_port);
				break;
			case IP_PROTO_TCP:
				log_cb(RETRO_LOG_INFO, "DEV9: Socket: Closed Dead TCP Connection to %d\n", s->key.srv_port);
				break;
			case IP_PROTO_ICMP:
				log_cb(RETRO_LOG_INFO, "DEV9: Socket: Closed Dead ICMP Connection\n");
				break;
			default:
				log_cb(RETRO_LOG_INFO, "DEV9: Socket: Closed Dead Unk Connection\n");
				break;
		}
	}
}

/* ---- The wake socket ------------------------------------------------- */

static void open_wake_socket(sockets_adapter* a)
{
	struct sockaddr_in addr;
#ifdef _WIN32
	int len = sizeof(addr);
#else
	socklen_t len = sizeof(addr);
#endif
	net_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == NET_SOCK_BAD)
		return;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		getsockname(s, (struct sockaddr*)&addr, &len) != 0 ||
		connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
		!net_sock_nonblocking(s))
	{
		net_sock_close(s);
		return;
	}
	a->wake = s;
}

static void sockets_wake_recv(netif* n)
{
	sockets_adapter* a = (sockets_adapter*)n;
	const char b = 0;
	if (a->wake != NET_SOCK_BAD)
		send(a->wake, &b, 1, 0);
}

/* Asleep until a session's socket is ready for what its recv does, or the
 * PS2 sends (send wakes it: a new session, a window, an ACK, an ARP
 * reply queued), or the bound passes. A session that can say nothing of
 * the sort (a ping) makes it the old millisecond poll. */
static void sockets_wait_recv(netif* n)
{
	sockets_adapter* a = (sockets_adapter*)n;
	rx_pollfd fds[RX_POLL_MAX];
	int count = 0, timeout = RX_WAIT_MS, i;
	char drain[16];

	if (a->wake == NET_SOCK_BAD || a->ready_unread)
	{
		a->ready_unread = false;
		retro_sleep(1);
		return;
	}
	if (a->arp_count)
		return;
	fds[count].fd = a->wake;
	fds[count].events = POLLIN;
	fds[count].revents = 0;
	count++;
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* s = &a->sessions[i];
		uintptr_t fd = 0;
		int events = 0;
		if (!s->used || s->closed)
			continue;
		if (!s->ops->poll_interest(s, &fd, &events))
		{
			timeout = 1;
			continue;
		}
		if (!events)
			continue;
		if (count == RX_POLL_MAX)
		{
			timeout = 1;
			continue;
		}
		fds[count].fd = (net_sock)fd;
		fds[count].events = (short)(((events & SESSION_POLL_READ) ? POLLIN : 0) |
			((events & SESSION_POLL_WRITE) ? POLLOUT : 0));
		fds[count].revents = 0;
		count++;
	}
	if (RX_POLL(fds, count, timeout) <= 0)
		return;
	if (fds[0].revents & POLLIN)
		while (recv(a->wake, drain, sizeof(drain), 0) > 0)
		{
		}
	for (i = 1; i < count; i++)
		if (fds[i].revents)
			a->ready_unread = true;
}

/* ---- From the host --------------------------------------------------- */

static void arp_frame(sockets_adapter* a, NetPacket* pkt, const arp_reply* r)
{
	u8* f = (u8*)pkt->buffer;
	u8* arp = f + ETH_HEADER;
	net_put_eth(f, a->base.ps2_mac, net_internal_mac, ETHERTYPE_ARP);
	net_wr16(arp, 1);
	net_wr16(arp + 2, ETHERTYPE_IPV4);
	arp[4] = 6;
	arp[5] = 4;
	net_wr16(arp + 6, 2);
	memcpy(arp + 8, net_internal_mac, 6);
	memcpy(arp + 14, r->spa, 4);
	memcpy(arp + 18, r->tha, 6);
	memcpy(arp + 24, r->tpa, 4);
	pkt->size = ETH_HEADER + ARP_IPV4;
}

static bool sockets_recv(netif* n, NetPacket* pkt)
{
	sockets_adapter* a = (sockets_adapter*)n;
	u8* frame = (u8*)pkt->buffer;
	int i;

	if (a->arp_count)
	{
		arp_frame(a, pkt, &a->arp[a->arp_head]);
		a->arp_head = (a->arp_head + 1) % ARP_PENDING;
		a->arp_count--;
		netif_inspect_recv(n, pkt);
		a->ready_unread = false;
		return true;
	}
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* s = &a->sessions[i];
		int len;
		if (!s->used || s->closed)
			continue;
		len = s->ops->recv(s, frame);
		if (len <= 0)
		{
			reap(a);
			continue;
		}
		/* Before the session can be freed: it may have ended with this. */
		net_put_ip(frame, s->remote_ip, s->ps2_ip, s->key.proto, 64, 0, len);
		net_put_eth(frame, a->base.ps2_mac, net_internal_mac, ETHERTYPE_IPV4);
		pkt->size = NET_IP_DATA + len;
		reap(a);
		netif_inspect_recv(n, pkt);
		a->ready_unread = false;
		return true;
	}
	return false;
}

/* ---- From the PS2 ---------------------------------------------------- */

static bool send_icmp(sockets_adapter* a, const net_key* key, const net_ip* ip)
{
	net_session* s = sockets_find(a, key);
	if (!s)
	{
		log_cb(RETRO_LOG_DEBUG, "DEV9: Socket: Creating New ICMP Connection\n");
		s = session_new(a, key, ip->dst);
		if (!s)
			return false;
		icmp_session_init(s);
	}
	return s->ops->send(s, ip);
}

static bool send_tcp(sockets_adapter* a, net_key* key, const net_ip* ip)
{
	net_tcp tcp;
	net_session* s;
	if (!net_tcp_parse(ip->data, ip->len, &tcp))
		return false;
	key->ps2_port = tcp.sport;
	key->srv_port = tcp.dport;
	s = sockets_find(a, key);
	if (!s)
	{
		log_cb(RETRO_LOG_INFO, "DEV9: Socket: Creating New TCP Connection to %d\n", tcp.dport);
		s = session_new(a, key, ip->dst);
		if (!s)
			return false;
		tcp_session_init(s);
	}
	return s->ops->send(s, ip);
}

static bool send_udp(sockets_adapter* a, net_key* key, const net_ip* ip)
{
	net_udp udp;
	net_session* s;
	bool broadcast, multicast;
	if (!net_udp_parse(ip->data, ip->len, &udp))
		return false;
	key->ps2_port = udp.sport;
	key->srv_port = udp.dport;
	s = sockets_find(a, key);
	if (s)
		return s->ops->send(s, ip);

	broadcast = memcmp(ip->dst, a->base.dhcp.broadcast, 4) == 0 || memcmp(ip->dst, limited_broadcast, 4) == 0;
	multicast = (ip->dst[0] & 0xF0) == 0xE0; /* 0b1110 */
	/* Games that expect the replies on the port they sent from, and
	 * broadcasts, go through a socket bound to that port. */
	if (abs((int)udp.sport - (int)udp.dport) <= 10 || broadcast || multicast)
	{
		net_key fkey;
		net_session* port;
		memset(&fkey, 0, sizeof(fkey));
		fkey.proto = IP_PROTO_UDP;
		fkey.ps2_port = udp.sport;
		port = sockets_find(a, &fkey);
		if (!port)
		{
			log_cb(RETRO_LOG_INFO, "DEV9: Socket: Creating New UDPFixedPort with port %d\n", udp.sport);
			port = session_new(a, &fkey, any_ip);
			if (!port)
				return false;
			if (!udp_fixed_init(port, udp.sport))
			{
				session_closed(port);
				return false;
			}
		}
		log_cb(RETRO_LOG_INFO, "DEV9: Socket: Creating New UDP Connection from FixedPort %d to %d\n", udp.sport, udp.dport);
		s = session_new(a, key, ip->dst);
		if (!s)
		{
			if (port->u.fixed.children == 0)
				session_closed(port);
			return false;
		}
		udp_session_init_fixed(s, port, broadcast, multicast);
	}
	else
	{
		log_cb(RETRO_LOG_INFO, "DEV9: Socket: Creating New UDP Connection to %d\n", udp.dport);
		s = session_new(a, key, ip->dst);
		if (!s)
			return false;
		udp_session_init(s);
	}
	return s->ops->send(s, ip);
}

static bool send_ip(sockets_adapter* a, const net_ip* ip)
{
	net_key key;
	if (!net_ip_csum_ok(ip))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Socket: IP packet with bad CSUM\n");
		return false;
	}
	memset(&key, 0, sizeof(key));
	memcpy(key.ip, ip->dst, 4);
	key.proto = ip->proto;
	switch (ip->proto)
	{
		case IP_PROTO_ICMP:
			return send_icmp(a, &key, ip);
		case IP_PROTO_IGMP:
			log_cb(RETRO_LOG_ERROR, "DEV9: Socket: IGMP Packets not supported in socket mode\n");
			return false;
		case IP_PROTO_TCP:
			return send_tcp(a, &key, ip);
		case IP_PROTO_UDP:
			return send_udp(a, &key, ip);
		default:
			log_cb(RETRO_LOG_ERROR, "DEV9: Socket: Unkown IPv4 Protocol %X\n", ip->proto);
			return false;
	}
}

static bool send_arp(sockets_adapter* a, const net_eth* eth)
{
	net_arp arp;
	arp_reply* r;
	/* A request for any address but the PS2's own: the gateway's MAC is
	 * the answer to every one. */
	if (!net_arp_parse(eth->data, eth->len, &arp) || arp.op != 1 || memcmp(arp.tpa, a->base.dhcp.ps2_ip, 4) == 0)
		return true;
	if (a->arp_count == ARP_PENDING)
		return true;
	r = &a->arp[(a->arp_head + a->arp_count) % ARP_PENDING];
	memcpy(r->tha, arp.sha, 6);
	memcpy(r->tpa, arp.spa, 4);
	memcpy(r->spa, arp.tpa, 4);
	a->arp_count++;
	return true;
}

static bool sockets_send(netif* n, NetPacket* pkt)
{
	sockets_adapter* a = (sockets_adapter*)n;
	net_eth eth;
	net_ip ip;
	bool ret;

	netif_inspect_send(n, pkt);
	if (netif_internal_send(n, pkt))
		return true;
	if (!net_eth_parse((const u8*)pkt->buffer, pkt->size, &eth))
		return false;
	switch (eth.type)
	{
		case 0x0000:
		case 0x0C00:
			/* Sent when the adapter is reset */
			return true;
		case ETHERTYPE_IPV4:
			if (!net_ip_parse(eth.data, eth.len, &ip))
				return false;
			ret = send_ip(a, &ip);
			reap(a);
			return ret;
		case ETHERTYPE_ARP:
			return send_arp(a, &eth);
		default:
			log_cb(RETRO_LOG_ERROR, "DEV9: Socket: Unkown EtherframeType %X\n", eth.type);
			return false;
	}
}

/* ---- Control --------------------------------------------------------- */

static void sockets_reset(netif* n)
{
	sockets_adapter* a = (sockets_adapter*)n;
	int i, open = 0;
	for (i = 0; i < NET_SESSIONS; i++)
		if (a->sessions[i].used && !a->sessions[i].closed)
			open++;
	log_cb(RETRO_LOG_DEBUG, "DEV9: Socket: Reset %d Connections\n", open);
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* s = &a->sessions[i];
		if (s->used && !s->closed)
			s->ops->reset(s);
	}
	reap(a);
}

/* The internal servers as the socket adapter has them: the PS2 at .100
 * of the gateway's /24. */
static void init_servers(sockets_adapter* a, const net_adapter* adapter)
{
	u8 ps2_ip[4], subnet[4] = {255, 255, 255, 0};
	memcpy(ps2_ip, net_internal_ip, 3);
	ps2_ip[3] = 100;
	netif_init_servers(&a->base, adapter, true, ps2_ip, subnet, net_internal_ip);
}

static const net_adapter* find_adapter(void** list)
{
	if (strcmp(EmuConfig.DEV9.EthDevice, "Auto") != 0)
		return net_adapter_get(list, EmuConfig.DEV9.EthDevice);
	return net_adapter_get_auto(list);
}

static void sockets_reload(netif* n)
{
	void* list;
	init_servers((sockets_adapter*)n, find_adapter(&list));
	net_adapter_free(list);
}

static void sockets_close(netif* n)
{
	sockets_wake_recv(n);
}

static void sockets_destroy(netif* n)
{
	sockets_adapter* a = (sockets_adapter*)n;
	int i;
	for (i = 0; i < NET_SESSIONS; i++)
		if (a->sessions[i].used)
			session_closed(&a->sessions[i]);
	reap(a);
	net_sock_close(a->wake);
#ifdef _WIN32
	if (a->wsa)
		WSACleanup();
#endif
	netif_deinit(n);
	free(a);
}

static const netif_ops sockets_ops = {
	sockets_recv, sockets_wait_recv, sockets_wake_recv, sockets_send,
	sockets_reset, sockets_reload, sockets_close, sockets_destroy};

netif* net_sockets_new(void)
{
	sockets_adapter* a = (sockets_adapter*)calloc(1, sizeof(*a));
	const net_adapter* adapter;
	void* list;
	u8 host_mac[6];

	if (!a)
		return NULL;
	netif_init(&a->base, &sockets_ops);
	a->wake = NET_SOCK_BAD;
#ifdef _WIN32
	{
		WSADATA wsa;
		const int err = WSAStartup(MAKEWORD(2, 2), &wsa);
		if (err != 0)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: WSAStartup failed with error: %d\n", err);
			sockets_destroy(&a->base);
			return NULL;
		}
		a->wsa = true;
	}
#endif

	adapter = find_adapter(&list);
	if (!adapter)
	{
		if (strcmp(EmuConfig.DEV9.EthDevice, "Auto") != 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: Socket: Failed to Get Adapter\n");
		else
			log_cb(RETRO_LOG_ERROR, "DEV9: Socket: Auto Selection Failed, Check You Connection or Manually Specify Adapter\n");
		sockets_destroy(&a->base);
		return NULL;
	}
	/* A chosen adapter's address is the one the sockets bind to; Auto
	 * leaves it to the host. */
	if (strcmp(EmuConfig.DEV9.EthDevice, "Auto") != 0 && !net_adapter_ip(adapter, a->adapter_ip))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Socket: Failed To Get Adapter IP\n");
		net_adapter_free(list);
		sockets_destroy(&a->base);
		return NULL;
	}

	init_servers(a, adapter);
	if (net_adapter_mac(adapter, host_mac))
	{
		/* The host's last 2 bytes, swapped, to be unique on Xlink */
		u8 mac[6];
		memcpy(mac, a->base.ps2_mac, 6);
		mac[5] = host_mac[4];
		mac[4] = host_mac[5];
		netif_set_mac(&a->base, mac);
	}
	else
		log_cb(RETRO_LOG_ERROR, "DEV9: Socket: Failed to get MAC address for adapter\n");
	net_adapter_free(list);

	open_wake_socket(a);
	a->initialized = true;
	return &a->base;
}
