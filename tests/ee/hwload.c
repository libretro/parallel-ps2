/*  EE aligned loads against console captures.
 *
 *  Scores LB, LBU, LH, LHU, LW, LWU, LD and LQ against ps2autotests
 *  tests/cpu/ee/lsu.expected, using the semantics in
 *  pcsx2/R5900OpcodeImpl.c.
 *
 *  The harness reads from a fixed 48-byte pattern with the base pointer
 *  at its middle row, so offsets run from -16 to +16 and the memory is
 *  fully known:
 *
 *      -16  45678123 9ABCDEF0 DEADBEEF C0DE1337
 *        0  23456789 ABCDEF01 BEEFDEAD C0DEC0DE     <- base
 *      +16  8899AABB CCDDEEFF 00112233 44556677
 *
 *  rt is the destination and carries between cases the same way rd does
 *  in alu.expected -- SET_U32 writes only its low half, SET_M writes all
 *  128 bits from C_GARBAGE1 -- so this walks the file in order and tracks
 *  it. Everything but LQ writes only the low doubleword, and LQ writes
 *  all four words, which is worth checking rather than masking off: it is
 *  the one op here whose upper half should change.
 *
 *  The unaligned forms are covered too. Their blocks print single ops and
 *  pairs ("lwl +0/lwr +3"), and they merge into rt rather than replacing
 *  it, so the seed has to be right: the first five cases of each block use
 *  SET_U32<0xABCD4321> and the next five SET_M(C_GARBAGE1), which the
 *  block header resets. LWR is the interesting one -- it sign-extends into
 *  the full 64 bits only when the address is word-aligned, and otherwise
 *  leaves rt's upper 32 bits alone, an asymmetry LWL does not have.
 *
 *  Usage: tests/ee/hwload <path-to-lsu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

/* C_PATTERN, three rows of four words, little-endian in memory. */
static const u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
#define BASE 16   /* byte offset of C_PATTERN[1] */

static u8 mem8(int off)
{
	const int i = BASE + off;
	const u8* p = (const u8*)kPattern;
	if (i < 0 || i >= 48) { fprintf(stderr, "offset %d out of pattern\n", off); exit(2); }
	return p[i];
}
static u32 mem16(int off) { return (u32)mem8(off) | ((u32)mem8(off+1) << 8); }
static u32 mem32(int off) { return mem16(off) | (mem16(off+2) << 16); }
static u64 mem64(int off) { return (u64)mem32(off) | ((u64)mem32(off+4) << 32); }

enum { L_LB, L_LBU, L_LH, L_LHU, L_LW, L_LWU, L_LD, L_LQ,
       L_LWLR, L_LDLR, L_COUNT };
static const char* const kName[L_COUNT] =
	{ "lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "lq",
	  "lwl / lwr", "ldl / ldr" };

