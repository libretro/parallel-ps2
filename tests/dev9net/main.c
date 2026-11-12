/* pcsx2/DEV9's network stack as the real C units: the frame reader
 * (netpkt.c) against what a guest can send, and the sockets adapter
 * (sockets.c, its sessions, net.c's internal servers) driven the way the
 * network thread drives it, against what the guest and the host can do.
 *
 * Each case is one the C++ stack it replaces failed:
 *  - an IP or TCP option of length 0 or 1: its reader looped forever, or
 *    resized a vector to -2 and aborted;
 *  - an ICMP port unreachable quoting no IPv4 header: it scanned past the
 *    packet for one;
 *  - a host datagram longer than a frame: it was written into the 2 KB
 *    packet whole;
 *  - a SYN with a maximum segment size under 12 and timestamps: the size
 *    underflowed and a host's whole receive buffer went into one frame;
 *  - a host that stops reading: send returned -1, the loop took it for
 *    bytes sent, counted backwards through the guest's segment and never
 *    left.
 * Packets are in exact-size heap buffers and frames in exact-size
 * NetPackets, so under ASan a byte past either fails here.
 *
 * The adapter half is skipped, not failed, where the host has no IPv4
 * adapter to use. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libretro.h>
#include <retro_timers.h>
#include <features/features_cpu.h>
#include <rthreads/rthreads.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "Config.h"
#include "DEV9/DEV9.h"
#include "DEV9/net.h"
#include "DEV9/PacketReader/netpkt.h"
#include "DEV9/Sessions/Session.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
retro_log_printf_t log_cb = quiet;
Pcsx2Config EmuConfig;
struct EmuFoldersData EmuFolders;
dev9Struct dev9;
static u16 s_eeprom[32];
/* The SMAP's side of net.c, not used here. */
bool rx_process(NetPacket* pk) { (void)pk; return true; }
bool rx_fifo_can_rx(void) { return false; }

#ifdef _WIN32
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

