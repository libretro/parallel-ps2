/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <memalign.h>

#include "common/Pcsx2Defs.h"
#include "common/VectorIntrin.h"
#include "../../gs_cvt.h"
#include "GSDeviceSW.h"
#include "GSTextureSW.h"

/* (x86's SSE 4.1, the core's baseline there; elsewhere the scalar code,
 * which makes the same pixels) */
#if defined(_M_SSE) && _M_SSE >= 0x401
#define GS_DEVICE_SW_SIMD 1
#else
#define GS_DEVICE_SW_SIMD 0
#endif

/* Rows of the present buffer, as the textures' */
#define GS_DEVICE_SW_ROW_ALIGN 32

/* ====================================================================
 * Pixels
 * ==================================================================== */

/* One channel of s over d at alpha a (0..255): each product +128 and >>8
 * on its own, the sum saturated, as the SSE code does. */
static INLINE u32 sw_blend_channel(u32 s, u32 d, u32 a)
{
	const u32 v = ((s * a + 128u) >> 8) + ((d * (255u - a) + 128u) >> 8);
	return v > 255u ? 255u : v;
}

/* PS2-ABGR s over d at alpha a, d's alpha kept */
static INLINE u32 sw_blend_px(u32 s, u32 d, u32 a)
{
	return (d & 0xFF000000u)
		| (sw_blend_channel((s >> 16) & 0xFFu, (d >> 16) & 0xFFu, a) << 16)
		| (sw_blend_channel((s >>  8) & 0xFFu, (d >>  8) & 0xFFu, a) << 8)
		|  sw_blend_channel( s        & 0xFFu,  d        & 0xFFu, a);
}

static INLINE u32 sw_load(const u8* row, int x)
{
	u32 v;
	memcpy(&v, row + (size_t)x * 4, 4);
	return v;
}

static INLINE void sw_store(u8* row, int x, u32 v)
{
	memcpy(row + (size_t)x * 4, &v, 4);
}

/* PS2 ABGR to the frontend's XRGB8888, opaque */
static INLINE u32 sw_to_xrgb(u32 abgr)
{
	const u32 r = (abgr      ) & 0xFFu;
	const u32 g = (abgr >>  8) & 0xFFu;
	const u32 b = (abgr >> 16) & 0xFFu;
	return (0xFFu << 24) | (r << 16) | (g << 8) | b;
}

static void sw_clear(u8* dst, int dst_pitch, int dst_w, int dst_h, u32 color)
{
	int x, y;
	for (y = 0; y < dst_h; y++)
	{
		u8* row = dst + (size_t)y * dst_pitch;
		for (x = 0; x < dst_w; x++)
			sw_store(row, x, color);
	}
}

/* (a sampled row or column: the texel centre's, clamped into the source) */
static INLINE int sw_sample(float base, float scale, int i, int n)
{
	const int s = gs_f2i(base + ((float)i + 0.5f) * scale);
	return s < 0 ? 0 : s >= n ? n - 1 : s;
}

/* A source region into a destination region, nearest texel, as it is
 * (to_xrgb: converted for the frontend). (sx0, sy0, sx1, sy1) are the
 * source's normalised; the destination clipped to dst_clip. */
static void sw_nearest_copy(
	const u8* src, int src_pitch, int src_w, int src_h,
	float sx0, float sy0, float sx1, float sy1,
	u8* dst, int dst_pitch,
	int dx0, int dy0, int dx1, int dy1,
	int dst_clip_w, int dst_clip_h, int to_xrgb)
{
	const int out_w = dx1 - dx0;
	const int out_h = dy1 - dy0;
	float src_x_scale, src_y_scale, src_x_base, src_y_base;
	int identity, x, y;

	if (out_w <= 0 || out_h <= 0)
		return;

	src_x_scale = (sx1 - sx0) * (float)src_w / (float)out_w;
	src_y_scale = (sy1 - sy0) * (float)src_h / (float)out_h;
	src_x_base  = sx0 * (float)src_w;
	src_y_base  = sy0 * (float)src_h;

	/* x exactly 1:1 and in bounds: rows copied whole */
	identity = sx0 == 0.0f && sx1 == 1.0f && out_w == src_w && dx0 >= 0 && dx1 <= dst_clip_w && dx1 <= src_w;

	for (y = 0; y < out_h; y++)
	{
		const int dy = dy0 + y;
		const u8* src_row;
		u8* dst_row;

		if (dy < 0 || dy >= dst_clip_h)
			continue;

		src_row = src + (size_t)sw_sample(src_y_base, src_y_scale, y, src_h) * src_pitch;
		dst_row = dst + (size_t)dy * dst_pitch;

		if (identity && !to_xrgb)
		{
			memcpy(dst_row + dx0 * 4, src_row + dx0 * 4, (size_t)out_w * 4);
			continue;
		}

#if GS_DEVICE_SW_SIMD
		if (identity)
		{
			/* pshufb, a quad: bytes 2 1 0 of each pixel to 0 1 2, the
			 * fourth zeroed, then the opaque alpha in */
			const __m128i shuf     = _mm_setr_epi8((char)2, (char)1, (char)0, (char)0x80, (char)6, (char)5, (char)4,
				(char)0x80, (char)10, (char)9, (char)8, (char)0x80, (char)14, (char)13, (char)12, (char)0x80);
			const __m128i alpha_or = _mm_set1_epi32((int)0xFF000000);

			for (x = 0; x + 4 <= out_w; x += 4)
			{
				const __m128i q = _mm_loadu_si128((const __m128i*)(src_row + (dx0 + x) * 4));
				_mm_storeu_si128((__m128i*)(dst_row + (dx0 + x) * 4), _mm_or_si128(_mm_shuffle_epi8(q, shuf), alpha_or));
			}
			for (; x < out_w; x++)
				sw_store(dst_row, dx0 + x, sw_to_xrgb(sw_load(src_row, dx0 + x)));
			continue;
		}
#endif

		for (x = 0; x < out_w; x++)
		{
			const int dx = dx0 + x;
			u32 p;
			if (dx < 0 || dx >= dst_clip_w)
				continue;
			p = sw_load(src_row, sw_sample(src_x_base, src_x_scale, x, src_w));
			sw_store(dst_row, dx, to_xrgb ? sw_to_xrgb(p) : p);
		}
	}
}

