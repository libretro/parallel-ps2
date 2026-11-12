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
#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#if defined(__linux__)
#define ICMP_SOCKETS_LINUX
#endif
#if defined(__FreeBSD__) || defined(__APPLE__)
#define ICMP_SOCKETS_BSD
#endif
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#ifdef __linux__
#include <linux/errqueue.h>
#endif
#include <unistd.h>
#endif

#include <features/features_cpu.h>

#include "DEV9/sockets.h"

/* Pings, which every platform makes awkward:
 *
 * Windows has an API for them (IcmpSendEcho2), asynchronous with an
 * event.
 *
 * Linux has socket(PF_INET, SOCK_DGRAM, IPPROTO_ICMP), which filters the
 * replies itself but needs the net.ipv4.ping_group_range sysctl, off on
 * many distributions; else a raw socket, with CAP_NET_RAW. Errors come
 * as sock_extended_err control messages (IP_RECVERR).
 *
 * macOS has the same datagram socket without the restriction, but does
 * not filter the replies, and reports errors as ordinary packets.
 * FreeBSD has only raw sockets. */

#define ICMP_TIMEOUT_USEC ((s64)30 * 1000000)
/* What a reply may echo back to fit one frame. */
#define ICMP_DATA_MAX (ETH_FRAME_MAX - NET_ICMP_DATA)

/* A finished ping: an ICMP type and code, or -1 for an error and -2 for
 * a timeout, from address, with data. */
typedef struct ping_result
{
	int type;
	int code;
	u8 address[4];
	const u8* data;
	int len;
} ping_result;

#ifndef _WIN32
enum
{
	PING_ICMP,
	PING_RAW,
	PING_NONE
};
static int s_ping_kind = PING_ICMP;
#endif

static bool ping_open(icmp_ping* p)
{
#ifdef _WIN32
	p->file = IcmpCreateFile();
	if (p->file == INVALID_HANDLE_VALUE)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Failed to Create Icmp File\n");
		return false;
	}
	p->event = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (p->event == NULL)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Failed to Create Event\n");
		IcmpCloseHandle(p->file);
		p->file = INVALID_HANDLE_VALUE;
		return false;
	}
	return true;
#else
	p->sock = -1;
#if defined(ICMP_SOCKETS_LINUX) || defined(ICMP_SOCKETS_BSD)
	if (s_ping_kind == PING_ICMP)
	{
		p->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
		if (p->sock != -1)
			return true;
		log_cb(RETRO_LOG_DEBUG, "DEV9: ICMP: Failed To Open ICMP Socket\n");
		s_ping_kind = PING_RAW;
	}
	if (s_ping_kind == PING_RAW)
	{
		p->sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
		if (p->sock != -1)
			return true;
		log_cb(RETRO_LOG_DEBUG, "DEV9: ICMP: Failed To Open RAW Socket\n");
	}
#endif
	log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Failed To Ping\n");
	return false;
#endif
}

static void ping_close(icmp_ping* p)
{
#ifdef _WIN32
	if (p->file != INVALID_HANDLE_VALUE)
		IcmpCloseHandle(p->file);
	if (p->event != NULL)
		CloseHandle(p->event);
	p->file = INVALID_HANDLE_VALUE;
	p->event = NULL;
#else
	if (p->sock != -1)
		close(p->sock);
	p->sock = -1;
#endif
}

#ifdef _WIN32
static bool ping_recv(icmp_ping* p, ping_result* r)
{
	const ICMP_ECHO_REPLY* reply = (const ICMP_ECHO_REPLY*)p->buffer;
	if (WaitForSingleObject(p->event, 0) != WAIT_OBJECT_0)
		return false;
	ResetEvent(p->event);
	IcmpParseReplies(p->buffer, (DWORD)p->buffer_len);
	r->data = NULL;
	r->len = 0;
	switch (reply->Status)
	{
		case IP_SUCCESS: r->type = 0; r->code = 0; break;
		case IP_DEST_NET_UNREACHABLE: r->type = 3; r->code = 0; break;
		case IP_DEST_HOST_UNREACHABLE: r->type = 3; r->code = 1; break;
		case IP_DEST_PROT_UNREACHABLE: r->type = 3; r->code = 2; break;
		case IP_DEST_PORT_UNREACHABLE: r->type = 3; r->code = 3; break;
		case IP_PACKET_TOO_BIG: r->type = 3; r->code = 4; break;
		case IP_BAD_ROUTE: r->type = 3; r->code = 5; break; /* bad source route */
		case IP_BAD_DESTINATION: r->type = 3; r->code = 7; break;
		case IP_REQ_TIMED_OUT: r->type = -2; r->code = 0; break;
		case IP_TTL_EXPIRED_TRANSIT: r->type = 11; r->code = 0; break;
		case IP_TTL_EXPIRED_REASSEM: r->type = 11; r->code = 1; break;
		case IP_SOURCE_QUENCH: r->type = 4; r->code = 0; break;
		default: r->type = -1; r->code = (int)reply->Status; break;
	}
	/* The echoed data, where it lies inside the buffer */
	if ((const u8*)reply->Data >= p->buffer &&
		(const u8*)reply->Data + reply->DataSize <= p->buffer + p->buffer_len)
	{
		r->data = (const u8*)reply->Data;
		r->len = reply->DataSize;
	}
	memcpy(r->address, &reply->Address, 4);
	return true;
}

