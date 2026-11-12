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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/Pcsx2Defs.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#elif defined(__POSIX__)
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <compat/strl.h>

#ifdef __linux__
#include <sys/ioctl.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

/* The routing table is read through a PF_ROUTE sysctl, and <net/route.h> is
 * not in the iOS or tvOS SDK - those platforms do not expose routing sockets.
 * They fall through to the "unsupported OS" gateways below, which is what
 * every other platform without a way to ask already does. */
#if defined(__FreeBSD__) || (defined(__APPLE__) && TARGET_OS_OSX)
#define NET_ROUTE_SYSCTL 1
#include <sys/param.h>
#include <sys/sysctl.h>
#include <net/route.h>
#endif
#endif

#include "AdapterUtils.h"

#ifdef _WIN32
/* Every adapter, in one block the caller frees. */
static IP_ADAPTER_ADDRESSES* adapters_list(void)
{
	ULONG size = 128 * sizeof(IP_ADAPTER_ADDRESSES);
	int tries;
	for (tries = 0; tries < 3; tries++)
	{
		IP_ADAPTER_ADDRESSES* list = (IP_ADAPTER_ADDRESSES*)malloc(size);
		DWORD status;
		if (!list)
			return NULL;
		status = GetAdaptersAddresses(AF_UNSPEC,
			GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS, NULL, list, &size);
		if (status == ERROR_SUCCESS)
			return list;
		free(list);
		if (status != ERROR_BUFFER_OVERFLOW)
			return NULL;
	}
	return NULL;
}

const net_adapter* net_adapter_get(void** list, const char* name)
{
	IP_ADAPTER_ADDRESSES* all = adapters_list();
	const IP_ADAPTER_ADDRESSES* a;
	*list = NULL;
	for (a = all; a; a = a->Next)
	{
		if (strcmp(a->AdapterName, name) == 0)
		{
			*list = all;
			return a;
		}
	}
	free(all);
	return NULL;
}

const net_adapter* net_adapter_get_auto(void** list)
{
	IP_ADAPTER_ADDRESSES* all = adapters_list();
	const IP_ADAPTER_ADDRESSES* a;
	*list = NULL;
	for (a = all; a; a = a->Next)
	{
		u8 addrs[4 * NET_ADAPTER_MAX_ADDRS];
		u8 ip[4];
		if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->OperStatus != IfOperStatusUp)
			continue;
		if (net_adapter_ip(a, ip) &&
			net_adapter_dns(a, addrs, NET_ADAPTER_MAX_ADDRS) > 0 &&
			net_adapter_gateways(a, addrs, NET_ADAPTER_MAX_ADDRS) > 0)
		{
			*list = all;
			return a;
		}
	}
	free(all);
	return NULL;
}

void net_adapter_free(void* list)
{
	free(list);
}

bool net_adapter_mac(const net_adapter* adapter, u8 mac[6])
{
	if (!adapter || adapter->PhysicalAddressLength != 6)
		return false;
	memcpy(mac, adapter->PhysicalAddress, 6);
	return true;
}

bool net_adapter_ip(const net_adapter* adapter, u8 ip[4])
{
	const IP_ADAPTER_UNICAST_ADDRESS* a;
	if (!adapter)
		return false;
	for (a = adapter->FirstUnicastAddress; a; a = a->Next)
	{
		if (a->Address.lpSockaddr && a->Address.lpSockaddr->sa_family == AF_INET)
		{
			memcpy(ip, &((const struct sockaddr_in*)a->Address.lpSockaddr)->sin_addr, 4);
			return true;
		}
	}
	return false;
}

bool net_adapter_netmask(const net_adapter* adapter, u8 mask[4])
{
	const IP_ADAPTER_UNICAST_ADDRESS* a;
	if (!adapter)
		return false;
	for (a = adapter->FirstUnicastAddress; a; a = a->Next)
	{
		if (a->Address.lpSockaddr && a->Address.lpSockaddr->sa_family == AF_INET)
		{
			ULONG m;
			if (ConvertLengthToIpv4Mask(a->OnLinkPrefixLength, &m) != NO_ERROR)
				return false;
			memcpy(mask, &m, 4);
			return true;
		}
	}
	return false;
}

int net_adapter_gateways(const net_adapter* adapter, u8* out, int max)
{
	const IP_ADAPTER_GATEWAY_ADDRESS* a;
	int n = 0;
	if (!adapter)
		return 0;
	for (a = adapter->FirstGatewayAddress; a && n < max; a = a->Next)
	{
		if (a->Address.lpSockaddr && a->Address.lpSockaddr->sa_family == AF_INET)
			memcpy(out + 4 * n++, &((const struct sockaddr_in*)a->Address.lpSockaddr)->sin_addr, 4);
	}
	return n;
}