#if GS_DEVICE_SW_SIMD
/* Four PS2-ABGR pixels of s over d, d's alpha kept: each channel's
 * products +128 and >>8, as sw_blend_px. The alphas a 16-bit lane a
 * channel, 255 - them in ia. */
static INLINE __m128i sw_blend_quad(__m128i s, __m128i d, __m128i a_lo, __m128i a_hi, __m128i ia_lo, __m128i ia_hi)
{
	const __m128i zero       = _mm_setzero_si128();
	const __m128i bias       = _mm_set1_epi16(128);
	const __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000);
	const __m128i sa_lo      = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), a_lo), bias), 8);
	const __m128i sa_hi      = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(s, zero), a_hi), bias), 8);
	const __m128i di_lo      = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), ia_lo), bias), 8);
	const __m128i di_hi      = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), ia_hi), bias), 8);
	const __m128i out        = _mm_packus_epi16(_mm_add_epi16(sa_lo, di_lo), _mm_add_epi16(sa_hi, di_hi));
	return _mm_or_si128(_mm_andnot_si128(alpha_mask, out), _mm_and_si128(alpha_mask, d));
}
#endif

/* (a source pixel's alpha, 0..0x80 opaque, to 0..255: doubled, saturated) */
static INLINE u32 sw_alpha(u32 s)
{
	return pcsx2_min_u(((s >> 24) & 0xFFu) * 2u, 255u);
}

/* A source region blended over a destination region, nearest texel: at
 * a constant alpha (PMODE.MMOD, PMODE.ALP's alpha8) or the source's.
 * Alphas are the PS2's, 0x80 opaque, doubled with saturation as the HW
 * merge shader does. */
static void sw_blend_over(
	const u8* src, int src_pitch, int src_w, int src_h,
	float sx0, float sy0, float sx1, float sy1,
	u8* dst, int dst_pitch,
	int dx0, int dy0, int dx1, int dy1,
	int dst_clip_w, int dst_clip_h,
	bool constant_alpha, u8 alpha8)
{
	const int out_w = dx1 - dx0;
	const int out_h = dy1 - dy0;
	const u32 const_a2 = pcsx2_min_u((u32)alpha8 * 2u, 255u);
	float src_x_scale, src_y_scale, src_x_base, src_y_base;
	int identity, x, y;

	if (out_w <= 0 || out_h <= 0)
		return;

	src_x_scale = (sx1 - sx0) * (float)src_w / (float)out_w;
	src_y_scale = (sy1 - sy0) * (float)src_h / (float)out_h;
	src_x_base  = sx0 * (float)src_w;
	src_y_base  = sy0 * (float)src_h;

	/* x exactly 1:1 and in bounds: every merge the SW renderer makes */
	identity = sx0 == 0.0f && sx1 == 1.0f && out_w == src_w && dx0 >= 0 && dx1 <= dst_clip_w && dx1 <= src_w;
	(void)identity;

	for (y = 0; y < out_h; y++)
	{
		const int dy = dy0 + y;
		const u8* src_row;
		u8* dst_row;

		if (dy < 0 || dy >= dst_clip_h)
			continue;

		src_row = src + (size_t)sw_sample(src_y_base, src_y_scale, y, src_h) * src_pitch;
		dst_row = dst + (size_t)dy * dst_pitch;

#if GS_DEVICE_SW_SIMD
		if (identity)
		{
			const __m128i zero = _mm_setzero_si128();

			for (x = 0; x + 4 <= out_w; x += 4)
			{
				const __m128i s = _mm_loadu_si128((const __m128i*)(src_row + (dx0 + x) * 4));
				const __m128i d = _mm_loadu_si128((const __m128i*)(dst_row + (dx0 + x) * 4));
				__m128i a_lo, a_hi, ia_lo, ia_hi;
				if (constant_alpha)
				{
					a_lo = a_hi   = _mm_set1_epi16((short)const_a2);
					ia_lo = ia_hi = _mm_set1_epi16((short)(255u - const_a2));
				}
				else
				{
					/* each pixel's alpha byte across its channels, doubled
					 * with saturation */
					const __m128i splat = _mm_setr_epi8(3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15);
					__m128i a8          = _mm_shuffle_epi8(s, splat);
					__m128i ia8;
					a8    = _mm_adds_epu8(a8, a8);
					ia8   = _mm_sub_epi8(_mm_set1_epi8((char)255), a8);
					a_lo  = _mm_unpacklo_epi8(a8, zero);
					a_hi  = _mm_unpackhi_epi8(a8, zero);
					ia_lo = _mm_unpacklo_epi8(ia8, zero);
					ia_hi = _mm_unpackhi_epi8(ia8, zero);
				}
				_mm_storeu_si128((__m128i*)(dst_row + (dx0 + x) * 4), sw_blend_quad(s, d, a_lo, a_hi, ia_lo, ia_hi));
			}
			for (; x < out_w; x++)
			{
				const u32 s = sw_load(src_row, dx0 + x);
				sw_store(dst_row, dx0 + x, sw_blend_px(s, sw_load(dst_row, dx0 + x), constant_alpha ? const_a2 : sw_alpha(s)));
			}
			continue;
		}
#endif

		for (x = 0; x < out_w; x++)
		{
			const int dx = dx0 + x;
			u32 s;
			if (dx < 0 || dx >= dst_clip_w)
				continue;
			s = sw_load(src_row, sw_sample(src_x_base, src_x_scale, x, src_w));
			sw_store(dst_row, dx, sw_blend_px(s, sw_load(dst_row, dx), constant_alpha ? const_a2 : sw_alpha(s)));
		}
	}
}

/* ====================================================================
 * Deinterlacing: full frames, the field offset of weave and bob in
 * whole rows. A line is `block` rows (the upscale).
 * ==================================================================== */

/* The source's rows of the lines of `field` into dst, y_shift down; the
 * other lines left as they are, the previous frame's field. */
