/*  EE stores against console captures.
 *
 *  Scores SB, SH, SW, SD, SQ and the unaligned SWL/SWR and SDL/SDR pairs
 *  against ps2autotests tests/cpu/ee/lsu.expected, using the semantics in
 *  pcsx2/R5900OpcodeImpl.c.
 *
 *  These are checked the other way round from the loads: the harness
 *  copies C_PATTERN into a buffer, stores into it at base + offset, and
 *  prints the sixteen-byte row the store landed in -- row 1 + offset/16,
 *  with C's truncation toward zero putting -16 in row 0. So the result
 *  under test is memory, not a register, and a store that writes the
 *  right bytes to the wrong place shows up as a changed neighbour rather
 *  than as a wrong value.
 *
 *  Every block runs its offsets twice, first with rt seeded from
 *  SET_U32<0xABCD4321> and then from SET_M(C_GARBAGE1) -- the latter sets
 *  all 128 bits, which is what SQ stores. The number of offsets differs
 *  per block though: three for the byte through doubleword stores, four
 *  for SQ, five for the unaligned pairs. So the block is counted first and
 *  the seed switches at its midpoint, rather than assuming a fixed three.
 *
 *  Offsets print as "%+d" and the second half prints through a "+%d"
 *  format, so a negative one arrives as "+-16". That parses to nothing
 *  under %d and silently drops one case per block.
 *
 *  SQ silently aligns its address, which is why it has no unaligned
 *  variants and why its offsets still land on row boundaries.
 *
 *  Usage: tests/ee/hwstore <path-to-lsu.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;

static const u32 kPattern[12] = {
	0x45678123u, 0x9ABCDEF0u, 0xDEADBEEFu, 0xC0DE1337u,
	0x23456789u, 0xABCDEF01u, 0xBEEFDEADu, 0xC0DEC0DEu,
	0x8899AABBu, 0xCCDDEEFFu, 0x00112233u, 0x44556677u,
};
#define BASE 16

/* Working copy, refreshed before each case. */
static u8 mem[48];
static void reset_mem(void) { memcpy(mem, kPattern, sizeof(mem)); }
static int inb(int i) { return i >= 0 && i < 48; }

static u32 rd32(int off)
{
	const int i = BASE + off;
	u32 v = 0; int k;
	for (k = 0; k < 4; k++) if (inb(i+k)) v |= (u32)mem[i+k] << (8*k);
	return v;
}
static u64 rd64(int off)
{
	return (u64)rd32(off) | ((u64)rd32(off+4) << 32);
}
static void wr(int off, u64 val, int bytes)
{
	const int i = BASE + off; int k;
	for (k = 0; k < bytes; k++)
		if (inb(i+k)) mem[i+k] = (u8)(val >> (8*k));
}

enum { S_SB, S_SH, S_SW, S_SD, S_SQ, S_SWLR, S_SDLR, S_COUNT };
static const char* const kName[S_COUNT] =
	{ "sb", "sh", "sw", "sd", "sq", "swl / swr", "sdl / sdr" };

/* "+0", "-16" and "+-16" all appear. */
static int read_off(char** pp, int* out)
{
	char* p = *pp;
	while (*p == ' ') p++;
	if (*p == '+') p++;
	if (*p != '-' && (*p < '0' || *p > '9')) return 0;
	*out = (int)strtol(p, &p, 10);
	*pp = p;
	return 1;
}

/* Case count per block, so the seed can switch at the midpoint. */
static int count_block(const char* path, const char* name)
{
	FILE* f = fopen(path, "r");
	char buf[512];
	int in = 0, n = 0;
	if (!f) return 0;
	while (fgets(buf, sizeof(buf), f))
	{
		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			if (in) break;
			if (c && c[1] == '\n') { *c = '\0'; in = !strcmp(buf, name); *c = ':'; }
			continue;
		}
		if (!in) continue;
		if (strstr(buf, "-> $0")) continue;
		n++;
	}
	fclose(f);
	return n;
}

