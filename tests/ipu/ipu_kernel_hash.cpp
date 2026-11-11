/* IPU colour-conversion and dither kernels against a scalar model.
 *
 * yuv2rgb() and ipu_dither() each carry SIMD paths and a scalar path that
 * are meant to agree, and nothing in the tree checks that they do. The
 * scalar model here is written out from what the operation means rather
 * than lifted from any of them, so a shared mistake is still caught. Every
 * tier the build can reach is checked: SSE2 and SSE4.1 take different
 * routes through the dither, AVX2 changes what the compiler emits
 * underneath both, and aarch64 takes a NEON route of its own.
 *
 * The hashes are pinned so a change meant to be bit-exact can be shown to
 * be. Re-pin with --print only when the output is meant to change.
 */
#include "IPU/IPU_MultiISA.h"
#include "IPU/yuv2rgb.h"
#include "IPU/ipu_csc.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* The kernels read and write this; IPU.c is not linked. */
alignas(16) decoder_t decoder;
alignas(16) tIPU_BP g_BP;

/* ------------------------------------------------------------------ */

static uint64_t hash_init(void) { return 1469598103934665603ull; }

static void hash_bytes(uint64_t *h, const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	size_t i;
	for (i = 0; i < n; i++)
		*h = (*h ^ (uint64_t)b[i]) * 1099511628211ull;
}

static uint32_t rs = 0x2545f491u;
static uint32_t rnd(void)
{
	rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
	return rs;
}

/* The CLUT draws from its own stream so that adding it did not shift the
 * one the macroblocks come from -- the hashes pinned before it existed
 * still stand, and stay comparable across the change. */
static uint32_t rs_clut = 0x6a09e667u;
static uint32_t rnd_clut(void)
{
	rs_clut ^= rs_clut << 13; rs_clut ^= rs_clut >> 17; rs_clut ^= rs_clut << 5;
	return rs_clut;
}

/* Macroblocks with the edges seeded: the conversion clamps at both ends
 * and the dither saturates at both ends, so a run of mid-grey would leave
 * every clamp in both kernels untested. */
static void fill_mb8(macroblock_8 *mb, int k)
{
	int y, x;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			mb->Y[y][x] = (u8)rnd();
	for (y = 0; y < 8; y++)
		for (x = 0; x < 8; x++)
		{
			mb->Cb[y][x] = (u8)rnd();
			mb->Cr[y][x] = (u8)rnd();
		}

	switch (k & 7)
	{
	case 0: memset(mb, 0x00, sizeof(*mb)); break;
	case 1: memset(mb, 0xff, sizeof(*mb)); break;
	case 2: /* luma at the bias, chroma at the rails */
		memset(mb->Y, 16, sizeof(mb->Y));
		memset(mb->Cb, 0x00, sizeof(mb->Cb));
		memset(mb->Cr, 0xff, sizeof(mb->Cr));
		break;
	case 3: /* luma below the bias, which the conversion floors */
		memset(mb->Y, 0, sizeof(mb->Y));
		memset(mb->Cb, 0xff, sizeof(mb->Cb));
		memset(mb->Cr, 0x00, sizeof(mb->Cr));
		break;
	case 4: memset(mb->Y, 0xff, sizeof(mb->Y)); break;
	default: break;
	}
}

static void fill_rgb32(macroblock_rgb32 *rgb, int k)
{
	int y, x;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			rgb->c[y][x].r = (u8)rnd();
			rgb->c[y][x].g = (u8)rnd();
			rgb->c[y][x].b = (u8)rnd();
			/* alpha is compared against 0x40 by both paths, so both
			 * sides of that test have to occur */
			rgb->c[y][x].a = (k + y + x) & 1 ? 0x40 : 0x80;
		}

	switch (k & 7)
	{
	case 0: /* at the bottom, where the dither subtracts into the floor */
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++)
				rgb->c[y][x].r = rgb->c[y][x].g = rgb->c[y][x].b = (u8)(x & 3);
		break;
	case 1: /* at the top, where it adds into the ceiling */
		for (y = 0; y < 16; y++)
			for (x = 0; x < 16; x++)
				rgb->c[y][x].r = rgb->c[y][x].g = rgb->c[y][x].b =
					(u8)(252 + (x & 3));
		break;
	default: break;
	}
}

