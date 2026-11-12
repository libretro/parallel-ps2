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

#ifndef DEV9_DNS_SERVER_H
#define DEV9_DNS_SERVER_H

/* The internal DNS server: the PS2's A queries to 192.0.2.1:53 answered
 * from the host's resolver, a thread per name. Queries in flight sit in a
 * fixed set of slots; the network thread fills a free one and takes a
 * finished one back, the last of its lookups to finish writes its reply.
 * No locks, and nothing allocated per query. */

#include "common/Pcsx2Defs.h"
#include "DEV9/AdapterUtils.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dns_server dns_server;

dns_server* dns_server_new(void);
/* Waits for lookups still in flight, which write into the server. */
void dns_server_free(dns_server* server);

/* 127.0.0.1 in an answer becomes the adapter's address. */
void dns_server_set_adapter(dns_server* server, const net_adapter* adapter);

/* A query the PS2 sent from src_port; whether it was taken. */
bool dns_server_send(dns_server* server, const u8* payload, int len, u16 src_port);
/* A finished reply into out and the port it goes to: its length, or 0. */
int dns_server_recv(dns_server* server, u8* out, int cap, u16* dst_port);

#ifdef __cplusplus
}
#endif

#endif
