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

#ifndef DEV9_SOCKETS_H
#define DEV9_SOCKETS_H

/* The sockets adapter: the PS2's packets carried on the host's own
 * sockets, a session for each conversation (Sessions/), with the
 * internal DHCP and DNS servers as its gateway. */

#include "common/Pcsx2Defs.h"
#include "net.h"
#include "Sessions/Session.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* What a ping's reply is read into: the host's reply record, an IP
 * header and a frame's worth of echoed data. */
#define ICMP_PING_BUFFER 2048
/* The PS2's IP header (options and all) and 8 bytes of what it carried,
 * returned in an ICMP error. */
#define ICMP_ORIGINAL_MAX (60 + 8)
/* ARP replies waiting for the PS2. */
#define ARP_PENDING 8

typedef struct icmp_ping
{
	bool used;
	net_session* session;
#ifdef _WIN32
	HANDLE file;
	HANDLE event;
#else
	int sock;
	s64 deadline; /* usec */
	u16 id;
#endif
	u8 rest[4]; /* the PS2's identifier and sequence number */
	int original_len;
	u8 original[ICMP_ORIGINAL_MAX];
	int buffer_len;
	u8 buffer[ICMP_PING_BUFFER];
} icmp_ping;

typedef struct arp_reply
{
	u8 tha[6];
	u8 tpa[4];
	u8 spa[4];
} arp_reply;

struct sockets_adapter
{
	netif base;
#ifdef _WIN32
	bool wsa;
#endif
	bool initialized;
	u8 adapter_ip[4];
	/* A UDP socket connected to itself: a byte sent to it ends the
	 * receive thread's poll (wake_recv). NET_SOCK_BAD when none. */
	net_sock wake;
	/* The last poll ended on a session's socket and recv has found
	 * nothing since: the next wait paces itself, rather than spin on a
	 * socket its recv leaves ready. */
	bool ready_unread;
	int arp_head;
	int arp_count;
	arp_reply arp[ARP_PENDING];
	net_session sessions[NET_SESSIONS];
	icmp_ping pings[NET_PINGS];
};

/* For the ICMP sessions: a free ping, or NULL; one given back. */
icmp_ping* sockets_ping_alloc(sockets_adapter* a, net_session* s);
void sockets_ping_free(icmp_ping* p);

#ifdef __cplusplus
}
#endif

#endif
