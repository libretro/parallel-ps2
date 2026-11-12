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
#include "Config.h"
#include "DHCP_Server.h"

#define DHCP_COOKIE 0x63825363u
/* Messages (option 53). */
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_RELEASE 7
/* What the client may say it can take, and what is assumed without. */
#define DHCP_MIN_MESSAGE 576

static const u8 zero_ip[4] = {0, 0, 0, 0};
static const u8 internal_dns[4] = {192, 0, 2, 1};

static bool is_zero(const u8 ip[4])
{
	return memcmp(ip, zero_ip, 4) == 0;
}

static u32 rd32(const u8* p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void wr32(u8* p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

/* DNS1 and 2 from the adapter, where the settings ask for that. */
static void auto_dns(dhcp_server* s, const net_adapter* adapter, bool auto1, bool auto2)
{
	u8 found[8];
	const int count = net_adapter_dns(adapter, found, 2);
	if (auto1)
	{
		if (count >= 1)
			memcpy(s->dns1, found, 4);
		if (auto2 && count >= 2)
			memcpy(s->dns2, found + 4, 4);
	}
	else if (auto2 && count > 0)
		memcpy(s->dns2, found + 4 * (count - 1), 4); /* its second, else its first */
	if (is_zero(s->dns1) && !is_zero(s->dns2))
	{
		log_cb(RETRO_LOG_ERROR, "DHCP: DNS1 is zero, but DNS2 is valid, using DNS2 as DNS1\n");
		memcpy(s->dns1, s->dns2, 4);
		memset(s->dns2, 0, 4);
	}
}

static void dns_setting(u8 out[4], enum EmuDnsMode mode, const u8 manual[4])
{
	if (mode == EMU_DNS_MANUAL)
		memcpy(out, manual, 4);
	else if (mode == EMU_DNS_INTERNAL)
		memcpy(out, internal_dns, 4);
}

void dhcp_server_init(dhcp_server* s, const net_adapter* adapter, const u8 server_ip[4],
	const u8 ip_override[4], const u8 subnet_override[4], const u8 gateway_override[4])
{
	const EmuDev9Options* cfg = &EmuConfig.DEV9;
	int i;

	memset(s, 0, sizeof(*s));
	memcpy(s->server_ip, server_ip, 4);
	memcpy(s->ps2_ip, ip_override && !is_zero(ip_override) ? ip_override : cfg->PS2IP, 4);

	if (subnet_override && !is_zero(subnet_override))
		memcpy(s->netmask, subnet_override, 4);
	else if (cfg->AutoMask)
		net_adapter_netmask(adapter, s->netmask);
	else
		memcpy(s->netmask, cfg->Mask, 4);

	if (gateway_override && !is_zero(gateway_override))
		memcpy(s->gateway, gateway_override, 4);
	else if (cfg->AutoGateway)
		net_adapter_gateways(adapter, s->gateway, 1);
	else
		memcpy(s->gateway, cfg->Gateway, 4);

	dns_setting(s->dns1, cfg->ModeDNS1, cfg->DNS1);
	dns_setting(s->dns2, cfg->ModeDNS2, cfg->DNS2);
	auto_dns(s, adapter, cfg->ModeDNS1 == EMU_DNS_AUTO, cfg->ModeDNS2 == EMU_DNS_AUTO);

	if (!is_zero(s->netmask))
		for (i = 0; i < 4; i++)
			s->broadcast[i] = (u8)(s->ps2_ip[i] | ~s->netmask[i]);
}

/* What a request says, every option bounded by the packet. */
typedef struct dhcp_request
{
	u8 msg;
	u32 lease;
	u16 max_message;
	const u8* wanted; /* option 55, in the request */
	int wanted_len;
} dhcp_request;

static void read_options(const dhcp_server* s, const u8* pkt, int len, dhcp_request* req)
{
	int pos = DHCP_HEADER_SIZE;
	while (pos < len)
	{
		const int code = pkt[pos];
		int olen;
		const u8* data;
		if (code == 0)
		{
			pos++;
			continue;
		}
		if (code == 255)
			return;
		if (pos + 2 > len || pos + 2 + pkt[pos + 1] > len)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: DHCP_Packet: Unexpected end of packet\n");
			return;
		}
		olen = pkt[pos + 1];
		data = pkt + pos + 2;
		switch (code)
		{
			case 1:
				if (olen < 4 || memcmp(data, s->netmask, 4) != 0)
					log_cb(RETRO_LOG_ERROR, "DHCP: SubnetMask missmatch\n");
				break;
			case 3:
				if (olen != 4)
					log_cb(RETRO_LOG_ERROR, "DHCP: Routers count missmatch\n");
				if (olen < 4 || memcmp(data, s->gateway, 4) != 0)
					log_cb(RETRO_LOG_ERROR, "DHCP: RouterIP missmatch\n");
				break;
			case 6:
			{
				const int n = olen / 4;
				if (!((n == 0 && is_zero(s->dns1)) ||
						(n == 1 && !is_zero(s->dns1) && is_zero(s->dns2)) ||
						(n == 2 && !is_zero(s->dns2))))
					log_cb(RETRO_LOG_ERROR, "DHCP: DNS count missmatch\n");
				if ((n > 0 && memcmp(data, s->dns1, 4) != 0) || (n > 1 && memcmp(data + 4, s->dns2, 4) != 0))
					log_cb(RETRO_LOG_ERROR, "DHCP: DNS missmatch\n");
				break;
			}
			case 50:
				if (olen < 4 || memcmp(data, s->ps2_ip, 4) != 0)
					log_cb(RETRO_LOG_ERROR, "DHCP: ReqIP missmatch\n");
				break;
			case 51:
				if (olen >= 4)
					req->lease = rd32(data);
				break;
			case 53:
				if (olen >= 1)
					req->msg = data[0];
				break;
			case 54:
				if (olen < 4 || memcmp(data, s->server_ip, 4) != 0)
					log_cb(RETRO_LOG_ERROR, "DHCP: ServIP missmatch\n");
				break;
			case 55:
				req->wanted = data;
				req->wanted_len = olen;
				break;
			case 57:
				if (olen >= 2)
					req->max_message = (u16)((data[0] << 8) | data[1]);
				break;
			case 12: /* host name */
			case 56: /* message */
			case 60: /* class id */
			case 61: /* client id */
				break;
			default:
				log_cb(RETRO_LOG_ERROR, "DHCP: Got Unhandled Option %d\n", code);
				break;
		}
		pos += 2 + olen;
	}
}

/* An option into the reply, while it and the End after it fit. */
static bool put(u8* out, int* pos, int limit, int code, int len, const void* data)
{
	if (*pos + 2 + len + 1 > limit)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: DHCP_Packet: Oversized DHCP packet not handled\n");
		return false;
	}
	out[*pos] = (u8)code;
	out[*pos + 1] = (u8)len;
	memcpy(out + *pos + 2, data, (size_t)len);
	*pos += 2 + len;
	return true;
}

