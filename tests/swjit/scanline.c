/* The scanline code (GS/Renderers/SW/GSDrawScanlineJit.c): random
 * selectors, and every value of each field with the rest random, for
 * SSE4.1, SSE4.1 with AVX encodings and with AVX2 (a build without the
 * tiers), AVX, AVX with AVX2, and AVX2, for both calling conventions,
 * with the constants at fixed distances from the code, hashed against
 * SCANLINE in build.sh: the bytes the xbyak generator made for them when
 * it was the oracle. */
#include "common.h"

/* the selector's fields: position, width */
static const struct { unsigned pos, n; } s_fields[] = {
	{0, 2}, {2, 2}, {4, 2}, {6, 3}, {9, 2}, {11, 1}, {12, 3}, {15, 1}, {16, 1}, {17, 1}, {18, 1}, {19, 1},
	{20, 1}, {21, 1}, {22, 2}, {24, 2}, {26, 2}, {28, 2}, {30, 1}, {31, 1}, {32, 1}, {33, 1}, {34, 1}, {35, 1},
	{36, 1}, {37, 1}, {38, 1}, {39, 2}, {41, 2}, {43, 1}, {44, 1}, {45, 1}, {46, 1}, {47, 2}, {49, 1}, {50, 3},
	{53, 1}, {54, 2}, {56, 1}, {57, 1}, {58, 1}, {59, 1}};
#define NFIELDS (sizeof(s_fields) / sizeof(s_fields[0]))

/* (a selector the renderer can make: tfx 0-4) */
static u64 sane(u64 key)
{
	if (((key >> 12) & 7) > 4)
		key &= ~(7ull << 12);
	return key & ((1ull << 60) - 1);
}

#define RANDOM 6000

int main(int argc, char** argv)
{
	/* tier, AVX encodings, AVX2 instructions */
	static const struct { int tier, avx, avx2; } cfg[] = {
		{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {1, 1, 0}, {1, 1, 1}, {2, 1, 1}};
	u8* const buf = swj_buf();
	unsigned c, i, n = 0;

	s_rng = 0x9E3779B97F4A7C15ull;
	for (c = 0; c < sizeof(cfg) / sizeof(cfg[0]); c++)
	{
		/* random selectors, then each field's every value */
		for (i = 0; i < RANDOM + 64 * NFIELDS; i++)
		{
			u64 key = rnd64();
			gs_swj_target t;
			int abi;
			if (i >= RANDOM)
			{
				const unsigned f = (i - RANDOM) / 64;
				const unsigned v = ((i - RANDOM) % 64) & ((1u << s_fields[f].n) - 1);
				key = (key & ~(((1ull << s_fields[f].n) - 1) << s_fields[f].pos)) | ((u64)v << s_fields[f].pos);
			}
			memset(&t, 0, sizeof(t));
			t.key    = sane(key);
			t.avx    = cfg[c].avx;
			t.avx2   = cfg[c].avx2;
			t.ymm    = cfg[c].tier == 2;
			t.consts = buf + 0x01000000;
			t.zbias  = buf + 0x01000800;
			for (abi = 0; abi < 2; abi++)
			{
				t.win64 = abi;
				hash_bytes(buf, gs_swj_draw_scanline(&t, buf, SWJ_ROOM));
				n++;
			}
		}
	}
	return swj_done("scanline", n, argc, argv);
}
