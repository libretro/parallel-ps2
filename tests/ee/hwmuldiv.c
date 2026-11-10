/*  EE integer multiply and divide against console captures.
 *
 *  Scores DIV, DIVU, MULT, MULTU and their pipeline-1 variants, plus the
 *  MADD family and the HI/LO moves, against ps2autotests
 *  tests/cpu/ee/muldiv.expected. Transcribed from
 *  pcsx2/R5900OpcodeImpl.c.
 *
 *  Operand loading is simpler than the VU and MMI harnesses: SET_U32 here
 *  is a bare lui/ori with no pcpyld and no ISUBIU trick, so the decimal a
 *  line prints is the register's low word as written, signed. The line
 *  shapes are not uniform though, and both traps cost coverage silently:
 *  each block prints its operands as literal decimals first and as the
 *  names of constants afterwards, and MULT and MULTU write an rd as well,
 *  printed as four words between the operands and the HI/LO pair. Reading
 *  only the decimal form finds 15 of the 26 DIV cases; missing the rd
 *  finds none of the 25 MULT ones. The case counts are checked against
 *  the blocks for that reason.
 *
 *  HI and LO are seeded from C_HILO before every case and printed as two
 *  doublewords each; these ops leave the second of each untouched, which
 *  is checked rather than ignored, since writing the wrong lane is
 *  exactly the mistake that produces correct-looking results.
 *
 *  Usage: tests/ee/hwmuldiv <path-to-muldiv.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

/* C_HILO, the seed the harness restores before each case. */
#define HI0 0x0123456789ABCDEFull
#define LO0 0x123456789ABCDEF0ull
#define HI1 0x23456789ABCDEF01ull
#define LO1 0x456789ABCDEF0123ull

typedef struct { u64 hi, lo, hi1, lo1; } HILO;

/* The named constants, from tests/cpu/ee/shared.h. Only the low word is
 * needed: these ops read SL[0]/UL[0]. */
static const struct { const char* name; u32 v; } kConst[] = {
	{ "C_ZERO",     0x00000000u }, { "C_S16_MAX",  0x00007FFFu },
	{ "C_S16_MIN",  0xFFFF8000u }, { "C_S32_MAX",  0x7FFFFFFFu },
	{ "C_S32_MIN",  0x80000000u }, { "C_S64_MAX",  0xFFFFFFFFu },
	{ "C_S64_MIN",  0x00000000u }, { "C_NEGONE",   0xFFFFFFFFu },
	{ "C_ONE",      0x00000001u }, { "C_GARBAGE1", 0x00001337u },
	{ "C_GARBAGE2", 0xDEADBEEFu },
	{ "C_TWOTWOTWOTWO",  0x00000002u },
	{ "C_ONEONEONEONE",  0x00000001u },
};

/* Read one operand: a signed decimal, or a constant name. */
static int operand(char** pp, u32* out)
{
	char* p = *pp;
	while (*p == ' ' || *p == ',') p++;
	if (*p == 'C' && p[1] == '_')
	{
		size_t i, best = (size_t)-1, bestlen = 0;
		for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
		{
			const size_t n = strlen(kConst[i].name);
			if (!strncmp(p, kConst[i].name, n) && n > bestlen)
			{ best = i; bestlen = n; }
		}
		if (best == (size_t)-1) return 0;
		*out = kConst[best].v;
		p += bestlen;
	}
	else
	{
		long v; char* e;
		v = strtol(p, &e, 10);
		if (e == p) return 0;
		*out = (u32)v;
		p = e;
	}
	*pp = p;
	return 1;
}

enum { E_DIV, E_DIVU, E_MULT, E_MULTU, E_COUNT };
static const char* const kName[E_COUNT] = { "div", "divu", "mult", "multu" };

/* Transcribed from R5900OpcodeImpl.c. Only lane 0 is written; the
 * pipeline-1 forms are the same arithmetic against lane 1 and are covered
 * by their own blocks in the capture. */
