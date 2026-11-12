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

#ifndef DEV9_DHCP_SERVER_H
#define DEV9_DHCP_SERVER_H

/* The internal DHCP server: answers the PS2's DISCOVER and REQUEST with
 * the address, mask, gateway and DNS servers the settings (or the host
 * adapter) give. Requests are answered as they come, on the network
 * thread, into a small fixed queue it reads back; nothing is allocated. */

#include "common/Pcsx2Defs.h"
#include "DEV9/AdapterUtils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
/* The fixed header, the magic cookie included. */
#define DHCP_HEADER_SIZE 240
/* The longest reply: an Ethernet MTU less the IP and UDP headers. */
#define DHCP_REPLY_MAX (1500 - 20 - 8)
#define DHCP_REPLIES 4

typedef struct dhcp_reply
{
	u16 len;
	u8 data[DHCP_REPLY_MAX];
} dhcp_reply;

typedef struct dhcp_server
{
	/* What is handed out, all 4 bytes in network order. */
	u8 ps2_ip[4];
	u8 netmask[4];
	u8 gateway[4];
	u8 broadcast[4];
	u8 dns1[4];
	u8 dns2[4];
	u8 server_ip[4];
	/* Replies waiting for the network thread. */
	int head;
	int count;
	dhcp_reply replies[DHCP_REPLIES];
} dhcp_server;

/* From the settings and the adapter (which may be NULL); an override
 * that is not all zero wins over both. */
void dhcp_server_init(dhcp_server* server, const net_adapter* adapter, const u8 server_ip[4],
	const u8 ip_override[4], const u8 subnet_override[4], const u8 gateway_override[4]);

/* A request the PS2 sent to port 67; whether it was taken. */
bool dhcp_server_send(dhcp_server* server, const u8* payload, int len);
/* The oldest reply waiting, for port 68: its length, or 0. */
int dhcp_server_recv(dhcp_server* server, u8* out, int cap);

#ifdef __cplusplus
}
#endif

#endif
