/* The Konami Python 2 I/O board (pcsx2/USB/usb-python2/p2io.c) and the
 * .py2 descriptor parser (py2desc.c), byte for byte.
 *
 * Commands go in the way the game sends them, escaped and split across
 * transfers where that matters; answers come back through p2io_in and are
 * checked against what the board sends. The serial ports are driven with
 * ACIO packets built here, checksummed and escaped twice over as on the
 * wire. Dongles are made with an independent Dallas CRC, cards and
 * descriptors are written to files. */
#include "USB/usb-python2/p2io.h"
#include "USB/usb-python2/py2desc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_bad;

static void check(int ok, const char* what)
{
	printf("  %-66s %s\n", what, ok ? "ok" : "WRONG");
	if (!ok)
		s_bad++;
}

/* ---- the wire ---------------------------------------------------- */

static size_t escape(uint8_t* dst, const uint8_t* src, size_t n)
{
	size_t i, k = 0;
	for (i = 0; i < n; i++)
	{
		if (src[i] == 0xaa || src[i] == 0xff)
		{
			dst[k++] = 0xff;
			dst[k++] = (uint8_t)~src[i];
		}
		else
			dst[k++] = src[i];
	}
	return k;
}

static size_t unescape(uint8_t* dst, const uint8_t* src, size_t n)
{
	size_t i, k = 0;
	int inv = 0;
	for (i = 0; i < n; i++)
	{
		if (src[i] == 0xff)
			inv = 1;
		else
		{
			dst[k++] = inv ? (uint8_t)~src[i] : src[i];
			inv = 0;
		}
	}
	return k;
}

/* A command on the wire: magic, then length, sequence, command and
 * arguments, escaped. */
static size_t command(uint8_t* out, uint8_t seq, uint8_t cmd, const uint8_t* args, size_t nargs)
{
	uint8_t body[300];
	body[0] = (uint8_t)(nargs + 2);
	body[1] = seq;
	body[2] = cmd;
	if (nargs)
		memcpy(body + 3, args, nargs);
	out[0] = 0xaa;
	return 1 + escape(out + 1, body, nargs + 3);
}

/* Sends a command and reads the whole answer; returns the answer's body
 * (sequence, status, payload) length, or -1 when it is not framed. */
static int ask(p2io_t* p, uint8_t seq, uint8_t cmd, const uint8_t* args, size_t nargs, uint8_t* body)
{
	uint8_t wire[700], raw[700], plain[700];
	size_t n = command(wire, seq, cmd, args, nargs), got = 0, k;
	p2io_out(p, wire, n);
	got = p2io_in(p, raw, sizeof(raw));
	if (got < 2 || raw[0] != 0xaa)
		return -1;
	k = unescape(plain, raw + 1, got - 1);
	if (k < 1 || plain[0] != k - 1)
		return -1;
	memcpy(body, plain + 1, k - 1);
	return (int)(k - 1);
}

static p2io_t* board(int game)
{
	static p2io_t p;
	p2io_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.game = game;
	p2io_init(&p, &cfg);
	return &p;
}

static void press(p2io_t* p, uint32_t p1, uint32_t p2)
{
	p2io_input in;
	memset(&in, 0, sizeof(in));
	in.pad[0] = p1;
	in.pad[1] = p2;
	in.wheel  = P2IO_WHEEL_CENTER;
	p2io_set_input(p, &in);
}