static bool ping_send(icmp_ping* p, const u8 adapter_ip[4], const u8 dst[4], int ttl, const u8* data, int len)
{
	static const u8 any[4] = {0, 0, 0, 0};
	IP_OPTION_INFORMATION info;
	IPAddr src_addr, dst_addr;
	DWORD ret;
	memset(&info, 0, sizeof(info));
	info.Ttl = (UCHAR)ttl;
	memcpy(&src_addr, adapter_ip, 4);
	memcpy(&dst_addr, dst, 4);
	p->buffer_len = ICMP_PING_BUFFER;
	if (memcmp(adapter_ip, any, 4) != 0)
		ret = IcmpSendEcho2Ex(p->file, p->event, NULL, NULL, src_addr, dst_addr, (LPVOID)data, (WORD)len,
			&info, p->buffer, (DWORD)p->buffer_len, (DWORD)(ICMP_TIMEOUT_USEC / 1000));
	else
		ret = IcmpSendEcho2(p->file, p->event, NULL, NULL, dst_addr, (LPVOID)data, (WORD)len,
			&info, p->buffer, (DWORD)p->buffer_len, (DWORD)(ICMP_TIMEOUT_USEC / 1000));
	if (ret == 0)
		ret = GetLastError();
	if (ret != ERROR_IO_PENDING)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Failed to Send Echo, %lu\n", (unsigned long)GetLastError());
		return false;
	}
	return true;
}
#else
static bool timed_out(icmp_ping* p, ping_result* r)
{
	if ((s64)cpu_features_get_time_usec() <= p->deadline)
		return false;
	r->type = -2;
	r->code = 0;
	return true;
}

static bool ping_recv(icmp_ping* p, ping_result* r)
{
	struct sockaddr_in from;
	struct iovec iov;
	struct msghdr msg;
	net_icmp icmp;
	int ret;
#if defined(ICMP_SOCKETS_LINUX)
	u8 cbuf[64];
	struct cmsghdr* cmsg;
	struct sock_extended_err* ee = NULL;
#endif

	r->data = NULL;
	r->len = 0;
	memset(r->address, 0, 4);
	if (p->sock == -1)
	{
		r->type = -1;
		r->code = 0;
		return true;
	}
	memset(&from, 0, sizeof(from));
	memset(&msg, 0, sizeof(msg));
	iov.iov_base = p->buffer;
	iov.iov_len = ICMP_PING_BUFFER;
	msg.msg_name = &from;
	msg.msg_namelen = sizeof(from);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	ret = recvmsg(p->sock, &msg, MSG_DONTWAIT);
	if (ret == -1)
	{
		const int err = errno;
		if (err == EAGAIN || err == EWOULDBLOCK)
			return timed_out(p, r);
#if defined(ICMP_SOCKETS_LINUX)
		msg.msg_control = cbuf;
		msg.msg_controllen = sizeof(cbuf);
		ret = recvmsg(p->sock, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
#endif
		if (ret == -1)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: RecvMsg Error: %d\n", err);
			r->type = -1;
			r->code = err;
			return true;
		}
	}
	if (msg.msg_flags & MSG_TRUNC)
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: RecvMsg Truncated\n");

#if defined(ICMP_SOCKETS_LINUX)
	if (msg.msg_flags & MSG_CTRUNC)
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: RecvMsg Control Truncated\n");
	if (msg.msg_controllen)
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg))
			if (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_RECVERR)
				ee = (struct sock_extended_err*)CMSG_DATA(cmsg);
	if (ee)
	{
		if (ee->ee_origin == SO_EE_ORIGIN_ICMP)
		{
			const struct sockaddr_in* off = (const struct sockaddr_in*)SO_EE_OFFENDER(ee);
			r->type = ee->ee_type;
			r->code = ee->ee_code;
			memcpy(r->address, &off->sin_addr, 4);
			return true;
		}
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Recv Error %d\n", ee->ee_errno);
		r->type = -1;
		r->code = (int)ee->ee_errno;
		return true;
	}