static void sw_weave(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch, int dst_w,
	int dst_h, int y_shift, int field, int block)
{
	const int copy_w = pcsx2_min_i(src_w, dst_w);
	const int copy_h = pcsx2_min_i(src_h, dst_h);
	int y;

	for (y = 0; y < copy_h; y++)
	{
		const int dy = y + y_shift;
		if (dy < 0 || dy >= dst_h || ((dy / block) & 1) != field)
			continue;
		memcpy(dst + (size_t)dy * dst_pitch, src + (size_t)y * src_pitch, (size_t)copy_w * 4);
	}
}

/* The source y_shift rows down, the rows it leaves opaque black. */
static void sw_bob(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch, int dst_w,
	int dst_h, int y_shift)
{
	const int copy_w     = pcsx2_min_i(src_w, dst_w);
	const int copy_start = pcsx2_max_i(0, y_shift);
	const int copy_end   = pcsx2_min_i(dst_h, src_h + y_shift);
	int dy;

	if (copy_start > 0)
		sw_clear(dst, dst_pitch, dst_w, copy_start, 0xFF000000u);
	if (copy_end < dst_h)
		sw_clear(dst + (size_t)copy_end * dst_pitch, dst_pitch, dst_w, dst_h - copy_end, 0xFF000000u);

	for (dy = copy_start; dy < copy_end; dy++)
		memcpy(dst + (size_t)dy * dst_pitch, src + (size_t)(dy - y_shift) * src_pitch, (size_t)copy_w * 4);
}

/* Each row (above + 2 * centre + below + 2) / 4, a line away, alpha the
 * centre's: blend's second pass, from the weave into another target. */
static void sw_blend_three(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch,
	int dst_w, int dst_h, int block)
{
	const int copy_w = pcsx2_min_i(src_w, dst_w);
	const int copy_h = pcsx2_min_i(src_h, dst_h);
	int x, y;

	for (y = 0; y < copy_h; y++)
	{
		const u8* row_above = src + (size_t)pcsx2_max_i(y - block, 0) * src_pitch;
		const u8* row_cent  = src + (size_t)y * src_pitch;
		const u8* row_below = src + (size_t)pcsx2_min_i(y + block, src_h - 1) * src_pitch;
		u8* dst_row         = dst + (size_t)y * dst_pitch;

		x = 0;
#if GS_DEVICE_SW_SIMD
		{
			const __m128i zero       = _mm_setzero_si128();
			const __m128i two        = _mm_set1_epi16(2);
			const __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000);

			for (; x + 4 <= copy_w; x += 4)
			{
				const __m128i a = _mm_loadu_si128((const __m128i*)(row_above + x * 4));
				const __m128i c = _mm_loadu_si128((const __m128i*)(row_cent + x * 4));
				const __m128i b = _mm_loadu_si128((const __m128i*)(row_below + x * 4));
				const __m128i s_lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_unpacklo_epi8(a, zero),
					_mm_unpacklo_epi8(b, zero)), _mm_add_epi16(_mm_slli_epi16(_mm_unpacklo_epi8(c, zero), 1), two)), 2);
				const __m128i s_hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_unpackhi_epi8(a, zero),
					_mm_unpackhi_epi8(b, zero)), _mm_add_epi16(_mm_slli_epi16(_mm_unpackhi_epi8(c, zero), 1), two)), 2);
				const __m128i out = _mm_packus_epi16(s_lo, s_hi);
				_mm_storeu_si128((__m128i*)(dst_row + x * 4),
					_mm_or_si128(_mm_andnot_si128(alpha_mask, out), _mm_and_si128(alpha_mask, c)));
			}
		}
#endif
		for (; x < copy_w; x++)
		{
			const u32 a = sw_load(row_above, x);
			const u32 c = sw_load(row_cent, x);
			const u32 b = sw_load(row_below, x);
			const u32 r = ((a & 0xFFu) + (c & 0xFFu) * 2u + (b & 0xFFu) + 2u) >> 2;
			const u32 g = (((a >> 8) & 0xFFu) + ((c >> 8) & 0xFFu) * 2u + ((b >> 8) & 0xFFu) + 2u) >> 2;
			const u32 bl = (((a >> 16) & 0xFFu) + ((c >> 16) & 0xFFu) * 2u + ((b >> 16) & 0xFFu) + 2u) >> 2;
			sw_store(dst_row, x, (c & 0xFF000000u) | (bl << 16) | (g << 8) | r);
		}
	}
}

/* FastMAD, interlace.glsl's two passes: the buffer twice the frame's
 * height, two banks (top, bottom) of two fields each, bufIdx (0..3)
 * which bank (idx >> 1) and field (idx & 1) is the newest. */

/* One field of the frame into its bank (rows dy_begin to dy_end): the
 * rows of its parity, the others left with the earlier frames'. */
static void sw_mad_buffer(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch, int dst_w,
	int dst_h, int dy_begin, int dy_end, int field, int bank, int vres, int block)
{
	const int copy_w = pcsx2_min_i(src_w, dst_w);
	/* (the shader's parity correction for an odd vres in bank 1) */
	const int lofs = (((((vres + 1) >> 1) << 1) - vres) & bank);
	int n;

	for (n = 0; n < src_h; n++)
	{
		const int dst_y = dy_begin + n;
		if (dst_y < 0 || dst_y >= dst_h || dst_y >= dy_end)
			continue;
		if ((((dst_y + lofs) / block) & 1) != field)
			continue;
		memcpy(dst + (size_t)dst_y * dst_pitch, src + (size_t)n * src_pitch, (size_t)copy_w * 4);
	}
}

/* The largest of the RGB channels' |newer - older| - threshold */
static INLINE int sw_motion(u32 newer, u32 older, int threshold)
{
	int m = -0x7fffffff;
	int k;
	for (k = 0; k < 24; k += 8)
	{
		int d = (int)((newer >> k) & 0xFFu) - (int)((older >> k) & 0xFFu);
		if (d < 0)
			d = -d;
		d -= threshold;
		if (d > m)
			m = d;
	}
	return m;
}

/* RGB rounded up half way between, a's alpha (the vector code's pavgb
 * takes the alpha's mean too: it is not read after) */