/* ------------------------------------------------------------------ */
/* What the kernels are supposed to compute.                            */
/* ------------------------------------------------------------------ */

#define IPU_Y_BIAS    16
#define IPU_Y_COEFF   0x95
#define IPU_GCR_COEFF (-0x68)
#define IPU_GCB_COEFF (-0x32)
#define IPU_RCR_COEFF 0xcc
#define IPU_BCB_COEFF 0x102

static int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void model_yuv2rgb(const macroblock_8 *mb8, macroblock_rgb32 *out)
{
	int y, x;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			const int Y  = (int)mb8->Y[y][x] - IPU_Y_BIAS;
			const int cb = (int)mb8->Cb[y >> 1][x >> 1] - 128;
			const int cr = (int)mb8->Cr[y >> 1][x >> 1] - 128;
			const int lum = (IPU_Y_COEFF * (Y < 0 ? 0 : Y)) >> 6;
			const int rcr = (IPU_RCR_COEFF * cr) >> 6;
			const int gcr = (IPU_GCR_COEFF * cr) >> 6;
			const int gcb = (IPU_GCB_COEFF * cb) >> 6;
			const int bcb = (IPU_BCB_COEFF * cb) >> 6;

			out->c[y][x].r = (u8)clamp255((lum + rcr + 1) >> 1);
			out->c[y][x].g = (u8)clamp255((lum + gcr + gcb + 1) >> 1);
			out->c[y][x].b = (u8)clamp255((lum + bcb + 1) >> 1);
			out->c[y][x].a = 0x80;
		}
}

static void model_dither(const macroblock_rgb32 *in, macroblock_rgb16 *out,
                         int dte)
{
	static const int coeff[4][4] = {
		{ -4,  0, -3,  1 },
		{  2, -2,  3, -1 },
		{ -3,  1, -4,  0 },
		{  3, -1,  2, -2 }
	};
	int y, x;
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
		{
			const int d = dte ? coeff[y & 3][x & 3] : 0;
			out->c[y][x].r = (u16)(clamp255(in->c[y][x].r + d) >> 3);
			out->c[y][x].g = (u16)(clamp255(in->c[y][x].g + d) >> 3);
			out->c[y][x].b = (u16)(clamp255(in->c[y][x].b + d) >> 3);
			out->c[y][x].a = (u16)(in->c[y][x].a == 0x40);
		}
}

/* What ipu_csc adds on top of the conversion, and what ipu_vq computes.  */

static void model_csc(const macroblock_8 *mb8, macroblock_rgb32 *out, int sgn,
                      const u16 *thresh)
{
	model_yuv2rgb(mb8, out);

	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
		{
			int r = out->c[y][x].r, g = out->c[y][x].g;
			int b = out->c[y][x].b, a = out->c[y][x].a;

			if (thresh[0] > 0 && r < thresh[0] && g < thresh[0] && b < thresh[0])
				r = g = b = a = 0;
			else if (thresh[1] > 0 && r < thresh[1] && g < thresh[1]
			      && b < thresh[1])
				a = 0x40;

			if (sgn)
			{
				r ^= 0x80; g ^= 0x80; b ^= 0x80;
			}

			out->c[y][x].r = (u8)r; out->c[y][x].g = (u8)g;
			out->c[y][x].b = (u8)b; out->c[y][x].a = (u8)a;
		}
}

static void model_vq(const macroblock_rgb16 *in, u8 *indx4, const rgb16_t *clut)
{
	u8 idx[16][16];

	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 16; j++)
		{
			int best = 0, bestd = -1;
			for (int k = 0; k < 16; k++)
			{
				const int dr = (int)in->c[i][j].r - (int)clut[k].r;
				const int dg = (int)in->c[i][j].g - (int)clut[k].g;
				const int db = (int)in->c[i][j].b - (int)clut[k].b;
				const int d = dr * dr + dg * dg + db * db;
				/* First strictly-smaller wins, so ties keep the
				 * lower index. */
				if (bestd < 0 || d < bestd) { bestd = d; best = k; }
			}
			idx[i][j] = (u8)best;
		}

	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 8; j++)
			indx4[i * 8 + j] = (u8)((idx[i][2 * j + 1] << 4) | idx[i][2 * j]);
}

