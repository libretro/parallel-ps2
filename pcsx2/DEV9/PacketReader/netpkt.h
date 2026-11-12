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

#ifndef DEV9_NETPKT_H
#define DEV9_NETPKT_H

/* The frames DEV9 reads and writes: Ethernet, ARP, IPv4 and the ICMP,
 * UDP and TCP inside it. A frame is bytes in a buffer; a parse is a view
 * of them, every length checked against what is there, and a write puts
 * a header in front of a payload already in place. Addresses are bytes
 * in network order: 6 for a MAC, 4 for IPv4. */

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ETH_HEADER 14
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
/* What one frame carries at most: the PS2's MTU, as the SMAP's. */
#define ETH_FRAME_MAX 1514

#define IP_HEADER 20
#define IP_PROTO_ICMP 1
#define IP_PROTO_IGMP 2
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define UDP_HEADER 8
#define ICMP_HEADER 8
#define TCP_HEADER 20
#define ARP_IPV4 28

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

u16 net_rd16(const u8* p);
u32 net_rd32(const u8* p);
void net_wr16(u8* p, u32 v);
void net_wr32(u8* p, u32 v);

/* The ones' complement sum of p, added to sum, unfolded; and its folded
 * complement, the checksum. */
u32 net_sum(const u8* p, int len, u32 sum);
u16 net_fold(u32 sum);
/* The sum of an IPv4 pseudo header, for UDP and TCP. */
u32 net_pseudo_sum(const u8 src[4], const u8 dst[4], u8 proto, int len);

typedef struct net_eth
{
	const u8* dst;
	const u8* src;
	u16 type;
	const u8* data;
	int len;
} net_eth;

typedef struct net_ip
{
	const u8* hdr;
	int hlen;
	u8 proto;
	u8 ttl;
	const u8* src;
	const u8* dst;
	const u8* data; /* the payload: hdr + hlen */
	int len;        /* as the total length says, no further than the buffer */
} net_ip;

typedef struct net_udp
{
	u16 sport;
	u16 dport;
	const u8* data;
	int len;
} net_udp;

typedef struct net_tcp
{
	u16 sport;
	u16 dport;
	u32 seq;
	u32 ack;
	u8 flags;
	u16 window;
	int hlen;
	const u8* data;
	int len;
	/* Its options; -1 where absent. */
	int mss;
	int wscale;
	bool has_ts;
	u32 ts_val;
	u32 ts_ecr;
	int unknown; /* the first option it does not know, else -1 */
} net_tcp;

typedef struct net_icmp
{
	u8 type;
	u8 code;
	const u8* rest; /* the 4 bytes after the checksum */
	const u8* data;
	int len;
} net_icmp;

/* Ethernet over IPv4 ARP: the addresses are 6 and 4 bytes, or it is
 * not one. */
typedef struct net_arp
{
	u16 op;
	const u8* sha;
	const u8* spa;
	const u8* tha;
	const u8* tpa;
} net_arp;

bool net_eth_parse(const u8* p, int len, net_eth* out);
/* IPv4 with its header, options included, whole; a total length past
 * the buffer is taken as the buffer's, one short of the header is not
 * IPv4. */
bool net_ip_parse(const u8* p, int len, net_ip* out);
bool net_ip_csum_ok(const net_ip* ip);
bool net_udp_parse(const u8* p, int len, net_udp* out);
bool net_tcp_parse(const u8* p, int len, net_tcp* out);
bool net_icmp_parse(const u8* p, int len, net_icmp* out);
bool net_arp_parse(const u8* p, int len, net_arp* out);
/* A UDP or TCP segment's checksum against its pseudo header. UDP's 0 is
 * no checksum. */
bool net_l4_csum_ok(const net_ip* ip);

/* Writing a frame: the payload goes first, at the offset these return,
 * then the headers in front of it, innermost first. */
#define NET_IP_DATA (ETH_HEADER + IP_HEADER)
#define NET_UDP_DATA (NET_IP_DATA + UDP_HEADER)
#define NET_ICMP_DATA (NET_IP_DATA + ICMP_HEADER)
#define NET_TCP_DATA(optlen) (NET_IP_DATA + TCP_HEADER + (optlen))

void net_put_eth(u8* frame, const u8 dst[6], const u8 src[6], u16 type);
/* An IPv4 header, no options, of a payload len long; its checksum. */
void net_put_ip(u8* frame, const u8 src[4], const u8 dst[4], u8 proto, u8 ttl, u16 id, int len);
/* A UDP header at frame + NET_IP_DATA for len bytes of data after it,
 * and its checksum. */
void net_put_udp(u8* frame, const u8 src[4], const u8 dst[4], u16 sport, u16 dport, int len);
/* An ICMP header and its checksum over len bytes of data after it. */
void net_put_icmp(u8* frame, u8 type, u8 code, const u8 rest[4], int len);
/* A TCP header with optlen bytes of options already after it, then len
 * bytes of data, and its checksum. */
void net_put_tcp(u8* frame, const u8 src[4], const u8 dst[4], u16 sport, u16 dport,
	u32 seq, u32 ack, u8 flags, u16 window, int optlen, int len);

#ifdef __cplusplus
}
#endif

#endif
