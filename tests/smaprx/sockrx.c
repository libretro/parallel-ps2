/* The sockets backend's receive thread, on the real net.c, smap.c,
 * sockets.c and sessions.
 *
 * This thread is the EE: it sends UDP datagrams from the PS2 through the
 * adapter to an echo server on the host's own address, then opens a TCP
 * connection to another and sends data down it, and reads every reply
 * out of the SMAP as the IOP driver would. Every reply must come back
 * intact: the TCP session's connect (a socket waited on for writing),
 * its data (for reading) and the PS2's ACKs that let it read again (a
 * send waking the receive thread) all go through the new wait.
 *
 * The receive thread used to sleep a millisecond between polls of every
 * session's socket, so it woke a thousand times a second with nothing to
 * do and a reply waited up to that long. It now sleeps in poll() on the
 * sockets the sessions name and wakes when one is ready or the PS2 sends.
 * With a session open and no traffic for half a second, the process's
 * threads may switch out a few dozen times, not hundreds (Linux, where
 * /proc counts them).
 *
 * Skipped, not failed, where the host has no IPv4 adapter to use.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include <libretro.h>
#include <rthreads/rthreads.h>
#include <retro_atomic.h>
#include <retro_timers.h>
#include <features/features_cpu.h>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <ifaddrs.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#define ROUND_TRIPS 50
#define PAYLOAD     32

/* glue.cpp */
int smaprx_bd_count(void);
unsigned smaprx_bd_empty_flag(void);
void smaprx_open(void);
void smaprx_close(void);
void smaprx_tick(void);
int smaprx_net_up(void);
void smaprx_tx(const unsigned char* frame, int len);
unsigned smaprx_frame_count(void);
unsigned smaprx_bd_read(int bd, int word);
void smaprx_bd_write(int bd, int word, unsigned v);
unsigned smaprx_fifo_read32(void);
void smaprx_frame_dec(void);

int smaprx_host_recv(char* buf, int max) { (void)buf; (void)max; return 0; }

static void harness_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
retro_log_printf_t log_cb = harness_log;

static sock_t s_echo = SOCK_BAD;
static sock_t s_tcp_listen = SOCK_BAD;
static retro_atomic_int_t s_echo_stop;

/* Accepts one connection and sends back what comes, until it closes or
 * the harness stops. */