/* ------------------------------------------------------------------ */

static long mismatches;
static long compared;

static void cmp(const char *what, const void *got, const void *want, size_t n,
                int iter)
{
	compared++;
	if (memcmp(got, want, n) == 0)
		return;
	if (mismatches < 6)
	{
		const unsigned char *a = (const unsigned char *)got;
		const unsigned char *b = (const unsigned char *)want;
		size_t i;
		for (i = 0; i < n; i++)
			if (a[i] != b[i])
				break;
		printf("  MISMATCH %s, iteration %d, first at byte %u: %02x vs %02x\n",
		       what, iter, (unsigned)i, a[i], b[i]);
	}
	mismatches++;
}

int main(int argc, char **argv)
{
	const int iters = argc > 1 ? atoi(argv[1]) : 20000;
	const int print = (argc > 2 && strcmp(argv[2], "--print") == 0);
	uint64_t h_rgb = hash_init(), h_d1 = hash_init(), h_d0 = hash_init();
	uint64_t h_csc = hash_init(), h_vq = hash_init();
	int k;

	/* Pinned from the tree as it stands, at the default 20000 blocks. */
	const uint64_t want_rgb = 0x61e1becba7406f39ull;
	const uint64_t want_d1  = 0x46fa8d3aefd74da2ull;
	const uint64_t want_d0  = 0x3a4261bc98fd5da4ull;
	const uint64_t want_csc = 0xe3d24f86c3865f05ull;
	const uint64_t want_vq  = 0x171c29283a27fe37ull;

	printf("tier: ");
#if defined(__AVX2__)
	printf("avx2\n");
#elif defined(__SSE4_1__)
	printf("sse4.1\n");
#elif defined(__SSE2__) || defined(__x86_64__)
	printf("sse2\n");
#else
	printf("scalar\n");
#endif

	for (k = 0; k < iters; k++)
	{
		/* The kernels load and store these 16 bytes at a time with
		 * aligned instructions, so the caller has to supply the
		 * alignment -- the macroblock types do not carry it. Without
		 * this the dither faults under clang, which is how the
		 * requirement came to light. */
		alignas(16) macroblock_rgb32 ref_rgb;
		alignas(16) macroblock_rgb16 got16, ref16;
		alignas(16) macroblock_rgb32 src;

		/* colour conversion: the kernel reads and writes the decoder */
		fill_mb8(&decoder.mb8, k);
		memset(&decoder.rgb32, 0xcd, sizeof(decoder.rgb32));
		yuv2rgb(&decoder.mb8, &decoder.rgb32);
		model_yuv2rgb(&decoder.mb8, &ref_rgb);
		cmp("yuv2rgb", &decoder.rgb32, &ref_rgb, sizeof(ref_rgb), k);
		hash_bytes(&h_rgb, &decoder.rgb32, sizeof(decoder.rgb32));

		/* csc: the conversion plus the two alpha thresholds and the
		 * sign flip. Both thresholds are swept, including the
		 * disabled cases, since which branch runs depends on them. */
		{
			alignas(16) macroblock_rgb32 got_csc, ref_csc;
			static const u16 thresh_sets[4][2] = {
				{ 0, 0 }, { 0, 0x60 }, { 0x30, 0x90 }, { 0xff, 0xff }
			};
			const u16 *th = thresh_sets[k & 3];
			const int sgn = (k >> 2) & 1;

			memset(&got_csc, 0xcd, sizeof(got_csc));
			memset(&ref_csc, 0xab, sizeof(ref_csc));
			ipu_csc(&decoder.mb8, &got_csc, sgn, th);
			model_csc(&decoder.mb8, &ref_csc, sgn, th);
			cmp("ipu_csc", &got_csc, &ref_csc, sizeof(ref_csc), k);
			hash_bytes(&h_csc, &got_csc, sizeof(got_csc));
		}

		/* dither, both with the matrix and without */
		fill_rgb32(&src, k);

		memset(&got16, 0xcd, sizeof(got16));
		memset(&ref16, 0xab, sizeof(ref16));
		ipu_dither(&src, &got16, 1);
		model_dither(&src, &ref16, 1);
		cmp("dither dte=1", &got16, &ref16, sizeof(ref16), k);
		hash_bytes(&h_d1, &got16, sizeof(got16));

		memset(&got16, 0xcd, sizeof(got16));
		memset(&ref16, 0xab, sizeof(ref16));
		ipu_dither(&src, &got16, 0);
		model_dither(&src, &ref16, 0);
		cmp("dither dte=0", &got16, &ref16, sizeof(ref16), k);
		hash_bytes(&h_d0, &got16, sizeof(got16));

		/* vq: nearest of sixteen CLUT entries, two indices per byte.
		 * The CLUT is reseeded every block, and every so often it is
		 * given duplicate entries so the tie rule is exercised. */
		{
			rgb16_t clut[16];
			u8 got_vq[16 * 8], ref_vq[16 * 8];

			for (int c = 0; c < 16; c++)
			{
				clut[c].r = rnd_clut() & 31;
				clut[c].g = rnd_clut() & 31;
				clut[c].b = rnd_clut() & 31;
				clut[c].a = 0;
			}
			if ((k & 7) == 0)
				for (int c = 8; c < 16; c++)
					clut[c] = clut[c - 8];

			memset(got_vq, 0xcd, sizeof(got_vq));
			memset(ref_vq, 0xab, sizeof(ref_vq));
			ipu_vq(&got16, got_vq, clut);
			model_vq(&got16, ref_vq, clut);
			cmp("ipu_vq", got_vq, ref_vq, sizeof(ref_vq), k);
			hash_bytes(&h_vq, got_vq, sizeof(got_vq));
		}
	}

	if (print)
	{
		printf("  { %016llx, %016llx, %016llx, %016llx, %016llx }\n",
		       (unsigned long long)h_rgb, (unsigned long long)h_d1,
		       (unsigned long long)h_d0, (unsigned long long)h_csc,
		       (unsigned long long)h_vq);
		return 0;
	}

	printf("  yuv2rgb      %016llx%s\n", (unsigned long long)h_rgb,
	       want_rgb == 0 ? "  (unpinned)" :
	       (h_rgb == want_rgb ? "  ok" : "  UNEXPECTED"));
	printf("  dither dte=1 %016llx%s\n", (unsigned long long)h_d1,
	       want_d1 == 0 ? "  (unpinned)" :
	       (h_d1 == want_d1 ? "  ok" : "  UNEXPECTED"));
	printf("  dither dte=0 %016llx%s\n", (unsigned long long)h_d0,
	       want_d0 == 0 ? "  (unpinned)" :
	       (h_d0 == want_d0 ? "  ok" : "  UNEXPECTED"));

	printf("  ipu_csc      %016llx%s\n", (unsigned long long)h_csc,
	       want_csc == 0 ? "  (unpinned)" :
	       (h_csc == want_csc ? "  ok" : "  UNEXPECTED"));
	printf("  ipu_vq       %016llx%s\n", (unsigned long long)h_vq,
	       want_vq == 0 ? "  (unpinned)" :
	       (h_vq == want_vq ? "  ok" : "  UNEXPECTED"));

	{
		const int bad = (mismatches != 0)
		              || (want_rgb && h_rgb != want_rgb)
		              || (want_d1 && h_d1 != want_d1)
		              || (want_d0 && h_d0 != want_d0)
		              || (want_csc && h_csc != want_csc)
		              || (want_vq && h_vq != want_vq);
		printf("%s: IPU kernels, %ld comparisons, %ld mismatches\n",
		       bad ? "FAIL" : "PASS", compared, mismatches);
		return bad;
	}
}