static void do_swl(u64 rt, int off)
{
	static const u32 mask[4]  = { 0xffffff00u, 0xffff0000u, 0xff000000u, 0u };
	static const u8  shift[4] = { 24, 16, 8, 0 };
	const u32 sh = (u32)(off & 3);
	const u32 v = ((u32)rt >> shift[sh]) | (rd32(off & ~3) & mask[sh]);
	wr(off & ~3, v, 4);
}
static void do_swr(u64 rt, int off)
{
	static const u32 mask[4]  = { 0u, 0x000000ffu, 0x0000ffffu, 0x00ffffffu };
	static const u8  shift[4] = { 0, 8, 16, 24 };
	const u32 sh = (u32)(off & 3);
	const u32 v = ((u32)rt << shift[sh]) | (rd32(off & ~3) & mask[sh]);
	wr(off & ~3, v, 4);
}
static void do_sdl(u64 rt, int off)
{
	static const u64 mask[8] = {
		0xffffffffffffff00ull, 0xffffffffffff0000ull, 0xffffffffff000000ull,
		0xffffffff00000000ull, 0xffffff0000000000ull, 0xffff000000000000ull,
		0xff00000000000000ull, 0x0000000000000000ull };
	static const u8 shift[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	const u32 sh = (u32)(off & 7);
	wr(off & ~7, (rt >> shift[sh]) | (rd64(off & ~7) & mask[sh]), 8);
}
static void do_sdr(u64 rt, int off)
{
	static const u64 mask[8] = {
		0x0000000000000000ull, 0x00000000000000ffull, 0x000000000000ffffull,
		0x0000000000ffffffull, 0x00000000ffffffffull, 0x000000ffffffffffull,
		0x0000ffffffffffffull, 0x00ffffffffffffffull };
	static const u8 shift[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	const u32 sh = (u32)(off & 7);
	wr(off & ~7, (rt << shift[sh]) | (rd64(off & ~7) & mask[sh]), 8);
}

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "lsu.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int op = -1, failures = 0, total = 0, i;
	int pass[S_COUNT], cases[S_COUNT], total_in_block[S_COUNT];

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
	for (i = 0; i < S_COUNT; i++)
	{
		pass[i] = 0; cases[i] = 0;
		total_in_block[i] = count_block(path, kName[i]);
	}

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned w3, w2, w1, w0;
		int off1 = 0, off2 = 0, row, n;
		u64 rt, rt_hi;
		char op1[8], op2[8];
		char* p;
		(void)op2;

		if (buf[0] != ' ')
		{
			char* c = strchr(buf, ':');
			op = -1;
			if (c && c[1] == '\n')
			{
				*c = '\0';
				for (i = 0; i < S_COUNT; i++)
					if (!strcmp(buf, kName[i])) { op = i; break; }
				*c = ':';
			}
			continue;
		}
		if (op < 0) continue;
		if (strstr(buf, "-> $0")) continue;

		/* "sb +0:" or "swl +0/swr +3:" */
		p = buf + 2;
		{
			int k = 0;
			while (*p && *p != ' ' && k < 7) op1[k++] = *p++;
			op1[k] = '\0';
		}
		if (!read_off(&p, &off1)) continue;
		n = 2;
		if (*p == '/')
		{
			int k = 0;
			p++;
			while (*p && *p != ' ' && k < 7) op2[k++] = *p++;
			op2[k] = '\0';
			if (!read_off(&p, &off2)) continue;
			n = 4;
		}
		p = strchr(buf, ':');
		if (!p || sscanf(p + 1, " %x %x %x %x", &w3, &w2, &w1, &w0) != 4) continue;

		/* Each block runs its offsets twice: the first half with rt seeded
		 * from SET_U32<0xABCD4321>, the second from SET_M(C_GARBAGE1),
		 * which writes all 128 bits and so is what SQ stores. The number of
		 * offsets differs per block -- three here, four for SQ, five for
		 * the unaligned pairs -- so the switch is at the counted midpoint
		 * rather than a fixed three. */
		if (cases[op] < total_in_block[op] / 2)
		{ rt = (u64)(s64)(s32)0xABCD4321u; rt_hi = 0x0000133A00001339ull; }
		else
		{ rt = 0x0000133800001337ull; rt_hi = 0x0000133A00001339ull; }

		reset_mem();
		switch (op)
		{
		case S_SB: wr(off1, rt, 1); break;
		case S_SH: wr(off1, rt, 2); break;
		case S_SW: wr(off1, rt, 4); break;
		case S_SD: wr(off1, rt, 8); break;
		case S_SQ: wr(off1 & ~15, rt, 8); wr((off1 & ~15) + 8, rt_hi, 8); break;
		default:
			if      (!strcmp(op1, "swl")) do_swl(rt, off1);
			else if (!strcmp(op1, "swr")) do_swr(rt, off1);
			else if (!strcmp(op1, "sdl")) do_sdl(rt, off1);
			else if (!strcmp(op1, "sdr")) do_sdr(rt, off1);
			if (n == 4)
			{
				if      (!strcmp(op2, "swl")) do_swl(rt, off2);
				else if (!strcmp(op2, "swr")) do_swr(rt, off2);
				else if (!strcmp(op2, "sdl")) do_sdl(rt, off2);
				else if (!strcmp(op2, "sdr")) do_sdr(rt, off2);
			}
			break;
		}

		/* The printed row is 1 + offset/16, truncating toward zero. */
		row = 1 + off1 / 16;
		cases[op]++;
		{
			const u32 g0 = rd32(row * 16 - BASE);
			const u32 g1 = rd32(row * 16 - BASE + 4);
			const u32 g2 = rd32(row * 16 - BASE + 8);
			const u32 g3 = rd32(row * 16 - BASE + 12);
			if (g0 == w0 && g1 == w1 && g2 == w2 && g3 == w3) pass[op]++;
			else if (failures++ < 6)
				printf("  %-9s %+d: console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
				       kName[op], off1, w3, w2, w1, w0, g3, g2, g1, g0);
		}
	}
	fclose(f);

	for (i = 0; i < S_COUNT; i++)
	{
		printf("%-9s %2d/%-3d console cases\n", kName[i], pass[i], cases[i]);
		total += cases[i];
		if (pass[i] != cases[i]) failures++;
	}
	printf("hwstore: %d cases across %d ops\n", total, S_COUNT);
	return failures != 0;
}
