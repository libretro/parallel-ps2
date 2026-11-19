/* The scanline code (GS/Renderers/SW/GSDrawScanlineJit.c) against the
 * xbyak generator it replaced (ref/, built per tier as the core built it),
 * for SSE4.1, SSE4.1 with AVX encodings and with AVX2 (a build without
 * the tiers), AVX, AVX with AVX2, and AVX2: random selectors, and every
 * value of each field with the rest random, must come out the same bytes
 * from the same address (the constants are addressed RIP-relative).
 *
 * The C generator alone also hashes what it emits for all of them, for
 * both calling conventions, with the constants at fixed distances from
 * the code: the oracle when the reference is gone (EXPECTED in build.sh). */
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

extern "C" size_t ref_draw_scanline_isa_sse4(u64 key, void* code, size_t room);
extern "C" size_t ref_draw_scanline_isa_avx(u64 key, void* code, size_t room);
extern "C" size_t ref_draw_scanline_isa_avx2(u64 key, void* code, size_t room);

alignas(64) static u8 s_buf[65536];
alignas(64) static u8 s_ref[65536];

static unsigned long long s_rng = 0x9E3779B97F4A7C15ull;
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

/* the selector's fields: position, width */
static const struct { unsigned pos, n; } s_fields[] = {
	{0, 2}, {2, 2}, {4, 2}, {6, 3}, {9, 2}, {11, 1}, {12, 3}, {15, 1}, {16, 1}, {17, 1}, {18, 1}, {19, 1},
	{20, 1}, {21, 1}, {22, 2}, {24, 2}, {26, 2}, {28, 2}, {30, 1}, {31, 1}, {32, 1}, {33, 1}, {34, 1}, {35, 1},
	{36, 1}, {37, 1}, {38, 1}, {39, 2}, {41, 2}, {43, 1}, {44, 1}, {45, 1}, {46, 1}, {47, 2}, {49, 1}, {50, 3},
	{53, 1}, {54, 2}, {56, 1}, {57, 1}, {58, 1}, {59, 1}};
#define NFIELDS (sizeof(s_fields) / sizeof(s_fields[0]))

/* (a selector the renderer can make: tfx 0-4, tw 0-7) */
static u64 sane(u64 key)
{
	if (((key >> 12) & 7) > 4)
		key &= ~(7ull << 12);
	return key & ((1ull << 60) - 1);
}

#define RANDOM 6000

