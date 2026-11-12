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

#include <string.h>

#include "common/Pcsx2Defs.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <errno.h>
#include <netinet/in.h>
#endif

#include <features/features_cpu.h>

#include "DEV9/Sessions/Session.h"

/* RFC 4787 section 4.3 */
#define UDP_MAX_IDLE_USEC ((s64)120 * 1000000)
/* What one datagram from the host may carry to fit one frame. */
#define UDP_DATA_MAX (ETH_FRAME_MAX - NET_UDP_DATA)

static s64 now_usec(void)
{
	return (s64)cpu_features_get_time_usec();
}

static void endpoint(struct sockaddr_in* sa, const u8 ip[4], u16 port)
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_family = AF_INET;
	memcpy(&sa->sin_addr, ip, 4);
	sa->sin_port = htons(port);
}

/* One datagram into frame + NET_UDP_DATA: its length, 0 for none, -1 for
 * a socket that failed. One too long for a frame is dropped whole. */
static int recv_datagram(net_sock sock, u8* frame, struct sockaddr_in* from)
{
#ifdef _WIN32
	int fromlen = sizeof(*from);
#else
	socklen_t fromlen = sizeof(*from);
#endif
	int ret, err;
	/* One byte more than fits: a datagram that fills it did not fit. */
	ret = recvfrom(sock, (char*)frame + NET_UDP_DATA, UDP_DATA_MAX + 1, 0, (struct sockaddr*)from, &fromlen);
	if (ret >= 0)
	{
		if (ret <= UDP_DATA_MAX)
			return ret;
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Dropped a datagram too long for one frame\n");
		return 0;
	}
	err = net_sock_error();
	if (net_sock_would_block(err))
		return 0;
#ifdef _WIN32
	if (err == WSAEMSGSIZE)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Dropped a datagram too long for one frame\n");
		return 0;
	}
#endif
	log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Recv Error: %d\n", err);
	return -1;
}

/* ---- A UDP flow ------------------------------------------------------ */

static bool udp_poll_interest(net_session* s, uintptr_t* fd, int* events)
{
	udp_state* u = &s->u.udp;
	*events = 0;
	/* A fixed port's client receives through the port; its idle clock
	 * is minutes, which the receive thread's bounded wait covers. */
	if (!u->open || s->parent)
		return true;
	*fd = (uintptr_t)u->sock;
	*events = SESSION_POLL_READ;
	return true;
}

static int udp_recv(net_session* s, u8* frame)
{
	udp_state* u = &s->u.udp;
	struct sockaddr_in from;
	int n;
	if (!u->open)
		return 0;
	if (!s->parent)
	{
		n = recv_datagram(u->sock, frame, &from);
		if (n < 0)
		{
			session_closed(s);
			return 0;
		}
		if (n > 0)
		{
			u->last_use = now_usec();
			net_put_udp(frame, s->remote_ip, s->ps2_ip, u->dst_port, u->src_port, n);
			return UDP_HEADER + n;
		}
	}
	if (now_usec() - u->last_use > UDP_MAX_IDLE_USEC)
	{
		log_cb(RETRO_LOG_INFO, "DEV9: UDP: Max Idle Reached\n");
		u->open = false;
		session_closed(s);
	}
	return 0;
}

bool udp_session_will_receive(net_session* s, const u8 ip[4])
{
	udp_state* u = &s->u.udp;
	if (s->closed || !u->open)
		return false;
	if (u->broadcast || memcmp(ip, s->remote_ip, 4) == 0)
	{
		u->last_use = now_usec();
		return true;
	}
	return false;
}