static uint32_t jamma(p2io_t* p)
{
	uint8_t r[P2IO_JAMMA_REPORT];
	p2io_jamma_read(p, r);
	return (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
}

/* ---- dongles ----------------------------------------------------- */

/* Dallas 1-Wire CRC-8, x^8+x^5+x^4+1 reflected, from 0xff. */
static uint8_t crc8(const uint8_t* d, size_t n)
{
	uint8_t crc = 0xff;
	size_t i;
	int b;
	for (i = 0; i < n; i++)
	{
		crc ^= d[i];
		for (b = 0; b < 8; b++)
			crc = (uint8_t)((crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1);
	}
	return crc;
}

static void make_payload(uint8_t pay[32], uint8_t seed)
{
	int i;
	for (i = 0; i < 31; i++)
		pay[i] = (uint8_t)(seed + i * 7);
	pay[31] = (uint8_t)~crc8(pay, 31);
}

static void test_dongles(void)
{
	uint8_t pay[32], ser[8], file[40], out[40], body[64];
	p2io_config cfg;
	p2io_t p;
	int n, i;

	make_payload(pay, 0x31);
	for (i = 0; i < 8; i++)
		ser[i] = (uint8_t)(0xa0 + i);

	memcpy(file, ser, 8);
	memcpy(file + 8, pay, 32);
	check(p2io_parse_dongle(file, out) && !memcmp(out, ser, 8) && !memcmp(out + 8, pay, 32),
		"a serial-first dongle reads as serial then payload");

	memcpy(file, pay, 32);
	memcpy(file + 32, ser, 8);
	check(p2io_parse_dongle(file, out) && !memcmp(out, ser, 8) && !memcmp(out + 8, pay, 32),
		"MAME's payload-first layout is put in the board's order");

	file[5] ^= 1;
	memset(out, 0x55, sizeof(out));
	check(!p2io_parse_dongle(file, out) && out[0] == 0 && out[39] == 0, "a payload whose CRC fails is refused, zeroed");

	/* On the board: dongle 0 (black) loaded, dongle 1 (white) not. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.game             = P2IO_GAME_DDR;
	cfg.dongle_loaded[0] = 1;
	memcpy(cfg.dongle[0], ser, 8);
	memcpy(cfg.dongle[0] + 8, pay, 32);
	p2io_init(&p, &cfg);
	{
		/* The sub-command, then 40 bytes of data. */
		uint8_t args[41];
		memset(args, 0, sizeof(args));
		for (i = 0; i < 40; i++)
			args[1 + i] = (uint8_t)(0x10 + i);

		args[0] = 2; /* read, no dongle picked yet: the request's data back */
		n = ask(&p, 1, 0x25, args, 41, body);
		check(n == 43 && body[2] == 0 && body[3] == 0x10 && body[42] == 0x10 + 39,
			"a read before a dongle is picked echoes the request's data");

		args[0] = 0;
		n = ask(&p, 2, 0x25, args, 1, body);
		check(n == 43 && body[0] == 2 && body[1] == 0 && body[2] == 1 && !memcmp(body + 3, ser, 8)
				&& !memcmp(body + 11, pay, 32),
			"the black dongle: present, serial and payload");

		args[0] = 2; /* read again: the last one picked */
		n = ask(&p, 3, 0x25, args, 1, body);
		check(n == 43 && body[2] == 1 && !memcmp(body + 3, ser, 8), "sub-command 2 reads the dongle last picked");

		args[0] = 1;
		n = ask(&p, 4, 0x25, args, 1, body);
		check(n == 43 && body[2] == 0 && body[3] == 0 && body[42] == 0, "the white dongle, absent: zeroes");

		args[0] = 3; /* a write: the request's data back, short request zero-filled */
		args[1] = 0x77;
		n = ask(&p, 5, 0x25, args, 3, body);
		check(n == 43 && body[2] == 0 && body[3] == 0x77 && body[4] == 0x11 && body[5] == 0 && body[42] == 0,
			"a write echoes; past a short request, zeroes");
	}
}

/* ---- framing ----------------------------------------------------- */

static void test_framing(void)
{
	p2io_t* p = board(P2IO_GAME_DDR);
	uint8_t wire[64], raw[64], body[64];
	size_t n, got;

	/* GET_VERSION: D44 1.6.4. */
	n = command(wire, 0x10, 0x01, NULL, 0);
	p2io_out(p, wire, n);
	got = p2io_in(p, raw, sizeof(raw));
	{
		static const uint8_t want[] = { 0xaa, 9, 0x10, 0, 'D', '4', '4', 0, 1, 6, 4 };
		check(got == sizeof(want) && !memcmp(raw, want, got), "GET_VERSION answers aa 09 seq 00 D44 1.6.4");
	}

	/* Nothing waiting: a single zero. */
	got = p2io_in(p, raw, sizeof(raw));
	check(got == 1 && raw[0] == 0, "with no command waiting the board answers one zero");

	/* A command split mid-escape: sequence 0xaa goes out as ff 55. */
	n = command(wire, 0xaa, 0x01, NULL, 0);
	check(n == 5 && wire[2] == 0xff && wire[3] == 0x55, "a sequence number of 0xaa is escaped");
	p2io_out(p, wire, 3); /* ends on the ff */
	got = p2io_in(p, raw, sizeof(raw));
	check(got == 1 && raw[0] == 0, "half a command is not answered");
	p2io_out(p, wire + 3, n - 3);
	got = p2io_in(p, raw, sizeof(raw));
	check(got == 12 && raw[2] == 0xff && raw[3] == 0x55,
		"an escape split across two transfers pairs up, and the answer escapes it back");

	/* Noise ahead of a command is dropped. */
	{
		uint8_t noisy[64];
		noisy[0] = 0x12;
		noisy[1] = 0x34;
		n = command(noisy + 2, 0x11, 0x01, NULL, 0);
		p2io_out(p, noisy, n + 2);
		check(ask(p, 0x12, 0x01, NULL, 0, body) == 9 && body[0] == 0x11, "noise before the magic is skipped");
		got = p2io_in(p, raw, sizeof(raw));
		check(got == 12 - 1 && raw[2] == 0x12, "and the command after it is answered next");
	}

	/* Read in small pieces: the rest waits for the next transfer. */
	n = command(wire, 0x20, 0x01, NULL, 0);
	p2io_out(p, wire, n);
	got = p2io_in(p, raw, 4);
	check(got == 4 && raw[2] == 0x20, "a 4-byte transfer takes the answer's first 4 bytes");
	got = p2io_in(p, raw, 64);
	check(got == 7 && raw[0] == 'D', "the next transfer takes the rest, no new command run");

	/* An unknown command: sequence and status only. */
	check(ask(p, 0x21, 0x7e, NULL, 0, body) == 2 && body[0] == 0x21 && body[1] == 0, "an unknown command gets sequence and status");
}

static void test_settings(void)
{
	p2io_config cfg;
	p2io_t p;
	uint8_t body[64], arg = 0;

	memset(&cfg, 0, sizeof(cfg));
	cfg.dipsw[0] = 1; /* DIPSW1 is bit 3 */
	cfg.dipsw[3] = 1;
	p2io_init(&p, &cfg);
	check(ask(&p, 1, 0x27, &arg, 1, body) == 3 && body[2] == 0x09, "DIP switches 1 and 4 read 0x09");
	check(ask(&p, 2, 0x23, NULL, 0, body) == 3 && body[2] == 0x80, "the AV report says 15 kHz by default");
	cfg.force_31khz = 1;
	p2io_init(&p, &cfg);
	check(ask(&p, 3, 0x23, NULL, 0, body) == 3 && body[2] == 0x00, "and 31 kHz when the descriptor forces it");
	cfg.game = 99;
	p2io_init(&p, &cfg);
	check(p.cfg.game == P2IO_GAME_DRUMMANIA, "an input type out of range is DrumMania's");
	{
		uint8_t fw = 0xaa;
		check(ask(&p, 4, 0x03, &fw, 1, body) == 3 && body[2] == 0, "firmware write mode with its key is acknowledged");
		fw = 0x00;
		check(ask(&p, 5, 0x03, &fw, 1, body) == 2, "and without the key gets no payload");
	}
}

/* ---- JAMMA ------------------------------------------------------- */

static void test_jamma(void)
{
	p2io_t* p;
	uint32_t a, b;
	uint8_t body[16], r[P2IO_JAMMA_REPORT];

	p = board(P2IO_GAME_DDR);
	press(p, 0, 0);
	a = jamma(p);
	b = jamma(p);
	check(a == 0xf0ffff82u && b == 0xf0ffff80u, "idle: the board's rest state, watchdog bit flipping each report");

	press(p, P2IO_IN_START | P2IO_DDR_UP | P2IO_IN_RIGHT, P2IO_DDR_LEFT | P2IO_IN_START);
	a = jamma(p);
	check(!(a & 0x100) && !(a & 0x200) && !(a & 0x8000), "DDR player 1: start, up and select right pull their bits low");
	check(!(a & 0x10000) && !(a & 0x80000) && (a & 0x20000), "DDR player 2: start and left low, up still high");

	press(p, P2IO_IN_TEST | P2IO_IN_SERVICE, 0);
	a = jamma(p);
	check(!(a & 0x10000000u) && !(a & 0x40000000u), "test and service read from player 1's buttons");

	/* Coins count on the press, once. */
	press(p, P2IO_IN_COIN, 0);
	jamma(p);
	jamma(p);
	press(p, 0, 0);
	jamma(p);
	press(p, P2IO_IN_COIN, P2IO_IN_COIN);
	jamma(p);
	press(p, 0, 0);
	jamma(p);
	check(ask(p, 9, 0x31, NULL, 0, body) == 7 && body[2] == 0 && body[3] == 0 && body[4] == 2 && body[5] == 0 && body[6] == 1,
		"coin stock: two coins in slot 1, one in slot 2, each press once");

	p = board(P2IO_GAME_DRUMMANIA);
	press(p, P2IO_DM_BASS | P2IO_IN_RIGHT, P2IO_DM_SNARE);
	a = jamma(p);
	check(!(a & 0x8000) && !(a & 0x100000) && (a & 0x400), "DrumMania: bass and select right low; player 2's pad ignored");

	p = board(P2IO_GAME_GUITARFREAKS);
	press(p, P2IO_GF_RED, 0);
	a = jamma(p);
	check(!(a & 0x2000) && !(a & 0x1000) && (a & 0x0800), "GuitarFreaks: red low, the knob at effect 1 by default");
	press(p, P2IO_GF_EFFECT3, P2IO_GF_EFFECT2);
	jamma(p);
	press(p, 0, 0);
	a = jamma(p);
	check(!(a & 0x1800) && (a & 0x100000) && !(a & 0x80000), "the knobs stay where turned: 1P on 3, 2P on 2");

	p = board(P2IO_GAME_THRILLDRIVE);
	{
		p2io_input in;
		memset(&in, 0, sizeof(in));
		in.wheel = 0x1234;
		in.accel = 0x10000; /* past the end: clamped */
		in.brake = -5;
		p2io_set_input(p, &in);
		p2io_jamma_read(p, r);
		check(r[4] == 0x12 && r[5] == 0x34 && r[6] == 0xff && r[7] == 0xff && r[8] == 0 && r[9] == 0 && r[10] == 0,
			"Thrill Drive: wheel, accelerator and brake big-endian after the bits, clamped");
	}

	p = board(P2IO_GAME_TOYSMARCH);
	press(p, P2IO_IN_LEFT, P2IO_IN_RIGHT);
	a = jamma(p);
	check(!(a & 0x800) && !(a & 0x100000), "Toy's March: player 1 left, player 2 right");
}

/* ---- the serial ports -------------------------------------------- */

/* An ACIO packet on the wire inside SCI_WRITE: magic, address, code,
 * sequence, length, payload and checksum, escaped after the magic. */
static size_t acio_packet(uint8_t* out, uint8_t addr, unsigned code, uint8_t seq, const uint8_t* pay, uint8_t n)
{
	uint8_t raw[80];
	unsigned sum = 0, i;
	raw[0] = addr;
	raw[1] = (uint8_t)(code >> 8);
	raw[2] = (uint8_t)code;
	raw[3] = seq;
	raw[4] = n;
	if (n)
		memcpy(raw + 5, pay, n);
	for (i = 0; i < (unsigned)n + 5; i++)
		sum += raw[i];
	raw[5 + n] = (uint8_t)sum;
	out[0] = 0xaa;
	return 1 + escape(out + 1, raw, (size_t)n + 6);
}

static void sci_open(p2io_t* p, uint8_t port)
{
	uint8_t args[3], body[8];
	args[0] = port;
	args[1] = 0;
	args[2] = 0;
	ask(p, 0x40, 0x38, args, 3, body);
}

static int sci_write(p2io_t* p, uint8_t port, const uint8_t* pkt, size_t n)
{
	uint8_t args[200], body[16];
	args[0] = port;
	args[1] = (uint8_t)n;
	memcpy(args + 2, pkt, n);
	return ask(p, 0x41, 0x3a, args, n + 2, body) == 3 && body[2] == (uint8_t)n;
}

/* Reads up to want bytes from a port; returns the count, data in out. */
static int sci_read(p2io_t* p, uint8_t port, uint8_t want, uint8_t* out)
{
	uint8_t args[2], body[300];
	int n;
	args[0] = port;
	args[1] = want;
	n = ask(p, 0x42, 0x3b, args, 2, body);
	if (n < 3)
		return -1;
	memcpy(out, body + 3, body[2]);
	return body[2];
}

/* One ACIO request and its answer's payload; -1 for no answer. */
static int acio(p2io_t* p, uint8_t port, uint8_t addr, unsigned code, const uint8_t* pay, uint8_t n, uint8_t* answer)
{
	uint8_t pkt[200], got[300], plain[300];
	size_t len = acio_packet(pkt, addr, code, 0x05, pay, n);
	int k;
	size_t m;
	unsigned sum = 0, i;
	if (!sci_write(p, port, pkt, len))
		return -2;
	k = sci_read(p, port, 255, got);
	if (k <= 0 || got[0] != 0xaa)
		return -1;
	m = unescape(plain, got + 1, (size_t)k - 1);
	if (m < 6 || plain[0] != (addr | 0x80) || plain[3] != 0x05 || plain[4] != m - 6)
		return -3;
	for (i = 0; i < m - 1; i++)
		sum += plain[i];
	if ((uint8_t)sum != plain[m - 1])
		return -4;
	memcpy(answer, plain + 5, m - 6);
	return (int)(m - 6);
}

static void test_acio(void)
{
	p2io_config cfg;
	p2io_t p;
	uint8_t a[64], pay[4];
	int n;

	memset(&cfg, 0, sizeof(cfg));
	cfg.game           = P2IO_GAME_DDR;
	cfg.card_loaded[0] = 1;
	memcpy(cfg.card[0], "\xe0\x04\x01\x02\x03\x04\x05\x06", 8);
	p2io_init(&p, &cfg);

	check(acio(&p, 1, 0, 0x0001, NULL, 0, a) == -1, "a closed port answers nothing");
	sci_open(&p, 1);
	n = acio(&p, 1, 0, 0x0001, NULL, 0, a);
	check(n == 1 && a[0] == 2, "DDR's reader bus: two nodes");
	n = acio(&p, 1, 1, 0x0002, NULL, 0, a);
	check(n == 44 && !memcmp(a + 8, "ICCA", 4) && !memcmp(a + 12, "Oct 26 2005", 11), "node 1's version: ICCA, built Oct 26 2005");
	check(acio(&p, 1, 3, 0x0002, NULL, 0, a) == -1, "an address with no node answers nothing");

	{
		/* A checksum one off: dropped. */
		uint8_t pkt[64];
		size_t len = acio_packet(pkt, 1, 0x0002, 5, NULL, 0);
		pkt[len - 1] ^= 1;
		sci_write(&p, 1, pkt, len);
		check(sci_read(&p, 1, 255, a) == 0, "a packet with a bad checksum is dropped");
	}

	/* The keypad: 5 down is code 6, an event with a counter. */
	press(&p, P2IO_IN_KEY(5), 0);
	n = acio(&p, 1, 1, 0x0134, NULL, 0, a);
	check(n == 16 && a[0] == 1 && a[11] == 3 && a[12] == 0x86 && a[13] == 0, "keypad 5 down: event 0x86, the reader empty");
	n = acio(&p, 1, 1, 0x0134, NULL, 0, a);
	check(n == 16 && a[12] == 0x86, "held, no new event");
	press(&p, P2IO_IN_KEY(10), 0);
	n = acio(&p, 1, 1, 0x0134, NULL, 0, a);
	check(n == 16 && a[12] == 0x86, "5 to 00 (6 to 4) adds no key bit: no event");
	press(&p, 0, 0);
	acio(&p, 1, 1, 0x0134, NULL, 0, a);
	press(&p, P2IO_IN_KEY(10), 0);
	n = acio(&p, 1, 1, 0x0134, NULL, 0, a);
	check(n == 16 && a[12] == 0x94 && a[13] == 0x86, "released, then 00: event 0x94, its counter stepped, the last behind it");

	/* Cards: accepted, pressed in - the first poll reads the card file
	 * and takes it back out, the next one with the button still down
	 * puts it in. */
	press(&p, 0, 0);
	pay[0] = 0;
	pay[1] = 0x11;
	acio(&p, 1, 1, 0x0135, pay, 2, a);
	press(&p, P2IO_IN_CARD, 0);
	acio(&p, 1, 1, 0x0134, NULL, 0, a);
	n = acio(&p, 1, 1, 0x0134, NULL, 0, a);
	check(n == 16 && a[0] == 2 && a[1] == 0x30 && !memcmp(a + 2, "\xe0\x04\x01\x02", 4), "a card in: status 2, sensors 0x30, its ID");
	press(&p, 0, 0);
	acio(&p, 1, 1, 0x0134, NULL, 0, a);
	press(&p, P2IO_IN_CARD, 0);
	n = acio(&p, 1, 1, 0x0134, NULL, 0, a);
	check(n == 16 && a[0] == 1 && a[2] == 0, "pressed again: out");

	/* Player 2's reader has no card file: it stays empty. */
	press(&p, 0, 0);
	acio(&p, 1, 2, 0x0135, pay, 2, a);
	press(&p, 0, P2IO_IN_CARD);
	acio(&p, 1, 2, 0x0134, NULL, 0, a);
	n = acio(&p, 1, 2, 0x0134, NULL, 0, a);
	check(n == 16 && a[1] == 0 && a[2] == 0, "a reader with no card file never reports a card");

	/* The extio on port 0: a lamp packet with its 7-bit sum is acked. */
	sci_open(&p, 0);
	{
		uint8_t lamps[4];
		lamps[0] = 0x90;
		lamps[1] = 0x00;
		lamps[2] = 0x00;
		lamps[3] = 0x10;
		sci_write(&p, 0, lamps, 4);
		check(sci_read(&p, 0, 8, a) == 1 && a[0] == 0x11, "extio: a lamp packet is acked with 0x11");
		lamps[3] = 0x11;
		sci_write(&p, 0, lamps, 4);
		check(sci_read(&p, 0, 8, a) == 0, "extio: a bad sum is not");
	}

	/* A port past the board's two: ignored, still answered. */
	{
		uint8_t args[4], body[16];
		args[0] = 7;
		args[1] = 1;
		args[2] = 0x55;
		check(ask(&p, 1, 0x3a, args, 3, body) == 3 && body[2] == 1, "a write to port 7 is acknowledged and goes nowhere");
		args[1] = 4;
		check(ask(&p, 1, 0x3b, args, 2, body) == 3 && body[2] == 0, "a read from it is empty");
	}

	/* Closing a port stops its answers. */
	{
		uint8_t args[3], body[8];
		args[0] = 1;
		args[1] = 0xff;
		args[2] = 0;
		ask(&p, 0x43, 0x38, args, 3, body);
		check(acio(&p, 1, 0, 0x0001, NULL, 0, a) == -1, "a port closed with 0xff answers nothing");
	}
}

static void test_thrilldrive(void)
{
	p2io_t* p = board(P2IO_GAME_THRILLDRIVE);
	uint8_t a[64], pay[8], r[P2IO_JAMMA_REPORT];
	p2io_input in;
	int n;

	sci_open(p, 1);
	check(acio(p, 1, 0, 0x0001, NULL, 0, a) == 1 && a[0] == 2, "Thrill Drive's bus: wheel and seatbelt");
	n = acio(p, 1, 1, 0x0002, NULL, 0, a);
	check(n == 44 && !memcmp(a + 8, "HNDL", 4), "node 1 is the wheel (HNDL)");
	n = acio(p, 1, 2, 0x0002, NULL, 0, a);
	check(n == 44 && !memcmp(a + 8, "BELT", 4), "node 2 the seatbelt (BELT)");

	/* The seatbelt flips on each press. */
	press(p, 0, 0);
	n = acio(p, 1, 2, 0x0113, NULL, 0, a);
	check(n == 3 && a[2] == 0xff, "the belt starts unbuckled");
	press(p, P2IO_TD_SEATBELT, 0);
	acio(p, 1, 2, 0x0113, NULL, 0, a);
	n = acio(p, 1, 2, 0x0113, NULL, 0, a);
	check(n == 3 && a[2] == 0, "a press buckles it, holding does not undo it");

	/* Calibration: the wheel follows the force the game drives. */
	pay[0] = 64;
	pay[1] = 0;
	pay[2] = 0;
	pay[3] = 0xfe;
	n = acio(p, 1, 1, 0x0120, pay, 4, a);
	check(n == 4, "a force-feedback packet is answered with four zeroes");
	memset(&in, 0, sizeof(in));
	in.wheel = 0x100;
	p2io_set_input(p, &in);
	p2io_jamma_read(p, r);
	{
		const int32_t want = 0xffff - (int32_t)((float)0xffff * (64.0f / 127.0f));
		check(r[4] == (uint8_t)(want >> 8) && r[5] == (uint8_t)want, "while calibrating the wheel reads where the force puts it");
	}
	acio(p, 1, 1, 0x0100, NULL, 0, a);
	p2io_jamma_read(p, r);
	check(r[4] == 0x01 && r[5] == 0x00, "a reset hands the wheel back to the player");
}

static void test_drumpad(void)
{
	p2io_t* p = board(P2IO_GAME_TOYSMARCH);
	uint8_t d[32];
	int n;

	press(p, P2IO_TM_CYMBAL, P2IO_TM_DRUM_R);
	n = sci_read(p, 0, 4, d);
	check(n == 11 && d[0] == 0xaa && d[1] == 0 && d[2] == 0x80 && d[3] == 0 && d[8] == 0x80,
		"drum pads: the whole 11-byte state whatever was asked, hits on the press");
	check(d[10] == (uint8_t)(d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7] + d[8] + d[9]), "with its sum");
	n = sci_read(p, 0, 11, d);
	check(n == 11 && d[1] == 1 && d[2] == 0 && d[8] == 0, "held, no second hit; the counter steps");
}

