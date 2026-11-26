/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2019  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../../common/VectorIntrin.h"


#include "yuv2rgb.h"
#include "ipu_macroblock.h"

void ipu_dither(const macroblock_rgb32 *rgb32, macroblock_rgb16 *rgb16, const int dte)
{
	int i;
	int n;
#if _M_SSE >= 0x200 /* SSE2 codepath */
	const __m128i alpha_test = _mm_set1_epi16(0x40);
	if (dte)
	{
		const __m128i dither_add_matrix[] = {
			_mm_setr_epi32(0x00000000, 0x00000000, 0x00000000, 0x00010101),
			_mm_setr_epi32(0x00020202, 0x00000000, 0x00030303, 0x00000000),
			_mm_setr_epi32(0x00000000, 0x00010101, 0x00000000, 0x00000000),
			_mm_setr_epi32(0x00030303, 0x00000000, 0x00020202, 0x00000000),
		};
		const __m128i dither_sub_matrix[] = {
			_mm_setr_epi32(0x00040404, 0x00000000, 0x00030303, 0x00000000),
			_mm_setr_epi32(0x00000000, 0x00020202, 0x00000000, 0x00010101),
			_mm_setr_epi32(0x00030303, 0x00000000, 0x00040404, 0x00000000),
			_mm_setr_epi32(0x00000000, 0x00010101, 0x00000000, 0x00020202),
		};
		for (i = 0; i < 16; ++i)
		{
			const __m128i dither_add = dither_add_matrix[i & 3];
			const __m128i dither_sub = dither_sub_matrix[i & 3];
			for (n = 0; n < 2; ++n)
			{
				__m128i rgba_8_0123          = _mm_load_si128((const __m128i *)(&rgb32->c[i][n * 8]));
				__m128i rgba_8_4567          = _mm_load_si128((const __m128i *)(&rgb32->c[i][n * 8 + 4]));
				__m128i rgba_16_0415, rgba_16_2637, rgba_32_0246, rgba_32_1357;
				__m128i rg_64_01234567, ba_64_01234567, zero;
				__m128i r, g, b, a, rgba16;

				/* Dither and clamp */
				rgba_8_0123                  = _mm_adds_epu8(rgba_8_0123, dither_add);
				rgba_8_0123                  = _mm_subs_epu8(rgba_8_0123, dither_sub);
				rgba_8_4567                  = _mm_adds_epu8(rgba_8_4567, dither_add);
				rgba_8_4567                  = _mm_subs_epu8(rgba_8_4567, dither_sub);

				/* Split into channel components and extend to 16 bits */
				rgba_16_0415   = _mm_unpacklo_epi8(rgba_8_0123, rgba_8_4567);
				rgba_16_2637   = _mm_unpackhi_epi8(rgba_8_0123, rgba_8_4567);
				rgba_32_0246   = _mm_unpacklo_epi8(rgba_16_0415, rgba_16_2637);
				rgba_32_1357   = _mm_unpackhi_epi8(rgba_16_0415, rgba_16_2637);
				rg_64_01234567 = _mm_unpacklo_epi8(rgba_32_0246, rgba_32_1357);
				ba_64_01234567 = _mm_unpackhi_epi8(rgba_32_0246, rgba_32_1357);

				zero           = _mm_setzero_si128();
				r                    = _mm_unpacklo_epi8(rg_64_01234567, zero);
				g                    = _mm_unpackhi_epi8(rg_64_01234567, zero);
				b                    = _mm_unpacklo_epi8(ba_64_01234567, zero);
				a                    = _mm_unpackhi_epi8(ba_64_01234567, zero);

				/* Create RGBA */
				r                            = _mm_srli_epi16(r, 3);
				g                            = _mm_slli_epi16(_mm_srli_epi16(g, 3), 5);
				b                            = _mm_slli_epi16(_mm_srli_epi16(b, 3), 10);
				a                            = _mm_slli_epi16(_mm_cmpeq_epi16(a, alpha_test), 15);

				rgba16         = _mm_or_si128(_mm_or_si128(r, g), _mm_or_si128(b, a));

				_mm_store_si128((__m128i *)(&rgb16->c[i][n * 8]), rgba16);
			}
		}
	}
	else
	{
		for (i = 0; i < 16; ++i)
		{
			for (n = 0; n < 2; ++n)
			{
				__m128i rgba_8_0123          = _mm_load_si128((const __m128i *)(&rgb32->c[i][n * 8]));
				__m128i rgba_8_4567          = _mm_load_si128((const __m128i *)(&rgb32->c[i][n * 8 + 4]));
				__m128i rgba_16_0415, rgba_16_2637, rgba_32_0246, rgba_32_1357;
				__m128i rg_64_01234567, ba_64_01234567, zero;
				__m128i r, g, b, a, rgba16;

				/* Split into channel components and extend to 16 bits */
				rgba_16_0415   = _mm_unpacklo_epi8(rgba_8_0123, rgba_8_4567);
				rgba_16_2637   = _mm_unpackhi_epi8(rgba_8_0123, rgba_8_4567);
				rgba_32_0246   = _mm_unpacklo_epi8(rgba_16_0415, rgba_16_2637);
				rgba_32_1357   = _mm_unpackhi_epi8(rgba_16_0415, rgba_16_2637);
				rg_64_01234567 = _mm_unpacklo_epi8(rgba_32_0246, rgba_32_1357);
				ba_64_01234567 = _mm_unpackhi_epi8(rgba_32_0246, rgba_32_1357);

				zero           = _mm_setzero_si128();
				r                    = _mm_unpacklo_epi8(rg_64_01234567, zero);
				g                    = _mm_unpackhi_epi8(rg_64_01234567, zero);
				b                    = _mm_unpacklo_epi8(ba_64_01234567, zero);
				a                    = _mm_unpackhi_epi8(ba_64_01234567, zero);

				/* Create RGBA */
				r                            = _mm_srli_epi16(r, 3);
				g                            = _mm_slli_epi16(_mm_srli_epi16(g, 3), 5);
				b                            = _mm_slli_epi16(_mm_srli_epi16(b, 3), 10);
				a                            = _mm_slli_epi16(_mm_cmpeq_epi16(a, alpha_test), 15);

				rgba16         = _mm_or_si128(_mm_or_si128(r, g), _mm_or_si128(b, a));

				_mm_store_si128((__m128i *)(&rgb16->c[i][n * 8]), rgba16);
			}
		}
	}
#elif defined(_M_ARM64) || defined(__aarch64__)
	/* NEON deinterleaves the four channels in the load itself, so the
	 * unpack chain the SSE2 path needs has no counterpart here. The dither
	 * is still a saturating add and a saturating subtract, which is what
	 * gives the clamp at both ends; the two are never both non-zero for a
	 * pixel, so together they are one clamped signed add. */
	const uint8x8_t alpha_test = vdup_n_u8(0x40);
	if (dte)
	{
		/* Per-pixel, repeating every four across the row. Row class picks
		 * the pair, and the same eight lanes serve both halves of the row
		 * because the row is sixteen pixels. */
		static const u8 dither_add[4][8] = {
			{ 0, 0, 0, 1, 0, 0, 0, 1 },
			{ 2, 0, 3, 0, 2, 0, 3, 0 },
			{ 0, 1, 0, 0, 0, 1, 0, 0 },
			{ 3, 0, 2, 0, 3, 0, 2, 0 }
		};
		static const u8 dither_sub[4][8] = {
			{ 4, 0, 3, 0, 4, 0, 3, 0 },
			{ 0, 2, 0, 1, 0, 2, 0, 1 },
			{ 3, 0, 4, 0, 3, 0, 4, 0 },
			{ 0, 1, 0, 2, 0, 1, 0, 2 }
		};
		for (i = 0; i < 16; ++i)
		{
			const uint8x8_t vadd = vld1_u8(dither_add[i & 3]);
			const uint8x8_t vsub = vld1_u8(dither_sub[i & 3]);
			for (n = 0; n < 2; ++n)
			{
				const uint8x8x4_t px =
					vld4_u8((const u8 *)&rgb32->c[i][n * 8]);
				const uint8x8_t r =
					vqsub_u8(vqadd_u8(px.val[0], vadd), vsub);
				const uint8x8_t g =
					vqsub_u8(vqadd_u8(px.val[1], vadd), vsub);
				const uint8x8_t b =
					vqsub_u8(vqadd_u8(px.val[2], vadd), vsub);

				const uint16x8_t R = vshrq_n_u16(vmovl_u8(r), 3);
				const uint16x8_t G =
					vshlq_n_u16(vshrq_n_u16(vmovl_u8(g), 3), 5);
				const uint16x8_t B =
					vshlq_n_u16(vshrq_n_u16(vmovl_u8(b), 3), 10);
				const uint16x8_t A = vshlq_n_u16(
					vmovl_u8(vceq_u8(px.val[3], alpha_test)), 15);

				vst1q_u16((u16 *)&rgb16->c[i][n * 8],
				          vorrq_u16(vorrq_u16(R, G), vorrq_u16(B, A)));
			}
		}
	}
	else
	{
		for (i = 0; i < 16; ++i)
		{
			for (n = 0; n < 2; ++n)
			{
				const uint8x8x4_t px =
					vld4_u8((const u8 *)&rgb32->c[i][n * 8]);

				const uint16x8_t R = vshrq_n_u16(vmovl_u8(px.val[0]), 3);
				const uint16x8_t G =
					vshlq_n_u16(vshrq_n_u16(vmovl_u8(px.val[1]), 3), 5);
				const uint16x8_t B =
					vshlq_n_u16(vshrq_n_u16(vmovl_u8(px.val[2]), 3), 10);
				const uint16x8_t A = vshlq_n_u16(
					vmovl_u8(vceq_u8(px.val[3], alpha_test)), 15);

				vst1q_u16((u16 *)&rgb16->c[i][n * 8],
				          vorrq_u16(vorrq_u16(R, G), vorrq_u16(B, A)));
			}
		}
	}
#else /* Reference C implementation */
	int j;
	if (dte)
	{
		/* I'm guessing values are rounded down when clamping. */
		const int dither_coefficient[4][4] = {
			{-4, 0, -3, 1},
			{2, -2, 3, -1},
			{-3, 1, -4, 0},
			{3, -1, 2, -2},
		};
		for (i = 0; i < 16; ++i)
		{
			for (j = 0; j < 16; ++j)
			{
				const int dither = dither_coefficient[i & 3][j & 3];
				const int r = pcsx2_max_i(0, pcsx2_min_i(rgb32->c[i][j].r + dither, 255));
				const int g = pcsx2_max_i(0, pcsx2_min_i(rgb32->c[i][j].g + dither, 255));
				const int b = pcsx2_max_i(0, pcsx2_min_i(rgb32->c[i][j].b + dither, 255));

				rgb16->c[i][j].r = r >> 3;
				rgb16->c[i][j].g = g >> 3;
				rgb16->c[i][j].b = b >> 3;
				rgb16->c[i][j].a = rgb32->c[i][j].a == 0x40;
			}
		}
	}
	else
	{
		for (i = 0; i < 16; ++i)
		{
			for (j = 0; j < 16; ++j)
			{
				rgb16->c[i][j].r = rgb32->c[i][j].r >> 3;
				rgb16->c[i][j].g = rgb32->c[i][j].g >> 3;
				rgb16->c[i][j].b = rgb32->c[i][j].b >> 3;
				rgb16->c[i][j].a = rgb32->c[i][j].a == 0x40;
			}
		}
	}
#endif
}
