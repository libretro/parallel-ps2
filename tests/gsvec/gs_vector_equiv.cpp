/* gs_vector against GSVector4i.
 *
 * Every entry point is checked byte-for-byte against the class method it
 * replaces, over random inputs with the edges seeded, at whatever tier the
 * build selected. The SSE2 tier matters most here: GSVector4i has no SSE2
 * form of min_i32, max_i32, min_u32, max_u32, pu32, blend8, the lane
 * accessors or the tests, so on that tier there is nothing to compare
 * against except a scalar model, which is what this file supplies.
 */
#include "GS/GSVector.h"
extern "C" {
#include "GS/gs_vector.h"
}
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <algorithm>

static uint64_t X = 0x243F6A8885A308D3ull;
static uint32_t rnd(){ X ^= X<<13; X ^= X>>7; X ^= X<<17; return (uint32_t)(X>>16); }

static long total = 0, fails = 0;

static void seed(void *p, int k)
{
	uint32_t *w = (uint32_t *)p;
	int j;
	for (j = 0; j < 4; j++) w[j] = rnd();
	switch (k & 15) {
	case 0: memset(p, 0x00, 16); break;
	case 1: memset(p, 0xff, 16); break;
	case 2: w[0] = 0x80000000u; w[1] = 0x7fffffffu; break;
	case 3: w[0] = 0xffff0000u; w[2] = 0x0000ffffu; break;
	case 4: w[0] = 0x00008000u; w[1] = 0xffff8000u; break;
	default: break;
	}
}

/* Compares a C89 result against a GSVector4i one. */
static void chk(const char *what, gs_vec4i got, GSVector4i want)
{
	alignas(16) uint8_t a[16], b[16];
	gs_v4i_store(a, got);
	GSVector4i::store<true>(b, want);
	total++;
	if (memcmp(a, b, 16)) {
		if (fails < 8) printf("  MISMATCH %s\n", what);
		fails++;
	}
}

