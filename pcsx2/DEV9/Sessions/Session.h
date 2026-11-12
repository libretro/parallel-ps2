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

#ifndef DEV9_SESSION_H
#define DEV9_SESSION_H

/* The sockets adapter's sessions: one per conversation the PS2 has with
 * the host's network (a TCP connection, a UDP flow, the pings to an
 * address), each on a host socket. They live in the adapter's fixed pool
 * and are touched by the network thread alone. A session that ends is
 * marked closed and freed by the adapter once the call that closed it
 * has returned, never from inside it. */

#include "common/Pcsx2Defs.h"
#include "DEV9/PacketReader/netpkt.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET net_sock;
#define NET_SOCK_BAD INVALID_SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int net_sock;
#define NET_SOCK_BAD (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Sessions an adapter holds at once; past them a new one is refused. */
#define NET_SESSIONS 128
/* Pings in flight across the adapter. */
#define NET_PINGS 16
/* Control segments (ACK, FIN, RST) a TCP session holds for the PS2. */
#define TCP_PENDING 16

typedef struct net_key
{
	u8 ip[4];
	u8 proto;
	u16 ps2_port;
	u16 srv_port;
} net_key;

enum
{
	SESSION_POLL_READ = 1,
	SESSION_POLL_WRITE = 2
};

typedef struct net_session net_session;
typedef struct sockets_adapter sockets_adapter;

typedef struct net_session_ops
{
	/* What the host sent, as an IPv4 payload (the ICMP, UDP or TCP header
	 * and its data) at frame + NET_IP_DATA: its length, or 0. The
	 * adapter puts the IP and Ethernet headers in front. */
	int (*recv)(net_session* s, u8* frame);
	/* What recv waits on, for the receive thread to sleep on it: a
	 * socket in *fd and SESSION_POLL_* in *events, or 0 events for
	 * nothing. False when recv may have something whatever the sockets
	 * do (a queue, a timer), so it is called again soon. */
	bool (*poll_interest)(net_session* s, uintptr_t* fd, int* events);
	/* A packet from the PS2. */
	bool (*send)(net_session* s, const net_ip* ip);
	void (*reset)(net_session* s);
	/* Its sockets and handles released, once closed. */
	void (*release)(net_session* s);
} net_session_ops;

typedef struct udp_state
{
	net_sock sock;
	bool open;
	bool broadcast;
	bool multicast;
	u16 src_port;
	u16 dst_port;
	s64 last_use; /* usec */
} udp_state;

typedef struct udp_fixed_state
{
	net_sock sock;
	u16 port;
	int children;
} udp_fixed_state;

typedef struct tcp_segment
{
	u8 flags;
	u32 seq;
	u32 ack;
} tcp_segment;

typedef struct tcp_state
{
	net_sock sock;
	int state;
	u16 src_port;
	u16 dst_port;
	int mss;
	int wscale;
	int window;
	bool send_ts;
	u32 last_ts;
	s64 ts_start; /* usec */
	u32 expected_seq;      /* the next the PS2 sends */
	u32 ps2_seqs[5];       /* the last it sent */
	u32 my_seq;            /* the next we send */
	u32 my_old[2];         /* the two before */
	bool my_acked;         /* everything we sent is ACKed */
	int pending_head;
	int pending_count;
	tcp_segment pending[TCP_PENDING];
} tcp_state;

typedef struct icmp_state
{
	int pings; /* in flight */
} icmp_state;

struct net_session
{
	const net_session_ops* ops;
	sockets_adapter* owner;
	net_session* parent; /* a fixed port's client: the port */
	net_key key;
	u8 ps2_ip[4];
	u8 remote_ip[4];
	u8 adapter_ip[4];
	bool used;
	bool closed;
	union
	{
		udp_state udp;
		udp_fixed_state fixed;
		tcp_state tcp;
		icmp_state icmp;
	} u;
};

/* From the adapter (sockets.c): a live session by key; all of them. */
net_session* sockets_find(sockets_adapter* a, const net_key* key);
net_session* sockets_session_array(sockets_adapter* a);

void session_closed(net_session* s);

/* The kinds. */
void udp_session_init(net_session* s);
/* A fixed port's client: sends through the port's socket. */
void udp_session_init_fixed(net_session* s, net_session* port, bool broadcast, bool multicast);
/* Whether a datagram from ip is one this client takes. */
bool udp_session_will_receive(net_session* s, const u8 ip[4]);
bool udp_fixed_init(net_session* s, u16 port);
void tcp_session_init(net_session* s);
void icmp_session_init(net_session* s);

/* Sockets, for every platform. */
int net_sock_error(void);
bool net_sock_would_block(int err);
void net_sock_close(net_sock sock);
bool net_sock_nonblocking(net_sock sock);
/* What a socket is ready for now: SESSION_POLL_*, and 4 for an error. */
int net_sock_ready(net_sock sock, int events);
#define NET_SOCK_FAILED 4

#ifdef __cplusplus
}
#endif

#endif