static INLINE u32 sw_average(u32 a, u32 b)
{
	const u32 r  = ((a & 0xFFu) + (b & 0xFFu) + 1u) >> 1;
	const u32 g  = (((a >> 8) & 0xFFu) + ((b >> 8) & 0xFFu) + 1u) >> 1;
	const u32 bl = (((a >> 16) & 0xFFu) + ((b >> 16) & 0xFFu) + 1u) >> 1;
	return (a & 0xFF000000u) | (bl << 16) | (g << 8) | r;
}

/* The frame from the buffer: the current field's rows as they are, the
 * top and bottom line woven, and between, the line woven where the
 * three pixels around it hold still against the frame before and
 * interpolated where any moves (above the threshold). */
static void sw_mad_reconstruct(const u8* src, int src_pitch, u8* dst, int dst_pitch, int dst_w, int dst_h, int idx,
	int threshold, int block)
{
	const int field   = idx & 1;
	const int frame_h = dst_h; /* a bank */
	int t0_base, t1_base, t2_base, t3_base;
	int x, y;

	/* (the shader's switch(idx)) */
	switch (idx)
	{
		case 1:  t0_base = 0;       t1_base = 0;       t2_base = frame_h; t3_base = frame_h; break;
		case 2:  t0_base = frame_h; t1_base = 0;       t2_base = 0;       t3_base = frame_h; break;
		case 3:  t0_base = frame_h; t1_base = frame_h; t2_base = 0;       t3_base = 0;       break;
		default: t0_base = 0;       t1_base = frame_h; t2_base = frame_h; t3_base = 0;       break;
	}

	for (y = 0; y < dst_h; y++)
	{
		const int p_t0_row    = y + t0_base;
		const int p_t2_row    = y + t2_base;
		const u8* row_t0      = src + (size_t)p_t0_row * src_pitch;
		const u8* row_t1      = src + (size_t)(y + t1_base) * src_pitch;
		const u8* row_t3      = src + (size_t)(y + t3_base) * src_pitch;
		/* (the neighbours held in their bank) */
		const u8* row_t0_up   = src + (size_t)pcsx2_max_i(t0_base, p_t0_row - block) * src_pitch;
		const u8* row_t0_down = src + (size_t)pcsx2_min_i(t0_base + frame_h - 1, p_t0_row + block) * src_pitch;
		const u8* row_t2_up   = src + (size_t)pcsx2_max_i(t2_base, p_t2_row - block) * src_pitch;
		const u8* row_t2_down = src + (size_t)pcsx2_min_i(t2_base + frame_h - 1, p_t2_row + block) * src_pitch;
		u8* dst_row           = dst + (size_t)y * dst_pitch;

		if (((y / block) & 1) == field)
		{
			memcpy(dst_row, row_t0, (size_t)dst_w * 4);
			continue;
		}
		if (y < block || y >= dst_h - block)
		{
			memcpy(dst_row, row_t1, (size_t)dst_w * 4);
			continue;
		}

		x = 0;
#if GS_DEVICE_SW_SIMD
		{
			/* |a - b| as two saturated differences, the threshold off
			 * saturated: a pixel still where none of RGB is left */
			const __m128i thr    = _mm_set1_epi8((char)threshold);
			const __m128i rgb    = _mm_set1_epi32(0x00FFFFFF);
			const __m128i zero   = _mm_setzero_si128();

			for (; x + 4 <= dst_w; x += 4)
			{
				const __m128i hn = _mm_loadu_si128((const __m128i*)(row_t0_up + x * 4));
				const __m128i cn = _mm_loadu_si128((const __m128i*)(row_t1 + x * 4));
				const __m128i ln = _mm_loadu_si128((const __m128i*)(row_t0_down + x * 4));
				const __m128i ho = _mm_loadu_si128((const __m128i*)(row_t2_up + x * 4));
				const __m128i co = _mm_loadu_si128((const __m128i*)(row_t3 + x * 4));
				const __m128i lo = _mm_loadu_si128((const __m128i*)(row_t2_down + x * 4));
				const __m128i mh = _mm_subs_epu8(_mm_or_si128(_mm_subs_epu8(hn, ho), _mm_subs_epu8(ho, hn)), thr);
				const __m128i mc = _mm_subs_epu8(_mm_or_si128(_mm_subs_epu8(cn, co), _mm_subs_epu8(co, cn)), thr);
				const __m128i ml = _mm_subs_epu8(_mm_or_si128(_mm_subs_epu8(ln, lo), _mm_subs_epu8(lo, ln)), thr);
				const __m128i still = _mm_cmpeq_epi32(_mm_and_si128(_mm_or_si128(_mm_or_si128(mh, mc), ml), rgb), zero);
				_mm_storeu_si128((__m128i*)(dst_row + x * 4),
					_mm_or_si128(_mm_and_si128(still, cn), _mm_andnot_si128(still, _mm_avg_epu8(hn, ln))));
			}
		}
#endif
		for (; x < dst_w; x++)
		{
			const u32 hn = sw_load(row_t0_up, x);
			const u32 cn = sw_load(row_t1, x);
			const u32 ln = sw_load(row_t0_down, x);
			const int moves = sw_motion(hn, sw_load(row_t2_up, x), threshold) > 0
				|| sw_motion(cn, sw_load(row_t3, x), threshold) > 0
				|| sw_motion(ln, sw_load(row_t2_down, x), threshold) > 0;
			sw_store(dst_row, x, moves ? sw_average(hn, ln) : cn);
		}
	}
}

/* Half-height fields (FFMD), as interlace.glsl takes them at 1x: the
 * merge drew each field twice as tall, every field row on two rows. */

/* A row of the source read at vertical position r (in rows, 0.5 the
 * first row's centre) with the linear sampler, clamped to [lo, hi) */