static int failures;
static void check(int ok, const char* what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

/* A copy of p in a heap block exactly len long. */
static u8* exact(const u8* p, int len)
{
	u8* b = (u8*)malloc((size_t)(len ? len : 1));
	memcpy(b, p, (size_t)len);
	return b;
}

static void test_reader(void)
{
	u8 p[128];
	u8* b;
	net_ip ip;
	net_tcp tcp;
	net_udp udp;
	net_icmp icmp;
	net_arp arp;
	int len;

	printf("the frame reader\n");
	/* IPv4 with options: a zero-length one, then one past the header. */
	memset(p, 0, sizeof(p));
	p[0] = 0x46; /* 24-byte header */
	p[3] = 24 + 8;
	p[9] = IP_PROTO_UDP;
	p[20] = 7;
	p[21] = 0;
	b = exact(p, 32);
	check(net_ip_parse(b, 32, &ip) && ip.hlen == 24 && ip.len == 8, "an IP option of length 0 is skipped with the header");
	free(b);
	p[0] = 0x4F; /* 60-byte header, 32 bytes there */
	b = exact(p, 32);
	check(!net_ip_parse(b, 32, &ip), "a header longer than the packet");
	free(b);
	p[0] = 0x44;
	b = exact(p, 32);
	check(!net_ip_parse(b, 32, &ip), "a header shorter than 20");
	free(b);
	p[0] = 0x45;
	p[3] = 10;
	b = exact(p, 32);
	check(!net_ip_parse(b, 32, &ip), "a total length shorter than the header");
	free(b);
	p[3] = 200;
	b = exact(p, 32);
	check(net_ip_parse(b, 32, &ip) && ip.len == 12, "a total length past the packet is the packet's");
	free(b);

	/* TCP options: length 0, length 1, past the header, and good ones. */
	memset(p, 0, sizeof(p));
	p[12] = 8 << 4; /* 32-byte header */
	p[20] = 2;
	p[21] = 0;
	b = exact(p, 32);
	check(net_tcp_parse(b, 32, &tcp) && tcp.mss == -1, "a TCP option of length 0 ends them");
	free(b);
	p[21] = 1;
	b = exact(p, 32);
	check(net_tcp_parse(b, 32, &tcp) && tcp.mss == -1, "a TCP option of length 1 ends them");
	free(b);
	p[20] = 8;
	p[21] = 40;
	b = exact(p, 32);
	check(net_tcp_parse(b, 32, &tcp) && !tcp.has_ts, "a TCP option past the header ends them");
	free(b);
	p[20] = 2;
	p[21] = 4;
	p[22] = 0x05;
	p[23] = 0xB4;
	p[24] = 1;
	p[25] = 3;
	p[26] = 3;
	p[27] = 7;
	p[28] = 0;
	b = exact(p, 32);
	check(net_tcp_parse(b, 32, &tcp) && tcp.mss == 1460 && tcp.wscale == 7, "MSS and window scale");
	free(b);
	p[12] = 15 << 4;
	b = exact(p, 32);
	check(!net_tcp_parse(b, 32, &tcp), "a TCP header longer than the segment");
	free(b);

	memset(p, 0, sizeof(p));
	p[5] = 4;
	b = exact(p, 8);
	check(!net_udp_parse(b, 8, &udp), "a UDP length shorter than its header");
	free(b);
	b = exact(p, 7);
	check(!net_udp_parse(b, 7, &udp) && !net_icmp_parse(b, 7, &icmp), "a UDP or ICMP header cut short");
	free(b);
	len = ARP_IPV4 - 1;
	p[1] = 1;
	p[2] = 8;
	p[4] = 6;
	p[5] = 4;
	b = exact(p, len);
	check(!net_arp_parse(b, len, &arp), "an ARP packet cut short");
	free(b);
	p[4] = 200;
	b = exact(p, ARP_IPV4);
	check(!net_arp_parse(b, ARP_IPV4, &arp), "an ARP packet with other address lengths");
	free(b);
}

/* ---- The adapter ------------------------------------------------------ */

static const u8 ps2_ip[4] = {192, 0, 2, 100};
static u8 host_ip[4];
static netif* nif;
static NetPacket* pkt; /* exactly one NetPacket, on the heap */

static int host_address(u8 out[4])
{
	void* list;
	const net_adapter* a = net_adapter_get_auto(&list);
	const int ok = a && net_adapter_ip(a, out);
	net_adapter_free(list);
	return ok;
}

/* A frame from the PS2 to dst, with an IP payload of len bytes already
 * at NET_IP_DATA. */
static void put_ip(u8 proto, const u8 dst[4], int len)
{
	u8* f = (u8*)pkt->buffer;
	net_put_ip(f, ps2_ip, dst, proto, 64, 0, len);
	net_put_eth(f, net_internal_mac, nif->ps2_mac, ETHERTYPE_IPV4);
	pkt->size = NET_IP_DATA + len;
}

static void send_udp(const u8 dst[4], u16 sport, u16 dport, const void* data, int len)
{
	memcpy(pkt->buffer + NET_UDP_DATA, data, (size_t)len);
	net_put_udp((u8*)pkt->buffer, ps2_ip, dst, sport, dport, len);
	put_ip(IP_PROTO_UDP, dst, UDP_HEADER + len);
	nif->ops->send(nif, pkt);
}

static void send_tcp(const u8 dst[4], u16 dport, u8 flags, u32 seq, u32 ack, const u8* opt, int optlen,
	const void* data, int len)
{
	u8* f = (u8*)pkt->buffer;
	if (optlen)
		memcpy(f + NET_TCP_DATA(0), opt, (size_t)optlen);
	if (len)
		memcpy(f + NET_TCP_DATA(optlen), data, (size_t)len);
	net_put_tcp(f, ps2_ip, dst, 40100, dport, seq, ack, flags, 65535, optlen, len);
	put_ip(IP_PROTO_TCP, dst, TCP_HEADER + optlen + len);
	nif->ops->send(nif, pkt);
}

/* The next frame for the PS2 within ms, parsed to its IP: whether one
 * came. */
static bool next_ip(net_ip* ip, int ms)
{
	const retro_time_t until = cpu_features_get_time_usec() + (retro_time_t)ms * 1000;
	net_eth eth;
	do
	{
		if (nif->ops->recv(nif, pkt) || netif_internal_recv(nif, pkt))
		{
			if (pkt->size > ETH_FRAME_MAX)
			{
				check(0, "a frame for the PS2 is no longer than the Ethernet maximum");
				return false;
			}
			if (net_eth_parse((const u8*)pkt->buffer, pkt->size, &eth) && eth.type == ETHERTYPE_IPV4 &&
				net_ip_parse(eth.data, eth.len, ip))
				return true;
			continue;
		}
		retro_sleep(1);
	} while (cpu_features_get_time_usec() < until);
	return false;
}

static sock_t udp_server(u16* port)
{
	struct sockaddr_in a;
#ifdef _WIN32
	int alen = sizeof(a);
#else
	socklen_t alen = sizeof(a);
#endif
	sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	if (s == SOCK_BAD || bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || getsockname(s, (struct sockaddr*)&a, &alen) != 0)
		return SOCK_BAD;
	*port = ntohs(a.sin_port);
	return s;
}

static sock_t tcp_server(u16* port)
{
	struct sockaddr_in a;
#ifdef _WIN32
	int alen = sizeof(a);
#else
	socklen_t alen = sizeof(a);
#endif
	sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	if (s == SOCK_BAD || bind(s, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(s, 4) != 0 ||
		getsockname(s, (struct sockaddr*)&a, &alen) != 0)
		return SOCK_BAD;
	*port = ntohs(a.sin_port);
	return s;
}

/* A host datagram longer than a frame, then one that fits. */
static void test_big_datagram(void)
{
	static u8 big[4000];
	static const char small[] = "after the big one";
	struct sockaddr_in from;
#ifdef _WIN32
	int flen = sizeof(from);
#else
	socklen_t flen = sizeof(from);
#endif
	char buf[64];
	u16 port;
	sock_t srv = udp_server(&port);
	net_ip ip;
	net_udp udp;
	bool got_small = false;
	int n, i;

	if (srv == SOCK_BAD)
	{
		check(0, "a UDP server");
		return;
	}
	net_sock_nonblocking(srv);
	send_udp(host_ip, 40200, port, "hello", 5);
	for (i = 0; i < 500; i++)
	{
		n = (int)recvfrom(srv, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
		if (n > 0)
			break;
		nif->ops->recv(nif, pkt);
		retro_sleep(1);
	}
	memset(big, 0x5A, sizeof(big));
	sendto(srv, (const char*)big, sizeof(big), 0, (struct sockaddr*)&from, flen);
	sendto(srv, small, sizeof(small), 0, (struct sockaddr*)&from, flen);
	while (next_ip(&ip, 2000))
		if (ip.proto == IP_PROTO_UDP && net_udp_parse(ip.data, ip.len, &udp) && udp.len == (int)sizeof(small) &&
			memcmp(udp.data, small, sizeof(small)) == 0)
		{
			got_small = true;
			break;
		}
	check(got_small, "a 4000-byte datagram is dropped, the next one arrives whole");
	sock_close(srv);
}

/* A SYN asking for a 1-byte segment, with timestamps: the host's data
 * arrives in segments that fit, however small. */
static void test_tiny_mss(void)
{
	static u8 data[3000];
	static const u8 opt[] = {2, 4, 0, 1, 1, 1, 8, 10, 0, 0, 0, 1, 0, 0, 0, 0};
	u16 port;
	sock_t srv = tcp_server(&port), c = SOCK_BAD;
	net_ip ip;
	net_tcp tcp;
	u32 srv_seq = 0, got = 0;
	int i, biggest = 0;

	if (srv == SOCK_BAD)
	{
		check(0, "a TCP server");
		return;
	}
	net_sock_nonblocking(srv);
	send_tcp(host_ip, port, TCP_SYN, 5000, 0, opt, sizeof(opt), NULL, 0);
	while (next_ip(&ip, 2000))
		if (ip.proto == IP_PROTO_TCP && net_tcp_parse(ip.data, ip.len, &tcp) && (tcp.flags & TCP_SYN))
		{
			srv_seq = tcp.seq + 1;
			break;
		}
	check(srv_seq != 0, "the SYN is answered");
	send_tcp(host_ip, port, TCP_ACK, 5001, srv_seq, NULL, 0, NULL, 0);
	for (i = 0; i < 1000 && c == SOCK_BAD; i++)
	{
		c = accept(srv, NULL, NULL);
		retro_sleep(1);
	}
	if (c == SOCK_BAD)
	{
		check(0, "the host accepts the connection");
		sock_close(srv);
		return;
	}
	for (i = 0; i < (int)sizeof(data); i++)
		data[i] = (u8)i;
	send(c, (const char*)data, sizeof(data), 0);
	while (got < sizeof(data) && next_ip(&ip, 2000))
	{
		if (ip.proto != IP_PROTO_TCP || !net_tcp_parse(ip.data, ip.len, &tcp) || tcp.len == 0)
			continue;
		if (tcp.len > biggest)
			biggest = tcp.len;
		if (tcp.seq == srv_seq + got && memcmp(tcp.data, data + got, (size_t)tcp.len) == 0)
			got += (u32)tcp.len;
		/* ACK it, so the next is read */
		send_tcp(host_ip, port, TCP_ACK, 5001, srv_seq + got, NULL, 0, NULL, 0);
	}
	check(got == sizeof(data), "3000 bytes arrive in order");
	check(biggest > 0 && biggest <= 64 - 12, "in segments no larger than the smallest MSS taken, less timestamps");
	sock_close(c);
	sock_close(srv);
}

/* A host that never reads: the PS2 sends until the host's buffers are
 * full, and every segment is answered, at once, with what was taken. */
static void test_backpressure(void)
{
	static u8 seg[1400];
	u16 port;
	sock_t srv = tcp_server(&port), c = SOCK_BAD;
	net_ip ip;
	net_tcp tcp;
	u32 srv_seq = 0, seq = 9001, acked = 9001;
	int i, stalled = 0, answered = 0;
	const retro_time_t start = cpu_features_get_time_usec();

	if (srv == SOCK_BAD)
	{
		check(0, "a TCP server");
		return;
	}
	net_sock_nonblocking(srv);
	send_tcp(host_ip, port, TCP_SYN, 9000, 0, NULL, 0, NULL, 0);
	while (next_ip(&ip, 2000))
		if (ip.proto == IP_PROTO_TCP && net_tcp_parse(ip.data, ip.len, &tcp) && (tcp.flags & TCP_SYN))
		{
			srv_seq = tcp.seq + 1;
			break;
		}
	send_tcp(host_ip, port, TCP_ACK, 9001, srv_seq, NULL, 0, NULL, 0);
	for (i = 0; i < 1000 && c == SOCK_BAD; i++)
	{
		c = accept(srv, NULL, NULL);
		retro_sleep(1);
	}
	memset(seg, 0x33, sizeof(seg));
	/* Up to 64 MB, never read: far past any socket buffer. */
	for (i = 0; i < 48000 && stalled < 20; i++)
	{
		send_tcp(host_ip, port, TCP_ACK | TCP_PSH, seq, srv_seq, NULL, 0, seg, sizeof(seg));
		if (!next_ip(&ip, 1000) || !net_tcp_parse(ip.data, ip.len, &tcp))
			break;
		answered++;
		acked = tcp.ack;
		if (acked == seq)
			stalled++; /* nothing taken: send it again */
		else
			stalled = 0;
		seq = acked;
	}
	check(answered == i, "every segment is answered");
	check(stalled >= 20, "once the host's buffers fill, the PS2 is told nothing more was taken");
	check(cpu_features_get_time_usec() - start < 60 * 1000000, "and nothing waits for the host to read");
	sock_close(c);
	sock_close(srv);
}

/* An ICMP port unreachable from the PS2 quoting no IPv4 header at all,
 * then quoting a short one. */
static void test_icmp_unreachable(void)
{
	static const u8 rest[4] = {0, 0, 0, 0};
	u8* f = (u8*)pkt->buffer;
	int len;

	memset(f + NET_ICMP_DATA, 0x11, 64); /* no byte 0x4X anywhere */
	net_put_icmp(f, 3, 3, rest, 64);
	put_ip(IP_PROTO_ICMP, host_ip, ICMP_HEADER + 64);
	/* The NetPacket's buffer ends past the frame: the old scan went on. */
	len = pkt->size;
	{
		NetPacket* tight = (NetPacket*)malloc(sizeof(NetPacket));
		memcpy(tight, pkt, sizeof(NetPacket));
		memset(tight->buffer + len, 0x11, sizeof(tight->buffer) - (size_t)len);
		nif->ops->send(nif, tight);
		free(tight);
	}
	f[NET_ICMP_DATA] = 0x45;
	net_put_icmp(f, 3, 3, rest, 1);
	put_ip(IP_PROTO_ICMP, host_ip, ICMP_HEADER + 1);
	nif->ops->send(nif, pkt);
	check(1, "an ICMP port unreachable quoting nothing, and quoting one byte");
}

/* IP and TCP options of length 0 from the PS2. */
static void test_zero_options(void)
{
	static const u8 opt[] = {7, 0, 0, 0};
	u8* f = (u8*)pkt->buffer;
	u8* h = f + ETH_HEADER;
	u16 port;
	sock_t srv = tcp_server(&port);

	/* A TCP SYN with an option of length 0 */
	send_tcp(host_ip, port, TCP_SYN, 1, 0, opt, sizeof(opt), NULL, 0);
	/* A UDP datagram whose IP header has an option of length 0 */
	net_put_ip(f, ps2_ip, host_ip, IP_PROTO_UDP, 64, 0, UDP_HEADER + 4);
	h[0] = 0x46;
	net_wr16(h + 2, 24 + UDP_HEADER + 4);
	memset(h + 20, 0, 4);
	h[20] = 7;
	net_wr16(h + 10, 0);
	net_wr16(h + 10, net_fold(net_sum(h, 24, 0)));
	memset(h + 24, 0, UDP_HEADER + 4);
	net_wr16(h + 24, 40300);
	net_wr16(h + 26, port);
	net_wr16(h + 28, UDP_HEADER + 4);
	pkt->size = ETH_HEADER + 24 + UDP_HEADER + 4;
	nif->ops->send(nif, pkt);
	check(1, "IP and TCP options of length 0");
	if (srv != SOCK_BAD)
		sock_close(srv);
}

static void test_adapter(void)
{
	printf("the sockets adapter\n");
	if (!host_address(host_ip))
	{
		printf("  skip: no IPv4 host adapter\n");
		return;
	}
	memset(&EmuConfig, 0, sizeof(EmuConfig));
	strcpy(EmuConfig.DEV9.EthDevice, "Auto");
	EmuConfig.DEV9.EthApi = EMU_NETAPI_SOCKETS;
	EmuConfig.DEV9.EthEnable = true;
	dev9.eeprom = s_eeprom;
	nif = net_sockets_new();
	if (!nif)
	{
		printf("  skip: the sockets adapter did not come up\n");
		return;
	}
	pkt = (NetPacket*)malloc(sizeof(NetPacket));
	test_zero_options();
	test_icmp_unreachable();
	test_big_datagram();
	test_tiny_mss();
	test_backpressure();
	nif->ops->close(nif);
	nif->ops->destroy(nif);
	free(pkt);
}

int main(void)
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	test_reader();
	test_adapter();
	if (failures)
	{
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
