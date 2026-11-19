/* The primitive setup's code (GS/Renderers/SW/GSSetupPrimJit.c) against
 * the xbyak generator it replaced (ref/, built per tier as the core built
 * it): every combination of the selector fields it reads, with the others
 * random, for SSE4.1, SSE4.1 with AVX encodings (a build without the
 * tiers on an AVX machine), AVX and AVX2, must come out the same bytes.
 *
 * The C generator alone also hashes what it emits for all of them, for
 * this build's calling convention and the other, with the shift tables at
 * fixed addresses: the oracle when the reference is gone (EXPECTED in
 * build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

static unsigned s_cpu;
extern "C" uint64_t cpu_features_get(void) { return s_cpu; }
static void quiet(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
extern "C" { retro_log_printf_t log_cb = quiet; }

#include "GS/Renderers/SW/GSScanlineEnvironment.h"
#include "GS/Renderers/SW/GSSwJit.h"

constexpr GSScanlineConstantData g_const;

extern "C" size_t ref_setup_prim_isa_sse4(u64 key, void* code, size_t room);
extern "C" size_t ref_setup_prim_isa_avx(u64 key, void* code, size_t room);
extern "C" size_t ref_setup_prim_isa_avx2(u64 key, void* code, size_t room);

alignas(64) static u8 s_ref[65536];
alignas(64) static u8 s_c[65536];

static unsigned long long s_rng = 0x2545F4914F6CDD1Dull;
static u64 rnd64(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return s_rng;
}

static unsigned long long s_hash = 1469598103934665603ull;
static void hash_bytes(const u8* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		s_hash = (s_hash ^ p[i]) * 1099511628211ull;
}

/* the selector bits the setup reads: iip, tfx, tcc, fst, fge, fwrite and
 * ftest, zwrite and ztest, prim, notest */
static const unsigned s_bits[] = {11, 12, 13, 14, 15, 16, 19, 32, 33, 35, 36, 47, 48, 56};
#define NBITS (sizeof(s_bits) / sizeof(s_bits[0]))

int main(int argc, char** argv)
{
	const unsigned long long want = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	/* (the configurations: tier, AVX encodings) */
	static const struct { int tier, avx; } cfg[] = {{0, 0}, {0, 1}, {1, 1}, {2, 1}};
	/* fixed addresses for the hash, as far apart as g_const's tables */
	const u8* const fake256 = (const u8*)(size_t)0x00007F0012340000ull;
	const u8* const fake128 = fake256 + ((const u8*)g_const.m_shift_128b - (const u8*)g_const.m_shift_256b);
	unsigned c, combo, b, fails = 0, checked = 0;

	{
		/* the selector where GSSwJit.h reads it */
		GSScanlineSelector s;
#define SEL(field, macro) \
		s.key = 0; s.field = 1; \
		if (macro(s.key) != 1) { printf("  FAIL: selector." #field " is not where " #macro " reads it\n"); fails++; }
		SEL(iip, GS_SEL_IIP) SEL(tcc, GS_SEL_TCC) SEL(fst, GS_SEL_FST) SEL(fge, GS_SEL_FGE) SEL(notest, GS_SEL_NOTEST)
		s.key = 0; s.tfx = 5; if (GS_SEL_TFX(s.key) != 5) { printf("  FAIL: selector.tfx\n"); fails++; }
		s.key = 0; s.fb = 2; if (GS_SEL_FB(s.key) != 2) { printf("  FAIL: selector.fb\n"); fails++; }
		s.key = 0; s.zb = 3; if (GS_SEL_ZB(s.key) != 3) { printf("  FAIL: selector.zb\n"); fails++; }
		s.key = 0; s.prim = 2; if (GS_SEL_PRIM(s.key) != 2) { printf("  FAIL: selector.prim\n"); fails++; }
	}

	for (c = 0; c < sizeof(cfg) / sizeof(cfg[0]); c++)
	{
		s_cpu = RETRO_SIMD_SSE4 | (cfg[c].avx ? RETRO_SIMD_AVX : 0) | (cfg[c].tier == 2 ? RETRO_SIMD_AVX2 : 0);
		for (combo = 0; combo < (1u << NBITS); combo++)
		{
			u64 key = rnd64() & ((1ull << 60) - 1);
			gs_swj_target t;
			size_t rs, cs;
			int abi;
			for (b = 0; b < NBITS; b++)
				key = (key & ~(1ull << s_bits[b])) | ((u64)((combo >> b) & 1) << s_bits[b]);

			/* (each room a little short now and then: cut short the same) */
			{
				const size_t room = (combo % 97) == 0 ? 40 + combo % 200 : sizeof(s_ref);
				rs = cfg[c].tier == 0 ? ref_setup_prim_isa_sse4(key, s_ref, room)
					: cfg[c].tier == 1 ? ref_setup_prim_isa_avx(key, s_ref, room)
					: ref_setup_prim_isa_avx2(key, s_ref, room);
				t.key        = key;
				t.avx        = cfg[c].avx;
				t.ymm        = cfg[c].tier == 2;
				t.win64      = GS_SWJ_WIN64;
				t.shift_128b = g_const.m_shift_128b;
				t.shift_256b = g_const.m_shift_256b;
				cs = gs_swj_setup_prim(&t, s_c, room);
			}
			checked++;
			if (rs != cs || memcmp(s_ref, s_c, rs))
			{
				if (fails++ < 10)
				{
					size_t i, k = 0;
					while (k < rs && k < cs && s_ref[k] == s_c[k])
						k++;
					printf("  FAIL tier %d avx %d key %016llx: %zu and %zu bytes, first difference at %zu\n    xbyak:",
						cfg[c].tier, cfg[c].avx, (unsigned long long)key, rs, cs, k);
					for (i = k > 8 ? k - 8 : 0; i < rs && i < k + 16; i++)
						printf(" %02x", s_ref[i]);
					printf("\n    C:    ");
					for (i = k > 8 ? k - 8 : 0; i < cs && i < k + 16; i++)
						printf(" %02x", s_c[i]);
					printf("\n");
				}
			}

			/* the hash: both calling conventions, the tables at fixed
			 * addresses */
			for (abi = 0; abi < 2; abi++)
			{
				t.win64      = abi;
				t.shift_128b = fake128;
				t.shift_256b = fake256;
				cs = gs_swj_setup_prim(&t, s_c, sizeof(s_c));
				hash_bytes(s_c, cs);
			}
		}
	}

	printf("swjit setup: %u checked against xbyak, %u failed, %016llx\n", checked, fails, s_hash);
	if (want && want != s_hash)
	{
		printf("FAIL: expected %016llx\n", want);
		return 1;
	}
	return fails != 0;
}