/* The flow's own socket, connected to where the PS2 sends. */
static bool udp_open(net_session* s, u16 dport)
{
	udp_state* u = &s->u.udp;
	static const u8 any[4] = {0, 0, 0, 0};
	struct sockaddr_in sa;
	const int on = 1;

	u->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (u->sock == NET_SOCK_BAD)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to open socket. Error: %d\n", net_sock_error());
		return false;
	}
	if (setsockopt(u->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on)) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to set SO_REUSEADDR. Error: %d\n", net_sock_error());
	if (memcmp(s->adapter_ip, any, 4) != 0)
	{
		endpoint(&sa, s->adapter_ip, 0);
		if (bind(u->sock, (const struct sockaddr*)&sa, sizeof(sa)) != 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to bind socket. Error: %d\n", net_sock_error());
	}
	if (!net_sock_nonblocking(u->sock))
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to set non blocking. Error: %d\n", net_sock_error());
	endpoint(&sa, s->remote_ip, dport);
	if (connect(u->sock, (const struct sockaddr*)&sa, sizeof(sa)) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to connect socket. Error: %d\n", net_sock_error());
		return false;
	}
	return true;
}

static int udp_transmit(net_session* s, net_sock sock, const net_udp* udp)
{
	udp_state* u = &s->u.udp;
	struct sockaddr_in sa;
	if (u->broadcast)
	{
		static const u8 all[4] = {255, 255, 255, 255};
		endpoint(&sa, all, u->dst_port);
		return sendto(sock, (const char*)udp->data, udp->len, 0, (const struct sockaddr*)&sa, sizeof(sa));
	}
	if (u->multicast || s->parent)
	{
		endpoint(&sa, s->remote_ip, u->dst_port);
		return sendto(sock, (const char*)udp->data, udp->len, 0, (const struct sockaddr*)&sa, sizeof(sa));
	}
	return send(sock, (const char*)udp->data, udp->len, 0);
}

static bool udp_send(net_session* s, const net_ip* ip)
{
	udp_state* u = &s->u.udp;
	net_sock sock;
	net_udp udp;
	int err;

	if (!net_udp_parse(ip->data, ip->len, &udp))
		return false;
	u->last_use = now_usec();
	if (u->dst_port != 0)
	{
		if (udp.dport != u->dst_port || udp.sport != u->src_port)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Packet invalid for current session (Duplicate key?)\n");
			return false;
		}
	}
	else
	{
		u->dst_port = udp.dport;
		u->src_port = udp.sport;
		/* Multicast addresses start with 0b1110 */
		if ((s->remote_ip[0] & 0xF0) == 0xE0)
		{
			u->multicast = true;
			log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Unexpected Multicast Connection\n");
		}
		if (!udp_open(s, u->dst_port))
		{
			session_closed(s);
			return false;
		}
		if (u->src_port != 0)
			u->open = true;
	}

	sock = s->parent ? s->parent->u.fixed.sock : u->sock;
	if (udp_transmit(s, sock, &udp) < 0)
	{
		err = net_sock_error();
		/* The host's buffer is full: the datagram is lost, as on a wire. */
		if (net_sock_would_block(err))
			return true;
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Send Error %d\n", err);
		/* An ICMP port unreachable for an earlier send can surface here;
		 * it is ignored and the send retried, so a game's capture of
		 * everything (R&C Deadlock) still goes out. */
#ifdef _WIN32
		if (err != WSAECONNRESET)
#else
		if (err != ECONNREFUSED)
#endif
		{
			session_closed(s);
			return false;
		}
		if (udp_transmit(s, sock, &udp) < 0)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Send Error (Second attempt) %d\n", net_sock_error());
			return false;
		}
	}
	if (u->src_port == 0)
		session_closed(s);
	return true;
}

static void udp_reset(net_session* s)
{
	session_closed(s);
}

static void udp_release(net_session* s)
{
	udp_state* u = &s->u.udp;
	u->open = false;
	if (!s->parent)
		net_sock_close(u->sock);
	u->sock = NET_SOCK_BAD;
}

static const net_session_ops udp_ops = {
	udp_recv, udp_poll_interest, udp_send, udp_reset, udp_release};