static void sw_sample_row(const u8* src, int src_pitch, int lo, int hi, float r, u8* out_row, int w)
{
	const float fy = r - 0.5f;
	int y0 = gs_f2i(gs_floorf(fy));
	const u32 wt = (u32)gs_f2i((fy - (float)y0) * 256.0f + 0.5f);
	int y1 = y0 + 1;
	const u8* a;
	const u8* b;
	int x, k;

	if (y0 < lo) y0 = lo;
	if (y0 > hi - 1) y0 = hi - 1;
	if (y1 < lo) y1 = lo;
	if (y1 > hi - 1) y1 = hi - 1;
	a = src + (size_t)y0 * src_pitch;
	b = src + (size_t)y1 * src_pitch;
	if (wt == 0 || y0 == y1)
	{
		memcpy(out_row, a, (size_t)w * 4);
		return;
	}
	for (x = 0; x < w; x++)
	{
		const u32 pa = sw_load(a, x);
		const u32 pb = sw_load(b, x);
		u32 c = 0;
		for (k = 0; k < 32; k += 8)
		{
			const u32 ca = (pa >> k) & 0xFFu;
			const u32 cb = (pb >> k) & 0xFFu;
			c |= (((ca * (256u - wt) + cb * wt + 128u) >> 8) & 0xFFu) << k;
		}
		sw_store(out_row, x, c);
	}
}

/* BOB into a rect of its own height (a slot of the adaptive buffer):
 * the source resampled into rows dy0 to dy1, the rest left as it is */
static void sw_bob_scaled(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch, int dst_w,
	int dst_h, int dy0, int dy1)
{
	const int w = pcsx2_min_i(src_w, dst_w);
	const float ratio = (float)src_h / (float)(dy1 - dy0);
	int y;
	for (y = pcsx2_max_i(dy0, 0); y < pcsx2_min_i(dy1, dst_h); y++)
		sw_sample_row(src, src_pitch, 0, src_h, ((float)(y - dy0) + 0.5f) * ratio, dst + (size_t)y * dst_pitch, w);
}

/* The weave of half-height fields: a line of the current field takes
 * its rows from its field's, which the merge drew twice each; the first
 * half of a line's rows its field's own, the second the other field's
 * places, the mean of the rows around them */
static void sw_weave_half(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch, int dst_w,
	int dst_h, int field, int block)
{
	const int w = pcsx2_min_i(src_w, dst_w);
	int y;
	for (y = 0; y < dst_h; y++)
	{
		const int   l     = y / block;
		const float place = (float)(y - l * block) * 0.5f;
		const float pf    = gs_floorf(place);
		const float row   = (float)((l >> 1) * block) + pf;
		if ((l & 1) != field)
			continue;
		sw_sample_row(src, src_pitch, 0, src_h, 2.0f * row + 0.5f + 3.0f * (place - pf), dst + (size_t)y * dst_pitch, w);
	}
}

static float sw_luma(u32 p)
{
	return ((float)(p & 0xFFu) * 0.299f + (float)((p >> 8) & 0xFFu) * 0.587f +
		(float)((p >> 16) & 0xFFu) * 0.114f) * (1.0f / 255.0f);
}

static float sw_absf(float f) { return f < 0.0f ? -f : f; }

/* mad_field: the row output row y reads from the field `back` fields
 * ago, with the linear sampler; at a scale of two or more a field's rows
 * each cover two output rows from its parity's first */
static void sw_mad_field_row(const u8* src, int src_pitch, int slot_h, int idx, int block, int back, int y, u8* out,
	int w)
{
	const int   slot = (idx - back) & 3;
	const int   par  = (idx ^ back) & 1;
	const float rel  = (float)(y - par * block) * 0.5f;
	float r = ((block >= 2) ? gs_floorf(rel) : rel) + 0.5f;
	if (r < 0.5f)
		r = 0.5f;
	if (r > (float)slot_h - 0.5f)
		r = (float)slot_h - 0.5f;
	sw_sample_row(src, src_pitch, slot * slot_h, (slot + 1) * slot_h, (float)(slot * slot_h) + r, out, w);
}

/* The adaptive reconstruction of half-height fields (mad_fields): the
 * buffer holds the last four fields whole, one to a quarter, the current
 * in slot idx. The current field's lines are taken as they are; between
 * them, the previous field's line where the picture holds still, the
 * current field's own picture where it moves, blended by the luma
 * motion, measured once a native pixel at its block's centre (at an
 * even scale the mean of the two pixels either side of it), of the lines
 * around against the field two back and of the line against its field
 * two back. scratch holds 8 rows and 7 floats a pixel. */
static void sw_mad_fields(const u8* src, int src_pitch, int src_w, int src_h, u8* dst, int dst_pitch, int dst_w,
	int dst_h, int idx, int block, u8* scratch)
{
	const int w      = pcsx2_min_i(src_w, dst_w);
	const int slot_h = src_h / 4;
	const int field  = idx & 1;
	u8* rows[8];
	float* moving = (float*)(scratch + (size_t)8 * w * 4);
	float* luma   = moving + w; /* rows 2 to 7, w each */
	int y, x, k, c;
	int line = -1;

	for (k = 0; k < 8; k++)
		rows[k] = scratch + (size_t)k * w * 4;

	for (y = 0; y < dst_h; y++)
	{
		const int l = y / block;
		u8* d = dst + (size_t)y * dst_pitch;

		sw_mad_field_row(src, src_pitch, slot_h, idx, block, 0, y, rows[0], w);
		if ((l & 1) == field)
		{
			memcpy(d, rows[0], (size_t)w * 4);
			continue;
		}

		if (l != line)
		{
			const int yl = l * block;
			line = l;
			sw_mad_field_row(src, src_pitch, slot_h, idx, block, 0, yl - block, rows[2], w);
			sw_mad_field_row(src, src_pitch, slot_h, idx, block, 2, yl - block, rows[3], w);
			sw_mad_field_row(src, src_pitch, slot_h, idx, block, 0, yl + block, rows[4], w);
			sw_mad_field_row(src, src_pitch, slot_h, idx, block, 2, yl + block, rows[5], w);
			sw_mad_field_row(src, src_pitch, slot_h, idx, block, 1, yl, rows[6], w);
			sw_mad_field_row(src, src_pitch, slot_h, idx, block, 3, yl, rows[7], w);
			for (k = 0; k < 6; k++)
				for (x = 0; x < w; x++)
					luma[k * w + x] = sw_luma(sw_load(rows[2 + k], x));
			for (x = 0; x < w; x++)
			{
				float lm[6], m, t;
				if (block == 1)
				{
					for (k = 0; k < 6; k++)
						lm[k] = luma[k * w + x];
				}
				else
				{
					const int x0 = (x / block) * block;
					const int xb = pcsx2_min_i(x0 + block / 2, w - 1);
					const int xa = (block & 1) ? xb : pcsx2_max_i(xb - 1, 0);
					for (k = 0; k < 6; k++)
						lm[k] = 0.5f * (luma[k * w + xa] + luma[k * w + xb]);
				}
				m = sw_absf(lm[0] - lm[1]);
				t = sw_absf(lm[2] - lm[3]);
				if (t > m)
					m = t;
				t = sw_absf(lm[4] - lm[5]);
				if (t > m)
					m = t;
				t = (m - 0.01f) / 0.02f;
				if (t < 0.0f)
					t = 0.0f;
				if (t > 1.0f)
					t = 1.0f;
				moving[x] = t * t * (3.0f - 2.0f * t);
			}
		}

		sw_mad_field_row(src, src_pitch, slot_h, idx, block, 1, y, rows[1], w);
		for (x = 0; x < w; x++)
		{
			const u32 cur  = sw_load(rows[0], x);
			const u32 prev = sw_load(rows[1], x);
			u32 out = 0;
			for (c = 0; c < 32; c += 8)
			{
				const float a = (float)((prev >> c) & 0xFFu);
				const float b = (float)((cur >> c) & 0xFFu);
				out |= ((u32)gs_f2i(a + (b - a) * moving[x] + 0.5f) & 0xFFu) << c;
			}
			sw_store(d, x, out);
		}
	}
}

