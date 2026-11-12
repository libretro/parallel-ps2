/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#ifndef DEV9_ADAPTERUTILS_H
#define DEV9_ADAPTERUTILS_H

/* The host's network adapters: finding one by name or the first usable
 * one, and what it has -- MAC, IPv4 address, gateways, DNS servers.
 * Addresses are 4 bytes in network order, as a sockaddr_in holds them. */

#include "common/Pcsx2Defs.h"

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#elif defined(__POSIX__)
#include <sys/types.h>
#include <compat/ifaddrs.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef IP_ADAPTER_ADDRESSES net_adapter;
#else
typedef struct ifaddrs net_adapter;
#endif

/* The most addresses of one kind an adapter is asked for. */
#define NET_ADAPTER_MAX_ADDRS 8

/* An adapter inside a list *list now owns, or NULL with *list NULL; the
 * list is released with net_adapter_free once the adapter is done with.
 * "Auto" picks the first adapter that is up, not loopback, and has an
 * IPv4 address and a gateway (and on Windows a DNS server). */
const net_adapter* net_adapter_get(void** list, const char* name);
const net_adapter* net_adapter_get_auto(void** list);
void net_adapter_free(void* list);

bool net_adapter_mac(const net_adapter* adapter, u8 mac[6]);
bool net_adapter_ip(const net_adapter* adapter, u8 ip[4]);
bool net_adapter_netmask(const net_adapter* adapter, u8 mask[4]);
/* How many were written to out, 4 bytes each, at most max. */
int net_adapter_gateways(const net_adapter* adapter, u8* out, int max);
int net_adapter_dns(const net_adapter* adapter, u8* out, int max);

#ifndef _WIN32
/* The pieces the POSIX half is built from, for the tests: the IPv4 entry
 * named name in a getifaddrs list; a resolv.conf line's nameserver; a
 * /proc/net/route line's gateway for ifname. */
const net_adapter* net_adapter_find(const struct ifaddrs* list, const char* name);
bool net_resolv_line(const char* line, u8 ip[4]);
bool net_route_line(const char* line, const char* ifname, u8 gw[4]);
/* The nameservers of a resolv.conf at path. */
int net_resolv_file(const char* path, u8* out, int max);
#endif

#ifdef __cplusplus
}
#endif

#endif