int main(int argc, char** argv)
{
	const unsigned long long want = argc > 1 ? strtoull(argv[1], NULL, 16) : 0;
	/* tier, AVX encodings, AVX2 instructions */
	static const struct { int tier, avx, avx2; } cfg[] = {
		{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {1, 1, 0}, {1, 1, 1}, {2, 1, 1}};
	unsigned c, n, fails = 0, checked = 0;

	{
		/* the selector where GSSwJit.h reads it */
		GSScanlineSelector s;
		unsigned f;
		for (f = 0; f < NFIELDS; f++)
		{
			s.key = 1ull << s_fields[f].pos;
			if (s.lo == 0 && s.hi == 0)
			{
				printf("  FAIL: field %u\n", f);
				fails++;
			}
		}
#define SEL(field, macro, v) \
		s.key = 0; s.field = v; \
		if (macro(s.key) != v) { printf("  FAIL: selector." #field " is not where " #macro " reads it\n"); fails++; }
		SEL(fpsm, GS_SEL_FPSM, 3) SEL(zpsm, GS_SEL_ZPSM, 3) SEL(ztst, GS_SEL_ZTST, 3) SEL(atst, GS_SEL_ATST, 7)
		SEL(afail, GS_SEL_AFAIL, 3) SEL(ltf, GS_SEL_LTF, 1) SEL(tlu, GS_SEL_TLU, 1) SEL(date, GS_SEL_DATE, 1)
		SEL(abe, GS_SEL_ABE, 1) SEL(aba, GS_SEL_ABA, 3) SEL(abb, GS_SEL_ABB, 3) SEL(abc, GS_SEL_ABC, 3)
		SEL(abd, GS_SEL_ABD, 3) SEL(pabe, GS_SEL_PABE, 1) SEL(aa1, GS_SEL_AA1, 1) SEL(fwrite, GS_SEL_FWRITE, 1)
		SEL(rfb, GS_SEL_RFB, 1) SEL(zwrite, GS_SEL_ZWRITE, 1) SEL(ztest, GS_SEL_ZTEST, 1)
		SEL(zoverflow, GS_SEL_ZOVERFLOW, 1) SEL(zclamp, GS_SEL_ZCLAMP, 1) SEL(wms, GS_SEL_WMS, 3)
		SEL(wmt, GS_SEL_WMT, 3) SEL(datm, GS_SEL_DATM, 1) SEL(colclamp, GS_SEL_COLCLAMP, 1) SEL(fba, GS_SEL_FBA, 1)
		SEL(dthe, GS_SEL_DTHE, 1) SEL(edge, GS_SEL_EDGE, 1) SEL(tw, GS_SEL_TW, 7) SEL(lcm, GS_SEL_LCM, 1)
		SEL(mmin, GS_SEL_MMIN, 3) SEL(notest, GS_SEL_NOTEST, 1) SEL(zequal, GS_SEL_ZEQUAL, 1)
		SEL(hires, GS_SEL_HIRES, 1) SEL(tw_hi, GS_SEL_TW_HI, 1)
	}

	for (c = 0; c < sizeof(cfg) / sizeof(cfg[0]); c++)
	{
		unsigned f, v;
		s_cpu = RETRO_SIMD_SSE4 | (cfg[c].avx ? RETRO_SIMD_AVX : 0) | (cfg[c].avx2 ? RETRO_SIMD_AVX2 : 0);
		/* random selectors, then each field's every value */
		for (n = 0, f = 0, v = 0; n < RANDOM + 64 * NFIELDS; n++)
		{
			u64 key = rnd64();
			gs_swj_target t;
			size_t rs, cs, room;
			int abi;
			if (n >= RANDOM)
			{
				const unsigned i = n - RANDOM;
				f = i / 64;
				v = (i % 64) & ((1u << s_fields[f].n) - 1);
				key = (key & ~(((1ull << s_fields[f].n) - 1) << s_fields[f].pos)) | ((u64)v << s_fields[f].pos);
			}
			key = sane(key);

			/* (each room a little short now and then: cut short the same) */
			room = (n % 211) == 0 ? 64 + n % 900 : sizeof(s_buf);
			rs = cfg[c].tier == 0 ? ref_draw_scanline_isa_sse4(key, s_buf, room)
				: cfg[c].tier == 1 ? ref_draw_scanline_isa_avx(key, s_buf, room)
				: ref_draw_scanline_isa_avx2(key, s_buf, room);
			memcpy(s_ref, s_buf, rs);
			t.key        = key;
			t.avx        = cfg[c].avx;
			t.avx2       = cfg[c].avx2;
			t.ymm        = cfg[c].tier == 2;
			t.win64      = GS_SWJ_WIN64;
			t.shift_128b = g_const.m_shift_128b;
			t.shift_256b = g_const.m_shift_256b;
			t.consts     = &g_const;
			t.zbias      = &GSVector4::m_xc1e00000000fffff;
			cs = gs_swj_draw_scanline(&t, s_buf, room);
			checked++;
			if (rs != cs || memcmp(s_ref, s_buf, rs))
			{
				if (fails++ < 10)
				{
					size_t i, k = 0;
					while (k < rs && k < cs && s_ref[k] == s_buf[k])
						k++;
					printf("  FAIL tier %d avx %d avx2 %d key %016llx: %zu and %zu bytes, first difference at %zu\n    xbyak:",
						cfg[c].tier, cfg[c].avx, cfg[c].avx2, (unsigned long long)key, rs, cs, k);
					for (i = k > 8 ? k - 8 : 0; i < rs && i < k + 16; i++)
						printf(" %02x", s_ref[i]);
					printf("\n    C:    ");
					for (i = k > 8 ? k - 8 : 0; i < cs && i < k + 16; i++)
						printf(" %02x", s_buf[i]);
					printf("\n");
				}
			}

			/* the hash: both calling conventions, the constants at fixed
			 * distances from the code */
			for (abi = 0; abi < 2; abi++)
			{
				t.win64  = abi;
				t.consts = s_buf + 0x01000000;
				t.zbias  = s_buf + 0x01000800;
				cs = gs_swj_draw_scanline(&t, s_buf, sizeof(s_buf));
				hash_bytes(s_buf, cs);
			}
		}
	}

	printf("swjit scanline: %u checked against xbyak, %u failed, %016llx\n", checked, fails, s_hash);
	if (want && want != s_hash)
	{
		printf("FAIL: expected %016llx\n", want);
		return 1;
	}
	return fails != 0;
}
