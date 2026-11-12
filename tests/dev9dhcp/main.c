/* The internal DHCP server, the real C unit (DHCP_Server.c): what a
 * guest's request can make it do, and the replies it writes.
 *
 * Every byte of a request is the guest's. The C++ server it replaces
 * read the 240-byte header from whatever it was given, followed option
 * lengths past the packet, took the first router of a router option
 * with none in it, and sized its reply from the request's maximum
 * message size option -- under 268 the reply's length went negative and
 * its padding memset ran off with it; at 65535 a 65507-byte reply was
 * written into a 2 KB frame. Here a short or cookieless request is
 * dropped, an option is read only as far as the packet goes, and the
 * reply is between 576 and one Ethernet frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libretro.h>

#include "Config.h"
#include "DEV9/InternalServers/DHCP_Server.h"

Pcsx2Config EmuConfig;
static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;

/* Unused here: no adapter is passed. */
int net_adapter_dns(const net_adapter* a, u8* out, int max) { (void)a; (void)out; (void)max; return 0; }
int net_adapter_gateways(const net_adapter* a, u8* out, int max) { (void)a; (void)out; (void)max; return 0; }
bool net_adapter_netmask(const net_adapter* a, u8 mask[4]) { (void)a; (void)mask; return false; }

static int failures;
static void check(int ok, const char* what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

static const u8 server_ip[4] = {192, 0, 2, 1};
static const u8 mac[6] = {0x00, 0x04, 0x1F, 0x82, 0x30, 0x31};
static dhcp_server server;
static u8 pkt[2048];
static u8 out[DHCP_REPLY_MAX];

/* A request's header, then its options from 240: returns where they
 * start. */
static int request(u32 xid)
{
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 1;
	pkt[1] = 1;
	pkt[2] = 6;
	pkt[4] = (u8)(xid >> 24);
	pkt[5] = (u8)(xid >> 16);
	pkt[6] = (u8)(xid >> 8);
	pkt[7] = (u8)xid;
	memcpy(pkt + 28, mac, 6);
	pkt[236] = 0x63;
	pkt[237] = 0x82;
	pkt[238] = 0x53;
	pkt[239] = 0x63;
	return DHCP_HEADER_SIZE;
}

static int opt(int pos, int code, int len, const u8* data)
{
	pkt[pos] = (u8)code;
	pkt[pos + 1] = (u8)len;
	memcpy(pkt + pos + 2, data, (size_t)len);
	return pos + 2 + len;
}

/* The reply's option code, its data and length, or NULL. */
static const u8* find(const u8* r, int len, int code, int* olen)
{
	int pos = DHCP_HEADER_SIZE;
	while (pos < len && r[pos] != 255)
	{
		if (r[pos] == 0)
		{
			pos++;
			continue;
		}
		if (r[pos] == code)
		{
			*olen = r[pos + 1];
			return r + pos + 2;
		}
		pos += 2 + r[pos + 1];
	}
	return NULL;
}

/* Whether the options run, whole, to an End inside the reply. */
static bool ends(const u8* r, int len)
{
	int pos = DHCP_HEADER_SIZE;
	while (pos < len)
	{
		if (r[pos] == 255)
			return true;
		if (r[pos] == 0)
			pos++;
		else if (pos + 2 > len)
			return false;
		else
			pos += 2 + r[pos + 1];
	}
	return false;
}

static void configure(void)
{
	static const u8 ip[4] = {192, 168, 1, 50}, mask[4] = {255, 255, 255, 0}, gw[4] = {192, 168, 1, 1};
	static const u8 dns1[4] = {9, 9, 9, 9};
	memset(&EmuConfig, 0, sizeof(EmuConfig));
	memcpy(EmuConfig.DEV9.PS2IP, ip, 4);
	memcpy(EmuConfig.DEV9.Mask, mask, 4);
	memcpy(EmuConfig.DEV9.Gateway, gw, 4);
	memcpy(EmuConfig.DEV9.DNS1, dns1, 4);
	EmuConfig.DEV9.ModeDNS1 = EMU_DNS_MANUAL;
	EmuConfig.DEV9.ModeDNS2 = EMU_DNS_INTERNAL;
	dhcp_server_init(&server, NULL, server_ip, NULL, NULL, NULL);
}

static void test_exchange(void)
{
	static const u8 wanted[] = {1, 3, 6, 15, 28, 50, 54, 53, 99};
	const u8* v;
	u8 m;
	int pos, len, olen;

	printf("a DISCOVER and a REQUEST\n");
	configure();
	pos = request(0xCAFEF00D);
	m = 1;
	pos = opt(pos, 53, 1, &m);
	pos = opt(pos, 55, sizeof(wanted), wanted);
	pkt[pos++] = 255;
	check(dhcp_server_send(&server, pkt, pos), "a DISCOVER is taken");
	len = dhcp_server_recv(&server, out, sizeof(out));
	check(len == 576 - 28, "an OFFER padded to 576 less the IP and UDP headers");
	check(out[0] == 2 && out[1] == 1 && out[2] == 6 && memcmp(out + 4, pkt + 4, 4) == 0 &&
			  out[16] == 192 && out[19] == 50 && memcmp(out + 20, server_ip, 4) == 0 &&
			  memcmp(out + 28, mac, 6) == 0 && out[236] == 0x63 && out[239] == 0x63,
		"its header: a reply, the xid, the address given, the server's, the MAC, the cookie");
	v = find(out, len, 53, &olen);
	check(v && olen == 1 && v[0] == 2, "it is an OFFER");
	v = find(out, len, 1, &olen);
	check(v && olen == 4 && v[0] == 255 && v[3] == 0, "the mask");
	v = find(out, len, 3, &olen);
	check(v && olen == 4 && v[3] == 1, "the gateway");
	v = find(out, len, 6, &olen);
	check(v && olen == 8 && v[0] == 9 && v[4] == 192 && v[7] == 1, "both DNS servers, the internal one second");
	v = find(out, len, 15, &olen);
	check(v && olen == 5 && memcmp(v, "PCSX2", 5) == 0, "the domain name");
	v = find(out, len, 28, &olen);
	check(v && olen == 4 && v[3] == 255, "the broadcast address");
	v = find(out, len, 51, &olen);
	check(v && olen == 4 && v[1] == 1 && v[2] == 0x51 && v[3] == 0x80, "the default lease of a day");
	v = find(out, len, 54, &olen);
	check(v && olen == 4 && memcmp(v, server_ip, 4) == 0 && ends(out, len), "the server, then End");
	check(dhcp_server_recv(&server, out, sizeof(out)) == 0, "one reply for one request");

	m = 3;
	pkt[DHCP_HEADER_SIZE + 2] = m;
	dhcp_server_send(&server, pkt, pos);
	len = dhcp_server_recv(&server, out, sizeof(out));
	v = find(out, len, 53, &olen);
	check(v && v[0] == 5, "a REQUEST is ACKed");

	m = 7;
	pkt[DHCP_HEADER_SIZE + 2] = m;
	dhcp_server_send(&server, pkt, pos);
	check(dhcp_server_recv(&server, out, sizeof(out)) == 0, "a RELEASE has no reply");
}

static void test_guest(void)
{
	u8 data[255];
	u8 m = 1;
	int pos, len, i;

	printf("what a guest can send\n");
	configure();
	pos = request(1);
	dhcp_server_send(&server, pkt, 239);
	pkt[239] = 0;
	dhcp_server_send(&server, pkt, 300);
	check(dhcp_server_recv(&server, out, sizeof(out)) == 0, "a short request and one without the cookie are dropped");

	/* A router option with no router in it, then one that runs past the
	 * end of the packet. */
	pos = request(2);
	pos = opt(pos, 53, 1, &m);
	pkt[pos++] = 3;
	pkt[pos++] = 0;
	pkt[pos++] = 55;
	pkt[pos++] = 200;
	pkt[pos++] = 1;
	dhcp_server_send(&server, pkt, pos);
	len = dhcp_server_recv(&server, out, sizeof(out));
	check(len == 548 && ends(out, len), "an empty router option and one past the end are read no further");

	/* The maximum message size: under 576 and past one frame. */
	pos = request(3);
	pos = opt(pos, 53, 1, &m);
	data[0] = 0;
	data[1] = 100;
	pos = opt(pos, 57, 2, data);
	pkt[pos++] = 255;
	dhcp_server_send(&server, pkt, pos);
	len = dhcp_server_recv(&server, out, sizeof(out));
	check(len == 548 && ends(out, len), "a maximum of 100 still gets 576");
	data[0] = 0xFF;
	data[1] = 0xFF;
	opt(pos - 5, 57, 2, data);
	dhcp_server_send(&server, pkt, pos);
	len = dhcp_server_recv(&server, out, sizeof(out));
	check(len == DHCP_REPLY_MAX && ends(out, len), "a maximum of 65535 gets one frame");

	/* More wanted than the reply holds: as many as fit, then End. */
	pos = request(4);
	pos = opt(pos, 53, 1, &m);
	memset(data, 15, sizeof(data));
	pos = opt(pos, 55, 255, data);
	pkt[pos++] = 255;
	dhcp_server_send(&server, pkt, pos);
	len = dhcp_server_recv(&server, out, sizeof(out));
	check(len == 548 && ends(out, len), "a request that overfills the reply is cut at an End");

	/* Five requests before any reply is read: four wait. */
	for (i = 0; i < 5; i++)
		dhcp_server_send(&server, pkt, pos);
	for (i = 0; dhcp_server_recv(&server, out, sizeof(out)) > 0; i++)
		;
	check(i == DHCP_REPLIES, "the queue holds four replies and drops past them");

	/* Every request length from nothing to a frame, of garbage after a
	 * valid header: under ASan, any read past the end fails here. */
	for (len = DHCP_HEADER_SIZE; len < 1500; len += 7)
	{
		request((u32)len);
		for (i = DHCP_HEADER_SIZE; i < len; i++)
			pkt[i] = (u8)(i * 37 + len);
		{
			u8* heap = (u8*)malloc((size_t)len); /* exactly len: a read past it is caught */
			memcpy(heap, pkt, (size_t)len);
			dhcp_server_send(&server, heap, len);
			free(heap);
		}
		while (dhcp_server_recv(&server, out, sizeof(out)) > 0)
			;
	}
	check(1, "garbage options of every length");
}

int main(void)
{
	test_exchange();
	test_guest();
	if (failures)
	{
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
