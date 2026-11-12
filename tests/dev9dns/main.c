/* The internal DNS server and what it reads, as the real C units:
 * DNS_Packet.c, DNS_Server.c, AdapterUtils.c.
 *
 * A query comes from the PS2, so every byte of it is the guest's. The
 * C++ reader it replaces followed lengths and compression pointers with
 * no bound: a pointer to itself recursed until the stack ran out, a
 * count past the packet read past it, a label past the end read past
 * it. Here each is malformed and dropped. A query answered from the
 * host's resolver -- numeric names, so no network is needed -- comes
 * back as a reply with the questions as asked and an A record each, and
 * the same name asked twice is two independent lookups (it shared one
 * map entry, written by both threads).
 *
 * The adapter half: an interface with no address (a tunnel) was
 * dereferenced; resolv.conf's first line was never read; a nameserver
 * line without an address read past its fields; an interface whose name
 * began with the one asked for was taken for it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libretro.h>
#include <retro_timers.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <unistd.h>
#endif

#include "DEV9/AdapterUtils.h"
#include "DEV9/InternalServers/DNS_Server.h"
#include "DEV9/PacketReader/IP/UDP/DNS/DNS_Packet.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

static int failures;
static void check(int ok, const char* what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

static int put_name(u8* p, const char* dotted)
{
	int n = 0;
	while (*dotted)
	{
		const char* dot = strchr(dotted, '.');
		const int len = dot ? (int)(dot - dotted) : (int)strlen(dotted);
		p[n++] = (u8)len;
		memcpy(p + n, dotted, (size_t)len);
		n += len;
		dotted += len;
		if (*dotted == '.')
			dotted++;
	}
	p[n++] = 0;
	return n;
}

/* A standard query with RD set for each name, type A, class IN. */
static int make_query(u8* p, u16 id, const char* const* names, int count)
{
	int n = DNS_HEADER_SIZE, i;
	memset(p, 0, DNS_HEADER_SIZE);
	p[0] = (u8)(id >> 8);
	p[1] = (u8)id;
	p[2] = DNS_F1_RD;
	p[5] = (u8)count;
	for (i = 0; i < count; i++)
	{
		n += put_name(p + n, names[i]);
		p[n++] = 0;
		p[n++] = DNS_TYPE_A;
		p[n++] = 0;
		p[n++] = DNS_CLASS_IN;
	}
	return n;
}

