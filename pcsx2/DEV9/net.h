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

#ifndef DEV9_NET_H
#define DEV9_NET_H

/* The host side of the network: an adapter (sockets, pcap) the network
 * thread drives, with the internal DHCP and DNS servers in front of it.
 * Once the network thread runs (net.c), an adapter is touched by it
 * alone, and on the control side only while it is stopped: nothing here
 * takes a lock. */

#include "common/Pcsx2Defs.h"
#include "Config.h"
#include "netio.h"
#include "AdapterUtils.h"
#include "InternalServers/DHCP_Server.h"
#include "InternalServers/DNS_Server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct netif netif;

typedef struct netif_ops
{
	/* A frame from the host into pkt, at most ETH_FRAME_MAX bytes. */
	bool (*recv)(netif* nif, NetPacket* pkt);
	/* Once recv has nothing: return when it may have something, or
	 * after a bound. NULL polls every millisecond. */
	void (*wait_recv)(netif* nif);
	/* Ends a wait_recv early, from any thread. May be NULL. */
	void (*wake_recv)(netif* nif);
	/* A frame the PS2 sent, to the host. */
	bool (*send)(netif* nif, NetPacket* pkt);
	void (*reset)(netif* nif);
	void (*reload)(netif* nif);
	void (*close)(netif* nif);
	void (*destroy)(netif* nif);
} netif_ops;

struct netif
{
	const netif_ops* ops;
	u8 ps2_mac[6];
	/* The PS2's address, as last seen sending to the internal one. */
	u8 ps2_ip[4];
	bool dhcp_on;
	dhcp_server dhcp;
	dns_server* dns;
};

/* The gateway the internal servers answer as, and its MAC. */
extern const u8 net_internal_ip[4];
extern const u8 net_internal_mac[6];
extern const u8 net_broadcast_mac[6];
/* The first three are recognised by Xlink as a PS2's. */
extern const u8 net_default_mac[6];

void netif_init(netif* nif, const netif_ops* ops);
void netif_deinit(netif* nif);
/* The PS2's MAC, also into the EEPROM; NULL for the default. */
void netif_set_mac(netif* nif, const u8* mac);
/* Whether a frame from the host is the PS2's to see. */
bool netif_verify(netif* nif, NetPacket* pkt, int size);
/* DNS logging, when it is on. */
void netif_inspect_send(netif* nif, const NetPacket* pkt);
void netif_inspect_recv(netif* nif, const NetPacket* pkt);
/* The internal servers from the adapter (may be NULL) and overrides
 * (all-zero or NULL for none). */
void netif_init_servers(netif* nif, const net_adapter* adapter, bool dhcp_force,
	const u8* ip, const u8* subnet, const u8* gateway);
/* A frame to the internal servers: whether it was theirs. */
bool netif_internal_send(netif* nif, NetPacket* pkt);
/* A reply of theirs into pkt; whether there was one. */
bool netif_internal_recv(netif* nif, NetPacket* pkt);

/* The adapters: each NULL where it cannot be opened. */
netif* net_sockets_new(void);
#ifdef HAVE_PCAP
netif* net_pcap_new(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