#endif

	{
		const u8* body = p->buffer;
		int len = ret;
		/* A raw socket's reply has its IP header in front, as every
		 * reply does on the BSDs. */
#if defined(ICMP_SOCKETS_LINUX)
		const bool has_ip = s_ping_kind == PING_RAW;
#else
		const bool has_ip = true;
#endif
		if (has_ip)
		{
			net_ip ip;
			if (!net_ip_parse(p->buffer, ret, &ip))
				return false;
			body = ip.data;
			len = ip.len;
		}
		if (!net_icmp_parse(body, len, &icmp))
			return false;
	}
	r->type = icmp.type;
	r->code = icmp.code;
	memcpy(r->address, &from.sin_addr, 4);
	if (icmp.type == 0)
	{
		if (s_ping_kind == PING_RAW && net_rd16(icmp.rest) != p->id)
			return false;
		r->data = icmp.data;
		r->len = icmp.len;
		return true;
	}
#if defined(ICMP_SOCKETS_BSD)
	if (icmp.type == 3 || icmp.type == 4 || icmp.type == 5 || icmp.type == 11)
	{
		/* An error, about the packet it quotes: ours if its id is. */
		net_ip inner;
		net_icmp quoted;
		if (!net_ip_parse(icmp.data, icmp.len, &inner) || !net_icmp_parse(inner.data, inner.len, &quoted) ||
			net_rd16(quoted.rest) != p->id)
			return false;
		return true;
	}
#else
	log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Unexpected packet\n");
#endif
	return false;
}

static bool ping_fail(icmp_ping* p, const char* what)
{
	log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: %s. Error: %d\n", what, errno);
	close(p->sock);
	p->sock = -1;
	return false;
}

static bool ping_send(icmp_ping* p, const u8 adapter_ip[4], const u8 dst[4], int ttl, const u8* data, int len)
{
	static const u8 any[4] = {0, 0, 0, 0};
	struct sockaddr_in sa;
	u8 packet[ICMP_HEADER + ICMP_DATA_MAX];

	p->deadline = (s64)cpu_features_get_time_usec() + ICMP_TIMEOUT_USEC;
	if (memcmp(adapter_ip, any, 4) != 0)
	{
		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		memcpy(&sa.sin_addr, adapter_ip, 4);
		if (bind(p->sock, (const struct sockaddr*)&sa, sizeof(sa)) == -1)
			return ping_fail(p, "Failed to bind socket");
	}
#if defined(ICMP_SOCKETS_LINUX)
	{
		const int on = 1;
		if (setsockopt(p->sock, SOL_IP, IP_RECVERR, (const char*)&on, sizeof(on)))
			return ping_fail(p, "Failed to setsockopt IP_RECVERR");
	}
#endif
	if (setsockopt(p->sock, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof(ttl)) == -1)
		return ping_fail(p, "Failed to set TTL");
#if defined(ICMP_SOCKETS_LINUX)
	if (s_ping_kind == PING_ICMP)
	{
		/* The kernel's own identifier, which it replaces ours with */
		socklen_t salen = sizeof(sa);
		if (getsockname(p->sock, (struct sockaddr*)&sa, &salen) == -1)
			return ping_fail(p, "Failed to get id");
		p->id = ntohs(sa.sin_port);
	}
	else
#endif
		p->id = (u16)((s64)cpu_features_get_time_usec() / 1000);

	if (len > ICMP_DATA_MAX)
		len = ICMP_DATA_MAX;
	memcpy(packet + ICMP_HEADER, data, (size_t)len);
	/* An echo request: our identifier, sequence 1 */
	packet[0] = 8;
	packet[1] = 0;
	net_wr16(packet + 2, 0);
	net_wr16(packet + 4, p->id);
	net_wr16(packet + 6, 1);
	net_wr16(packet + 2, net_fold(net_sum(packet, ICMP_HEADER + len, 0)));
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	memcpy(&sa.sin_addr, dst, 4);
	if (sendto(p->sock, (const char*)packet, (size_t)(ICMP_HEADER + len), 0, (const struct sockaddr*)&sa, sizeof(sa)) == -1)
		return ping_fail(p, "Send Error");
	return true;
}
#endif

/* ---- The session: the PS2's pings to one address -------------------- */

static bool icmp_poll_interest(net_session* s, uintptr_t* fd, int* events)
{
	(void)fd;
	*events = 0;
	/* A ping waits on its reply or its timeout, which are polled. */
	return s->u.icmp.pings == 0;
}