void udp_session_init(net_session* s)
{
	memset(&s->u.udp, 0, sizeof(s->u.udp));
	s->ops = &udp_ops;
	s->u.udp.sock = NET_SOCK_BAD;
	s->u.udp.last_use = now_usec();
}

void udp_session_init_fixed(net_session* s, net_session* port, bool broadcast, bool multicast)
{
	udp_session_init(s);
	s->parent = port;
	port->u.fixed.children++;
	s->u.udp.open = true;
	s->u.udp.broadcast = broadcast;
	s->u.udp.multicast = multicast;
	s->u.udp.src_port = s->key.ps2_port;
	s->u.udp.dst_port = s->key.srv_port;
}

/* ---- A fixed port: one host socket on the PS2's port, for the flows
 * that games expect to come from a port they chose, and broadcasts. -- */

static bool fixed_poll_interest(net_session* s, uintptr_t* fd, int* events)
{
	*events = 0;
	if (s->u.fixed.sock == NET_SOCK_BAD)
		return true;
	*fd = (uintptr_t)s->u.fixed.sock;
	*events = SESSION_POLL_READ;
	return true;
}

static void fixed_reset(net_session* s);

static int fixed_recv(net_session* s, u8* frame)
{
	net_session* all = sockets_session_array(s->owner);
	udp_fixed_state* f = &s->u.fixed;
	struct sockaddr_in from;
	u8 from_ip[4];
	int i, n;

	if (f->sock == NET_SOCK_BAD)
		return 0;
	n = recv_datagram(f->sock, frame, &from);
	if (n < 0)
	{
		/* Its clients go with it: they send through its socket. */
		fixed_reset(s);
		session_closed(s);
		return 0;
	}
	if (n == 0)
		return 0;
	memcpy(from_ip, &from.sin_addr, 4);
	/* A client of this port must expect it. */
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* c = &all[i];
		if (c->used && c->parent == s && udp_session_will_receive(c, from_ip))
		{
			memcpy(s->remote_ip, from_ip, 4);
			net_put_udp(frame, s->remote_ip, s->ps2_ip, ntohs(from.sin_port), f->port, n);
			return UDP_HEADER + n;
		}
	}
	log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Unexpected packet, dropping\n");
	return 0;
}

static bool fixed_send(net_session* s, const net_ip* ip)
{
	(void)s;
	(void)ip;
	return false;
}

static void fixed_reset(net_session* s)
{
	net_session* all = sockets_session_array(s->owner);
	int i;
	for (i = 0; i < NET_SESSIONS; i++)
	{
		net_session* c = &all[i];
		if (c->used && c->parent == s && !c->closed)
			c->ops->reset(c);
	}
}

static void fixed_release(net_session* s)
{
	net_sock_close(s->u.fixed.sock);
	s->u.fixed.sock = NET_SOCK_BAD;
}

static const net_session_ops fixed_ops = {
	fixed_recv, fixed_poll_interest, fixed_send, fixed_reset, fixed_release};

bool udp_fixed_init(net_session* s, u16 port)
{
	udp_fixed_state* f = &s->u.fixed;
	struct sockaddr_in sa;
	const int on = 1;

	memset(f, 0, sizeof(*f));
	s->ops = &fixed_ops;
	f->port = port;
	f->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (f->sock == NET_SOCK_BAD)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to open socket. Error: %d\n", net_sock_error());
		return false;
	}
	if (setsockopt(f->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on)) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to set SO_REUSEADDR. Error: %d\n", net_sock_error());
	if (setsockopt(f->sock, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof(on)) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to set SO_BROADCAST. Error: %d\n", net_sock_error());
	if (!net_sock_nonblocking(f->sock))
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to set non blocking. Error: %d\n", net_sock_error());
	endpoint(&sa, s->adapter_ip, port);
	if (bind(f->sock, (const struct sockaddr*)&sa, sizeof(sa)) != 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: UDP: Failed to bind socket. Error: %d\n", net_sock_error());
	return true;
}
