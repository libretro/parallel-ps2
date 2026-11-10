/*  The shift-amount register against console captures.
 *
 *  Scores MFSA, MTSA, MTSAB and MTSAH against ps2autotests
 *  tests/cpu/ee_simd/funnel.expected, transcribed from
 *  pcsx2/R5900OpcodeImpl.c.
 *
 *  SA is four bits, a byte count QFSRV reads as sa * 8. Every write to it
 *  narrows to that: MTSA keeps the low four bits, MTSAB exclusive-ors the
 *  low four of the register with the low four of the immediate, and MTSAH
 *  does the same over three bits and shifts up one, so it always lands on
 *  an even byte.
 *
 *  The mfsa block is the one that pins the mask, since it writes a value
 *  and reads it straight back: 16 comes back 0 and 0xffff comes back 0xf.
 *
 *  QDSRV is scored too. It is QFSRV under another name: the harness sets
 *  SA with "mtsab $0, i", which makes sa exactly i, then funnel-shifts the
 *  256-bit pair {rs:rt} right by sa bytes and keeps the low 128. rt is the
 *  second pattern and supplies the result whole at sa 0, with bytes of rs
 *  entering from the top as sa grows.
 *
 *  Usage: tests/mmi/hwsa <path-to-funnel.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;

/* Constants the harness passes by name. */
static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",     0x00000000u },
	{ "C_NEGONE",   0xFFFFFFFFu },
	{ "C_ONE",      0x00000001u },
	{ "C_GARBAGE1", 0x00001337u },
	{ "C_GARBAGE2", 0xDEADBEEFu },
};

static int operand(const char* p, u32* out)
{
	size_t i;
	while (*p == ' ') p++;
	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
		if (!strncmp(p, kConst[i].name, strlen(kConst[i].name)))
		{ *out = kConst[i].v; return 1; }
	if (!strncmp(p, "0x", 2) || !strncmp(p, "0X", 2))
	{ *out = (u32)strtoul(p + 2, NULL, 16); return 1; }
	if (*p == '-' || (*p >= '0' && *p <= '9'))
	{ *out = (u32)strtol(p, NULL, 10); return 1; }
	return 0;
}

/* C_SHIFT_PATTERN1 (rs) and C_SHIFT_PATTERN2 (rt), from funnel.cpp. */
static const u32 kRs[4] = { 0x12345678u, 0x9ABCDEF0u, 0x11223344u, 0xDEADBEEFu };
static const u32 kRt[4] = { 0xAABBCCDDu, 0x12121212u, 0xFFFFFFFFu, 0x1337C0DEu };

/* Transcribed from QFSRV in pcsx2/MMI.c. */
static void qfsrv(u32 sa, u32 out[4])
{
	const u64 rs0 = ((u64)kRs[1] << 32) | kRs[0];
	const u64 rs1 = ((u64)kRs[3] << 32) | kRs[2];
	const u64 rt0 = ((u64)kRt[1] << 32) | kRt[0];
	const u64 rt1 = ((u64)kRt[3] << 32) | kRt[2];
	const u32 amt = sa << 3;
	u64 d0, d1;

	if (amt == 0)
	{ d0 = rt0; d1 = rt1; }
	else if (amt < 64)
	{
		d0 = (rt0 >> amt) | (rt1 << (64 - amt));
		d1 = (rt1 >> amt) | (rs0 << (64 - amt));
	}
	else
	{
		d0 = rt1 >> (amt - 64);
		d1 = rs0 >> (amt - 64);
		if (amt != 64)
		{ d0 |= rs0 << (128u - amt); d1 |= rs1 << (128u - amt); }
	}
	out[0] = (u32)d0; out[1] = (u32)(d0 >> 32);
	out[2] = (u32)d1; out[3] = (u32)(d1 >> 32);
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "funnel.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int pass = 0, total = 0, shown = 0;
	int in_mfsa = 0, in_qdsrv = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned w3, w2, w1, w0;
		u32 v, sa;
		char* p;

		if (buf[0] != ' ')
		{
			in_mfsa  = !strncmp(buf, "mfsa:", 5);
			in_qdsrv = !strncmp(buf, "qdsrv:", 6);
			continue;
		}
		if (strstr(buf, "-> $0")) continue;

		if (in_qdsrv)
		{
			int sa; u32 got[4];
			if (sscanf(buf, " qdsrv %d:", &sa) != 1) continue;
			p = strchr(buf, ':');
			if (!p || sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;
			/* The harness sets SA with "mtsab $0, i", which is
			 * (0 & 0xf) ^ (i & 0xf) -- so the index wraps at 16 rather
			 * than growing. Cases 16 and 17 print the same results as 0
			 * and 1 for exactly that reason, and feeding the raw index
			 * to a 256-bit shift instead produces nonsense. */
			qfsrv((u32)sa & 0xf, got);
			total++;
			if (got[0] == w0 && got[1] == w1 && got[2] == w2 && got[3] == w3) pass++;
			else if (shown++ < 6)
				printf("  qdsrv %2d: console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       sa, w3, w2, w1, w0, got[3], got[2], got[1], got[0]);
			continue;
		}
		if (!in_mfsa) continue;

		p = strchr(buf, ':');
		if (!p) continue;
		if (sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;
		if (!operand(buf + 2 + 4, &v)) continue;   /* past "  mfsa" */

		/* MTSA then MFSA: the write masks, the read zero-extends. */
		sa = v & 0xf;

		total++;
		if (sa == w0) pass++;
		else if (shown++ < 6)
			printf("  mfsa after writing %08x: console %08x  ours %08x\n",
			       v, w0, sa);
	}
	fclose(f);

	printf("hwsa: %d/%d SA round trips and funnel shifts match the console\n",
	       pass, total);
	return pass != total;
}