static void test_reader(void)
{
	static u8 pkt[600];
	char name[DNS_NAME_MAX];
	dns_query q;
	const char* names[2];
	int n, next;

	printf("the reader\n");
	names[0] = "www.example.com";
	n = make_query(pkt, 0x1234, names, 1);
	check(dns_parse_query(pkt, n, &q) && q.qdcount == 1 && q.end == n && q.q[0].name == 12 &&
			  q.q[0].type == DNS_TYPE_A && q.q[0].klass == DNS_CLASS_IN && q.id == 0x1234,
		"a query's header and question");
	next = dns_read_name(pkt, n, 12, name, sizeof(name));
	check(next == n - 4 && strcmp(name, "www.example.com") == 0, "its name");

	/* A name that is a pointer to itself. */
	memset(pkt, 0, sizeof(pkt));
	pkt[5] = 1;
	pkt[12] = 0xC0;
	pkt[13] = 12;
	pkt[14] = 0;
	pkt[15] = 1;
	pkt[16] = 0;
	pkt[17] = 1;
	check(!dns_parse_query(pkt, 18, &q), "a pointer to itself is malformed");

	/* Two pointers to each other, the second reached by a label. */
	pkt[12] = 1;
	pkt[13] = 'a';
	pkt[14] = 0xC0;
	pkt[15] = 12;
	check(dns_read_name(pkt, 18, 12, name, sizeof(name)) < 0, "a loop through a label is malformed");

	/* A pointer forward, past the end, and to the reserved types. */
	pkt[12] = 0xC0;
	pkt[13] = 16;
	check(dns_read_name(pkt, 18, 12, name, sizeof(name)) < 0, "a pointer forward is malformed");
	pkt[13] = 0xFF;
	check(dns_read_name(pkt, 14, 12, name, sizeof(name)) < 0, "a pointer past the end is malformed");
	pkt[12] = 0x40;
	check(dns_read_name(pkt, 18, 12, name, sizeof(name)) < 0, "a reserved label type is malformed");

	/* A label longer than what is left, and a name that does not end. */
	pkt[12] = 60;
	check(dns_read_name(pkt, 18, 12, name, sizeof(name)) < 0, "a label past the end is malformed");
	memset(pkt + 12, 1, 6);
	check(dns_read_name(pkt, 18, 12, name, sizeof(name)) < 0, "a name past the end is malformed");

	/* More questions than the packet holds, and than the server takes. */
	n = make_query(pkt, 1, names, 1);
	pkt[5] = 2;
	check(!dns_parse_query(pkt, n, &q), "a count past the packet is malformed");
	pkt[4] = 0xFF;
	pkt[5] = 0xFF;
	check(!dns_parse_query(pkt, n, &q), "65535 questions are refused");
	check(!dns_parse_query(pkt, 11, &q), "a short header is malformed");

	/* A name longer than a name may be: 5 labels of 63. */
	{
		int i, p = 12;
		memset(pkt, 0, sizeof(pkt));
		pkt[5] = 1;
		for (i = 0; i < 5; i++)
		{
			pkt[p++] = 63;
			memset(pkt + p, 'x', 63);
			p += 63;
		}
		pkt[p++] = 0;
		p += 4;
		check(!dns_parse_query(pkt, p, &q), "a name past 255 bytes is malformed");
	}

	/* A compressed name in the second question reads as the first's. */
	names[0] = "a.example";
	n = make_query(pkt, 7, names, 1);
	pkt[5] = 2;
	pkt[n++] = 1;
	pkt[n++] = 'b';
	pkt[n++] = 0xC0;
	pkt[n++] = 14; /* "example" in the first */
	pkt[n++] = 0;
	pkt[n++] = 1;
	pkt[n++] = 0;
	pkt[n++] = 1;
	check(dns_parse_query(pkt, n, &q) && q.qdcount == 2 &&
			  dns_read_name(pkt, n, q.q[1].name, name, sizeof(name)) > 0 && strcmp(name, "b.example") == 0,
		"a pointer back reads the earlier name");
	/* The logger walks it, and garbage, without reading past either. */
	dns_log(pkt, n);
	dns_log(pkt, 13);
	pkt[7] = 0xFF;
	dns_log(pkt, n);
}

#ifdef _WIN32
typedef struct fake_adapter
{
	IP_ADAPTER_ADDRESSES a;
	IP_ADAPTER_UNICAST_ADDRESS u;
	struct sockaddr_in sin;
} fake_adapter;

static const net_adapter* make_adapter(fake_adapter* f, const u8 ip[4])
{
	memset(f, 0, sizeof(*f));
	f->sin.sin_family = AF_INET;
	memcpy(&f->sin.sin_addr, ip, 4);
	f->u.Address.lpSockaddr = (struct sockaddr*)&f->sin;
	f->u.Address.iSockaddrLength = sizeof(f->sin);
	f->a.FirstUnicastAddress = &f->u;
	return &f->a;
}
#else
typedef struct fake_adapter
{
	struct ifaddrs a;
	struct sockaddr_in sin;
} fake_adapter;

static const net_adapter* make_adapter(fake_adapter* f, const u8 ip[4])
{
	memset(f, 0, sizeof(*f));
	f->sin.sin_family = AF_INET;
	memcpy(&f->sin.sin_addr, ip, 4);
	f->a.ifa_name = (char*)"eth0";
	f->a.ifa_addr = (struct sockaddr*)&f->sin;
	return &f->a;
}
#endif

