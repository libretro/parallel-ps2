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

/* Built where HAVE_PCAP is: it needs libpcap or Npcap's headers. */
#ifdef HAVE_PCAP
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <pcap.h>

#include "net.h"
#include "AdapterUtils.h"
#include "PacketReader/netpkt.h"

#ifndef PCAP_NETMASK_UNKNOWN
#define PCAP_NETMASK_UNKNOWN 0xffffffff
#endif

#ifdef _WIN32
#define PCAPPREFIX "\\Device\\NPF_"
#endif

/* The pcap adapter: the PS2's frames on the host's own wire, switched
 * (its own MAC, a filter for it) or bridged (the host's MAC, rewritten
 * on the way in and out). */
typedef struct pcap_adapter
{
	netif base;
	pcap_t* hpcap;
	bool switched;
	u8 ps2_ip[4];
	u8 host_mac[6];
} pcap_adapter;

/* Opens the device for capture, non-blocking. */
static bool open_pcap(pcap_adapter* a, const char* device, bool promiscuous)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	int dlt;
	const char* dlt_name;
	log_cb(RETRO_LOG_INFO, "DEV9: Opening adapter '%s'...\n", device);
	/* 65536 captures a whole packet on every MAC; 1 ms read timeout */
	a->hpcap = pcap_open_live(device, 65536, promiscuous ? 1 : 0, 1, errbuf);
	if (a->hpcap == NULL)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s\n", errbuf);
		log_cb(RETRO_LOG_ERROR, "DEV9: Unable to open the adapter. %s is not supported by pcap\n", device);
		return false;
	}
	if (pcap_setnonblock(a->hpcap, 1, errbuf) == -1)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Error setting non-blocking: %s\n", pcap_geterr(a->hpcap));
		log_cb(RETRO_LOG_ERROR, "DEV9: Continuing in blocking mode\n");
	}
	dlt = pcap_datalink(a->hpcap);
	dlt_name = pcap_datalink_val_to_name(dlt);
	log_cb(RETRO_LOG_ERROR, "DEV9: Device uses DLT %d: %s\n", dlt, dlt_name);
	if (dlt != DLT_EN10MB)
	{
		log_cb(RETRO_LOG_ERROR, "ERROR: Unsupported DataLink Type (%d): %s\n", dlt, dlt_name);
		pcap_close(a->hpcap);
		a->hpcap = NULL;
		return false;
	}
	log_cb(RETRO_LOG_INFO, "DEV9: Adapter Ok.\n");
	return true;
}

static bool set_switched_filter(pcap_adapter* a)
{
	const u8* mac = a->base.ps2_mac;
	struct bpf_program fp;
	char filter[128];
	int ret;
	snprintf(filter, sizeof(filter), "ether broadcast or ether dst %.2x:%.2x:%.2x:%.2x:%.2x:%.2x",
		mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	if (pcap_compile(a->hpcap, &fp, filter, 1, PCAP_NETMASK_UNKNOWN) == -1)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Error calling pcap_compile: %s\n", pcap_geterr(a->hpcap));
		return false;
	}
	ret = pcap_setfilter(a->hpcap, &fp);
	if (ret == -1)
		log_cb(RETRO_LOG_ERROR, "DEV9: Error setting filter: %s\n", pcap_geterr(a->hpcap));
	pcap_freecode(&fp);
	return ret != -1;
}

/* Bridged, the wire knows the PS2 by the host's MAC: what is the PS2's
 * by its address gets its MAC back. */
static void bridged_recv(pcap_adapter* a, NetPacket* pkt)
{
	u8* f = (u8*)pkt->buffer;
	net_eth eth;
	net_ip ip;
	net_arp arp;
	if (!net_eth_parse(f, pkt->size, &eth))
		return;
	if (eth.type == ETHERTYPE_IPV4 && net_ip_parse(eth.data, eth.len, &ip) && memcmp(ip.dst, a->ps2_ip, 4) == 0)
		memcpy(f, a->base.ps2_mac, 6);
	if (eth.type == ETHERTYPE_ARP && net_arp_parse(eth.data, eth.len, &arp) && memcmp(arp.tpa, a->ps2_ip, 4) == 0)
	{
		memcpy(f, a->base.ps2_mac, 6);
		memcpy(f + ETH_HEADER + 18, a->base.ps2_mac, 6); /* its target hardware address */
	}
}

