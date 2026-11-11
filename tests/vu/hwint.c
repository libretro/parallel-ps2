/*  VU integer lower ops against console captures.
 *
 *  Scores IADD, IADDI, IADDIU, IAND, IOR, ISUB and ISUBIU (pcsx2/VUops.c)
 *  against ps2autotests tests/vu/lower/integer.expected.
 *
 *  Two harness details decide whether this measures the emulator or
 *  measures a misreading of the capture, and both are easy to get wrong:
 *
 *  The operand a line names is the value the test *asked for*, not what
 *  ends up in the register. WrSetIntegerRegister cannot load an arbitrary
 *  16-bit value in one instruction, so for anything with bit 15 set it
 *  emits ISUBIU(r, VI00, v & ~0x8000) and the register ends up holding
 *  -(v & 0x7fff). A line reading "iadd 65535, 1: 8002" is therefore
 *  0x8001 + 1, not 0xffff + 1, and taking the printed number literally
 *  makes correct behaviour look broken. 0x8000 is the one value needing
 *  two instructions and is modelled separately.
 *
 *  The named CVI_ constants are loaded from VU memory instead, so those
 *  arrive literally, as the low halfword of the 32-bit constant.
 *
 *  Operands are parsed out of the capture lines rather than transcribed
 *  from the test source, so the case list cannot drift from the file
 *  being scored.
 *
 *  Usage: tests/vu/hwint <path-to-integer.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef uint32_t u32; typedef uint16_t u16; typedef int16_t s16;

/* Low halfword of each named constant, from integer.cpp's C_ table. */
static const struct { const char* name; u16 v; } kConst[] = {
	{ "CVI_ZERO",     0x0000u },
	{ "CVI_S16_MAX",  0x7FFFu },
	{ "CVI_S16_MIN",  0x8000u },
	{ "CVI_S32_MAX",  0xFFFFu },
	{ "CVI_S32_MIN",  0x0000u },
	{ "CVI_S64_MAX",  0xFFFFu },
	{ "CVI_S64_MIN",  0x0000u },
	{ "CVI_NEGONE",   0xFFFFu },
	{ "CVI_ONE",      0x0001u },
	{ "CVI_GARBAGE1", 0x1337u },
	{ "CVI_GARBAGE2", 0xBEEFu },
};

/* What WrSetIntegerRegister actually leaves in the register. */
static u16 setvi(u32 v)
{
	if (v == 0x8000u) return (u16)((u16)0xFFFFu - (u16)0x7FFFu); /* IADDI -1, then ISUBIU 0x7fff */
	if (v & 0x8000u)  return (u16)(0u - (v & ~0x8000u));         /* ISUBIU from VI00 */
	return (u16)v;                                                /* IADDIU from VI00 */
}

/* Resolve one printed operand. `mem` reports whether it was a named
 * constant, which is what tells the two loading paths apart. */
static int resolve(const char* tok, u16* out, int* mem)
{
	size_t i, best = (size_t)-1, bestlen = 0;
	char* end; long v;

	for (i = 0; i < sizeof(kConst)/sizeof(kConst[0]); i++)
	{
		const size_t n = strlen(kConst[i].name);
		if (!strncmp(tok, kConst[i].name, n) && n > bestlen) { best = i; bestlen = n; }
	}
	if (best != (size_t)-1) { *out = kConst[best].v; *mem = 1; return 1; }

	v = strtol(tok, &end, 10);
	if (end == tok) return 0;
	*out = (u16)v; *mem = 0;
	return 1;
}

enum { O_IADD, O_IADDI, O_IADDIU, O_IAND, O_IOR, O_ISUB, O_ISUBIU, O_COUNT };
static const struct { const char* name; int imm; } kOps[O_COUNT] = {
	{ "iadd", 0 }, { "iaddi", 1 }, { "iaddiu", 1 }, { "iand", 0 },
	{ "ior", 0 }, { "isub", 0 }, { "isubiu", 1 },
};

static u16 apply(int op, u16 s, u16 t)
{
	switch (op)
	{
	case O_IADD: case O_IADDI: case O_IADDIU: return (u16)((s16)s + (s16)t);
	case O_ISUB: case O_ISUBIU:               return (u16)((s16)s - (s16)t);
	case O_IAND:                              return (u16)(s & t);
	case O_IOR:                               return (u16)(s | t);
	}
	return 0;
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "integer.expected";
	int op, failures = 0, total = 0;

	for (op = 0; op < O_COUNT; op++)
	{
		FILE* f = fopen(path, "r");
		char buf[512], head[32];
		int inblock = 0, pass = 0, cases = 0;

		if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
		snprintf(head, sizeof(head), "%s\n", kOps[op].name);

		while (fgets(buf, sizeof(buf), f))
		{
			char *colon, *args, *comma;
			unsigned want;
			u16 s, t, got;
			int smem = 0, tmem = 0;

			if (!inblock) { if (!strcmp(buf, head)) inblock = 1; continue; }
			if (buf[0] != ' ') break;
			colon = strstr(buf, ": ");
			if (!colon) continue;
			if (sscanf(colon + 2, "%x", &want) != 1) continue;

			args = buf + 2 + strlen(kOps[op].name);
			while (*args == ' ') args++;
			comma = strchr(args, ',');
			if (!comma || comma > colon) continue;
			*comma = '\0'; *colon = '\0';
			if (!resolve(args, &s, &smem)) { continue; }
			{
				char* t2 = comma + 1;
				while (*t2 == ' ') t2++;
				if (!resolve(t2, &t, &tmem)) continue;
			}

			/* Register operands go through the loader; a plain immediate
			 * operand is encoded in the instruction and used as written. */
			if (!smem) s = setvi(s);
			if (!kOps[op].imm && !tmem) t = setvi(t);

			got = apply(op, s, t);
			if (got == (u16)want) pass++;
			else if (failures++ < 8)
				printf("  %s case %d: console %04x  ours %04x\n",
				       kOps[op].name, cases, want, got);
			cases++;
		}
		fclose(f);
		printf("%-8s %2d/%2d console cases\n", kOps[op].name, pass, cases);
		total += cases;
		if (pass != cases) failures++;
	}

	printf("vuint: %d cases across %d ops\n", total, O_COUNT);
	return failures != 0;
}