/* The Dance 86.4 stage's handshake, after MAME's ksys573: the game clocks
 * 0xc90 in on lamp outputs, the stage answers through its pad inputs for
 * 22 clocks, then the pads read as pads. */
static void test_dance864(void)
{
	p2io_t* p = board(P2IO_GAME_DANCE864);
	uint8_t args[2], body[8];
	uint32_t a;
	int i, clocks = 0;
	const unsigned word = 0xc90;

	press(p, P2IO_D864_CENTER, 0);
	a = jamma(p);
	check((a & 0x400), "before the handshake the pads do not read");

	/* Each bit: data with the clock low, then the clock high. The stage
	 * shifts right, so 13 bits go in low bit first. Bits 0 and 2 of the
	 * lamp byte are the two stages' data, 1 and 3 their clocks, active
	 * low. */
	args[0] = 1;
	for (i = 0; i < 13; i++)
	{
		const int bit = (word >> i) & 1;
		args[1] = (uint8_t)((bit ? 5 : 0) | 0x0a); /* clocks idle (high) */
		ask(p, 1, 0x24, args, 2, body);
		args[1] = (uint8_t)(bit ? 5 : 0);         /* clocks active */
		ask(p, 1, 0x24, args, 2, body);
		clocks++;
	}
	a = jamma(p);
	check(((a >> 8) & 0xff) == 0xf9 && ((a >> 16) & 0xff) == 0xf9, "0xc90 clocked in: both stages answer 0xf9");
	for (i = 0; i < 22; i++)
	{
		args[1] = 0x0a;
		ask(p, 1, 0x24, args, 2, body);
		args[1] = 0x00;
		ask(p, 1, 0x24, args, 2, body);
	}
	press(p, P2IO_D864_CENTER, 0);
	a = jamma(p);
	check(!(a & 0x400) && (a & 0x800), "22 clocks later the stage is up: centre reads, left does not");
}