/* ====================================================================
 * The device
 * ==================================================================== */

#define SW(d) ((gs_device_sw*)(d))

static gs_texture_sw* sw_tex(gs_texture* t) { return (gs_texture_sw*)t; }
#define TEX_W(t) ((t)->base.m_size.x)
#define TEX_H(t) ((t)->base.m_size.y)

/* (a rect's edge to a pixel, as the C++ cast did) */
static INLINE int sw_px(float f) { return gs_f2i(f); }
/* (rounded) */
static INLINE int sw_px_round(float f) { return gs_f2i(gs_floorf(f + 0.5f)); }

static void sw_dev_free(gs_device* d)
{
	gs_device_sw* dev = SW(d);
	gs_scratch_free(&dev->scratch_mad);
	memalign_free(dev->present_buffer);
	gs_device_deinit(d);
	memalign_free(dev);
}

static bool sw_dev_create(gs_device* d)
{
	gs_device_acquire_window(d);
	/* (nothing but the CPU: every feature off) */
	gs_device_features_init(&d->m_features);
	return true;
}

static void sw_dev_destroy(gs_device* d)
{
	gs_device_sw* dev = SW(d);
	gs_device_destroy_base(d);
	memalign_free(dev->present_buffer);
	dev->present_buffer             = NULL;
	dev->present_buffer_capacity    = 0;
	dev->present_pitch              = 0;
	dev->present_width              = 0;
	dev->present_height             = 0;
	dev->direct_rendering_checked   = false;
	dev->direct_rendering_supported = false;
	dev->direct_render_buffer       = NULL;
	dev->direct_render_pitch        = 0;
	dev->present_pending            = false;
}

static int sw_dev_get_render_api(const gs_device* d)
{
	(void)d;
	return GS_RENDER_API_NONE;
}

static int sw_dev_begin_present(gs_device* d, bool frame_skip)
{
	gs_device_sw* dev = SW(d);
	dev->present_pending      = false;
	dev->direct_render_buffer = NULL;
	dev->direct_render_pitch  = 0;
	return frame_skip ? GS_PRESENT_FRAME_SKIPPED : GS_PRESENT_OK;
}

static void sw_dev_end_present(gs_device* d)
{
	gs_device_sw* dev = SW(d);

	/* (nothing presented: the frames before the renderer made any) */
	if (!dev->present_pending || !video_cb)
		return;

	if (dev->direct_render_buffer && dev->direct_render_pitch > 0)
		video_cb(dev->direct_render_buffer, (unsigned)dev->present_width, (unsigned)dev->present_height,
			dev->direct_render_pitch);
	else if (dev->present_buffer && dev->present_pitch > 0)
		video_cb(dev->present_buffer, (unsigned)dev->present_width, (unsigned)dev->present_height,
			(size_t)dev->present_pitch);
}

static gs_texture* sw_dev_create_surface(gs_device* d, unsigned type, int w, int h, int levels, unsigned format)
{
	(void)d;
	(void)levels;
	return gs_texture_sw_create(type, w, h, format);
}

static void* sw_dev_create_download_texture(gs_device* d, u32 w, u32 h, unsigned format)
{
	(void)d;
	return gs_download_texture_sw_create(w, h, format);
}

static void sw_dev_do_merge(gs_device* d, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear)
{
	gs_texture_sw* dst;
	(void)d;
	(void)linear;
	/* (the feedback write, EXTBUF's loopback, is the HW renderers'; the
	 * SW renderer reads its feedback through its own output) */
	(void)EXTBUF;

	if (!dTex)
		return;
	dst = sw_tex(dTex);
	if (!dst->data)
		return;

	/* The background, BGCOLOR (0x00BBGGRR), opaque */
	sw_clear(dst->data, dst->pitch, TEX_W(dst), TEX_H(dst), (c & 0x00FFFFFFu) | 0xFF000000u);

	/* Circuit 1 copied over it */
	if (sTex[1] && PMODE->SLBG == 0)
	{
		const gs_texture_sw* src = sw_tex(sTex[1]);
		sw_nearest_copy(src->data, src->pitch, TEX_W(src), TEX_H(src), sRect[1].x, sRect[1].y, sRect[1].z, sRect[1].w,
			dst->data, dst->pitch, sw_px(dRect[1].x), sw_px(dRect[1].y), sw_px(dRect[1].z), sw_px(dRect[1].w),
			TEX_W(dst), TEX_H(dst), 0);
	}

	/* Circuit 0 blended over what is there */
	if (sTex[0])
	{
		const gs_texture_sw* src = sw_tex(sTex[0]);
		sw_blend_over(src->data, src->pitch, TEX_W(src), TEX_H(src), sRect[0].x, sRect[0].y, sRect[0].z, sRect[0].w,
			dst->data, dst->pitch, sw_px(dRect[0].x), sw_px(dRect[0].y), sw_px(dRect[0].z), sw_px(dRect[0].w),
			TEX_W(dst), TEX_H(dst), PMODE->MMOD == 1, (u8)((c >> 24) & 0xFFu));
	}
}