static int wait_reply(dns_server* s, u8* out, u16* port)
{
	int i;
	for (i = 0; i < 5000; i++)
	{
		const int n = dns_server_recv(s, out, DNS_MAX_UDP, port);
		if (n)
			return n;
		retro_sleep(1);
	}
	return 0;
}

static void test_server(void)
{
	static const u8 host_ip[4] = {10, 1, 2, 3};
	static u8 pkt[DNS_MAX_UDP];
	static u8 reply[DNS_MAX_UDP];
	fake_adapter fake;
	dns_server* s = dns_server_new();
	const char* names[3];
	u16 port = 0;
	int n, len, round;

	printf("the server\n");
	if (!s)
	{
		check(0, "dns_server_new");
		return;
	}
	dns_server_set_adapter(s, make_adapter(&fake, host_ip));

	names[0] = "192.0.2.77";
	names[1] = "127.0.0.1";
	names[2] = "192.0.2.77";
	n = make_query(pkt, 0xBEEF, names, 3);
	check(dns_server_send(s, pkt, n, 4321), "a query is taken");
	len = wait_reply(s, reply, &port);
	check(len == n + 3 * 16 && port == 4321, "one reply, to the port it came from, three answers long");
	if (len == n + 3 * 16)
	{
		static const u8 a0[16] = {0xC0, 12, 0, 1, 0, 1, 0, 0, 0x2A, 0x30, 0, 4, 192, 0, 2, 77};
		u8 a1[16];
		check(reply[0] == 0xBE && reply[1] == 0xEF && reply[2] == (DNS_F1_QR | DNS_F1_RD) &&
				  reply[3] == DNS_F2_RA && reply[5] == 3 && reply[7] == 3 && reply[9] == 0 && reply[11] == 0,
			"its header: an answer, no error, three of each");
		check(memcmp(reply + 12, pkt + 12, (size_t)(n - 12)) == 0, "the questions as asked");
		check(memcmp(reply + n, a0, 16) == 0, "an A record pointing at its question");
		memcpy(a1, a0, 16);
		a1[1] = (u8)(12 + 16); /* "192.0.2.77" is 12 bytes on the wire, then 4 */
		memcpy(a1 + 12, host_ip, 4);
		check(memcmp(reply + n + 16, a1, 16) == 0, "127.0.0.1 answered as the adapter's address");
		check(reply[n + 32 + 1] == 12 + 16 + 15 && memcmp(reply + n + 32 + 12, a0 + 12, 4) == 0,
			"the same name twice, answered twice");
	}

	printf("the server, against what a guest can send\n");
	/* Each malformed: nothing taken, no reply. */
	pkt[12] = 0xC0;
	pkt[13] = 12;
	dns_server_send(s, pkt, n, 1);
	memset(pkt, 0, sizeof(pkt));
	pkt[4] = 0xFF;
	pkt[5] = 0xFF;
	dns_server_send(s, pkt, DNS_MAX_UDP, 1);
	dns_server_send(s, pkt, 5, 1);
	names[0] = "192.0.2.1";
	n = make_query(pkt, 1, names, 1);
	pkt[2] |= DNS_F1_QR;
	dns_server_send(s, pkt, n, 1);
	retro_sleep(20);
	check(dns_server_recv(s, reply, DNS_MAX_UDP, &port) == 0, "malformed queries and answers are dropped");

	/* Many rounds of three lookups each, the slots reused: under TSan
	 * this is where two threads writing one entry was seen. */
	for (round = 0; round < 40; round++)
	{
		names[0] = names[1] = names[2] = "192.0.2.9";
		n = make_query(pkt, (u16)round, names, 3);
		dns_server_send(s, pkt, n, (u16)(1000 + round));
		if (!wait_reply(s, reply, &port) || port != 1000 + round || reply[7] != 3)
			break;
	}
	check(round == 40, "forty queries of the same name three times, each answered whole");

	/* Freed with lookups in flight: it waits for them. */
	names[0] = "192.0.2.10";
	n = make_query(pkt, 2, names, 1);
	dns_server_send(s, pkt, n, 9);
	dns_server_free(s);
	check(1, "freed with a lookup in flight");
}

