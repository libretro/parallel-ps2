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
#include "netpkt.h"

u16 net_rd16(const u8* p)
{
	return (u16)((p[0] << 8) | p[1]);
}

u32 net_rd32(const u8* p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

void net_wr16(u8* p, u32 v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

void net_wr32(u8* p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

u32 net_sum(const u8* p, int len, u32 sum)
{
	int i;
	for (i = 0; i + 1 < len; i += 2)
		sum += (u32)((p[i] << 8) | p[i + 1]);
	if (len & 1)
		sum += (u32)p[len - 1] << 8;
	return sum;
}

u16 net_fold(u32 sum)
{
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (u16)~sum;
}

u32 net_pseudo_sum(const u8 src[4], const u8 dst[4], u8 proto, int len)
{
	u32 sum = net_sum(src, 4, 0);
	sum = net_sum(dst, 4, sum);
	return sum + proto + (u32)len;
}

bool net_eth_parse(const u8* p, int len, net_eth* out)
{
	if (len < ETH_HEADER)
		return false;
	out->dst = p;
	out->src = p + 6;
	out->type = net_rd16(p + 12);
	out->data = p + ETH_HEADER;
	out->len = len - ETH_HEADER;
	return true;
}

bool net_ip_parse(const u8* p, int len, net_ip* out)
{
	int hlen, total;
	if (len < IP_HEADER || (p[0] >> 4) != 4)
		return false;
	hlen = (p[0] & 0xF) << 2;
	total = net_rd16(p + 2);
	if (total > len)
		total = len;
	if (hlen < IP_HEADER || hlen > total)
		return false;
	out->hdr = p;
	out->hlen = hlen;
	out->ttl = p[8];
	out->proto = p[9];
	out->src = p + 12;
	out->dst = p + 16;
	out->data = p + hlen;
	out->len = total - hlen;
	return true;
}

bool net_ip_csum_ok(const net_ip* ip)
{
	return net_fold(net_sum(ip->hdr, ip->hlen, 0)) == 0;
}

bool net_udp_parse(const u8* p, int len, net_udp* out)
{
	int ulen;
	if (len < UDP_HEADER)
		return false;
	ulen = net_rd16(p + 4);
	if (ulen > len)
		ulen = len;
	if (ulen < UDP_HEADER)
		return false;
	out->sport = net_rd16(p);
	out->dport = net_rd16(p + 2);
	out->data = p + UDP_HEADER;
	out->len = ulen - UDP_HEADER;
	return true;
}

bool net_tcp_parse(const u8* p, int len, net_tcp* out)
{
	int hlen, i;
	if (len < TCP_HEADER)
		return false;
	hlen = (p[12] >> 4) << 2;
	if (hlen < TCP_HEADER || hlen > len)
		return false;
	out->sport = net_rd16(p);
	out->dport = net_rd16(p + 2);
	out->seq = net_rd32(p + 4);
	out->ack = net_rd32(p + 8);
	out->flags = p[13];
	out->window = net_rd16(p + 14);
	out->hlen = hlen;
	out->data = p + hlen;
	out->len = len - hlen;
	out->mss = -1;
	out->wscale = -1;
	out->has_ts = false;
	out->ts_val = 0;
	out->ts_ecr = 0;
	out->unknown = -1;
	/* The options, each inside the header or none past it read. */
	for (i = TCP_HEADER; i < hlen;)
	{
		const int kind = p[i];
		int olen;
		if (kind == 0)
			break;
		if (kind == 1)
		{
			i++;
			continue;
		}
		if (i + 2 > hlen)
			break;
		olen = p[i + 1];
		if (olen < 2 || i + olen > hlen)
			break;
		if (kind == 2 && olen == 4)
			out->mss = net_rd16(p + i + 2);
		else if (kind == 3 && olen == 3)
			out->wscale = p[i + 2];
		else if (kind == 8 && olen == 10)
		{
			out->has_ts = true;
			out->ts_val = net_rd32(p + i + 2);
			out->ts_ecr = net_rd32(p + i + 6);
		}
		else if (out->unknown < 0)
			out->unknown = kind;
		i += olen;
	}
	return true;
}

bool net_icmp_parse(const u8* p, int len, net_icmp* out)
{
	if (len < ICMP_HEADER)
		return false;
	out->type = p[0];
	out->code = p[1];
	out->rest = p + 4;
	out->data = p + ICMP_HEADER;
	out->len = len - ICMP_HEADER;
	return true;
}

bool net_arp_parse(const u8* p, int len, net_arp* out)
{
	if (len < ARP_IPV4 || net_rd16(p) != 1 || net_rd16(p + 2) != ETHERTYPE_IPV4 || p[4] != 6 || p[5] != 4)
		return false;
	out->op = net_rd16(p + 6);
	out->sha = p + 8;
	out->spa = p + 14;
	out->tha = p + 18;
	out->tpa = p + 24;
	return true;
}

bool net_l4_csum_ok(const net_ip* ip)
{
	if (ip->proto == IP_PROTO_UDP && ip->len >= UDP_HEADER && net_rd16(ip->data + 6) == 0)
		return true;
	return net_fold(net_sum(ip->data, ip->len, net_pseudo_sum(ip->src, ip->dst, ip->proto, ip->len))) == 0;
}

void net_put_eth(u8* frame, const u8 dst[6], const u8 src[6], u16 type)
{
	memcpy(frame, dst, 6);
	memcpy(frame + 6, src, 6);
	net_wr16(frame + 12, type);
}

void net_put_ip(u8* frame, const u8 src[4], const u8 dst[4], u8 proto, u8 ttl, u16 id, int len)
{
	u8* h = frame + ETH_HEADER;
	h[0] = 0x45;
	h[1] = 0;
	net_wr16(h + 2, (u32)(IP_HEADER + len));
	net_wr16(h + 4, id);
	h[6] = 0;
	h[7] = 0;
	h[8] = ttl;
	h[9] = proto;
	net_wr16(h + 10, 0);
	memcpy(h + 12, src, 4);
	memcpy(h + 16, dst, 4);
	net_wr16(h + 10, net_fold(net_sum(h, IP_HEADER, 0)));
}

void net_put_udp(u8* frame, const u8 src[4], const u8 dst[4], u16 sport, u16 dport, int len)
{
	u8* u = frame + NET_IP_DATA;
	u16 csum;
	net_wr16(u, sport);
	net_wr16(u + 2, dport);
	net_wr16(u + 4, (u32)(UDP_HEADER + len));
	net_wr16(u + 6, 0);
	csum = net_fold(net_sum(u, UDP_HEADER + len, net_pseudo_sum(src, dst, IP_PROTO_UDP, UDP_HEADER + len)));
	net_wr16(u + 6, csum ? csum : 0xFFFF);
}

void net_put_icmp(u8* frame, u8 type, u8 code, const u8 rest[4], int len)
{
	u8* c = frame + NET_IP_DATA;
	c[0] = type;
	c[1] = code;
	net_wr16(c + 2, 0);
	memcpy(c + 4, rest, 4);
	net_wr16(c + 2, net_fold(net_sum(c, ICMP_HEADER + len, 0)));
}

void net_put_tcp(u8* frame, const u8 src[4], const u8 dst[4], u16 sport, u16 dport,
	u32 seq, u32 ack, u8 flags, u16 window, int optlen, int len)
{
	u8* t = frame + NET_IP_DATA;
	const int seg = TCP_HEADER + optlen + len;
	net_wr16(t, sport);
	net_wr16(t + 2, dport);
	net_wr32(t + 4, seq);
	net_wr32(t + 8, ack);
	t[12] = (u8)(((TCP_HEADER + optlen) >> 2) << 4);
	t[13] = flags;
	net_wr16(t + 14, window);
	net_wr16(t + 16, 0);
	net_wr16(t + 18, 0);
	net_wr16(t + 16, net_fold(net_sum(t, seg, net_pseudo_sum(src, dst, IP_PROTO_TCP, seg))));
}