/* zrh (GSDevice::Interlace's): bufIdx (the field its parity), 1 / the
 * height, the height, and the rows a frame line takes, positive for
 * half-height fields and negative for whole ones */
static void sw_dev_do_interlace(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh)
{
	gs_device_sw* dev = SW(d);
	const gs_texture_sw* src;
	gs_texture_sw* dst;
	int field, y_shift, half_fields, block, src_w, src_h, dst_w, dst_h;
	(void)sRect;
	(void)linear;

	if (!sTex || !dTex)
		return;
	src = sw_tex(sTex);
	dst = sw_tex(dTex);
	if (!src->data || !dst->data)
		return;

	field       = sw_px(zrh->x) & 1;
	y_shift     = sw_px_round(dRect->y);
	half_fields = zrh->w > 0.0f;
	block       = pcsx2_max_i(1, sw_px((zrh->w < 0.0f ? -zrh->w : zrh->w) + 0.5f));
	src_w       = TEX_W(src);
	src_h       = TEX_H(src);
	dst_w       = TEX_W(dst);
	dst_h       = TEX_H(dst);

	switch (shader)
	{
		case 0: /* WEAVE */
			if (half_fields)
				sw_weave_half(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h, field, block);
			else
				sw_weave(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h, y_shift, field, block);
			break;

		case 1: /* BOB */
		{
			/* A rect of another height than the source is a slot of the
			 * adaptive buffer, which keeps the rest of it. */
			const int dy0 = sw_px_round(dRect->y);
			const int dy1 = sw_px_round(dRect->w);
			if (dy1 - dy0 != src_h && dy1 > dy0)
				sw_bob_scaled(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h, dy0, dy1);
			else
				sw_bob(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h, y_shift);
			break;
		}

		case 2: /* BLEND: the second pass, from the weave into another target */
			sw_blend_three(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h, block);
			break;

		case 3: /* MAD_BUFFER: the frame into the bank dRect is */
		{
			const int idx = sw_px(zrh->x);
			sw_mad_buffer(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h,
				sw_px_round(dRect->y), sw_px_round(dRect->w), field, (idx >> 1) & 1, sw_px(zrh->z) >> 1, block);
			break;
		}

		case 4: /* MAD_RECONSTRUCT */
		{
			const int idx = sw_px(zrh->x);
			if (half_fields)
			{
				u8* scratch = (u8*)gs_scratch_get(&dev->scratch_mad,
					(size_t)pcsx2_min_i(src_w, dst_w) * (4 * 8 + sizeof(float) * 7));
				if (scratch)
					sw_mad_fields(src->data, src->pitch, src_w, src_h, dst->data, dst->pitch, dst_w, dst_h, idx, block,
						scratch);
			}
			else
			{
				/* (the shader's sensitivity, 0.08 of full scale) */
				sw_mad_reconstruct(src->data, src->pitch, dst->data, dst->pitch, dst_w, dst_h, idx, 20, block);
			}
			break;
		}

		default:
			break;
	}
}

static void sw_dev_copy_rect(gs_device* d, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY)
{
	const gs_texture_sw* src;
	gs_texture_sw* dst;
	int row, col;
	(void)d;

	if (!sTex || !dTex)
		return;
	src = sw_tex(sTex);
	dst = sw_tex(dTex);
	if (!src->data || !dst->data || r->z - r->x <= 0 || r->w - r->y <= 0)
		return;

	for (row = 0; row < r->w - r->y; row++)
	{
		const int sy = r->y + row;
		const int dy = (int)destY + row;
		if (sy < 0 || sy >= TEX_H(src) || dy < 0 || dy >= TEX_H(dst))
			continue;
		for (col = 0; col < r->z - r->x; col++)
		{
			const int sx = r->x + col;
			const int dx = (int)destX + col;
			if (sx < 0 || sx >= TEX_W(src) || dx < 0 || dx >= TEX_W(dst))
				continue;
			sw_store(dst->data + (size_t)dy * dst->pitch, dx, sw_load(src->data + (size_t)sy * src->pitch, sx));
		}
	}
}

/* (only COPY is the SW renderer's: any shader is a plain copy here) */
static void sw_dev_stretch_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, int shader, bool linear)
{
	const gs_texture_sw* src;
	gs_texture_sw* dst;
	(void)d;
	(void)shader;
	(void)linear;

	if (!sTex || !dTex)
		return;
	src = sw_tex(sTex);
	dst = sw_tex(dTex);
	if (!src->data || !dst->data)
		return;
	sw_nearest_copy(src->data, src->pitch, TEX_W(src), TEX_H(src), sRect->x, sRect->y, sRect->z, sRect->w, dst->data,
		dst->pitch, sw_px(dRect->x), sw_px(dRect->y), sw_px(dRect->z), sw_px(dRect->w), TEX_W(dst), TEX_H(dst), 0);
}

/* (the HW renderers' channel mask: a plain stretch here) */
static void sw_dev_stretch_rect_mask(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader)
{
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
	sw_dev_stretch_rect(d, sTex, sRect, dTex, dRect, shader, true);
}

/* The frontend's buffer for this frame, where it offers one of this size
 * as XRGB8888; asked every frame (it is good for one), and again after
 * a no only when the size changes */
static bool sw_acquire_direct(gs_device_sw* dev, int width, int height)
{
	struct retro_framebuffer fb;

	if (!environ_cb)
		return false;

	if (!dev->direct_rendering_checked || width != dev->present_width || height != dev->present_height)
	{
		dev->direct_rendering_supported = true;
		dev->direct_rendering_checked   = true;
	}
	else if (!dev->direct_rendering_supported)
		return false;

	memset(&fb, 0, sizeof(fb));
	fb.width        = (unsigned)width;
	fb.height       = (unsigned)height;
	fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) || !fb.data
		|| fb.format != RETRO_PIXEL_FORMAT_XRGB8888 || fb.width != (unsigned)width || fb.height != (unsigned)height)
	{
		dev->direct_rendering_supported = false;
		return false;
	}

	dev->direct_render_buffer = (u8*)fb.data;
	dev->direct_render_pitch  = fb.pitch;
	return true;
}

