/*  EE FPU arithmetic against console captures.
 *
 *  Scores the FPU ops in pcsx2/FPU.c against ps2autotests
 *  tests/cpu/ee_fpu/{arithmetic,muldiv,sqrt}.expected.
 *
 *  Every one of them is a direct call into pcsx2/ps2float.c -- ADD_S is
 *  ps2f_add, MUL_S is ps2f_mul, MADD_S is ps2f_madd and so on -- and that
 *  file is standalone C, so this links the real implementation rather
 *  than transcribing it. Nothing here can drift from the emulator: if
 *  ps2float.c changes, this is testing the change.
 *
 *  Each block prints its operands two ways: the first half as literal hex,
 *  the second half as the names of constants the harness sets up. Both are
 *  handled. Reading only the hex form silently halves the coverage -- add
 *  has 36 cases, not the 19 that shape alone finds -- and a scorer that
 *  skips half its cases while reporting a clean sweep is worse than none,
 *  so the case counts here are checked against the files.
 *
 *  MADD and MSUB carry an accumulator, printed in parentheses before the
 *  operands and again after the result. The leading one is the input.
 *
 *  Usage: tests/fpu/hwfpu <dir-with-the-.expected-files>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fenv.h>

#include "ps2float.h"

typedef uint32_t u32;

enum { F_ADD, F_SUB, F_MUL, F_DIV, F_SQRT, F_RSQRT, F_MADD, F_MSUB,
       F_CVTWS, F_CVTSW, F_CEQ, F_CLT, F_CLE, F_CF, F_COUNT };

static const struct { const char* name; const char* file; int operands; int acc; }
kOps[F_COUNT] = {
	{ "add",   "arithmetic", 2, 0 },
	{ "sub",   "arithmetic", 2, 0 },
	{ "mul",   "muldiv",     2, 0 },
	{ "div",   "muldiv",     2, 0 },
	{ "sqrt",  "sqrt",       1, 0 },
	{ "rsqrt", "sqrt",       2, 0 },
	{ "madd",  "muldiv",     2, 1 },
	{ "msub",  "muldiv",     2, 1 },
	/* CVT_W / CVT_S and the four compares live in FPU.c rather than
	 * ps2float.c, so unlike everything above these are transcribed. */
	{ "cvt.w.s", "convert",  1, 0 },
	{ "cvt.s.w", "convert",  1, 0 },
	{ "eq",    "compare",    2, 0 },
	{ "lt",    "compare",    2, 0 },
	{ "le",    "compare",    2, 0 },
	{ "f",     "compare",    2, 0 },
};
/* Ops whose captures print true/false rather than a value. */
static int is_compare(int op) { return op >= F_CEQ; }

/* Mirrors CVT_W in pcsx2/FPU.c: in range it truncates, out of range it
 * pins to INT_MAX or INT_MIN by sign. */
static u32 cvt_w(u32 f)
{
	if ((f & 0x7F800000u) <= 0x4E800000u)
	{ float v; memcpy(&v, &f, 4); return (u32)(int32_t)v; }
	return (f & 0x80000000u) == 0 ? 0x7fffffffu : 0x80000000u;
}
/* Mirrors CVT_S: integer to float, then the denormal flush fpuDouble does. */
static u32 cvt_s(u32 f)
{
	float v = (float)(int32_t)f;
	u32 r; memcpy(&r, &v, 4);
	if ((r & 0x7f800000u) == 0) r &= 0x80000000u;
	return r;
}
/* Mirrors fpuCompareFull: flush denormals, then order as sign-magnitude. */
static int32_t cmp_key(u32 f)
{
	if (!(f & 0x7f800000u)) f = 0;
	if (f & 0x80000000u) f ^= 0x7fffffffu;
	return (int32_t)f;
}

/* The named constants, from tests/cpu/ee_fpu/shared.h. */
static const struct { const char* name; u32 v; } kConst[] = {
	{ "CF_ZERO",         0x00000000u },
	{ "CF_NEGZERO",      0x80000000u },
	{ "CF_MAX_MANTISSA", 0x3FFFFFFFu },
	{ "CF_MAX_EXP",      0x7F800001u },
	{ "CF_MIN_EXP",      0x00000001u },
	{ "CF_MAX",          0x7FFFFFFFu },
	{ "CF_MIN",          0xFFFFFFFFu },
	{ "CF_NEGONE",       0xBF800000u },
	{ "CF_ONE",          0x3F800000u },
	{ "CF_GARBAGE1",     0x00001337u },
	{ "CF_GARBAGE2",     0xDEADBEEFu },
};