static void chk_int(const char *what, int got, int want)
{
	total++;
	if (got != want) {
		if (fails < 8) printf("  MISMATCH %s: %d vs %d\n", what, got, want);
		fails++;
	}
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 300000;
	int k;

	printf("tier: ");
#if defined(GS_VEC_CAN_AVX)
	printf("avx\n");
#elif defined(GS_VEC_CAN_SSE41)
	printf("sse4.1\n");
#elif defined(GS_VEC_X86)
	printf("sse2\n");
#else
	printf("neon\n");
#endif

	for (k = 0; k < iters; k++) {
		alignas(16) uint8_t ra[16], rb[16];
		seed(ra, k); seed(rb, k >> 4);

		gs_vec4i a = gs_v4i_load(ra), b = gs_v4i_load(rb);
		GSVector4i A = GSVector4i::load<true>(ra), B = GSVector4i::load<true>(rb);

		/* arithmetic and logic */
		chk("add32",  gs_v4i_add32(a, b),  A + B);
		chk("sub32",  gs_v4i_sub32(a, b),  A - B);
		chk("add16",  gs_v4i_add16(a, b),  A.add16(B));
		chk("sub16",  gs_v4i_sub16(a, b),  A.sub16(B));
		chk("and",    gs_v4i_and(a, b),    A & B);
		chk("or",     gs_v4i_or(a, b),     A | B);
		chk("xor",    gs_v4i_xor(a, b),    A ^ B);
		chk("andnot", gs_v4i_andnot(a, b), GSVector4i(_mm_andnot_si128(A, B)));
		chk("eq32",   gs_v4i_eq32(a, b),   A == B);
		chk("gt32",   gs_v4i_gt32(a, b),   A > B);

		/* min/max -- the four the class takes from SSE4.1 */
		chk("min_i32", gs_v4i_min_i32(a, b), A.min_i32(B));
		chk("max_i32", gs_v4i_max_i32(a, b), A.max_i32(B));
		chk("min_u32", gs_v4i_min_u32(a, b), A.min_u32(B));
		chk("max_u32", gs_v4i_max_u32(a, b), A.max_u32(B));

		/* unpack */
		chk("upl8v",  gs_v4i_upl8v(a, b),  A.upl8(B));
		chk("uph8v",  gs_v4i_uph8v(a, b),  A.uph8(B));
		chk("upl16v", gs_v4i_upl16v(a, b), A.upl16(B));
		chk("uph16v", gs_v4i_uph16v(a, b), A.uph16(B));
		chk("upl32v", gs_v4i_upl32v(a, b), A.upl32(B));
		chk("uph32v", gs_v4i_uph32v(a, b), A.uph32(B));
		chk("upl64v", gs_v4i_upl64v(a, b), A.upl64(B));
		chk("upl8",   gs_v4i_upl8(a),      A.upl8());
		chk("uph8",   gs_v4i_uph8(a),      A.uph8());
		chk("upl16",  gs_v4i_upl16(a),     A.upl16());
		chk("uph16",  gs_v4i_uph16(a),     A.uph16());
		chk("upl32",  gs_v4i_upl32(a),     A.upl32());
		chk("uph32",  gs_v4i_uph32(a),     A.uph32());

		/* the reverb FIR's three */
		chk("adds16",   gs_v4i_adds16(a, b),   A.adds16(B));
		chk("hadds16",  gs_v4i_hadds16(a, b),  A.hadds16(B));
		chk("mul16hrs", gs_v4i_mul16hrs(a, b), A.mul16hrs(B));

		/* pack */
		chk("ps32", gs_v4i_ps32(a, b), A.ps32(B));
		chk("pu32", gs_v4i_pu32(a, b), A.pu32(B));

		/* blend */
		chk("blend8",  gs_v4i_blend8(a, b, b),        A.blend8(B, B));
		chk("blend32", GS_V4I_BLEND32(a, b, 12),      A.blend32<12>(B));
		chk("blend16", GS_V4I_BLEND16(a, b, 0xa5),    A.blend16<0xa5>(B));

		/* shifts */
		chk("sll16",  GS_V4I_SLL16(a, 3),  A.sll16<3>());
		chk("srl16",  GS_V4I_SRL16(a, 5),  A.srl16<5>());
		chk("sra16",  GS_V4I_SRA16(a, 2),  A.sra16<2>());
		chk("sll32",  GS_V4I_SLL32(a, 7),  A.sll32<7>());
		chk("srl32",  GS_V4I_SRL32(a, 11), A.srl32<11>());
		chk("sra32",  GS_V4I_SRA32(a, 4),  A.sra32<4>());
		chk("sll",    GS_V4I_SLL(a, 5),    A.sll<5>());
		chk("srl",    GS_V4I_SRL(a, 8),    A.srl<8>());

		/* shuffles */
		chk("shuffle32", GS_V4I_SHUFFLE32(a, 0x44), A.xyxy());
		chk("zwzw",      GS_V4I_SHUFFLE32(a, 0xee), A.zwzw());

		/* rectangles */
		chk("rintersect", gs_v4i_rintersect(a, b), A.rintersect(B));
		chk("runion",     gs_v4i_runion(a, b),     A.runion(B));

		/* constants */
		chk("zero", gs_v4i_zero(), GSVector4i::zero());
		chk("ones", gs_v4i_ones(), GSVector4i::xffffffff());

		/* lane access */
		chk_int("extract32<0>", GS_V4I_EXTRACT32(a, 0), A.extract32<0>());
		chk_int("extract32<1>", GS_V4I_EXTRACT32(a, 1), A.extract32<1>());
		chk_int("extract32<2>", GS_V4I_EXTRACT32(a, 2), A.extract32<2>());
		chk_int("extract32<3>", GS_V4I_EXTRACT32(a, 3), A.extract32<3>());
		chk("insert32<1>", GS_V4I_INSERT32(a, 0x1234abcd, 1), A.insert32<1>(0x1234abcd));
		chk("insert32<3>", GS_V4I_INSERT32(a, -7, 3),          A.insert32<3>(-7));
		chk("insert8<5>",  GS_V4I_INSERT8(a, 0x5a, 5),         A.insert8<5>(0x5a));
		chk("insert8<10>", GS_V4I_INSERT8(a, 0xc3, 10),        A.insert8<10>(0xc3));

		/* tests */
		chk_int("alltrue",  gs_v4i_alltrue(a),  A.alltrue());
		chk_int("allfalse", gs_v4i_allfalse(a), A.allfalse());

		/* what the GIF path (GSState.c) takes */
		chk("load32",  gs_v4i_load32(A.extract32<0>()), GSVector4i::load(A.extract32<0>()));
		chk("loadlh",  gs_v4i_loadlh(ra, rb + 8),        GSVector4i::load(ra, rb + 8));
		chk("lt32",    gs_v4i_lt32(a, b),                A.lt32(B));
		chk("eq64",    gs_v4i_eq64(a, b),                A.eq64(B));
		chk("eq64 a",  gs_v4i_eq64(a, a),                A.eq64(A));
		chk("u16to32", gs_v4i_u16to32(a),                A.u16to32());
		chk("xxxx",    GS_V4I_SHUFFLE32(a, 0x00),        A.xxxx());
		chk("yyyy",    GS_V4I_SHUFFLE32(a, 0x55),        A.yyyy());
		chk_int("mask",   gs_v4i_mask(a),   A.mask());
		chk_int("eq",     gs_v4i_eq(a, b),  A.eq(B));
		chk_int("eq a",   gs_v4i_eq(a, a),  A.eq(A));
		chk_int("rempty", gs_v4i_rempty(a), A.rempty());
		chk("replace_nan_f32", gs_v4i_replace_nan_f32(a, b),
		    GSVector4i::cast(GSVector4::cast(A).replace_nan(GSVector4::cast(B))));
		chk("F32_ONE", gs_v4i_set32(GS_V4I_F32_ONE), GSVector4i::cast(GSVector4::m_one));
		chk("F32_MAX", gs_v4i_set32(GS_V4I_F32_MAX), GSVector4i::cast(GSVector4::m_max));
		chk("F32_MIN_POS", gs_v4i_set32(GS_V4I_F32_MIN_POS), GSVector4i::cast(GSVector4(FLT_MIN)));

		/* rect alignment and the high-half load, as GSDirtyRect uses them */
		{
			alignas(8) int32_t bs2[2];
			bs2[0] = 1 << (1 + (k & 3));
			bs2[1] = 1 << (1 + ((k >> 2) & 3));
			GSVector2i BS(bs2[0], bs2[1]);
			chk("ralign_outside", gs_v4i_ralign_outside(a, bs2),
			    A.ralign<Align_Outside>(BS));
			chk("ralign_neginf",  gs_v4i_ralign_neginf(a, bs2),
			    A.ralign<Align_NegInf>(BS));
			chk("loadh",          gs_v4i_loadh(bs2),            GSVector4i::loadh(BS));
			chk("set4", gs_v4i_set4(bs2[0], bs2[1], bs2[0], bs2[1]),
			    GSVector4i(bs2[0], bs2[1], bs2[0], bs2[1]));
		}

		/* the named view must see the same lanes the class does */
		{
			union gs_v4i_view vw;
			vw.v = a;
			chk_int("view.left",   vw.rect.left,   A.left);
			chk_int("view.bottom", vw.rect.bottom, A.bottom);
			chk_int("view.y",      vw.lane.y,      A.y);
			chk_int("view.i16[5]", vw.i16[5],      A.I16[5]);
			chk_int("view.u8[11]", vw.u8[11],      (int)(uint8_t)A.I8[11]);
		}

		/* load/store forms */
		{
			alignas(16) uint8_t o1[16], o2[16];
			memset(o1, 0xA5, 16); memset(o2, 0xA5, 16);
			gs_v4i_storel(o1, a); GSVector4i::storel(o2, A);
			total++; if (memcmp(o1, o2, 8)) { fails++; if (fails < 8) printf("  MISMATCH storel\n"); }
			chk("loadl", gs_v4i_loadl(ra), GSVector4i::loadl(ra));
			chk("loadu", gs_v4i_loadu(ra), GSVector4i::load<false>(ra));
		}
	}

	printf("%s: gs_vector, %ld comparisons, %ld mismatches\n",
	       fails ? "FAIL" : "PASS", total, fails);
	return fails != 0;
}