/* Transcribed from R5900OpcodeImpl.c, masks and all. */
static u64 do_lwl(u64 rt, int off)
{
	static const u32 mask[4]  = { 0xffffffu, 0x0000ffffu, 0x000000ffu, 0u };
	static const u8  shift[4] = { 24, 16, 8, 0 };
	const u32 sh = (u32)(off & 3);
	const u32 mem = mem32(off & ~3);
	return (u64)(s64)(s32)(((u32)rt & mask[sh]) | (mem << shift[sh]));
}
static u64 do_lwr(u64 rt, int off)
{
	static const u32 mask[4]  = { 0u, 0xff000000u, 0xffff0000u, 0xffffff00u };
	static const u8  shift[4] = { 0, 8, 16, 24 };
	const u32 sh = (u32)(off & 3);
	const u32 mem = ((u32)rt & mask[sh]) | (mem32(off & ~3) >> shift[sh]);
	/* Only the aligned case reaches into the upper 32 bits. */
	if (sh == 0) return (u64)(s64)(s32)mem;
	return (rt & 0xffffffff00000000ull) | mem;
}
static u64 do_ldl(u64 rt, int off)
{
	static const u64 mask[8] = {
		0x00ffffffffffffffull, 0x0000ffffffffffffull, 0x000000ffffffffffull,
		0x00000000ffffffffull, 0x0000000000ffffffull, 0x000000000000ffffull,
		0x00000000000000ffull, 0x0000000000000000ull };
	static const u8 shift[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	const u32 sh = (u32)(off & 7);
	return (rt & mask[sh]) | (mem64(off & ~7) << shift[sh]);
}
static u64 do_ldr(u64 rt, int off)
{
	static const u64 mask[8] = {
		0x0000000000000000ull, 0xff00000000000000ull, 0xffff000000000000ull,
		0xffffff0000000000ull, 0xffffffff00000000ull, 0xffffffffff000000ull,
		0xffffffffffff0000ull, 0xffffffffffffff00ull };
	static const u8 shift[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	const u32 sh = (u32)(off & 7);
	return (rt & mask[sh]) | (mem64(off & ~7) >> shift[sh]);
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "lsu.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int op = -1, failures = 0, total = 0, i;
	int pass[L_COUNT], cases[L_COUNT];
	/* rt across the file: only the low half is rewritten per case, so the
	 * upper one carries. It already holds C_GARBAGE1's upper doubleword at
	 * the very first case, before any named-constant case has run -- rt is
	 * declared uninitialised in the test function and inherits whatever the
	 * previous one left in that register. That starting value is taken from
	 * the capture rather than derived, which is worth saying: it is the one
	 * number here that is observed rather than reasoned. */
	u64 rt_hi = 0x0000133A00001339ull;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	for (i = 0; i < L_COUNT; i++) { pass[i] = 0; cases[i] = 0; }

	while (fgets(buf, sizeof(buf), f))
	{
		int off;
		unsigned w3, w2, w1, w0;
		u64 want_lo, want_hi, got_lo, got_hi;
		char* p;

		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			op = -1;
			if (c && c[1] == '\n')
			{
				*c = '\0';
				for (i = 0; i < L_COUNT; i++)
					if (!strcmp(buf, kName[i])) { op = i; break; }
				*c = ':';
			}
			continue;
		}

		/* Each block ends with a "-> $0" line checking a zero destination
		 * discards the load. It carries no offset and is not a case; the
		 * parse below would drop it anyway, but silently. */
		if (strstr(buf, "-> $0")) continue;

		if (op == L_LWLR || op == L_LDLR)
		{
			/* "lwl +0:" or "lwl +0/lwr +3:" -- one or two ops, applied in
			 * order to the seeded rt. Case index decides the seed: the
			 * first five use SET_U32<0xABCD4321>, the next five
			 * SET_M(C_GARBAGE1). */
			char op1[8], op2[8];
			int off1, off2, n;
			u64 rt;

			n = sscanf(buf, " %7[a-z] %d/%7[a-z] %d:", op1, &off1, op2, &off2);
			if (n != 2 && n != 4) continue;

			if (cases[op] < 5)
			{ rt = (u64)(s64)(s32)0xABCD4321u; got_hi = 0x0000133A00001339ull; }
			else
			{ rt = 0x0000133800001337ull; got_hi = 0x0000133A00001339ull; }

			p = strchr(buf, ':');
			if (!p) continue;
			if (sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;
			want_lo = ((u64)w1 << 32) | w0;
			want_hi = ((u64)w3 << 32) | w2;

			if      (!strcmp(op1, "lwl")) rt = do_lwl(rt, off1);
			else if (!strcmp(op1, "lwr")) rt = do_lwr(rt, off1);
			else if (!strcmp(op1, "ldl")) rt = do_ldl(rt, off1);
			else if (!strcmp(op1, "ldr")) rt = do_ldr(rt, off1);
			if (n == 4)
			{
				if      (!strcmp(op2, "lwl")) rt = do_lwl(rt, off2);
				else if (!strcmp(op2, "lwr")) rt = do_lwr(rt, off2);
				else if (!strcmp(op2, "ldl")) rt = do_ldl(rt, off2);
				else if (!strcmp(op2, "ldr")) rt = do_ldr(rt, off2);
			}

			cases[op]++;
			if (rt == want_lo && got_hi == want_hi) pass[op]++;
			else if (failures++ < 6)
				printf("  %-9s %+d: console %016llx:%016llx  ours %016llx:%016llx\n",
				       kName[op], off1, (unsigned long long)want_hi,
				       (unsigned long long)want_lo,
				       (unsigned long long)got_hi, (unsigned long long)rt);
			continue;
		}

		p = buf + 2;
		while (*p && *p != ' ') p++;
		/* "+0", "-16" and also "+-16" appear; strtol handles the first two
		 * and the third needs the leading '+' skipped. */
		while (*p == ' ') p++;
		if (*p == '+') p++;
		off = (int)strtol(p, &p, 10);
		while (*p == ' ' || *p == ':') p++;
		if (sscanf(p, "%x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

		want_lo = ((u64)w1 << 32) | w0;
		want_hi = ((u64)w3 << 32) | w2;

		if (op < 0) { rt_hi = want_hi; continue; }

		switch (op)
		{
		case L_LB:  got_lo = (u64)(s64)(s8)mem8(off);          got_hi = rt_hi; break;
		case L_LBU: got_lo = (u64)mem8(off);                   got_hi = rt_hi; break;
		case L_LH:  got_lo = (u64)(s64)(s16)mem16(off);        got_hi = rt_hi; break;
		case L_LHU: got_lo = (u64)(u32)mem16(off);             got_hi = rt_hi; break;
		case L_LW:  got_lo = (u64)(s64)(s32)mem32(off);        got_hi = rt_hi; break;
		case L_LWU: got_lo = (u64)mem32(off);                  got_hi = rt_hi; break;
		case L_LD:  got_lo = mem64(off);                       got_hi = rt_hi; break;
		default:    /* LQ writes the whole quadword, address forced 16-aligned */
			got_lo = mem64(off & ~15);
			got_hi = mem64((off & ~15) + 8);
			break;
		}

		cases[op]++;
		if (got_lo == want_lo && got_hi == want_hi) pass[op]++;
		else if (failures++ < 6)
			printf("  %-4s %+d: console %016llx:%016llx  ours %016llx:%016llx\n",
			       kName[op], off, (unsigned long long)want_hi,
			       (unsigned long long)want_lo,
			       (unsigned long long)got_hi, (unsigned long long)got_lo);
		rt_hi = want_hi;
	}
	fclose(f);

	for (i = 0; i < L_COUNT; i++)
	{
		printf("%-4s %2d/%-3d console cases\n", kName[i], pass[i], cases[i]);
		total += cases[i];
		if (pass[i] != cases[i]) failures++;
	}
	printf("hwload: %d cases across %d ops\n", total, L_COUNT);
	return failures != 0;
}