static void tcp_echo_thread(void* arg)
{
	char buf[2048];
	sock_t c = SOCK_BAD;
	(void)arg;
	while (!retro_atomic_load_acquire_int(&s_echo_stop))
	{
#ifdef _WIN32
		WSAPOLLFD p;
#else
		struct pollfd p;
#endif
		int n;
		p.fd = (c == SOCK_BAD) ? s_tcp_listen : c;
		p.events = POLLIN;
		p.revents = 0;
#ifdef _WIN32
		if (WSAPoll(&p, 1, 50) <= 0)
#else
		if (poll(&p, 1, 50) <= 0)
#endif
			continue;
		if (c == SOCK_BAD)
		{
			c = accept(s_tcp_listen, NULL, NULL);
			continue;
		}
		n = (int)recv(c, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		send(c, buf, n, 0);
	}
	if (c != SOCK_BAD)
		sock_close(c);
}

/* Sends each datagram back where it came from, until told to stop. */
static void echo_thread(void* arg)
{
	char buf[2048];
	(void)arg;
	while (!retro_atomic_load_acquire_int(&s_echo_stop))
	{
		struct sockaddr_in from;
#ifdef _WIN32
		int len = sizeof(from);
		WSAPOLLFD p;
#else
		socklen_t len = sizeof(from);
		struct pollfd p;
#endif
		int n;
		p.fd = s_echo;
		p.events = POLLIN;
		p.revents = 0;
#ifdef _WIN32
		if (WSAPoll(&p, 1, 50) <= 0)
#else
		if (poll(&p, 1, 50) <= 0)
#endif
			continue;
		n = (int)recvfrom(s_echo, buf, sizeof(buf), 0, (struct sockaddr*)&from, &len);
		if (n > 0)
			sendto(s_echo, buf, n, 0, (struct sockaddr*)&from, len);
	}
}

/* The host's first IPv4 address that is not loopback, network order. */
static int host_address(unsigned char out[4])
{
#ifdef _WIN32
	char name[256];
	struct hostent* h;
	if (gethostname(name, sizeof(name)) != 0 || !(h = gethostbyname(name)) || !h->h_addr_list[0])
		return 0;
	memcpy(out, h->h_addr_list[0], 4);
	return out[0] != 127;
#else
	struct ifaddrs *list, *i;
	int found = 0;
	if (getifaddrs(&list) != 0)
		return 0;
	for (i = list; i && !found; i = i->ifa_next)
	{
		const struct sockaddr_in* a = (const struct sockaddr_in*)i->ifa_addr;
		if (!a || a->sin_family != AF_INET)
			continue;
		memcpy(out, &a->sin_addr, 4);
		found = out[0] != 127;
	}
	freeifaddrs(list);
	return found;
#endif
}

static unsigned short csum(const unsigned char* p, int n)
{
	unsigned long s = 0;
	int i;
	for (i = 0; i + 1 < n; i += 2)
		s += (unsigned long)(p[i] << 8 | p[i + 1]);
	if (n & 1)
		s += (unsigned long)(p[n - 1] << 8);
	while (s >> 16)
		s = (s & 0xffff) + (s >> 16);
	return (unsigned short)~s;
}

/* An Ethernet frame from the PS2 (its address is the one the adapter's
 * DHCP server hands out, .100 of the internal network) to the echo
 * server, UDP from port 40000 with `payload`. Returns its length. */
static int build_frame(unsigned char* f, const unsigned char dst[4], unsigned port,
	const unsigned char* payload, int n)
{
	static const unsigned char gw_mac[6]  = {0x76, 0x6D, 0xF4, 0x63, 0x30, 0x31};
	static const unsigned char ps2_mac[6] = {0x00, 0x04, 0x1F, 0x82, 0x30, 0x31};
	unsigned char* ip = f + 14;
	unsigned char* udp = ip + 20;
	const int ip_len = 20 + 8 + n;
	unsigned short c;
	memcpy(f, gw_mac, 6);
	memcpy(f + 6, ps2_mac, 6);
	f[12] = 0x08; f[13] = 0x00;
	memset(ip, 0, 20);
	ip[0] = 0x45;
	ip[2] = (unsigned char)(ip_len >> 8); ip[3] = (unsigned char)ip_len;
	ip[8] = 64; ip[9] = 17;
	ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 100;
	memcpy(ip + 16, dst, 4);
	c = csum(ip, 20);
	ip[10] = (unsigned char)(c >> 8); ip[11] = (unsigned char)c;
	udp[0] = 40000 >> 8; udp[1] = 40000 & 0xff;
	udp[2] = (unsigned char)(port >> 8); udp[3] = (unsigned char)port;
	udp[4] = (unsigned char)((8 + n) >> 8); udp[5] = (unsigned char)(8 + n);
	udp[6] = 0; udp[7] = 0; /* no checksum */
	memcpy(udp + 8, payload, n);
	return 14 + ip_len;
}

/* A TCP segment from the PS2 to the echo server: from port 40001, the
 * flags (FIN 1, SYN 2, PSH 8, ACK 16), sequence and ACK numbers, and n
 * bytes of payload. Returns the frame's length. */
static int build_tcp(unsigned char* f, const unsigned char dst[4], unsigned port,
	unsigned flags, unsigned long seq, unsigned long ack, const unsigned char* payload, int n)
{
	static const unsigned char gw_mac[6]  = {0x76, 0x6D, 0xF4, 0x63, 0x30, 0x31};
	static const unsigned char ps2_mac[6] = {0x00, 0x04, 0x1F, 0x82, 0x30, 0x31};
	unsigned char pseudo[12 + 20 + 64];
	unsigned char* ip = f + 14;
	unsigned char* tcp = ip + 20;
	const int ip_len = 20 + 20 + n;
	unsigned short c;
	memcpy(f, gw_mac, 6);
	memcpy(f + 6, ps2_mac, 6);
	f[12] = 0x08; f[13] = 0x00;
	memset(ip, 0, 20);
	ip[0] = 0x45;
	ip[2] = (unsigned char)(ip_len >> 8); ip[3] = (unsigned char)ip_len;
	ip[6] = 0x40; /* don't fragment */
	ip[8] = 64; ip[9] = 6;
	ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 100;
	memcpy(ip + 16, dst, 4);
	c = csum(ip, 20);
	ip[10] = (unsigned char)(c >> 8); ip[11] = (unsigned char)c;
	memset(tcp, 0, 20);
	tcp[0] = 40001 >> 8; tcp[1] = 40001 & 0xff;
	tcp[2] = (unsigned char)(port >> 8); tcp[3] = (unsigned char)port;
	tcp[4] = (unsigned char)(seq >> 24); tcp[5] = (unsigned char)(seq >> 16);
	tcp[6] = (unsigned char)(seq >> 8);  tcp[7] = (unsigned char)seq;
	tcp[8] = (unsigned char)(ack >> 24); tcp[9] = (unsigned char)(ack >> 16);
	tcp[10] = (unsigned char)(ack >> 8); tcp[11] = (unsigned char)ack;
	tcp[12] = 5 << 4;
	tcp[13] = (unsigned char)flags;
	tcp[14] = 0xff; tcp[15] = 0xff; /* window */
	if (n)
		memcpy(tcp + 20, payload, n);
	/* The checksum over the pseudo header, the header and the data. */
	memcpy(pseudo, ip + 12, 8);
	pseudo[8] = 0; pseudo[9] = 6;
	pseudo[10] = (unsigned char)((20 + n) >> 8); pseudo[11] = (unsigned char)(20 + n);
	memcpy(pseudo + 12, tcp, 20 + n);
	c = csum(pseudo, 12 + 20 + n);
	tcp[16] = (unsigned char)(c >> 8); tcp[17] = (unsigned char)c;
	return 14 + ip_len;
}

static unsigned long be32(const unsigned char* p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

/* The next frame the SMAP has, read out as the driver does; its length,
 * or 0 when there is none yet. */
static int s_bd;
static int read_frame(unsigned char* out, int max)
{
	unsigned len, w;
	if (smaprx_frame_count() == 0)
		return 0;
	len = smaprx_bd_read(s_bd, 2);
	for (w = 0; w < (len + 3) / 4; w++)
	{
		unsigned v = smaprx_fifo_read32();
		if ((int)(w * 4 + 4) <= max)
			memcpy(out + w * 4, &v, 4);
	}
	smaprx_bd_write(s_bd, 0, smaprx_bd_empty_flag());
	smaprx_frame_dec();
	s_bd = (s_bd + 1) % smaprx_bd_count();
	return (int)len;
}

#ifndef _WIN32
/* Voluntary context switches of each thread of this process, by id. */
#define MAX_TASKS 64
struct tasks { int n; long id[MAX_TASKS]; long sw[MAX_TASKS]; };
static void switches(struct tasks* t)
{
	DIR* d = opendir("/proc/self/task");
	struct dirent* e;
	t->n = 0;
	if (!d)
		return;
	while ((e = readdir(d)) && t->n < MAX_TASKS)
	{
		char path[256], line[256];
		FILE* f;
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/proc/self/task/%s/status", e->d_name);
		if (!(f = fopen(path, "r")))
			continue;
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "voluntary_ctxt_switches:", 24))
			{
				t->id[t->n] = atol(e->d_name);
				t->sw[t->n] = atol(line + 24);
				t->n++;
			}
		fclose(f);
	}
	closedir(d);
}