bool dhcp_server_send(dhcp_server* s, const u8* pkt, int len)
{
	dhcp_request req;
	dhcp_reply* reply;
	u8* out;
	int limit, pos, i;
	bool room = true;

	if (len < DHCP_HEADER_SIZE || rd32(pkt + 236) != DHCP_COOKIE)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: DHCP: Malformed request (%d bytes), dropping\n", len);
		return true;
	}
	memset(&req, 0, sizeof(req));
	req.lease = 86400;
	read_options(s, pkt, len, &req);
	if (req.msg == DHCP_RELEASE)
		return true;
	if (s->count == DHCP_REPLIES)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: DHCP: %d replies waiting, dropping a request\n", DHCP_REPLIES);
		return true;
	}

	/* The reply is padded to what the client takes, between the 576 all
	 * must and what one Ethernet frame holds. */
	limit = req.max_message < DHCP_MIN_MESSAGE ? DHCP_MIN_MESSAGE : req.max_message;
	if (limit > 1500)
		limit = 1500;
	limit -= 20 + 8;

	reply = &s->replies[(s->head + s->count) % DHCP_REPLIES];
	out = reply->data;
	memset(out, 0, (size_t)limit);
	out[0] = 2; /* BOOTREPLY */
	out[1] = pkt[1];
	out[2] = pkt[2];
	memcpy(out + 4, pkt + 4, 4); /* xid */
	memcpy(out + 16, s->ps2_ip, 4);
	memcpy(out + 20, s->server_ip, 4);
	memcpy(out + 28, pkt + 28, 6); /* the client's MAC */
	wr32(out + 236, DHCP_COOKIE);
	pos = DHCP_HEADER_SIZE;

	if (req.msg == DHCP_DISCOVER || req.msg == DHCP_REQUEST)
	{
		const u8 type = req.msg == DHCP_DISCOVER ? DHCP_OFFER : DHCP_ACK;
		u8 lease[4];
		room = put(out, &pos, limit, 53, 1, &type);
		for (i = 0; room && i < req.wanted_len; i++)
		{
			switch (req.wanted[i])
			{
				case 1:
					room = put(out, &pos, limit, 1, 4, s->netmask);
					break;
				case 3:
					if (!is_zero(s->gateway))
						room = put(out, &pos, limit, 3, 4, s->gateway);
					break;
				case 6:
					if (!is_zero(s->dns1))
					{
						u8 dns[8];
						memcpy(dns, s->dns1, 4);
						memcpy(dns + 4, s->dns2, 4);
						room = put(out, &pos, limit, 6, is_zero(s->dns2) ? 4 : 8, dns);
					}
					break;
				case 15:
					room = put(out, &pos, limit, 15, 5, "PCSX2");
					break;
				case 28:
					room = put(out, &pos, limit, 28, 4, s->broadcast);
					break;
				case 50:
					room = put(out, &pos, limit, 50, 4, s->ps2_ip);
					break;
				case 53: /* already there */
				case 54: /* added last */
					break;
				default:
					log_cb(RETRO_LOG_ERROR, "DHCP: Got Unhandled Request %d\n", req.wanted[i]);
					break;
			}
		}
		wr32(lease, req.lease);
		if (room)
			room = put(out, &pos, limit, 51, 4, lease);
	}
	if (room)
		put(out, &pos, limit, 54, 4, s->server_ip);
	out[pos] = 255;
	reply->len = (u16)limit;
	s->count++;
	return true;
}

int dhcp_server_recv(dhcp_server* s, u8* out, int cap)
{
	const dhcp_reply* reply;
	if (s->count == 0)
		return 0;
	reply = &s->replies[s->head];
	s->head = (s->head + 1) % DHCP_REPLIES;
	s->count--;
	if (reply->len > cap)
		return 0;
	memcpy(out, reply->data, reply->len);
	return reply->len;
}