int net_adapter_dns(const net_adapter* adapter, u8* out, int max)
{
	const IP_ADAPTER_DNS_SERVER_ADDRESS* a;
	int n = 0;
	if (!adapter)
		return 0;
	for (a = adapter->FirstDnsServerAddress; a && n < max; a = a->Next)
	{
		if (a->Address.lpSockaddr && a->Address.lpSockaddr->sa_family == AF_INET)
			memcpy(out + 4 * n++, &((const struct sockaddr_in*)a->Address.lpSockaddr)->sin_addr, 4);
	}
	return n;
}

#elif defined(__POSIX__)

/* An entry with no address at all (a tunnel, an interface still coming
 * up) is skipped, not dereferenced. */
static bool is_ipv4(const struct ifaddrs* a)
{
	return a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET;
}

const net_adapter* net_adapter_find(const struct ifaddrs* list, const char* name)
{
	const struct ifaddrs* a;
	for (a = list; a; a = a->ifa_next)
	{
		if (is_ipv4(a) && a->ifa_name && strcmp(a->ifa_name, name) == 0)
			return a;
	}
	return NULL;
}

const net_adapter* net_adapter_get(void** list, const char* name)
{
	struct ifaddrs* all;
	const net_adapter* a;
	*list = NULL;
	if (getifaddrs(&all) != 0)
		return NULL;
	a = net_adapter_find(all, name);
	if (a)
		*list = all;
	else
		freeifaddrs(all);
	return a;
}

const net_adapter* net_adapter_get_auto(void** list)
{
	struct ifaddrs* all;
	const struct ifaddrs* a;
	*list = NULL;
	if (getifaddrs(&all) != 0)
		return NULL;
	for (a = all; a; a = a->ifa_next)
	{
		u8 gws[4 * NET_ADAPTER_MAX_ADDRS];
		if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP) || !is_ipv4(a))
			continue;
		if (net_adapter_gateways(a, gws, NET_ADAPTER_MAX_ADDRS) > 0)
		{
			*list = all;
			return a;
		}
	}
	freeifaddrs(all);
	return NULL;
}

void net_adapter_free(void* list)
{
	if (list)
		freeifaddrs((struct ifaddrs*)list);
}

bool net_adapter_mac(const net_adapter* adapter, u8 mac[6])
{
#ifdef __linux__
	struct ifreq ifr;
	int fd, ret;
	if (!adapter || !adapter->ifa_name)
		return false;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, adapter->ifa_name, sizeof(ifr.ifr_name));
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return false;
	ret = ioctl(fd, SIOCGIFHWADDR, &ifr);
	close(fd);
	if (ret != 0)
		return false;
	memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
	return true;
#else
	(void)adapter;
	(void)mac;
	log_cb(RETRO_LOG_ERROR, "DEV9: Unsupported OS, can't get MAC address\n");
	return false;
#endif
}

bool net_adapter_ip(const net_adapter* adapter, u8 ip[4])
{
	if (!adapter || !is_ipv4(adapter))
		return false;
	memcpy(ip, &((const struct sockaddr_in*)adapter->ifa_addr)->sin_addr, 4);
	return true;
}

bool net_adapter_netmask(const net_adapter* adapter, u8 mask[4])
{
	if (!adapter || !adapter->ifa_netmask || adapter->ifa_netmask->sa_family != AF_INET)
		return false;
	memcpy(mask, &((const struct sockaddr_in*)adapter->ifa_netmask)->sin_addr, 4);
	return true;
}

/* The line's whitespace-separated fields: up to max, NUL-terminated in
 * buf, which holds a copy of the line. */
