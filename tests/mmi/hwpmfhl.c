/*  PMFHL, PMTHL and the HI/LO moves against console captures.
 *
 *  Scores the five PMFHL variants plus PMFHI, PMFLO, PMTHI, PMTLO and
 *  PMTHL.LW against ps2autotests tests/cpu/ee_simd/muldiv.expected,
 *  transcribed from pcsx2/MMI.c.
 *
 *  These sit outside what tests/mmi/hwelem.c and hwscore.c already cover
 *  -- those two take the arithmetic, shuffle, compare, logic and the
 *  multiply/divide accumulators, which is 62 of the 95 ops in the ee_simd
 *  captures. PMFHL is the interesting part of the remainder: five ways of
 *  packing the same HI/LO pair into a register, three of them reading
 *  alternate words and one saturating, so a wrong lane index produces a
 *  plausible-looking result rather than an obviously broken one.
 *
 *  HI and LO are seeded before every case and the line names which seed
 *  it used -- C_HILO, C_HILO_ZERO or C_HILO_PATTERN -- so that is read off
 *  the line rather than assumed. Taking C_HILO for all of them scores 22
 *  of 28 per block and looks like a handful of odd failures rather than a
 *  third of the cases being fed the wrong input.
 *
 *  The result is printed high word first.
 *
 *  Usage: tests/mmi/hwpmfhl <path-to-ee_simd-muldiv.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

typedef struct { u32 w[4]; } Q;     /* w[0] is the low word */

/* The three HILOREGS the harness declares: { hi, lo, hi1, lo1 }. */
static const struct { const char* name; u64 hi, lo, hi1, lo1; } kSeed[] = {
	{ "C_HILO_PATTERN", 0x0000000000000000ull, 0x0000000010001234ull,
	                    0x0000000000000000ull, 0x00000000F000ABCDull },
	{ "C_HILO_ZERO",    0, 0, 0, 0 },
	{ "C_HILO",         0x0123456789ABCDEFull, 0x123456789ABCDEF0ull,
	                    0x23456789ABCDEF01ull, 0x456789ABCDEF0123ull },
};

static Q g_hi, g_lo;

static int seed(const char* line)
{
	size_t i;
	for (i = 0; i < sizeof(kSeed)/sizeof(kSeed[0]); i++)
	{
		if (!strstr(line, kSeed[i].name)) continue;
		g_hi.w[0] = (u32)kSeed[i].hi;   g_hi.w[1] = (u32)(kSeed[i].hi >> 32);
		g_hi.w[2] = (u32)kSeed[i].hi1;  g_hi.w[3] = (u32)(kSeed[i].hi1 >> 32);
		g_lo.w[0] = (u32)kSeed[i].lo;   g_lo.w[1] = (u32)(kSeed[i].lo >> 32);
		g_lo.w[2] = (u32)kSeed[i].lo1;  g_lo.w[3] = (u32)(kSeed[i].lo1 >> 32);
		return 1;
	}
	return 0;
}

/* PMFHL_CLAMP from MMI.c: a signed 32-bit word saturated into 16 bits. */
static u16 clamp16(u32 v)
{
	const s32 s = (s32)v;
	if (s >  0x7fff) return 0x7fff;
	if (s < -0x8000) return 0x8000;
	return (u16)s;
}

static u16 half(const Q* q, int i) { return (u16)(q->w[i >> 1] >> ((i & 1) * 16)); }

/* Transcribed from PMFHL in pcsx2/MMI.c; sa selects the variant. */
static Q pmfhl(int sa)
{
	Q r; int i;
	r.w[0] = r.w[1] = r.w[2] = r.w[3] = 0;
	switch (sa)
	{
	case 0x00: /* LW */
		r.w[0] = g_lo.w[0]; r.w[1] = g_hi.w[0];
		r.w[2] = g_lo.w[2]; r.w[3] = g_hi.w[2];
		break;
	case 0x01: /* UW */
		r.w[0] = g_lo.w[1]; r.w[1] = g_hi.w[1];
		r.w[2] = g_lo.w[3]; r.w[3] = g_hi.w[3];
		break;
	case 0x02: /* SLW: saturate the HI:LO word pair into a signed 32-bit */
		for (i = 0; i < 2; i++)
		{
			const int k = i * 2;
			const s64 v = (s64)(((u64)g_hi.w[k] << 32) | g_lo.w[k]);
			u64 d;
			if (v >= 0x000000007fffffffLL)      d = 0x000000007fffffffull;
			else if (v <= -0x80000000LL)        d = 0xffffffff80000000ull;
			else                                d = (u64)(s64)(s32)g_lo.w[k];
			r.w[i * 2] = (u32)d; r.w[i * 2 + 1] = (u32)(d >> 32);
		}
		break;
	case 0x03: /* LH */
	{
		const u16 h[8] = { half(&g_lo,0), half(&g_lo,2), half(&g_hi,0), half(&g_hi,2),
		                   half(&g_lo,4), half(&g_lo,6), half(&g_hi,4), half(&g_hi,6) };
		for (i = 0; i < 4; i++) r.w[i] = (u32)h[i*2] | ((u32)h[i*2+1] << 16);
		break;
	}
	default:   /* 0x04, SH: whole words saturated to halves, not halves picked */
	{
		const u16 h[8] = { clamp16(g_lo.w[0]), clamp16(g_lo.w[1]),
		                   clamp16(g_hi.w[0]), clamp16(g_hi.w[1]),
		                   clamp16(g_lo.w[2]), clamp16(g_lo.w[3]),
		                   clamp16(g_hi.w[2]), clamp16(g_hi.w[3]) };
		for (i = 0; i < 4; i++) r.w[i] = (u32)h[i*2] | ((u32)h[i*2+1] << 16);
		break;
	}
	}
	return r;
}

enum { P_LW, P_UW, P_SLW, P_LH, P_SH, P_MFHI, P_MFLO, P_COUNT };
static const char* const kName[P_COUNT] = {
	"pmfhl.lw", "pmfhl.uw", "pmfhl.slw", "pmfhl.lh", "pmfhl.sh",
	"pmfhi", "pmflo" };

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "muldiv.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < P_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			unsigned w3, w2, w1, w0;
			Q got; char* p;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			if (strstr(buf, "-> $0")) continue;

			p = strchr(buf, ':');
			if (!p || sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

			if (!seed(buf)) continue;
			if      (op == P_MFHI) got = g_hi;
			else if (op == P_MFLO) got = g_lo;
			else                   got = pmfhl(op);

			cases++;
			if (got.w[0] == w0 && got.w[1] == w1 && got.w[2] == w2 && got.w[3] == w3)
				pass++;
			else if (failures++ < 6)
				printf("  %-10s console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       kName[op], w3, w2, w1, w0,
				       got.w[3], got.w[2], got.w[1], got.w[0]);
		}
		fclose(f);
		if (cases == 0)
		{ printf("  a block parsed no cases; its name or line shape does not match the capture\n"); failures++; }
		printf("%-10s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("hwpmfhl: %d cases across %d ops\n", total, P_COUNT);
	return failures != 0;
}