static void bridged_send(pcap_adapter* a, NetPacket* pkt)
{
	u8* f = (u8*)pkt->buffer;
	net_eth eth;
	net_ip ip;
	net_arp arp;
	if (!net_eth_parse(f, pkt->size, &eth))
		return;
	if (eth.type == ETHERTYPE_IPV4 && net_ip_parse(eth.data, eth.len, &ip))
		memcpy(a->ps2_ip, ip.src, 4);
	if (eth.type == ETHERTYPE_ARP && net_arp_parse(eth.data, eth.len, &arp))
	{
		memcpy(a->ps2_ip, arp.spa, 4);
		memcpy(f + ETH_HEADER + 8, a->host_mac, 6); /* its sender hardware address */
	}
	memcpy(f + 6, a->host_mac, 6);
}

static bool pcap_recv(netif* n, NetPacket* pkt)
{
	pcap_adapter* a = (pcap_adapter*)n;
	struct pcap_pkthdr* header;
	const u_char* data;
	/* Bridged, pcap sees what is not the PS2's too: drop those and go on
	 * rather than wait a millisecond for the next. */
	while (pcap_next_ex(a->hpcap, &header, &data) > 0)
	{
		if (header->caplen != header->len || header->caplen > ETH_FRAME_MAX)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: Dropped frame of size: %u\n", header->len);
			continue;
		}
		memcpy(pkt->buffer, data, header->caplen);
		pkt->size = (int)header->caplen;
		if (!a->switched)
			bridged_recv(a, pkt);
		if (netif_verify(n, pkt, pkt->size))
		{
			netif_inspect_recv(n, pkt);
			return true;
		}
	}
	return false;
}

static bool pcap_send(netif* n, NetPacket* pkt)
{
	pcap_adapter* a = (pcap_adapter*)n;
	netif_inspect_send(n, pkt);
	if (netif_internal_send(n, pkt))
		return true;
	/* TODO: loop broadcasts back to the host when switched */
	if (!a->switched)
		bridged_send(a, pkt);
	return pcap_sendpacket(a->hpcap, (const u_char*)pkt->buffer, pkt->size) == 0;
}

static void pcap_reset(netif* n)
{
	(void)n;
}

static void pcap_reload(netif* n)
{
	void* list;
	const net_adapter* adapter = net_adapter_get(&list, EmuConfig.DEV9.EthDevice);
	netif_init_servers(n, adapter, false, NULL, NULL, NULL);
	net_adapter_free(list);
}

static void pcap_close_adapter(netif* n)
{
	(void)n;
}

static void pcap_destroy(netif* n)
{
	pcap_adapter* a = (pcap_adapter*)n;
	if (a->hpcap)
		pcap_close(a->hpcap);
	netif_deinit(n);
	free(a);
}

static const netif_ops pcap_ops = {
	pcap_recv, NULL, NULL, pcap_send, pcap_reset, pcap_reload, pcap_close_adapter, pcap_destroy};

netif* net_pcap_new(void)
{
	pcap_adapter* a;
	const net_adapter* adapter;
	void* list;
	char device[sizeof(EmuConfig.DEV9.EthDevice) + 16];

	if (!EmuConfig.DEV9.EthEnable)
		return NULL;
	a = (pcap_adapter*)calloc(1, sizeof(*a));
	if (!a)
		return NULL;
	netif_init(&a->base, &pcap_ops);
#ifdef _WIN32
	snprintf(device, sizeof(device), "%s%s", PCAPPREFIX, EmuConfig.DEV9.EthDevice);
#else
	snprintf(device, sizeof(device), "%s", EmuConfig.DEV9.EthDevice);
#endif
	a->switched = EmuConfig.DEV9.EthApi == EMU_NETAPI_PCAP_SWITCHED;
	if (!open_pcap(a, device, a->switched))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Can't open Device '%s'\n", EmuConfig.DEV9.EthDevice);
		pcap_destroy(&a->base);
		return NULL;
	}

	adapter = net_adapter_get(&list, EmuConfig.DEV9.EthDevice);
	if (!adapter)
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get adapter information\n");
	if (adapter && net_adapter_mac(adapter, a->host_mac))
	{
		/* The host's last 2 bytes, swapped, to be unique on Xlink */
		u8 mac[6];
		memcpy(mac, a->base.ps2_mac, 6);
		mac[5] = a->host_mac[4];
		mac[4] = a->host_mac[5];
		netif_set_mac(&a->base, mac);
	}
	else if (a->switched)
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get MAC address for adapter, proceeding with hardcoded MAC address\n");
	else
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed to get MAC address for adapter\n");
		net_adapter_free(list);
		pcap_destroy(&a->base);
		return NULL;
	}
	if (a->switched && !set_switched_filter(a))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Can't open Device '%s'\n", EmuConfig.DEV9.EthDevice);
		net_adapter_free(list);
		pcap_destroy(&a->base);
		return NULL;
	}
	netif_init_servers(&a->base, adapter, false, NULL, NULL, NULL);
	net_adapter_free(list);
	return &a->base;
}

#endif
