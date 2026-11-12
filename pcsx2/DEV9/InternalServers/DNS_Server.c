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

#include <stdlib.h>
#include <string.h>

#include "common/Pcsx2Defs.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>

#include "DNS_Server.h"
#include "DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.h"

/* Queries answered at once; one more is dropped until one is taken. */
#define DNS_SLOTS 8
/* How long the host's answer is said to hold, in seconds. */
#define DNS_TTL 10800

enum
{
	SLOT_FREE,     /* the network thread's to fill */
	SLOT_LOOKING,  /* the lookups' */
	SLOT_READY     /* the network thread's to take */
};

struct dns_slot;

typedef struct dns_lookup
{
	struct dns_server* server;
	struct dns_slot* slot;
	int index;
} dns_lookup;

typedef struct dns_slot
{
	retro_atomic_int_t state;
	retro_atomic_int_t pending; /* lookups still running */
	dns_query query;
	u16 client_port;
	u8 query_bytes[DNS_MAX_UDP];
	bool asked[DNS_MAX_QUESTIONS]; /* an A record of class IN */
	char names[DNS_MAX_QUESTIONS][DNS_NAME_MAX];
	u8 answers[DNS_MAX_QUESTIONS][4]; /* all zero: no answer */
	dns_lookup lookups[DNS_MAX_QUESTIONS];
	u8 reply[DNS_MAX_UDP];
	int reply_len;
} dns_slot;

struct dns_server
{
	dns_slot slots[DNS_SLOTS];
	retro_atomic_int_t local_ip; /* the adapter's address, as its 4 bytes */
#ifdef _WIN32
	bool wsa;
#endif
};

/* Lookup threads alive, in every server: freeing one waits for none to
 * be left. Static, so the last thread's notify never touches freed
 * memory. */
static retro_atomic_int_t s_live = RETRO_ATOMIC_INT_INITIALIZER(0);
static retro_eventcount_t s_live_ec;
static bool s_live_ec_ok;

dns_server* dns_server_new(void)
{
	dns_server* server = (dns_server*)calloc(1, sizeof(*server));
	static const u8 localhost[4] = {127, 0, 0, 1};
	int ip;
	int i;
	if (!server)
		return NULL;
	if (!s_live_ec_ok)
		s_live_ec_ok = retro_eventcount_init(&s_live_ec);
	for (i = 0; i < DNS_SLOTS; i++)
	{
		retro_atomic_store_release_int(&server->slots[i].state, SLOT_FREE);
		retro_atomic_store_release_int(&server->slots[i].pending, 0);
	}
	memcpy(&ip, localhost, 4);
	retro_atomic_store_release_int(&server->local_ip, ip);
#ifdef _WIN32
	{
		WSADATA wsa;
		const int err = WSAStartup(MAKEWORD(2, 2), &wsa);
		if (err != 0)
			log_cb(RETRO_LOG_ERROR, "DEV9: WSAStartup failed with error: %d\n", err);
		else
			server->wsa = true;
	}
#endif
	return server;
}

void dns_server_free(dns_server* server)
{
	if (!server)
		return;
	/* A lookup may still be blocked in the host resolver: wait for it to
	 * be done with the slots, parked, not polling. */
	while (retro_atomic_load_acquire_int(&s_live) != 0)
	{
		int key;
		if (!s_live_ec_ok)
			break;
		key = retro_eventcount_prepare_wait(&s_live_ec);
		if (retro_atomic_load_acquire_int(&s_live) == 0)
		{
			retro_eventcount_cancel_wait(&s_live_ec);
			break;
		}
		retro_eventcount_commit_wait(&s_live_ec, key);
	}
#ifdef _WIN32
	if (server->wsa)
		WSACleanup();
#endif
	free(server);
}

void dns_server_set_adapter(dns_server* server, const net_adapter* adapter)
{
	u8 addr[4] = {127, 0, 0, 1};
	int ip;
	if (!net_adapter_ip(adapter, addr))
		log_cb(RETRO_LOG_ERROR, "DEV9: Failed To Get Adapter IP\n");
	memcpy(&ip, addr, 4);
	retro_atomic_store_release_int(&server->local_ip, ip);
}

static void wr16(u8* p, unsigned v)
{
	p[0] = (u8)(v >> 8);
	p[1] = (u8)v;
}

/* The reply, once every lookup is in: the questions as asked, then an
 * A record for each answered one, its name a pointer to the question's. */
static void finish(dns_server* server, dns_slot* slot)
{
	static const u8 localhost[4] = {127, 0, 0, 1};
	const dns_query* q = &slot->query;
	u8* out = slot->reply;
	int pos = q->end;
	int answers = 0;
	int rcode = DNS_RCODE_NOERROR;
	int local = retro_atomic_load_acquire_int(&server->local_ip);
	int i;

	memcpy(out + DNS_HEADER_SIZE, slot->query_bytes + DNS_HEADER_SIZE, (size_t)(q->end - DNS_HEADER_SIZE));
	for (i = 0; i < q->qdcount; i++)
	{
		static const u8 none[4] = {0, 0, 0, 0};
		const u8* ip = slot->answers[i];
		if (!slot->asked[i])
			continue;
		if (memcmp(ip, none, 4) == 0)
		{
			rcode = DNS_RCODE_SERVFAIL;
			continue;
		}
		if (pos + 16 > DNS_MAX_UDP)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: Generated DNS response too large, dropping\n");
			slot->reply_len = 0;
			retro_atomic_store_release_int(&slot->state, SLOT_FREE);
			return;
		}
		/* TODO, might not be effective on pcap */
		if (memcmp(ip, localhost, 4) == 0)
			ip = (const u8*)&local;
		wr16(out + pos, 0xC000 | q->q[i].name);
		wr16(out + pos + 2, DNS_TYPE_A);
		wr16(out + pos + 4, DNS_CLASS_IN);
		wr16(out + pos + 6, DNS_TTL >> 16);
		wr16(out + pos + 8, DNS_TTL & 0xFFFF);
		wr16(out + pos + 10, 4);
		memcpy(out + pos + 12, ip, 4);
		pos += 16;
		answers++;
	}
	wr16(out, q->id);
	out[2] = DNS_F1_QR | DNS_F1_RD; /* a standard query's answer */
	out[3] = (u8)(DNS_F2_RA | rcode);
	wr16(out + 4, q->qdcount);
	wr16(out + 6, (unsigned)answers);
	wr16(out + 8, 0);
	wr16(out + 10, 0);
	slot->reply_len = pos;
	retro_atomic_store_release_int(&slot->state, SLOT_READY);
}