#ifndef _WIN32
static void test_adapters(void)
{
	static const u8 ip[4] = {10, 0, 0, 5};
	struct ifaddrs tun, eth, eth01;
	struct sockaddr_in sin, sin01;
	const net_adapter* a;
	u8 out[4 * 4];
	char path[64];
	FILE* f;

	printf("the adapters\n");
	memset(&tun, 0, sizeof(tun));
	memset(&eth, 0, sizeof(eth));
	memset(&eth01, 0, sizeof(eth01));
	memset(&sin, 0, sizeof(sin));
	memset(&sin01, 0, sizeof(sin01));
	sin.sin_family = AF_INET;
	memcpy(&sin.sin_addr, ip, 4);
	sin01 = sin;
	tun.ifa_name = (char*)"tun0";
	tun.ifa_addr = NULL;
	tun.ifa_next = &eth01;
	eth01.ifa_name = (char*)"eth01";
	eth01.ifa_addr = (struct sockaddr*)&sin01;
	eth01.ifa_next = &eth;
	eth.ifa_name = (char*)"eth0";
	eth.ifa_addr = (struct sockaddr*)&sin;
	a = net_adapter_find(&tun, "eth0");
	check(a == &eth, "an interface with no address is passed over, not read");
	check(net_adapter_find(&tun, "tun0") == NULL, "and is not an IPv4 adapter");
	check(net_adapter_ip(a, out) && memcmp(out, ip, 4) == 0, "the adapter's address");

	check(net_resolv_line("nameserver 9.9.9.9\n", out) && out[0] == 9 && out[3] == 9, "a nameserver line");
	check(net_resolv_line("nameserver\t1.1.1.1 # cloud\n", out) && out[0] == 1, "tab-separated, with a comment");
	check(!net_resolv_line("nameserver\n", out), "a nameserver line without an address");
	check(!net_resolv_line("nameserver ::1\n", out), "an IPv6 nameserver");
	check(!net_resolv_line("# nameserver 8.8.8.8\n", out), "a comment");
	check(!net_resolv_line("search lan\n", out), "another option");

	/* The kernel's table: Iface Destination Gateway Flags ... */
	check(net_route_line("eth0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\n", "eth0", out) &&
			  out[0] == 0xC0 && out[1] == 0xA8 && out[2] == 1 && out[3] == 1,
		"a default route's gateway, as the address's bytes");
	check(!net_route_line("eth01\t00000000\t0101A8C0\t0003\n", "eth0", out), "another interface whose name begins the same");
	check(!net_route_line("eth0\t0000A8C0\t00000000\t0001\n", "eth0", out), "a device route without a gateway");
	check(!net_route_line("eth0\t00000000\tzz\t0003\n", "eth0", out), "a gateway that is not hex");
	check(!net_route_line("eth0\n", "eth0", out), "a line cut short");

	sprintf(path, "dev9dns_resolv_%d.conf", (int)getpid());
	f = fopen(path, "w");
	if (f)
	{
		int n;
		fputs("nameserver 8.8.8.8\n", f);
		fputs("# a comment\n", f);
		fputs("nameserver\n", f);
		fputs("nameserver 8.8.4.4\n", f);
		fclose(f);
		n = net_resolv_file(path, out, 4);
		check(n == 2 && out[0] == 8 && out[3] == 8 && out[4] == 8 && out[7] == 4,
			"resolv.conf from its first line, the bare entry skipped");
		remove(path);
	}
	else
		check(0, "write a resolv.conf");
}
#endif

int main(void)
{
	test_reader();
	test_server();
#ifndef _WIN32
	test_adapters();
#endif
	if (failures)
	{
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
