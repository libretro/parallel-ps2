/* The primitive setup's code (GS/Renderers/SW/GSSetupPrimJit.c): every
 * combination of the selector fields it reads, with the others random,
 * for SSE4.1, SSE4.1 with AVX encodings (a build without the tiers on an
 * AVX machine), AVX and AVX2, for both calling conventions, with the shift
 * tables at fixed addresses, hashed against SETUP in build.sh: the bytes
 * the xbyak generator made for them when it was the oracle. */
#include "common.h"

/* the selector bits the setup reads: iip, tfx, tcc, fst, fge, fwrite and
 * ftest, zwrite and ztest, prim, notest */
static const unsigned s_bits[] = {11, 12, 13, 14, 15, 16, 19, 32, 33, 35, 36, 47, 48, 56};
#define NBITS (sizeof(s_bits) / sizeof(s_bits[0]))

int main(int argc, char** argv)
{
	/* (the configurations: tier, AVX encodings) */
	static const struct { int tier, avx; } cfg[] = {{0, 0}, {0, 1}, {1, 1}, {2, 1}};
	/* fixed addresses, as far apart as g_const's tables */
	const u8* const fake256 = (const u8*)(size_t)0x00007F0012340000ull;
	const u8* const fake128 = fake256 + (GS_SLC_SHIFT_128B - GS_SLC_SHIFT_256B);
	u8* const buf = swj_buf();
	unsigned c, combo, b, n = 0;

	s_rng = 0x2545F4914F6CDD1Dull;
	for (c = 0; c < sizeof(cfg) / sizeof(cfg[0]); c++)
	{
		for (combo = 0; combo < (1u << NBITS); combo++)
		{
			u64 key = rnd64() & ((1ull << 60) - 1);
			gs_swj_target t;
			int abi;
			for (b = 0; b < NBITS; b++)
				key = (key & ~(1ull << s_bits[b])) | ((u64)((combo >> b) & 1) << s_bits[b]);
			memset(&t, 0, sizeof(t));
			t.key        = key;
			t.avx        = cfg[c].avx;
			t.ymm        = cfg[c].tier == 2;
			t.shift_128b = fake128;
			t.shift_256b = fake256;
			for (abi = 0; abi < 2; abi++)
			{
				t.win64 = abi;
				hash_bytes(buf, gs_swj_setup_prim(&t, buf, SWJ_ROOM));
				n++;
			}
		}
	}
	return swj_done("setup", n, argc, argv);
}