static int icmp_recv(net_session* s, u8* frame)
{
	sockets_adapter* a = s->owner;
	int i;
	for (i = 0; i < NET_PINGS; i++)
	{
		icmp_ping* p = &a->pings[i];
		ping_result r;
		int len = 0;
		if (!p->used || p->session != s || !ping_recv(p, &r))
			continue;
		if (r.type >= 0)
		{
			u8* data = frame + NET_ICMP_DATA;
			if (r.type == 0)
			{
				len = r.len > ICMP_DATA_MAX ? ICMP_DATA_MAX : r.len;
				memcpy(data, r.data, (size_t)len);
			}
			else
			{
				len = p->original_len;
				memcpy(data, p->original, (size_t)len);
			}
			memcpy(s->remote_ip, r.address, 4);
			net_put_icmp(frame, (u8)r.type, (u8)r.code, p->rest, len);
			len += ICMP_HEADER;
			log_cb(RETRO_LOG_DEBUG, "DEV9: ICMP: Return Ping\n");
		}
		else if (r.type == -1)
			log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Unexpected ICMP status %d\n", r.code);
		else
			log_cb(RETRO_LOG_DEBUG, "DEV9: ICMP: ICMP timeout\n");
		ping_close(p);
		sockets_ping_free(p);
		if (--s->u.icmp.pings == 0)
			session_closed(s);
		return len;
	}
	return 0;
}

/* The PS2 says a port of its is closed: the session the host's packet
 * belonged to is reset. The packet is quoted after the ICMP header, an
 * IPv4 header first, possibly a few bytes late. */
static void port_closed(net_session* s, const net_icmp* icmp)
{
	net_ip inner;
	net_key key;
	net_session* target;
	int off = 0;
	log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Recived Packet Rejected, Port Closed\n");
	while (off < icmp->len && (icmp->data[off] & 0xF0) != 0x40)
		off++;
	if (off)
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Malformed ICMP Packet, payload delayed %d bytes\n", off);
	if (!net_ip_parse(icmp->data + off, icmp->len - off, &inner))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Failed To Reset Rejected Connection\n");
		return;
	}
	memset(&key, 0, sizeof(key));
	memcpy(key.ip, inner.src, 4);
	key.proto = inner.proto;
	if ((inner.proto == IP_PROTO_TCP || inner.proto == IP_PROTO_UDP) && inner.len >= 4)
	{
		key.srv_port = net_rd16(inner.data);
		key.ps2_port = net_rd16(inner.data + 2);
	}
	target = sockets_find(s->owner, &key);
	if (!target)
	{
		/* A fixed port's */
		memset(key.ip, 0, 4);
		key.srv_port = 0;
		target = sockets_find(s->owner, &key);
	}
	if (!target)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Failed To Reset Rejected Connection\n");
		return;
	}
	target->ops->reset(target);
	log_cb(RETRO_LOG_INFO, "DEV9: ICMP: Reset Rejected Connection\n");
}

static bool icmp_send(net_session* s, const net_ip* ip)
{
	net_icmp icmp;
	icmp_ping* p;
	int original;

	if (!net_icmp_parse(ip->data, ip->len, &icmp))
		return false;
	switch (icmp.type)
	{
		case 3:
			if (icmp.code == 3)
				port_closed(s, &icmp);
			else
				log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Unsupported ICMP Code For Destination Unreachable %d\n", icmp.code);
			return true;
		case 8:
			log_cb(RETRO_LOG_DEBUG, "DEV9: ICMP: Send Ping\n");
			p = sockets_ping_alloc(s->owner, s);
			if (!p)
			{
				log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: %d pings in flight, dropping one\n", NET_PINGS);
				if (s->u.icmp.pings == 0)
					session_closed(s);
				return false;
			}
			if (!ping_open(p) || !ping_send(p, s->adapter_ip, s->remote_ip, ip->ttl, icmp.data, icmp.len))
			{
				ping_close(p);
				sockets_ping_free(p);
				if (s->u.icmp.pings == 0)
					session_closed(s);
				return false;
			}
			memcpy(p->rest, icmp.rest, 4);
			/* The PS2's header and its first 8 bytes, for an error */
			original = ip->hlen + (ip->len < 8 ? ip->len : 8);
			p->original_len = original;
			memcpy(p->original, ip->hdr, (size_t)original);
			s->u.icmp.pings++;
			return true;
		default:
			log_cb(RETRO_LOG_ERROR, "DEV9: ICMP: Unsupported ICMP Type %d\n", icmp.type);
			return true;
	}
}

static void icmp_reset(net_session* s)
{
	session_closed(s);
}

static void icmp_release(net_session* s)
{
	sockets_adapter* a = s->owner;
	int i;
	for (i = 0; i < NET_PINGS; i++)
	{
		icmp_ping* p = &a->pings[i];
		if (p->used && p->session == s)
		{
			ping_close(p);
			sockets_ping_free(p);
		}
	}
	s->u.icmp.pings = 0;
}

static const net_session_ops icmp_ops = {
	icmp_recv, icmp_poll_interest, icmp_send, icmp_reset, icmp_release};

void icmp_session_init(net_session* s)
{
	memset(&s->u.icmp, 0, sizeof(s->u.icmp));
	s->ops = &icmp_ops;
}