/* (grown when the frame is: allocated when the output's size changes,
 * not a frame) */
static void sw_ensure_present_buffer(gs_device_sw* dev, int width, int height)
{
	const int pitch  = (int)(((u32)(width * 4) + GS_DEVICE_SW_ROW_ALIGN - 1) & ~(u32)(GS_DEVICE_SW_ROW_ALIGN - 1));
	const int needed = pitch * height;

	if (needed <= 0)
		return;

	if (needed > dev->present_buffer_capacity)
	{
		memalign_free(dev->present_buffer);
		dev->present_buffer          = (u8*)memalign_alloc(GS_DEVICE_SW_ROW_ALIGN, (size_t)needed);
		dev->present_buffer_capacity = dev->present_buffer ? needed : 0;
	}
	dev->present_pitch  = pitch;
	dev->present_width  = width;
	dev->present_height = height;
}

static void sw_dev_present_rect(gs_device* d, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
	const gs_rectf* dRect)
{
	gs_device_sw* dev = SW(d);
	gs_texture_sw* src;
	int blank, out_w, out_h, out_pitch;
	float sx0, sy0, sx1, sy1;
	u8* out;

	if (!sTex)
		return;
	src = sw_tex(sTex);
	if (!src->data)
		return;

	/* A source rect of (0, 0, 0, 0) blanks the frame (GSRenderer::VSync):
	 * the source cleared to opaque black and sampled whole, as the HW
	 * backends do */
	blank = sRect->z == 0.0f && sRect->w == 0.0f;
	if (blank)
		sw_clear(src->data, src->pitch, TEX_W(src), TEX_H(src), 0xFF000000u);
	sx0 = blank ? 0.0f : sRect->x;
	sy0 = blank ? 0.0f : sRect->y;
	sx1 = blank ? 1.0f : sRect->z;
	sy1 = blank ? 1.0f : sRect->w;

	if (dTex)
	{
		gs_texture_sw* dst = sw_tex(dTex);
		if (!dst->data)
			return;
		sw_nearest_copy(src->data, src->pitch, TEX_W(src), TEX_H(src), sx0, sy0, sx1, sy1, dst->data, dst->pitch,
			sw_px(dRect->x), sw_px(dRect->y), sw_px(dRect->z), sw_px(dRect->w), TEX_W(dst), TEX_H(dst), 0);
		return;
	}

	/* To the frontend: its buffer, or this device's, dRect's size */
	out_w = sw_px(dRect->z) - sw_px(dRect->x);
	out_h = sw_px(dRect->w) - sw_px(dRect->y);
	if (out_w <= 0 || out_h <= 0)
		return;

	if (sw_acquire_direct(dev, out_w, out_h))
	{
		out       = dev->direct_render_buffer;
		out_pitch = (int)dev->direct_render_pitch;
	}
	else
	{
		sw_ensure_present_buffer(dev, out_w, out_h);
		if (!dev->present_buffer)
			return;
		out       = dev->present_buffer;
		out_pitch = dev->present_pitch;
	}

	sw_nearest_copy(src->data, src->pitch, TEX_W(src), TEX_H(src), sx0, sy0, sx1, sy1, out, out_pitch, 0, 0, out_w,
		out_h, out_w, out_h, 1);

	dev->present_width   = out_w;
	dev->present_height  = out_h;
	dev->present_pending = true;
}

/* (the HW renderers' palette, indexed conversion and downsample) */
static void sw_dev_update_clut_texture(gs_device* d, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
	u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize)
{
	(void)d; (void)sTex; (void)sScale; (void)sSample; (void)offsetX; (void)offsetY; (void)dTex; (void)dOffset;
	(void)dSize;
}

static void sw_dev_convert_to_indexed_texture(gs_device* d, gs_texture* sTex, float sScale, u32 offsetX, u32 offsetY,
	u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM)
{
	(void)d; (void)sTex; (void)sScale; (void)offsetX; (void)offsetY; (void)SBW; (void)SPSM; (void)dTex; (void)DBW;
	(void)DPSM;
}

static void sw_dev_filtered_downsample_texture(gs_device* d, gs_texture* sTex, gs_texture* dTex, u32 factor,
	const gs_point* clamp_min, const gs_rectf* dRect)
{
	(void)d; (void)sTex; (void)dTex; (void)factor; (void)clamp_min; (void)dRect;
}

/* (a HW renderer wired to this device: a bug at init, not at run time) */
static void sw_dev_render_hw(gs_device* d, struct GSHWDrawConfig* config)
{
	(void)d;
	(void)config;
	log_cb(RETRO_LOG_ERROR, "GSDeviceSW: RenderHW called - the HW renderers are not compatible with the CPU-only device.\n");
}

/* NULL: GSDevice's own (no API state, no batched stretch, no texel move,
 * no samplers) */
const struct gs_device_ops gs_device_sw_ops = {
	sw_dev_free,
	sw_dev_create,
	sw_dev_destroy,
	NULL, /* reset_api_state */
	NULL, /* restore_api_state */
	sw_dev_get_render_api,
	sw_dev_begin_present,
	sw_dev_end_present,
	sw_dev_create_surface,
	sw_dev_create_download_texture,
	sw_dev_do_merge,
	sw_dev_do_interlace,
	sw_dev_copy_rect,
	sw_dev_stretch_rect,
	sw_dev_stretch_rect_mask,
	sw_dev_present_rect,
	NULL, /* draw_multi_stretch_rects */
	sw_dev_update_clut_texture,
	sw_dev_convert_to_indexed_texture,
	sw_dev_filtered_downsample_texture,
	NULL, /* move_texels: the software renderer moves in local memory */
	sw_dev_render_hw,
	NULL  /* clear_sampler_cache */
};

gs_device* gs_device_new_sw(void)
{
	gs_device_sw* dev = (gs_device_sw*)memalign_alloc(32, sizeof(gs_device_sw));
	if (!dev)
		return NULL;
	memset(dev, 0, sizeof(*dev));
	gs_device_init(&dev->base);
	dev->base.m_ops = &gs_device_sw_ops;
	return &dev->base;
}