static bool resolve(const char* name, u8 ip[4])
{
	struct addrinfo hints;
	struct addrinfo* result = NULL;
	const struct addrinfo* r;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	err = getaddrinfo(name, NULL, &hints, &result);
	if (err != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS failed to find host %s (%d)\n", name, err);
		return false;
	}
	for (r = result; r; r = r->ai_next)
	{
		if (r->ai_family == AF_INET && r->ai_addr)
		{
			memcpy(ip, &((const struct sockaddr_in*)r->ai_addr)->sin_addr, 4);
			break;
		}
	}
	freeaddrinfo(result);
	if (!r)
		log_cb(RETRO_LOG_ERROR, "DEV9: Internal DNS found no IPv4 address for %s\n", name);
	return r != NULL;
}

/* One name's lookup; the last of a query's writes the reply. Nothing of
 * the slot is touched once pending is dropped, but by that last one. */
static void lookup(dns_lookup* l)
{
	dns_slot* slot = l->slot;
	if (!resolve(slot->names[l->index], slot->answers[l->index]))
		memset(slot->answers[l->index], 0, 4);
	if (retro_atomic_fetch_sub_int(&slot->pending, 1) == 1)
		finish(l->server, slot);
}

static void lookup_thread(void* arg)
{
	lookup((dns_lookup*)arg);
	retro_atomic_fetch_sub_int(&s_live, 1);
	if (s_live_ec_ok)
		retro_eventcount_notify(&s_live_ec);
}

bool dns_server_send(dns_server* server, const u8* payload, int len, u16 src_port)
{
	dns_slot* slot = NULL;
	dns_query* q;
	int asked = 0;
	int i;

	for (i = 0; i < DNS_SLOTS; i++)
	{
		if (retro_atomic_load_acquire_int(&server->slots[i].state) == SLOT_FREE)
		{
			slot = &server->slots[i];
			break;
		}
	}
	if (!slot)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: DNS: %d queries in flight, dropping one\n", DNS_SLOTS);
		return true;
	}

	q = &slot->query;
	if (len > DNS_MAX_UDP || !dns_parse_query(payload, len, q))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: DNS: Malformed query (%d bytes), dropping\n", len);
		return true;
	}
	if (DNS_F1_OPCODE(q->flags1) != DNS_OPCODE_QUERY || (q->flags1 & DNS_F1_QR) || q->qdcount == 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Unexpected DNS OPCode, Code: %u\n", (unsigned)DNS_F1_OPCODE(q->flags1));
		return true;
	}
	if (q->flags1 & DNS_F1_TC)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: Truncated DNS packet Not Supported\n");
		return true;
	}

	memcpy(slot->query_bytes, payload, (size_t)len);
	for (i = 0; i < q->qdcount; i++)
	{
		slot->asked[i] = q->q[i].type == DNS_TYPE_A && q->q[i].klass == DNS_CLASS_IN;
		memset(slot->answers[i], 0, 4);
		if (!slot->asked[i])
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: Unexpected question type of class, T: %d C: %d\n",
				q->q[i].type, q->q[i].klass);
			continue;
		}
		dns_read_name(slot->query_bytes, len, q->q[i].name, slot->names[i], DNS_NAME_MAX);
		asked++;
	}
	if (asked == 0)
		return true;

	slot->client_port = src_port;
	retro_atomic_store_release_int(&slot->pending, asked);
	retro_atomic_store_release_int(&slot->state, SLOT_LOOKING);
	for (i = 0; i < q->qdcount; i++)
	{
		dns_lookup* l = &slot->lookups[i];
		sthread_t* t;
		if (!slot->asked[i])
			continue;
		l->server = server;
		l->slot = slot;
		l->index = i;
		retro_atomic_fetch_add_int(&s_live, 1);
		t = sthread_create(lookup_thread, l);
		if (t)
			sthread_detach(t);
		else
			lookup_thread(l); /* no thread: here, so it still completes */
	}
	return true;
}

int dns_server_recv(dns_server* server, u8* out, int cap, u16* dst_port)
{
	int i;
	for (i = 0; i < DNS_SLOTS; i++)
	{
		dns_slot* slot = &server->slots[i];
		int len;
		if (retro_atomic_load_acquire_int(&slot->state) != SLOT_READY)
			continue;
		len = slot->reply_len;
		if (len > cap)
			len = 0;
		else
			memcpy(out, slot->reply, (size_t)len);
		*dst_port = slot->client_port;
		retro_atomic_store_release_int(&slot->state, SLOT_FREE);
		if (len)
			return len;
	}
	return 0;
}