static int split_fields(const char* line, char* buf, size_t size, char** field, int max)
{
	int n = 0;
	char* p = buf;
	strlcpy(buf, line, size);
	while (n < max)
	{
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (!*p || *p == '#' || *p == ';')
			break;
		field[n++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n')
			p++;
		if (!*p)
			break;
		*p++ = '\0';
	}
	return n;
}

bool net_resolv_line(const char* line, u8 ip[4])
{
	char buf[256];
	char* field[2];
	if (split_fields(line, buf, sizeof(buf), field, 2) != 2 || strcmp(field[0], "nameserver") != 0)
		return false;
	return inet_pton(AF_INET, field[1], ip) == 1;
}

bool net_route_line(const char* line, const char* ifname, u8 gw[4])
{
	/* Iface Destination Gateway Flags ...; the gateway is the address's
	 * bytes in host order, as hex. */
	char buf[256];
	char* field[3];
	char* end;
	unsigned long value;
	u32 addr;
	if (split_fields(line, buf, sizeof(buf), field, 3) != 3 || strcmp(field[0], ifname) != 0)
		return false;
	value = strtoul(field[2], &end, 16);
	if (end == field[2] || *end || value == 0 || value > 0xFFFFFFFFul)
		return false;
	addr = (u32)value;
	memcpy(gw, &addr, 4);
	return true;
}

/* Each line of the file at path offered to take; a line longer than the
 * buffer is skipped whole rather than read as two. */
static int scan_lines(const char* path, bool (*take)(const char* line, const char* arg, u8 out[4]),
	const char* arg, u8* out, int max)
{
	char line[256];
	bool whole = true;
	int n = 0;
	FILE* f = fopen(path, "r");
	if (!f)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to open %s\n", path);
		return 0;
	}
	while (n < max && fgets(line, sizeof(line), f))
	{
		const bool was_whole = whole;
		whole = strchr(line, '\n') != NULL;
		if (was_whole && take(line, arg, out + 4 * n))
			n++;
	}
	fclose(f);
	return n;
}

static bool take_nameserver(const char* line, const char* arg, u8 out[4])
{
	static const u8 systemd_stub[4] = {127, 0, 0, 53};
	(void)arg;
	if (!net_resolv_line(line, out))
		return false;
	if (memcmp(out, systemd_stub, 4) == 0)
		log_cb(RETRO_LOG_ERROR, "DEV9: systemd-resolved DNS server is not supported\n");
	return true;
}

int net_resolv_file(const char* path, u8* out, int max)
{
	return scan_lines(path, take_nameserver, NULL, out, max);
}

int net_adapter_dns(const net_adapter* adapter, u8* out, int max)
{
	/* DNS is system wide here, not the adapter's: /etc/resolv.conf's
	 * nameservers. On macOS most processes do not read it, but it is
	 * generated with the machine's servers in it. */
	if (!adapter)
		return 0;
	return net_resolv_file("/etc/resolv.conf", out, max);
}

#if defined(__linux__)
static bool take_route(const char* line, const char* ifname, u8 out[4])
{
	return net_route_line(line, ifname, out);
}

int net_adapter_gateways(const net_adapter* adapter, u8* out, int max)
{
	if (!adapter || !adapter->ifa_name)
		return 0;
	return scan_lines("/proc/net/route", take_route, adapter->ifa_name, out, max);
}
#elif defined(NET_ROUTE_SYSCTL)
int net_adapter_gateways(const net_adapter* adapter, u8* out, int max)
{
	int mib[6];
	size_t size = 0;
	size_t i;
	unsigned index;
	u8* buffer;
	int n = 0;

	if (!adapter || !adapter->ifa_name)
		return 0;
	index = if_nametoindex(adapter->ifa_name);
	if (index == 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get index for adapter\n");
		return 0;
	}

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_INET;
	mib[4] = NET_RT_DUMP;
	mib[5] = 0;
	if (sysctl(mib, 6, NULL, &size, NULL, 0) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to perform NET_RT_DUMP\n");
		return 0;
	}
	/* An estimate; routes may be added between the two calls. */
	size *= 2;
	buffer = (u8*)malloc(size);
	if (!buffer)
		return 0;
	if (sysctl(mib, 6, buffer, &size, NULL, 0) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to perform NET_RT_DUMP\n");
		free(buffer);
		return 0;
	}

	for (i = 0; i + sizeof(struct rt_msghdr) <= size && n < max;)
	{
		const struct rt_msghdr* hdr = (const struct rt_msghdr*)(buffer + i);
		if (hdr->rtm_msglen == 0 || i + hdr->rtm_msglen > size)
			break;
		if ((hdr->rtm_flags & RTF_GATEWAY) && (hdr->rtm_addrs & RTA_GATEWAY) && hdr->rtm_index == index)
		{
			/* AF_INET: each address a 16-byte sockaddr_in. The default
			 * route is the one with no destination. */
			const struct sockaddr_in* sa = (const struct sockaddr_in*)(hdr + 1);
			if (sa[RTAX_DST].sin_addr.s_addr == 0)
				memcpy(out + 4 * n++, &sa[RTAX_GATEWAY].sin_addr, 4);
		}
		i += hdr->rtm_msglen;
	}
	free(buffer);
	return n;
}
#else
int net_adapter_gateways(const net_adapter* adapter, u8* out, int max)
{
	(void)adapter;
	(void)out;
	(void)max;
	log_cb(RETRO_LOG_ERROR, "DEV9: Unsupported OS, can't find Gateway\n");
	return 0;
}
#endif
#endif