static void apply(int op, u32 rs, u32 rt, HILO* h)
{
	switch (op)
	{
	case E_DIV:
		if (rs == 0x80000000u && rt == 0xffffffffu)
		{ h->lo = (u64)(s64)(s32)0x80000000; h->hi = 0; }
		else if ((s32)rt != 0)
		{ h->lo = (u64)(s64)((s32)rs / (s32)rt);
		  h->hi = (u64)(s64)((s32)rs % (s32)rt); }
		else
		{ h->lo = (u64)(s64)(((s32)rs < 0) ? 1 : -1);
		  h->hi = (u64)(s64)(s32)rs; }
		break;
	case E_DIVU:
		if (rt != 0)
		{ h->lo = (u64)(s64)(s32)(rs / rt);
		  h->hi = (u64)(s64)(s32)(rs % rt); }
		else
		{ h->lo = (u64)(s64)-1; h->hi = (u64)(s64)(s32)rs; }
		break;
	case E_MULT:
	{
		const s64 res = (s64)(s32)rs * (s64)(s32)rt;
		h->lo = (u64)(s64)(s32)(res & 0xffffffffu);
		h->hi = (u64)(s64)(s32)(res >> 32);
		break;
	}
	case E_MULTU:
	{
		const u64 res = (u64)rs * (u64)rt;
		h->lo = (u64)(s64)(s32)(u32)(res & 0xffffffffu);
		h->hi = (u64)(s64)(s32)(u32)(res >> 32);
		break;
	}
	}
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "muldiv.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < E_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char head[32], buf[512];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kName[op]);

		while (fgets(buf, sizeof(buf), f))
		{
			u32 a, b;
			unsigned long long wh, wh1, wl, wl1;
			unsigned rd0 = 0, rd1 = 0, rd2 = 0, rd3 = 0;
			HILO h;
			char* p;
			int writes_rd = (op == E_MULT || op == E_MULTU);

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;
			/* The trailing "-> $0" line checks that a zero destination is
			 * discarded; it carries no operands and is not a case. */
			if (strstr(buf, "-> $0")) continue;

			p = buf + 2 + strlen(kName[op]);
			if (!operand(&p, &a)) continue;
			if (!operand(&p, &b)) continue;
			while (*p == ' ' || *p == ':') p++;
			if (writes_rd)
			{
				if (sscanf(p, "%x %x %x %x", &rd3, &rd2, &rd1, &rd0) != 4) continue;
				while (*p && strncmp(p, "H:", 2)) p++;
			}
			if (sscanf(p, "H: %llx %llx L: %llx %llx", &wh, &wh1, &wl, &wl1) != 4)
				continue;

			h.hi = HI0; h.lo = LO0; h.hi1 = HI1; h.lo1 = LO1;
			apply(op, a, b, &h);

			cases++;
			{
				/* MULT and MULTU also write rd = LO, printed high word
				 * first. rd is 0x1337.. garbage when the op leaves it. */
				const int hilo_ok = (h.hi == wh && h.lo == wl
				                  && h.hi1 == wh1 && h.lo1 == wl1);
				const u64 rd = ((u64)rd1 << 32) | rd0;
				const int rd_ok = !writes_rd || rd == h.lo;
				if (hilo_ok && rd_ok) pass++;
				else if (failures++ < 6)
					printf("  %-6s %08x, %08x:\n"
					       "    console H %016llx %016llx L %016llx %016llx\n"
					       "    ours    H %016llx %016llx L %016llx %016llx\n",
					       kName[op], a, b, wh, wh1, wl, wl1,
					       (unsigned long long)h.hi, (unsigned long long)h.hi1,
					       (unsigned long long)h.lo, (unsigned long long)h.lo1);
			}
		}
		fclose(f);
		printf("%-6s %2d/%-3d console cases\n", kName[op], pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("hwmuldiv: %d cases across %d ops\n", total, E_COUNT);
	return failures != 0;
}