/* Switches between two snapshots, over the threads in both. */
static long switched(const struct tasks* a, const struct tasks* b)
{
	long total = 0;
	int i, j;
	for (i = 0; i < b->n; i++)
		for (j = 0; j < a->n; j++)
			if (a->id[j] == b->id[i])
				total += b->sw[i] - a->sw[j];
	return total;
}
#endif

int main(void)
{
	static unsigned char frame[2048], got[2048];
	unsigned char host[4], payload[PAYLOAD];
	struct sockaddr_in a;
#ifdef _WIN32
	int alen = sizeof(a);
	WSADATA wsa;
#else
	socklen_t alen = sizeof(a);
#endif
	sthread_t* echo;
	sthread_t* tcp_echo;
	unsigned port, tcp_port;
	int trip, failures = 0;
	retro_time_t worst = 0, total = 0;

#ifdef _WIN32
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	if (!host_address(host))
	{
		printf("  skip: no IPv4 host adapter\n");
		return 0;
	}

	s_echo = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	if (s_echo == SOCK_BAD || bind(s_echo, (struct sockaddr*)&a, sizeof(a)) != 0 ||
		getsockname(s_echo, (struct sockaddr*)&a, &alen) != 0)
	{
		printf("  skip: no UDP socket for the echo server\n");
		return 0;
	}
	port = ntohs(a.sin_port);
	s_tcp_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	alen = sizeof(a);
	if (s_tcp_listen == SOCK_BAD || bind(s_tcp_listen, (struct sockaddr*)&a, sizeof(a)) != 0 ||
		listen(s_tcp_listen, 1) != 0 || getsockname(s_tcp_listen, (struct sockaddr*)&a, &alen) != 0)
	{
		printf("  skip: no TCP socket for the echo server\n");
		return 0;
	}
	tcp_port = ntohs(a.sin_port);
	echo = sthread_create(echo_thread, NULL);
	tcp_echo = sthread_create(tcp_echo_thread, NULL);

	smaprx_open();
	if (!smaprx_net_up())
	{
		printf("  skip: the sockets adapter did not come up\n");
		retro_atomic_store_release_int(&s_echo_stop, 1);
		sthread_join(echo);
		sthread_join(tcp_echo);
		return 0;
	}

	for (trip = 0; trip < ROUND_TRIPS && !failures; trip++)
	{
		const retro_time_t t0 = cpu_features_get_time_usec();
		int i, len, n;
		for (i = 0; i < PAYLOAD; i++)
			payload[i] = (unsigned char)(trip * 7 + i);
		len = build_frame(frame, host, port, payload, PAYLOAD);
		smaprx_tx(frame, len);
		for (;;)
		{
			smaprx_tick();
			n = read_frame(got, (int)sizeof(got));
			if (n > 0)
			{
				/* Ethernet, IPv4 of 20 bytes, UDP: the payload at 42. */
				if (n < 42 + PAYLOAD || got[23] != 17 || memcmp(got + 42, payload, PAYLOAD))
					continue; /* not ours (an ARP, a DHCP offer) */
				break;
			}
			if (cpu_features_get_time_usec() - t0 > 5000000)
			{
				fprintf(stderr, "  FAIL: no reply to datagram %d\n", trip);
				failures++;
				break;
			}
			sthread_yield();
		}
		if (!failures)
		{
			const retro_time_t dt = cpu_features_get_time_usec() - t0;
			total += dt;
			if (dt > worst)
				worst = dt;
		}
	}

	/* TCP: connect, then data down the connection and back, each echo
	 * ACKed so the session reads again. */
	if (!failures)
	{
		unsigned char tcp_payload[PAYLOAD];
		unsigned long a = 1000, srv = 0;
		const int rounds = 20;
		int stage = 0, r = 0, len, n, i;
		retro_time_t t0 = cpu_features_get_time_usec(), tdata = 0, tworst = 0;

		len = build_tcp(frame, host, tcp_port, 2, a, 0, NULL, 0); /* SYN */
		smaprx_tx(frame, len);
		while (!failures && r < rounds)
		{
			smaprx_tick();
			n = read_frame(got, (int)sizeof(got));
			if (n >= 54 && got[23] == 6)
			{
				const unsigned flags = got[47];
				const int data = n - 54;
				if (stage == 0 && (flags & 0x12) == 0x12 && be32(got + 42) == a + 1)
				{
					/* SYN-ACK: ACK it, then the first data. */
					srv = be32(got + 38) + 1;
					a++;
					len = build_tcp(frame, host, tcp_port, 16, a, srv, NULL, 0);
					smaprx_tx(frame, len);
					stage = 1;
				}
				else if (stage == 2 && data >= PAYLOAD && !memcmp(got + 54, tcp_payload, PAYLOAD))
				{
					const retro_time_t dt = cpu_features_get_time_usec() - t0;
					tdata += dt;
					if (dt > tworst)
						tworst = dt;
					srv += (unsigned long)data;
					len = build_tcp(frame, host, tcp_port, 16, a, srv, NULL, 0);
					smaprx_tx(frame, len);
					r++;
					stage = 1;
				}
				if (stage == 1 && r < rounds)
				{
					for (i = 0; i < PAYLOAD; i++)
						tcp_payload[i] = (unsigned char)(r * 13 + i);
					len = build_tcp(frame, host, tcp_port, 8 | 16, a, srv, tcp_payload, PAYLOAD);
					t0 = cpu_features_get_time_usec();
					smaprx_tx(frame, len);
					a += PAYLOAD;
					stage = 2;
				}
				continue;
			}
			if (n == 0 && cpu_features_get_time_usec() - t0 > 5000000)
			{
				fprintf(stderr, "  FAIL: TCP %s %d did not come back\n", stage ? "data" : "connect", r);
				failures++;
			}
			if (n == 0)
				sthread_yield();
		}
		if (!failures)
			printf("  ok: %d TCP echoes, %.0f us mean, %.0f us worst\n", rounds, (double)tdata / rounds, (double)tworst);
		len = build_tcp(frame, host, tcp_port, 1 | 16, a, srv, NULL, 0); /* FIN */
		smaprx_tx(frame, len);
	}

#ifndef _WIN32
	if (!failures)
	{
		/* Quiet: a session open, nothing coming. The EE side sleeps too. */
		static struct tasks before, after;
		long woke;
		switches(&before);
		retro_sleep(500);
		switches(&after);
		woke = switched(&before, &after);
		printf("  idle half second: %ld context switches\n", woke);
		if (before.n == 0 || woke > 60)
		{
			fprintf(stderr, "  FAIL: the receive thread polls while nothing comes (%ld switches)\n", woke);
			failures++;
		}
	}
#endif

	smaprx_close();
	retro_atomic_store_release_int(&s_echo_stop, 1);
	sthread_join(echo);
	sthread_join(tcp_echo);
	sock_close(s_echo);
	sock_close(s_tcp_listen);

	if (failures)
		return 1;
	printf("  ok: %d datagrams echoed through the sockets adapter, %.0f us mean, %.0f us worst\n",
		ROUND_TRIPS, (double)total / ROUND_TRIPS, (double)worst);
	return 0;
}