static void test_state(void)
{
	p2io_t* p = board(P2IO_GAME_DDR);
	p2io_state saved;
	uint8_t raw[32], first[32], again[32], wire[16];
	size_t n, k1, k2;

	n = command(wire, 0x33, 0x01, NULL, 0);
	p2io_out(p, wire, n);
	k1 = p2io_in(p, raw, 3);
	saved = p->st;
	k1 = p2io_in(p, first, sizeof(first));
	p->st = saved;
	k2 = p2io_in(p, again, sizeof(again));
	check(k1 == k2 && !memcmp(first, again, k1), "the state is the struct: a copy taken mid-answer gives the rest again");
	(void)raw;
}

/* ---- files ------------------------------------------------------- */

static void put_file(const char* path, const void* data, size_t n)
{
	FILE* f = fopen(path, "wb");
	if (f)
	{
		fwrite(data, 1, n, f);
		fclose(f);
	}
}

static void test_cards(void)
{
	uint8_t id[8];
	check(p2io_parse_card("E004010203040506", 16, id) && id[0] == 0xe0 && id[7] == 0x06, "a card file: 16 hex digits");
	check(p2io_parse_card(" e004010203040506\r\n", 19, id) && id[1] == 0x04, "whitespace and CRLF around them are fine");
	check(!p2io_parse_card("E0040102030405", 14, id), "too short is refused");
	check(!p2io_parse_card("E00401020304050G", 16, id), "a non-hex digit is refused");
}