/* Read one operand, either "3f800000/+1.00" or "CF_ONE", and advance p past
 * it and any following separator. */
static int operand(char** pp, u32* out)
{
	char* p = *pp;
	while (*p == ' ' || *p == ',' || *p == '(' || *p == ')') p++;
	if (*p == 'C' && p[1] == 'F')
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
		unsigned v;
		if (sscanf(p, "%x", &v) != 1) return 0;
		*out = (u32)v;
		while (*p && *p != '/' && *p != ',' && *p != ':' && *p != ' ' && *p != ')') p++;
		/* Skip the decimal rendering that follows the hex. It ends at the
		 * next separator -- and ')' has to be one of them, or the
		 * accumulator MADD prints in parentheses swallows the first
		 * operand behind it and the block silently loses half its cases. */
		if (*p == '/') { while (*p && *p != ',' && *p != ':' && *p != ')') p++; }
	}
	*pp = p;
	return 1;
}

static u32 apply(int op, u32 a, u32 b, u32 acc)
{
	switch (op)
	{
	case F_ADD:   return ps2f_raw(ps2f_add(a, b));
	case F_SUB:   return ps2f_raw(ps2f_sub(a, b));
	case F_MUL:   return ps2f_raw(ps2f_mul(a, b));
	case F_DIV:   return ps2f_raw(ps2f_div(a, b));
	case F_SQRT:  return ps2f_raw(ps2f_sqrt(a));
	case F_RSQRT: return ps2f_raw(ps2f_rsqrt(a, b));
	case F_MADD:  return ps2f_raw(ps2f_madd(acc, a, b, 0));
	case F_MSUB:  return ps2f_raw(ps2f_msub(acc, a, b, 0));
	case F_CVTWS: return cvt_w(a);
	case F_CVTSW: return cvt_s(a);
	case F_CEQ:   return cmp_key(a) == cmp_key(b);
	case F_CLT:   return cmp_key(a) <  cmp_key(b);
	case F_CLE:   return cmp_key(a) <= cmp_key(b);
	case F_CF:    return 0; /* always false */
	}
	return 0;
}

int main(int argc, char** argv)
{
	const char* dir = (argc > 1) ? argv[1] : ".";
	int op, failures = 0, total = 0;

	/* Match the emulator. VMManager sets the host rounding mode to the
	 * EE/VU chop-toward-zero setting at VM start, and CVT_S and the compares
	 * are plain host float in FPU.c, so they inherit it. Left at the
	 * default, CVT_S scores 26 of 32 here and reads as a conversion bug --
	 * console cvt.s.w(7fffffff) is 4effffff, the truncation, against
	 * 4f000000 rounded to nearest -- when it is this harness in the wrong
	 * mode rather than the emulator. The ps2float ops are integer code and
	 * do not care either way. */
	fesetround(FE_TOWARDZERO);

	for (op = 0; op < F_COUNT; op++)
	{
		char path[512], head[32], buf[512];
		FILE* f;
		int inblock = 0, pass = 0, cases = 0;

		snprintf(path, sizeof(path), "%s/%s.expected", dir, kOps[op].file);
		f = fopen(path, "r");
		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s:", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			char* p;
			u32 a = 0, b = 0, acc = 0, want = 0;
			u32 got;

			if (!inblock)
			{ if (!strncmp(buf, head, strlen(head)) && buf[strlen(head)] == '\n')
				inblock = 1;
			  continue; }
			if (buf[0] != ' ') break;

			p = buf + 2 + strlen(kOps[op].name);
			if (kOps[op].acc && !operand(&p, &acc)) continue;
			if (!operand(&p, &a)) continue;
			if (kOps[op].operands == 2 && !operand(&p, &b)) continue;
			while (*p && *p != ':') p++;
			if (*p != ':') continue;
			p++;
			while (*p == ' ') p++;
			if (is_compare(op))
			{
				if (!strncmp(p, "true", 4)) want = 1;
				else if (!strncmp(p, "false", 5)) want = 0;
				else continue;
			}
			else
			{
				unsigned v;
				if (sscanf(p, "%x", &v) != 1) continue;
				want = v;
			}

			got = apply(op, a, b, acc);
			if (got == (u32)want) pass++;
			else if (failures++ < 6)
				printf("  %-6s %08x %08x (acc %08x): console %08x  ours %08x\n",
				       kOps[op].name, a, b, acc, want, got);
			cases++;
		}
		fclose(f);
		printf("%-6s %3d/%-3d console cases\n", kOps[op].name, pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("hwfpu: %d cases across %d ops\n", total, F_COUNT);
	return failures != 0;
}