static void test_desc(void)
{
	static const char text[] =
		"; a comment\r\n"
		"[Other]\r\n"
		"HddImagePath=wrong.chd\r\n"
		"[game]\r\n"
		"Name = DDR SuperNova 2\r\n"
		"GameId=GDJJA\r\n"
		"hddimagepath=gdj.chd\r\n"
		"HddIdPath=/abs/id.bin\r\n"
		"NvRamPath=nv\\\\ram.nvm\r\n"
		"DongleBlackPath=black.bin\r\n"
		"DongleWhitePath=white.bin\r\n"
		"Player1Card=card1.txt\r\n"
		"PatchFile=C:\\\\patch.pnach\r\n"
		"InputType=2\r\n"
		"DIPSW1=true\r\n"
		"DIPSW3=1\r\n"
		"DIPSW4=false\r\n"
		"Force31kHz=yes\r\n"
		"# done";
	py2_desc* d = (py2_desc*)malloc(sizeof(py2_desc));
	p2io_config cfg;
	uint8_t pay[32], file[40];
	unsigned missing;

	check(py2_parse(text, strlen(text), "games/ddr", d), "a descriptor parses");
	check(!strcmp(d->hdd_image, "games/ddr/gdj.chd"), "keys are case-blind, other sections ignored, paths joined to its directory");
	check(!strcmp(d->name, "DDR SuperNova 2") && !strcmp(d->game_id, "GDJJA"), "name and game ID, spaces trimmed");
	check(!strcmp(d->hdd_id, "/abs/id.bin") && !strcmp(d->patch, "C:\\\\patch.pnach"), "absolute paths kept as they are");
	check(d->input_type == 2 && d->dipsw[0] && !d->dipsw[1] && d->dipsw[2] && !d->dipsw[3] && d->force_31khz,
		"input type, DIP switches and 31 kHz");
	check(!py2_parse("[Game]\nName=x\n", 14, "", d), "no HddImagePath, no game");
	check(!py2_parse("HddImagePath=a.chd\n", 19, "", d), "no [Game] section, no game");

	/* The board's configuration from the files the descriptor names. */
	make_payload(pay, 9);
	memset(file, 0x42, 8);
	memcpy(file + 8, pay, 32);
	put_file("p2io_black.bin", file, 40);
	put_file("p2io_card.txt", "0123456789abcdef\n", 17);
	{
		static const char t[] = "[Game]\nHddImagePath=x\nInputType=1\nDongleBlackPath=p2io_black.bin\n"
								"DongleWhitePath=p2io_none.bin\nPlayer1Card=p2io_card.txt\n";
		py2_parse(t, strlen(t), "", d);
	}
	missing = py2_make_config(d, &cfg);
	check(cfg.game == 1 && cfg.dongle_loaded[0] && cfg.dongle[0][0] == 0x42 && !memcmp(cfg.dongle[0] + 8, pay, 32),
		"the black dongle is read and checked");
	check(cfg.card_loaded[0] && cfg.card[0][0] == 0x01 && cfg.card[0][7] == 0xef && !cfg.card_loaded[1], "player 1's card");
	check(missing == 2, "the white dongle's file is reported missing");
	remove("p2io_black.bin");
	remove("p2io_card.txt");
	free(d);
}

int main(void)
{
	printf("== P2IO ==\n");
	test_framing();
	test_settings();
	test_dongles();
	test_jamma();
	test_acio();
	test_thrilldrive();
	test_drumpad();
	test_dance864();
	test_state();
	test_cards();
	test_desc();
	printf(s_bad ? "FAIL (%d)\n" : "PASS\n", s_bad);
	return s_bad ? 1 : 0;
}
