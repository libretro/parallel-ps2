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

#include <algorithm>
#include "common/Align.h"
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#include <libretro.h>

#include "common/VectorIntrin.h"
#include "GS/GSExtra.h"
#include "GS/Renderers/SW/GSDeviceSW.h"

extern retro_environment_t    environ_cb;
extern retro_video_refresh_t  video_cb;

namespace
{
	/* ====================================================================
	 * Inline pixel helpers
	 *
	 * Everything in this device operates on 32 bpp BGRA/XRGB8888-ish
	 * pixels. PS2 PSMCT32 stores ABGR (R in byte 0). The libretro
	 * XRGB8888 format the frontend expects has B in byte 0. The HW
	 * backends paper over this with shader swizzles; we have to do it
	 * by hand on the way to the present buffer.
	 *
	 * However, for intermediate GSTextureSW <-> GSTextureSW copies
	 * we keep the source byte order as-is - swizzling once, at present
	 * time, is sufficient and cheapest.
	 * ==================================================================== */

	/* One channel of s over d at alpha a (0..255): each product +128
	 * and >>8 on its own, the sum saturated, as the SSE code does. */
	__fi u32 BlendChannel(u32 s, u32 d, u32 a)
	{
		const u32 v = ((s * a + 128u) >> 8) + ((d * (255u - a) + 128u) >> 8);
		return v > 255u ? 255u : v;
	}

	/* PS2-ABGR s over d at alpha a, d's alpha kept. */
	__fi u32 BlendPx(u32 s, u32 d, u32 a)
	{
		return (d & 0xFF000000u)
			| (BlendChannel((s >> 16) & 0xFFu, (d >> 16) & 0xFFu, a) << 16)
			| (BlendChannel((s >>  8) & 0xFFu, (d >>  8) & 0xFFu, a) << 8)
			|  BlendChannel( s        & 0xFFu,  d        & 0xFFu, a);
	}

#if _M_SSE >= 0x401	/* x86 SSE 4.1 - the de-facto baseline for PCSX2
			 * (GSVector4i uses _mm_shuffle_epi8 unconditionally,
			 * which is SSSE3, and the build system sets _M_SSE
			 * to at least 0x401 in non-AVX2 configurations). */
#define GSDEVICESW_HAVE_SIMD 1
#else
#define GSDEVICESW_HAVE_SIMD 0
#endif

	__fi u32 LoadPx(const u8* row, int x) { return *reinterpret_cast<const u32*>(row + x * 4); }
	__fi void StorePx(u8* row, int x, u32 v) { *reinterpret_cast<u32*>(row + x * 4) = v; }

	/* PS2-native ABGR -> libretro XRGB8888 (B in byte 0). Forces
	 * alpha to 0xFF since the frontend doesn't blend output. */
	__fi u32 PS2ToXRGB(u32 abgr)
	{
		const u32 r = (abgr      ) & 0xFFu;
		const u32 g = (abgr >>  8) & 0xFFu;
		const u32 b = (abgr >> 16) & 0xFFu;
		return (0xFFu << 24) | (r << 16) | (g << 8) | b;
	}

	/* Clear a rect of `dst` (BGRA/XRGB8888 layout) to the given
	 * packed color. */
	void ClearRectPx(u8* dst, int dst_pitch, int dst_w, int dst_h, u32 color_xrgb)
	{
		for (int y = 0; y < dst_h; y++)
		{
			u32* row = reinterpret_cast<u32*>(dst + (size_t)y * dst_pitch);
			for (int x = 0; x < dst_w; x++)
				row[x] = color_xrgb;
		}
	}

	/* Nearest-neighbor copy from a source GSTextureSW region to a
	 * destination GSTextureSW region. Source and destination texels
	 * are bit-equal (no PS2->XRGB conversion); the conversion happens
	 * only when writing into the libretro present buffer. */
	void NearestCopyPS2(
		const u8* src, int src_pitch, int src_w, int src_h,
		float sx0, float sy0, float sx1, float sy1,
		u8* dst, int dst_pitch,
		int dx0, int dy0, int dx1, int dy1,
		int dst_clip_w, int dst_clip_h)
	{
		const int out_w = dx1 - dx0;
		const int out_h = dy1 - dy0;
		if (out_w <= 0 || out_h <= 0)
			return;

		/* Normalized UV -> source pixel coords. Add 0.5 to sample
		 * texel centers, matching nearest-neighbor texturing. */
		const float src_x_scale = (sx1 - sx0) * static_cast<float>(src_w) / static_cast<float>(out_w);
		const float src_y_scale = (sy1 - sy0) * static_cast<float>(src_h) / static_cast<float>(out_h);
		const float src_x_base  = sx0 * static_cast<float>(src_w);
		const float src_y_base  = sy0 * static_cast<float>(src_h);

		/* Fast path: x mapping is exact 1:1 and fully in bounds. The
		 * inner loop is then just a memcpy of dst_w pixels. */
		[[maybe_unused]] const bool x_is_identity = /* consumed only by the SIMD fast path */
			(sx0 == 0.0f) && (sx1 == 1.0f) && (out_w == src_w) &&
			(dx0 >= 0) && (dx1 <= dst_clip_w) && (dx1 <= src_w);

		for (int y = 0; y < out_h; y++)
		{
			const int dy = dy0 + y;
			if (dy < 0 || dy >= dst_clip_h)
				continue;

			int sy = static_cast<int>(src_y_base + (static_cast<float>(y) + 0.5f) * src_y_scale);
			if (sy < 0)         sy = 0;
			else if (sy >= src_h) sy = src_h - 1;

			const u8* src_row = src + (size_t)sy * src_pitch;
			u8* dst_row       = dst + (size_t)dy * dst_pitch;

			if (x_is_identity)
			{
				memcpy(dst_row + dx0 * 4, src_row + dx0 * 4,
					(size_t)out_w * 4);
				continue;
			}

			for (int x = 0; x < out_w; x++)
			{
				const int dx = dx0 + x;
				if (dx < 0 || dx >= dst_clip_w)
					continue;

				int sx = static_cast<int>(src_x_base + (static_cast<float>(x) + 0.5f) * src_x_scale);
				if (sx < 0)         sx = 0;
				else if (sx >= src_w) sx = src_w - 1;

				StorePx(dst_row, dx, LoadPx(src_row, sx));
			}
		}
	}

	/* Variant that converts source ABGR (PS2 native) -> XRGB8888 on
	 * write. Used by PresentRect to fill the libretro framebuffer. */
	void NearestCopyToXRGB(
		const u8* src, int src_pitch, int src_w, int src_h,
		float sx0, float sy0, float sx1, float sy1,
		u8* dst, int dst_pitch,
		int dx0, int dy0, int dx1, int dy1,
		int dst_clip_w, int dst_clip_h)
	{
		const int out_w = dx1 - dx0;
		const int out_h = dy1 - dy0;
		if (out_w <= 0 || out_h <= 0)
			return;

		const float src_x_scale = (sx1 - sx0) * static_cast<float>(src_w) / static_cast<float>(out_w);
		const float src_y_scale = (sy1 - sy0) * static_cast<float>(src_h) / static_cast<float>(out_h);
		const float src_x_base  = sx0 * static_cast<float>(src_w);
		const float src_y_base  = sy0 * static_cast<float>(src_h);

		[[maybe_unused]] const bool x_is_identity = /* consumed only by the SIMD fast path */
			(sx0 == 0.0f) && (sx1 == 1.0f) && (out_w == src_w) &&
			(dx0 >= 0) && (dx1 <= dst_clip_w) && (dx1 <= src_w);

		for (int y = 0; y < out_h; y++)
		{
			const int dy = dy0 + y;
			if (dy < 0 || dy >= dst_clip_h)
				continue;

			int sy = static_cast<int>(src_y_base + (static_cast<float>(y) + 0.5f) * src_y_scale);
			if (sy < 0)         sy = 0;
			else if (sy >= src_h) sy = src_h - 1;

			const u8* src_row = src + (size_t)sy * src_pitch;
			u8* dst_row       = dst + (size_t)dy * dst_pitch;

#if GSDEVICESW_HAVE_SIMD
			if (x_is_identity)
			{
				/* pshufb: each quad maps ABGR -> XRGB:
				 *   out byte 0 = src byte 2 (B)
				 *   out byte 1 = src byte 1 (G)
				 *   out byte 2 = src byte 0 (R)
				 *   out byte 3 = 0x80 = "output zero" in pshufb
				 *                       semantics (high bit set).
				 * The opaque alpha 0xFF is OR'd in after. */
				const __m128i shuf = _mm_setr_epi8(
					(char)2, (char)1, (char)0, (char)0x80,
					(char)6, (char)5, (char)4, (char)0x80,
					(char)10, (char)9, (char)8, (char)0x80,
					(char)14, (char)13, (char)12, (char)0x80);
				const __m128i alpha_or = _mm_set1_epi32((int)0xFF000000);

				int x = 0;
				for (; x + 4 <= out_w; x += 4)
				{
					__m128i src_q = _mm_loadu_si128(
						reinterpret_cast<const __m128i*>(src_row + (dx0 + x) * 4));
					__m128i out_q = _mm_or_si128(_mm_shuffle_epi8(src_q, shuf), alpha_or);
					_mm_storeu_si128(
						reinterpret_cast<__m128i*>(dst_row + (dx0 + x) * 4), out_q);
				}
				/* Tail. */
				for (; x < out_w; x++)
				{
					const int dx = dx0 + x;
					StorePx(dst_row, dx, PS2ToXRGB(LoadPx(src_row, dx)));
				}
				continue;
			}
#endif

			for (int x = 0; x < out_w; x++)
			{
				const int dx = dx0 + x;
				if (dx < 0 || dx >= dst_clip_w)
					continue;

				int sx = static_cast<int>(src_x_base + (static_cast<float>(x) + 0.5f) * src_x_scale);
				if (sx < 0)         sx = 0;
				else if (sx >= src_w) sx = src_w - 1;

				StorePx(dst_row, dx, PS2ToXRGB(LoadPx(src_row, sx)));
			}
		}
	}

	/* Alpha-blend a source GSTextureSW region onto a destination
	 * region. Per-source-pixel alpha or a constant alpha is used:
	 *   - mode_constant_alpha=true: alpha8 is the same for every pixel
	 *     (PMODE.MMOD == 1, AKA constant alpha from PMODE.ALP).
	 *   - mode_constant_alpha=false: alpha8 ignored; per-pixel alpha
	 *     read from the source.
	 *
	 * PS2 PSMCT32 alpha is stored as 0..0x80, where 0x80 represents
	 * "fully opaque" (1.0 in shader space). Anything above 0x80
	 * saturates. The HW backends paper over this in the merge
	 * pixel shader by computing alpha * 2 with saturation; we do
	 * the same here. PMODE.ALP (constant alpha) follows the same
	 * convention. */

#if GSDEVICESW_HAVE_SIMD
	/* Blend 4 PS2-ABGR pixels at a time. For each pixel:
	 *   out_rgb = (src_rgb * a + dst_rgb * (255 - a) + 128) / 256
	 *   out_a   = dst_a (preserved)
	 *
	 * `alpha_lo`/`alpha_hi` hold 8 u16 lanes each, with the same
	 * alpha byte broadcast across the four channels of each pixel.
	 * Caller is responsible for the doubling/saturation of PS2's
	 * 0..0x80 alpha encoding before splatting into these vectors.
	 * `inv_alpha_lo`/`inv_alpha_hi` = 255 - alpha. */
	__fi __m128i BlendQuadSSE(__m128i src_q, __m128i dst_q,
		__m128i alpha_lo, __m128i alpha_hi,
		__m128i inv_alpha_lo, __m128i inv_alpha_hi)
	{
		const __m128i zero       = _mm_setzero_si128();
		const __m128i bias       = _mm_set1_epi16(128);
		const __m128i alpha_mask = _mm_set1_epi32(0xFF000000);

		__m128i src_lo = _mm_unpacklo_epi8(src_q, zero);
		__m128i src_hi = _mm_unpackhi_epi8(src_q, zero);
		__m128i dst_lo = _mm_unpacklo_epi8(dst_q, zero);
		__m128i dst_hi = _mm_unpackhi_epi8(dst_q, zero);

		__m128i sa_lo = _mm_mullo_epi16(src_lo, alpha_lo);
		__m128i sa_hi = _mm_mullo_epi16(src_hi, alpha_hi);
		__m128i di_lo = _mm_mullo_epi16(dst_lo, inv_alpha_lo);
		__m128i di_hi = _mm_mullo_epi16(dst_hi, inv_alpha_hi);

		/* Each product +128, >>8, as BlendPx. */
		sa_lo = _mm_srli_epi16(_mm_add_epi16(sa_lo, bias), 8);
		sa_hi = _mm_srli_epi16(_mm_add_epi16(sa_hi, bias), 8);
		di_lo = _mm_srli_epi16(_mm_add_epi16(di_lo, bias), 8);
		di_hi = _mm_srli_epi16(_mm_add_epi16(di_hi, bias), 8);

		__m128i out_lo = _mm_add_epi16(sa_lo, di_lo);
		__m128i out_hi = _mm_add_epi16(sa_hi, di_hi);
		__m128i out    = _mm_packus_epi16(out_lo, out_hi);

		/* Replace alpha with the dst's alpha byte. */
		return _mm_or_si128(
			_mm_andnot_si128(alpha_mask, out),
			_mm_and_si128(alpha_mask, dst_q));
	}

	/* Build (alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi) for a
	 * quad of constant-alpha pixels: every channel of every pixel
	 * gets the same broadcast alpha (already doubled / saturated by
	 * the caller). */
	__fi void BuildConstantAlphaVectors(u8 alpha_doubled,
		__m128i& alpha_lo, __m128i& alpha_hi,
		__m128i& inv_alpha_lo, __m128i& inv_alpha_hi)
	{
		alpha_lo = alpha_hi = _mm_set1_epi16((short)alpha_doubled);
		const u16 inv = (u16)(255u - alpha_doubled);
		inv_alpha_lo = inv_alpha_hi = _mm_set1_epi16((short)inv);
	}

	/* Build the same four vectors for per-pixel alpha taken from
	 * the source quad. PS2 alpha 0..0x80 -> 0..0xFF via saturated
	 * self-add (_mm_adds_epu8). */
	__fi void BuildPerPixelAlphaVectors(__m128i src_q,
		__m128i& alpha_lo, __m128i& alpha_hi,
		__m128i& inv_alpha_lo, __m128i& inv_alpha_hi)
	{
		/* Splat each pixel's alpha byte (positions 3,7,11,15) into
		 * all four channels of that pixel. */
		const __m128i splat_mask = _mm_setr_epi8(
			3, 3, 3, 3,
			7, 7, 7, 7,
			11, 11, 11, 11,
			15, 15, 15, 15);

		__m128i a8  = _mm_shuffle_epi8(src_q, splat_mask);
		a8          = _mm_adds_epu8(a8, a8);   /* x2 with saturation */
		__m128i ia8 = _mm_sub_epi8(_mm_set1_epi8((char)255), a8);

		const __m128i zero = _mm_setzero_si128();
		alpha_lo     = _mm_unpacklo_epi8(a8,  zero);
		alpha_hi     = _mm_unpackhi_epi8(a8,  zero);
		inv_alpha_lo = _mm_unpacklo_epi8(ia8, zero);
		inv_alpha_hi = _mm_unpackhi_epi8(ia8, zero);
	}
#endif

	void BlendOverPS2(
		const u8* src, int src_pitch, int src_w, int src_h,
		float sx0, float sy0, float sx1, float sy1,
		u8* dst, int dst_pitch,
		int dx0, int dy0, int dx1, int dy1,
		int dst_clip_w, int dst_clip_h,
		bool mode_constant_alpha, u8 alpha8)
	{
		const int out_w = dx1 - dx0;
		const int out_h = dy1 - dy0;
		if (out_w <= 0 || out_h <= 0)
			return;

		const float src_x_scale = (sx1 - sx0) * static_cast<float>(src_w) / static_cast<float>(out_w);
		const float src_y_scale = (sy1 - sy0) * static_cast<float>(src_h) / static_cast<float>(out_h);
		const float src_x_base  = sx0 * static_cast<float>(src_w);
		const float src_y_base  = sy0 * static_cast<float>(src_h);

		/* Pre-compute the doubled constant alpha (clamped to 255). */
		const u32 const_a2 = pcsx2_min_u(static_cast<u32>(alpha8) * 2u, 255u);

		/* Fast-path condition: x mapping is exactly 1:1 src->dst with
		 * no horizontal stretch, output range fully inside both src
		 * and dst clip rects. Every DoMerge call from the SW renderer
		 * hits this in practice (sRect=(0,0,1,1) and dRect matches the
		 * merge target's full extent). */
		[[maybe_unused]] const bool x_is_identity = /* consumed only by the SIMD fast path */
			(sx0 == 0.0f) && (sx1 == 1.0f) && (out_w == src_w) &&
			(dx0 >= 0) && (dx1 <= dst_clip_w) && (dx1 <= src_w);

		for (int y = 0; y < out_h; y++)
		{
			const int dy = dy0 + y;
			if (dy < 0 || dy >= dst_clip_h)
				continue;

			int sy = static_cast<int>(src_y_base + (static_cast<float>(y) + 0.5f) * src_y_scale);
			if (sy < 0)         sy = 0;
			else if (sy >= src_h) sy = src_h - 1;

			const u8* src_row = src + (size_t)sy * src_pitch;
			u8* dst_row       = dst + (size_t)dy * dst_pitch;

#if GSDEVICESW_HAVE_SIMD
			if (x_is_identity)
			{
				int x = 0;

				if (mode_constant_alpha)
				{
					__m128i alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi;
					BuildConstantAlphaVectors((u8)const_a2,
						alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi);

					for (; x + 4 <= out_w; x += 4)
					{
						__m128i src_q = _mm_loadu_si128(
							reinterpret_cast<const __m128i*>(src_row + (dx0 + x) * 4));
						__m128i dst_q = _mm_loadu_si128(
							reinterpret_cast<const __m128i*>(dst_row + (dx0 + x) * 4));
						__m128i out_q = BlendQuadSSE(src_q, dst_q,
							alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi);
						_mm_storeu_si128(
							reinterpret_cast<__m128i*>(dst_row + (dx0 + x) * 4), out_q);
					}
				}
				else
				{
					for (; x + 4 <= out_w; x += 4)
					{
						__m128i src_q = _mm_loadu_si128(
							reinterpret_cast<const __m128i*>(src_row + (dx0 + x) * 4));
						__m128i dst_q = _mm_loadu_si128(
							reinterpret_cast<const __m128i*>(dst_row + (dx0 + x) * 4));
						__m128i alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi;
						BuildPerPixelAlphaVectors(src_q,
							alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi);
						__m128i out_q = BlendQuadSSE(src_q, dst_q,
							alpha_lo, alpha_hi, inv_alpha_lo, inv_alpha_hi);
						_mm_storeu_si128(
							reinterpret_cast<__m128i*>(dst_row + (dx0 + x) * 4), out_q);
					}
				}

				/* Scalar tail for the < 4 leftover pixels. */
				for (; x < out_w; x++)
				{
					const int dx = dx0 + x;
					const u32 s = LoadPx(src_row, dx);
					const u32 d = LoadPx(dst_row, dx);
					u32 a;
					if (mode_constant_alpha)
						a = const_a2;
					else
					{
						const u32 raw = (s >> 24) & 0xFFu;
						a = pcsx2_min_u(raw * 2u, 255u);
					}
					StorePx(dst_row, dx, BlendPx(s, d, a));
				}
				continue;
			}
#endif

			/* Scalar / stretched / clipped fallback. */
			for (int x = 0; x < out_w; x++)
			{
				const int dx = dx0 + x;
				if (dx < 0 || dx >= dst_clip_w)
					continue;

				int sx = static_cast<int>(src_x_base + (static_cast<float>(x) + 0.5f) * src_x_scale);
				if (sx < 0)         sx = 0;
				else if (sx >= src_w) sx = src_w - 1;

				const u32 s = LoadPx(src_row, sx);
				const u32 d = LoadPx(dst_row, dx);

				u32 a;
				if (mode_constant_alpha)
					a = const_a2;
				else
				{
					const u32 raw = (s >> 24) & 0xFFu;
					a = pcsx2_min_u(raw * 2u, 255u);
				}
				StorePx(dst_row, dx, BlendPx(s, d, a));
			}
		}
	}

	/* ====================================================================
	 * Deinterlace helpers
	 *
	 * All three operate on full-frame GSTextureSW buffers. Source rect
	 * for the deinterlacers is always (0,0,1,1) and destination rect
	 * covers the destination texture (possibly shifted by a fractional
	 * line for weave/bob's field offset, which we round to whole
	 * pixels - the SW path doesn't have a way to do sub-pixel filtering
	 * cheaply and the visual difference is one row of vertical jitter).
	 * ==================================================================== */

	/* WeaveCopy: take the rows of the lines where (line & 1) == field,
	 * a line `block` rows, from `src` and
	 * write them to `dst` at the corresponding position. Lines that
	 * don't match are left UNTOUCHED (they hold the previous frame's
	 * field, providing the woven full-frame image). dst must have
	 * had its previous-frame content preserved by ResizeRenderTarget
	 * with preserve_contents=true. */
	void WeaveCopy(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int y_shift, int field, int block)
	{
		const int copy_w = pcsx2_min_i(src_w, dst_w);
		const int copy_h = pcsx2_min_i(src_h, dst_h);

		for (int y = 0; y < copy_h; y++)
		{
			const int dy = y + y_shift;
			if (dy < 0 || dy >= dst_h)
				continue;
			if (((dy / block) & 1) != field)
				continue;

			memcpy(
				dst + (size_t)dy * dst_pitch,
				src + (size_t)y * src_pitch,
				(size_t)copy_w * 4);
		}
	}

	/* BobCopy: copy src into dst shifted vertically by y_shift,
	 * filling unwritten rows with opaque black (no preservation -
	 * unlike weave, bob is a clean per-field render). */
	void BobCopy(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int y_shift)
	{
		const int copy_w = pcsx2_min_i(src_w, dst_w);

		/* Black-fill any destination rows we won't overwrite. With
		 * y_shift=0..1 in normal cases this is at most one row at
		 * top or bottom; with larger shifts more rows could be left
		 * unwritten. Use the same opaque-black sentinel
		 * 0xFF000000 to keep the field consistent. */
		const int copy_start = pcsx2_max_i(0, y_shift);
		const int copy_end   = pcsx2_min_i(dst_h, src_h + y_shift);

		if (copy_start > 0)
			ClearRectPx(dst, dst_pitch, dst_w, copy_start, 0xFF000000u);
		if (copy_end < dst_h)
			ClearRectPx(dst + (size_t)copy_end * dst_pitch, dst_pitch,
				dst_w, dst_h - copy_end, 0xFF000000u);

		for (int dy = copy_start; dy < copy_end; dy++)
		{
			const int sy = dy - y_shift;
			memcpy(
				dst + (size_t)dy * dst_pitch,
				src + (size_t)sy * src_pitch,
				(size_t)copy_w * 4);
		}
	}

	/* BlendThree: each output row = (above + 2*center + below) / 4,
	 * above and below a line (`block` rows) away.
	 * Source and destination are the same size; we read across rows
	 * of `src` and write to corresponding rows of `dst`. Used as the
	 * second pass of the Blend deinterlace mode, run on the output
	 * of WeaveCopy. */
	void BlendThree(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h, int block)
	{
		const int copy_w = pcsx2_min_i(src_w, dst_w);
		const int copy_h = pcsx2_min_i(src_h, dst_h);

		for (int y = 0; y < copy_h; y++)
		{
			const int y_above = pcsx2_max_i(y - block, 0);
			const int y_below = pcsx2_min_i(y + block, src_h - 1);

			const u8* row_above = src + (size_t)y_above * src_pitch;
			const u8* row_cent  = src + (size_t)y       * src_pitch;
			const u8* row_below = src + (size_t)y_below * src_pitch;
			u8* dst_row         = dst + (size_t)y       * dst_pitch;

			int x = 0;
#if GSDEVICESW_HAVE_SIMD
			/* 4-pixel SSE2 inner loop. Unpack to u16 per channel,
			 * compute (a + 2c + b + 2) >> 2, repack. Alpha goes
			 * through the same math but is then overwritten with
			 * the center row's alpha (matches the scalar path:
			 * "(c & 0xFF000000u) | (...)"). */
			const __m128i zero       = _mm_setzero_si128();
			const __m128i two        = _mm_set1_epi16(2);
			const __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000);

			for (; x + 4 <= copy_w; x += 4)
			{
				__m128i a = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_above + x * 4));
				__m128i c = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_cent  + x * 4));
				__m128i b = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_below + x * 4));

				__m128i a_lo = _mm_unpacklo_epi8(a, zero);
				__m128i a_hi = _mm_unpackhi_epi8(a, zero);
				__m128i c_lo = _mm_unpacklo_epi8(c, zero);
				__m128i c_hi = _mm_unpackhi_epi8(c, zero);
				__m128i b_lo = _mm_unpacklo_epi8(b, zero);
				__m128i b_hi = _mm_unpackhi_epi8(b, zero);

				/* sum_lo = a + 2c + b + 2; max value per lane ~=
				 * 255 + 510 + 255 + 2 = 1022, fits in 16 bits. */
				__m128i s_lo = _mm_add_epi16(
					_mm_add_epi16(a_lo, b_lo),
					_mm_add_epi16(_mm_slli_epi16(c_lo, 1), two));
				__m128i s_hi = _mm_add_epi16(
					_mm_add_epi16(a_hi, b_hi),
					_mm_add_epi16(_mm_slli_epi16(c_hi, 1), two));

				s_lo = _mm_srli_epi16(s_lo, 2);
				s_hi = _mm_srli_epi16(s_hi, 2);

				__m128i out = _mm_packus_epi16(s_lo, s_hi);

				/* Replace alpha with the center row's alpha. */
				out = _mm_or_si128(
					_mm_andnot_si128(alpha_mask, out),
					_mm_and_si128(alpha_mask, c));

				_mm_storeu_si128(
					reinterpret_cast<__m128i*>(dst_row + x * 4), out);
			}
#endif
			for (; x < copy_w; x++)
			{
				const u32 a = LoadPx(row_above, x);
				const u32 c = LoadPx(row_cent,  x);
				const u32 b = LoadPx(row_below, x);

				const u32 ar = (a      ) & 0xFFu, ag = (a >>  8) & 0xFFu, ab = (a >> 16) & 0xFFu;
				const u32 cr = (c      ) & 0xFFu, cg = (c >>  8) & 0xFFu, cb = (c >> 16) & 0xFFu;
				const u32 br = (b      ) & 0xFFu, bg = (b >>  8) & 0xFFu, bb_ = (b >> 16) & 0xFFu;

				/* (above + 2*center + below + 2) / 4, with rounding. */
				const u32 r = (ar + cr * 2u + br  + 2u) >> 2;
				const u32 g = (ag + cg * 2u + bg  + 2u) >> 2;
				const u32 bl= (ab + cb * 2u + bb_ + 2u) >> 2;

				StorePx(dst_row, x, (c & 0xFF000000u) | (bl << 16) | (g << 8) | r);
			}
		}
	}

	/* ====================================================================
	 * FastMAD (Motion Adaptive Deinterlacing)
	 *
	 * Two-pass algorithm translated from interlace.glsl's ps_main3 +
	 * ps_main4. m_mad is sized (frame_w, 2*frame_h) and split into
	 * two banks (top half = bank 0, bottom half = bank 1), each
	 * holding two interleaved PS2 fields. The reconstruct pass reads
	 * across both banks to compute per-pixel motion against the
	 * previous frame and chooses weave vs. linear interpolation.
	 *
	 * The four idx values 0..3 cycle through which bank holds the
	 * newest/oldest data. GSDevice::Interlace manages the rotation
	 * via a function-static bufIdx counter; we just read cb.ZrH.x
	 * and decode bank = idx >> 1, field = idx & 1.
	 * ==================================================================== */

	/* MAD_BUFFER pass: store one field of the current source frame
	 * (m_merge, height = frame_h) into one bank of m_mad (height =
	 * 2*frame_h). Each call writes only the half of m_mad that
	 * corresponds to the current bank, with a 1:1 source-to-dest
	 * row mapping within that half. GSDevice::Interlace restricts
	 * dRect to either the top half (bank 0) or bottom half (bank 1)
	 * of m_mad before invoking this op; we honor those bounds.
	 *
	 * Half the rows of the bank's half are written (the rows whose
	 * parity matches the current field); the other half are left
	 * untouched so they hold previous frames' content. */
	void MadBuffer(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int dy_begin, int dy_end, int field, int bank, int vres, int block)
	{
		const int copy_w = pcsx2_min_i(src_w, dst_w);

		/* lofs corrects parity for odd vres in bank 1 (the shader's
		 * standard formula). 0 in all other cases. */
		const int lofs = (((((vres + 1) >> 1) << 1) - vres) & bank);

		/* Within the bank's half: src row N maps to dst row
		 * (dy_begin + N). Half of those rows pass the parity test
		 * and get written. */
		for (int n = 0; n < src_h; n++)
		{
			const int dst_y = dy_begin + n;
			if (dst_y < 0 || dst_y >= dst_h || dst_y >= dy_end)
				continue;

			const int vpos = dst_y + lofs;
			if (((vpos / block) & 1) != field)
				continue;

			memcpy(
				dst + (size_t)dst_y * dst_pitch,
				src + (size_t)n * src_pitch,
				(size_t)copy_w * 4);
		}
	}

	/* Compute per-channel "motion" between two PS2-ABGR pixels:
	 * abs(new - old) per channel, clamped to non-negative after
	 * subtracting a sensitivity threshold. Returns the max of the
	 * three RGB channel values (matches the GPU shader's #if 1
	 * branch which evaluates each color separately). */
	__fi int PixelMotion(u32 newer, u32 older, int threshold)
	{
		const int nr = (int)((newer      ) & 0xFFu);
		const int ng = (int)((newer >>  8) & 0xFFu);
		const int nb = (int)((newer >> 16) & 0xFFu);
		const int or_ = (int)((older      ) & 0xFFu);
		const int og = (int)((older >>  8) & 0xFFu);
		const int ob = (int)((older >> 16) & 0xFFu);

		int dr = nr - or_; if (dr < 0) dr = -dr;
		int dg = ng - og;  if (dg < 0) dg = -dg;
		int db = nb - ob;  if (db < 0) db = -db;

		dr -= threshold;
		dg -= threshold;
		db -= threshold;

		int m = dr;
		if (dg > m) m = dg;
		if (db > m) m = db;
		return m;
	}

	/* Average two PS2-ABGR pixels per channel (alpha taken from
	 * `a`'s alpha byte to keep things simple - alpha is unused
	 * downstream of the reconstruct). */
	__fi u32 AveragePixels(u32 a, u32 b)
	{
		const u32 ar = (a      ) & 0xFFu, ag = (a >>  8) & 0xFFu, ab_ = (a >> 16) & 0xFFu;
		const u32 br = (b      ) & 0xFFu, bg = (b >>  8) & 0xFFu, bb_ = (b >> 16) & 0xFFu;
		const u32 r = (ar + br + 1u) >> 1;
		const u32 g = (ag + bg + 1u) >> 1;
		const u32 bl= (ab_ + bb_ + 1u) >> 1;
		return (a & 0xFF000000u) | (bl << 16) | (g << 8) | r;
	}

	/* MAD_RECONSTRUCT pass: read m_mad (height = 2*frame_h) and
	 * write to m_weavebob (height = frame_h). For each output row
	 * decide between three policies:
	 *   - row parity matches current field: row exists in this
	 *     frame, sample p_t0 (newest).
	 *   - row at top or bottom edge: weave with p_t1 (no neighbors
	 *     to interpolate from).
	 *   - otherwise: compute motion against p_t2/p_t3 (previous
	 *     frame). If any of three vertically-adjacent pixels has
	 *     motion above the sensitivity threshold, interpolate
	 *     (hn+ln)/2; otherwise weave with cn. */
	void MadReconstruct(
		const u8* src, int src_pitch, int /*src_w*/, int /*src_h*/,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int idx, int sensitivity_u8, int block)
	{
		const int field    = idx & 1;
		const int frame_h  = dst_h;       /* = src_h / 2, the bank height */

		/* p_t0..p_t3 each resolve to one of two bank-base offsets
		 * in m_mad: 0 (bank 0, top half) or frame_h (bank 1, bottom
		 * half). The four-case table directly mirrors the GPU
		 * shader's switch(idx) block. */
		int t0_base, t1_base, t2_base, t3_base;
		switch (idx)
		{
			case 1:
				t0_base = 0;
				t1_base = 0;
				t2_base = frame_h;
				t3_base = frame_h;
				break;
			case 2:
				t0_base = frame_h;
				t1_base = 0;
				t2_base = 0;
				t3_base = frame_h;
				break;
			case 3:
				t0_base = frame_h;
				t1_base = frame_h;
				t2_base = 0;
				t3_base = 0;
				break;
			default:	/* case 0 */
				t0_base = 0;
				t1_base = frame_h;
				t2_base = frame_h;
				t3_base = 0;
				break;
		}

		for (int dst_y = 0; dst_y < dst_h; dst_y++)
		{
			const int p_t0_row = dst_y + t0_base;
			const int p_t1_row = dst_y + t1_base;
			const int p_t2_row = dst_y + t2_base;
			const int p_t3_row = dst_y + t3_base;

			const u8* row_t0   = src + (size_t)p_t0_row * src_pitch;
			const u8* row_t1   = src + (size_t)p_t1_row * src_pitch;
			const u8* row_t3   = src + (size_t)p_t3_row * src_pitch;

			/* For the +/- 1 neighbors of t0/t2, clamp to bank
			 * boundaries: each bank is frame_h rows tall, so the
			 * "above" of the top bank row stays in the same bank
			 * (clamped). The shader doesn't explicitly clamp - it
			 * relies on the texture's wrap mode - but since
			 * neighbors are only consulted for non-edge rows
			 * (vpos in (0, frame_h-1)) the clamping is moot
			 * there anyway. We clamp defensively. */
			const int t0_up_row   = pcsx2_max_i(t0_base, p_t0_row - block);
			const int t0_down_row = pcsx2_min_i(t0_base + frame_h - 1, p_t0_row + block);
			const int t2_up_row   = pcsx2_max_i(t2_base, p_t2_row - block);
			const int t2_down_row = pcsx2_min_i(t2_base + frame_h - 1, p_t2_row + block);

			const u8* row_t0_up   = src + (size_t)t0_up_row   * src_pitch;
			const u8* row_t0_down = src + (size_t)t0_down_row * src_pitch;
			const u8* row_t2_up   = src + (size_t)t2_up_row   * src_pitch;
			const u8* row_t2_down = src + (size_t)t2_down_row * src_pitch;

			u8* dst_row = dst + (size_t)dst_y * dst_pitch;

			const bool current_field_row = (((dst_y / block) & 1) == field);
			const bool edge_row          = (dst_y < block || dst_y >= dst_h - block);

			if (current_field_row)
			{
				/* Row exists in current field - take it. */
				memcpy(dst_row, row_t0, (size_t)dst_w * 4);
				continue;
			}

			if (edge_row)
			{
				/* Top or bottom row - weave (no neighbors to
				 * interpolate from). */
				memcpy(dst_row, row_t1, (size_t)dst_w * 4);
				continue;
			}

			/* Per-pixel motion-adaptive blend. */
			int x = 0;
#if GSDEVICESW_HAVE_SIMD
			/* 4-pixel SSE2 inner loop. Saturated unsigned ops let us
			 * compute abs(a-b) without sign tracking:
			 *   absdiff = max(subs(a,b), subs(b,a))   (both saturated)
			 *   motion  = subs(absdiff, threshold)    (>0 = motion)
			 * Then OR the three motion vectors (alpha byte masked
			 * out so we test only RGB) and per-pixel cmpeq with zero
			 * gives us "no motion" mask. Final select between
			 * avg(hn,ln) and cn using that mask. */
			const __m128i thr_v   = _mm_set1_epi8((char)sensitivity_u8);
			const __m128i rgb_msk = _mm_set1_epi32(0x00FFFFFF);
			const __m128i zero_v  = _mm_setzero_si128();

			for (; x + 4 <= dst_w; x += 4)
			{
				__m128i hn = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_t0_up   + x * 4));
				__m128i cn = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_t1      + x * 4));
				__m128i ln = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_t0_down + x * 4));
				__m128i ho = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_t2_up   + x * 4));
				__m128i co = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_t3      + x * 4));
				__m128i lo = _mm_loadu_si128(
					reinterpret_cast<const __m128i*>(row_t2_down + x * 4));

				__m128i dh = _mm_or_si128(_mm_subs_epu8(hn, ho), _mm_subs_epu8(ho, hn));
				__m128i dc = _mm_or_si128(_mm_subs_epu8(cn, co), _mm_subs_epu8(co, cn));
				__m128i dl = _mm_or_si128(_mm_subs_epu8(ln, lo), _mm_subs_epu8(lo, ln));

				__m128i mh = _mm_subs_epu8(dh, thr_v);
				__m128i mc = _mm_subs_epu8(dc, thr_v);
				__m128i ml = _mm_subs_epu8(dl, thr_v);

				/* Combine and mask alpha so the cmpeq is RGB-only. */
				__m128i motion = _mm_and_si128(
					_mm_or_si128(_mm_or_si128(mh, mc), ml),
					rgb_msk);
				/* 0xFFFFFFFF per pixel where no RGB motion. */
				__m128i still = _mm_cmpeq_epi32(motion, zero_v);

				__m128i avg = _mm_avg_epu8(hn, ln);
				__m128i out = _mm_or_si128(
					_mm_and_si128(still, cn),
					_mm_andnot_si128(still, avg));

				_mm_storeu_si128(
					reinterpret_cast<__m128i*>(dst_row + x * 4), out);
			}
#endif
			for (; x < dst_w; x++)
			{
				const u32 hn = LoadPx(row_t0_up,   x);
				const u32 cn = LoadPx(row_t1,      x);
				const u32 ln = LoadPx(row_t0_down, x);
				const u32 ho = LoadPx(row_t2_up,   x);
				const u32 co = LoadPx(row_t3,      x);
				const u32 lo = LoadPx(row_t2_down, x);

				const int mh = PixelMotion(hn, ho, sensitivity_u8);
				const int mc = PixelMotion(cn, co, sensitivity_u8);
				const int ml = PixelMotion(ln, lo, sensitivity_u8);

				const u32 out = (mh > 0 || mc > 0 || ml > 0)
					? AveragePixels(hn, ln)   /* motion -> interpolate */
					: cn;                     /* still -> weave */
				StorePx(dst_row, x, out);
			}
		}
	}

	/* ====================================================================
	 * Half-height fields (FFMD), as interlace.glsl takes them at 1x: the
	 * merge drew each field twice as tall, every field row on two rows.
	 * ==================================================================== */

	/* A row of the source read at vertical position r (in rows, 0.5 the
	 * first row's centre) with the linear sampler, clamped to [lo, hi). */
	void SampleRowLinear(const u8* src, int src_pitch, int lo, int hi, float r, u8* out_row, int w)
	{
		float fy;
		int y0, y1, x;
		u32 wt;
		const u8* a;
		const u8* b;

		fy = r - 0.5f;
		y0 = (int)floorf(fy);
		wt = (u32)((fy - (float)y0) * 256.0f + 0.5f);
		y1 = y0 + 1;
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
			const u32 pa = LoadPx(a, x);
			const u32 pb = LoadPx(b, x);
			u32 c = 0;
			int k;
			for (k = 0; k < 32; k += 8)
			{
				const u32 ca = (pa >> k) & 0xFFu;
				const u32 cb = (pb >> k) & 0xFFu;
				c |= (((ca * (256u - wt) + cb * wt + 128u) >> 8) & 0xFFu) << k;
			}
			StorePx(out_row, x, c);
		}
	}

	/* BOB into a rect of its own height (a slot of the adaptive buffer):
	 * the source resampled into rows [dy0, dy1) with the linear sampler,
	 * the rest of the destination left as it is. */
	void BobScaled(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int dy0, int dy1)
	{
		const int w = pcsx2_min_i(src_w, dst_w);
		const float ratio = (float)src_h / (float)(dy1 - dy0);
		int y;
		for (y = pcsx2_max_i(dy0, 0); y < pcsx2_min_i(dy1, dst_h); y++)
			SampleRowLinear(src, src_pitch, 0, src_h, ((float)(y - dy0) + 0.5f) * ratio,
				dst + (size_t)y * dst_pitch, w);
	}

	/* The weave of half-height fields (line_rows): a line of the current
	 * field takes its rows from its field's rows, which the merge drew
	 * twice each. The first half of a line's `block` rows is its field's
	 * own; the second half is the other field's positions, the mean of the
	 * two rows around them. */
	void WeaveHalfFields(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int field, int block)
	{
		const int w = pcsx2_min_i(src_w, dst_w);
		int y;
		for (y = 0; y < dst_h; y++)
		{
			const int   l     = y / block;
			const float place = (float)(y - l * block) * 0.5f;
			const float pf    = floorf(place);
			const float row   = (float)((l >> 1) * block) + pf;
			if ((l & 1) != field)
				continue;
			SampleRowLinear(src, src_pitch, 0, src_h, 2.0f * row + 0.5f + 3.0f * (place - pf),
				dst + (size_t)y * dst_pitch, w);
		}
	}

	float PixelLuma(u32 p)
	{
		return ((float)(p & 0xFFu) * 0.299f + (float)((p >> 8) & 0xFFu) * 0.587f +
				(float)((p >> 16) & 0xFFu) * 0.114f) * (1.0f / 255.0f);
	}

	/* mad_field: the row output row y reads from the field `back` fields
	 * ago, with the linear sampler. At a scale of two or more a field's
	 * rows each cover two output rows from its parity's first. */
	void MadFieldRow(const u8* src, int src_pitch, int slot_h, int idx, int block,
		int back, int y, u8* out, int w)
	{
		const int   slot = (idx - back) & 3;
		const int   par  = (idx ^ back) & 1;
		const float rel  = (float)(y - par * block) * 0.5f;
		float r = ((block >= 2) ? floorf(rel) : rel) + 0.5f;
		if (r < 0.5f)
			r = 0.5f;
		if (r > (float)slot_h - 0.5f)
			r = (float)slot_h - 0.5f;
		SampleRowLinear(src, src_pitch, slot * slot_h, (slot + 1) * slot_h,
			(float)(slot * slot_h) + r, out, w);
	}

	/* The adaptive reconstruction of half-height fields (mad_fields): the
	 * buffer holds the last four fields whole, one to a quarter, the
	 * current in slot idx. The current field's lines are taken as they
	 * are; between them, the previous field's line where the picture
	 * holds still, the current field's own picture where it moves, blended
	 * by the luma motion, measured once per native pixel at its block's
	 * centre (at an even scale the mean of the two pixels either side of
	 * it), of the lines around against the field two back and of the line
	 * against its field two back. scratch holds 8 rows and 7 floats a
	 * pixel. */
	void MadFields(
		const u8* src, int src_pitch, int src_w, int src_h,
		u8* dst, int dst_pitch, int dst_w, int dst_h,
		int idx, int block, u8* scratch)
	{
		const int w = pcsx2_min_i(src_w, dst_w);
		const int slot_h = src_h / 4;
		const int field = idx & 1;
		u8* rows[8];
		float* moving = (float*)(scratch + (size_t)8 * w * 4);
		float* luma = moving + w; /* rows 2 to 7, w each */
		int y, x, k, c;
		int line = -1;

		for (k = 0; k < 8; k++)
			rows[k] = scratch + (size_t)k * w * 4;

		for (y = 0; y < dst_h; y++)
		{
			const int l = y / block;
			u8* d = dst + (size_t)y * dst_pitch;

			MadFieldRow(src, src_pitch, slot_h, idx, block, 0, y, rows[0], w);
			if ((l & 1) == field)
			{
				memcpy(d, rows[0], (size_t)w * 4);
				continue;
			}

			if (l != line)
			{
				const int yl = l * block;
				line = l;
				MadFieldRow(src, src_pitch, slot_h, idx, block, 0, yl - block, rows[2], w);
				MadFieldRow(src, src_pitch, slot_h, idx, block, 2, yl - block, rows[3], w);
				MadFieldRow(src, src_pitch, slot_h, idx, block, 0, yl + block, rows[4], w);
				MadFieldRow(src, src_pitch, slot_h, idx, block, 2, yl + block, rows[5], w);
				MadFieldRow(src, src_pitch, slot_h, idx, block, 1, yl, rows[6], w);
				MadFieldRow(src, src_pitch, slot_h, idx, block, 3, yl, rows[7], w);
				for (k = 0; k < 6; k++)
					for (x = 0; x < w; x++)
						luma[k * w + x] = PixelLuma(LoadPx(rows[2 + k], x));
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
					m = fabsf(lm[0] - lm[1]);
					t = fabsf(lm[2] - lm[3]);
					if (t > m)
						m = t;
					t = fabsf(lm[4] - lm[5]);
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

			MadFieldRow(src, src_pitch, slot_h, idx, block, 1, y, rows[1], w);
			for (x = 0; x < w; x++)
			{
				const u32 cur = LoadPx(rows[0], x);
				const u32 prev = LoadPx(rows[1], x);
				u32 out = 0;
				for (c = 0; c < 32; c += 8)
				{
					const float a = (float)((prev >> c) & 0xFFu);
					const float b = (float)((cur >> c) & 0xFFu);
					out |= ((u32)(a + (b - a) * moving[x] + 0.5f) & 0xFFu) << c;
				}
				StorePx(d, x, out);
			}
		}
	}
} // anonymous namespace

/* CPU-backed GSDownloadTexture. The SW renderer doesn't actually
 * invoke the download path in the present flow (its m_output is
 * already in CPU memory), but GSDevice::CreateDownloadTexture is a
 * pure virtual so we need *something* that satisfies the interface
 * for any code path that asks. */
/* Defined after the class, whose thunks it names. */
extern const struct gs_download_texture_ops s_sw_download_texture_ops;

/* Declared here, at file scope, because the class below is in an
 * anonymous namespace and befriends it. An unqualified friend
 * declaration of a name that has not been seen yet declares that name
 * in the nearest enclosing namespace - the anonymous one - and the real
 * struct further down is then a second, different type of the same
 * name. gcc lets the ambiguity pass; MSVC does not, and says so:
 * "'GSDownloadTextureSW_ops_access': ambiguous symbol". */
struct GSDownloadTextureSW_ops_access;

namespace
{
	class GSDownloadTextureSW final : public GSDownloadTexture
	{
		/* Its table is below; see gs_download_texture_ops. */
		friend struct ::GSDownloadTextureSW_ops_access;
	private:
		u8* m_buffer = nullptr;
		u32 m_buffer_size = 0;

	public:
		GSDownloadTextureSW(u32 width, u32 height, GSTexture::Format format)
			: GSDownloadTexture(width, height, format)
		{
			m_ops = &s_sw_download_texture_ops;
			const u32 bpp = (format == GSTexture::Format::UNorm8) ? 1 : 4;
			m_current_pitch = pcsx2_align_up_pow2_u32(width * bpp, VECTOR_ALIGNMENT);
			m_buffer_size = m_current_pitch * height;
			m_buffer = (u8*)memalign_alloc(VECTOR_ALIGNMENT, m_buffer_size ? m_buffer_size : VECTOR_ALIGNMENT);
			if (m_buffer && m_buffer_size)
				memset(m_buffer, 0, m_buffer_size);
		}

		~GSDownloadTextureSW()
		{
			memalign_free(m_buffer);
			m_buffer = NULL;
		}

		void CopyFromTexture(const GSVector4i& /*drc*/, GSTexture* /*stex*/, const GSVector4i& /*src*/,
			u32 /*src_level*/, bool /*use_transfer_pitch*/)
		{
			/* SW renderer's GSRendererSW already has its pixels in
			 * m_output (CPU); the present path does not go through
			 * CopyFromTexture. If something else hits this in a SW-
			 * only build, leaving the buffer zero-filled is a
			 * defensible failure mode. */
		}

		bool Map(const GSVector4i& /*read_rc*/)
		{
			m_map_pointer = m_buffer;
			return m_map_pointer != nullptr;
		}

		void Unmap()
		{
			m_map_pointer = nullptr;
		}

		void Flush()
		{
			/* No GPU work to flush. */
		}
	};
} // namespace

struct GSDownloadTextureSW_ops_access
{
	static void sw_dl_free(GSDownloadTexture* t) { delete static_cast<GSDownloadTextureSW*>(t); }
	static void sw_dl_copy_from_texture(GSDownloadTexture* t, const GSVector4i* drc, GSTexture* stex, const GSVector4i* src, u32 src_level, bool use_transfer_pitch) { static_cast<GSDownloadTextureSW*>(t)->CopyFromTexture(*drc, stex, *src, src_level, use_transfer_pitch); }
	static bool sw_dl_map(GSDownloadTexture* t, const GSVector4i* read_rc) { return static_cast<GSDownloadTextureSW*>(t)->Map(*read_rc); }
	static void sw_dl_unmap(GSDownloadTexture* t) { static_cast<GSDownloadTextureSW*>(t)->Unmap(); }
	static void sw_dl_flush(GSDownloadTexture* t) { static_cast<GSDownloadTextureSW*>(t)->Flush(); }
};

const struct gs_download_texture_ops s_sw_download_texture_ops = {
	GSDownloadTextureSW_ops_access::sw_dl_free,
	GSDownloadTextureSW_ops_access::sw_dl_copy_from_texture,
	GSDownloadTextureSW_ops_access::sw_dl_map,
	GSDownloadTextureSW_ops_access::sw_dl_unmap,
	GSDownloadTextureSW_ops_access::sw_dl_flush
};

/* The software device's table (gs_device_ops, Common/GSDevice.h). Written
 * out rather than made with GS_DEVICE_OPS_DEFINE: it has no version of
 * the two API-state calls, of the batched stretch, or of the sampler
 * cache, and NULL there means what GSDevice does by default. */
#define SWDEV(d) (static_cast<GSDeviceSW*>(d))
struct GSDeviceSW_ops_access
{
static void       sw_dev_free(GSDevice* d) { delete SWDEV(d); }
static bool       sw_dev_create(GSDevice* d) { return SWDEV(d)->Create(); }
static void       sw_dev_destroy(GSDevice* d) { SWDEV(d)->Destroy(); }
static RenderAPI  sw_dev_get_render_api(const GSDevice* d) { return static_cast<const GSDeviceSW*>(d)->GetRenderAPI(); }
static int        sw_dev_begin_present(GSDevice* d, bool frame_skip) { return static_cast<int>(SWDEV(d)->BeginPresent(frame_skip)); }
static void       sw_dev_end_present(GSDevice* d) { SWDEV(d)->EndPresent(); }
static GSTexture* sw_dev_create_surface(GSDevice* d, GSTexture::Type type, int w, int h, int levels, GSTexture::Format format) { return SWDEV(d)->CreateSurface(type, w, h, levels, format); }
static GSDownloadTexture* sw_dev_create_download_texture(GSDevice* d, u32 w, u32 h, GSTexture::Format format) { return SWDEV(d)->CreateDownloadTexture(w, h, format).release(); }
static void       sw_dev_do_merge(GSDevice* d, GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear) { SWDEV(d)->DoMerge(sTex, sRect, dTex, dRect, *PMODE, *EXTBUF, c, linear); }
static void       sw_dev_do_interlace(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, ShaderInterlace shader, bool linear, const InterlaceConstantBuffer* cb) { SWDEV(d)->DoInterlace(sTex, *sRect, dTex, *dRect, shader, linear, *cb); }
static void       sw_dev_copy_rect(GSDevice* d, GSTexture* sTex, GSTexture* dTex, const GSVector4i* r, u32 destX, u32 destY) { SWDEV(d)->CopyRect(sTex, dTex, *r, destX, destY); }
static void       sw_dev_stretch_rect(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, ShaderConvert shader, bool linear) { SWDEV(d)->StretchRect(sTex, *sRect, dTex, *dRect, shader, linear); }
static void       sw_dev_stretch_rect_mask(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect, bool red, bool green, bool blue, bool alpha, ShaderConvert shader) { SWDEV(d)->StretchRect(sTex, *sRect, dTex, *dRect, red, green, blue, alpha, shader); }
static void       sw_dev_present_rect(GSDevice* d, GSTexture* sTex, const GSVector4* sRect, GSTexture* dTex, const GSVector4* dRect) { SWDEV(d)->PresentRect(sTex, *sRect, dTex, *dRect); }
static void       sw_dev_update_clut_texture(GSDevice* d, GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize, u32 samples) { SWDEV(d)->UpdateCLUTTexture(sTex, sScale, offsetX, offsetY, dTex, dOffset, dSize, samples); }
static void       sw_dev_convert_to_indexed_texture(GSDevice* d, GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM) { SWDEV(d)->ConvertToIndexedTexture(sTex, sScale, offsetX, offsetY, SBW, SPSM, dTex, DBW, DPSM); }
static void       sw_dev_filtered_downsample_texture(GSDevice* d, GSTexture* sTex, GSTexture* dTex, u32 factor, const GSVector2i* clamp_min, const GSVector4* dRect) { SWDEV(d)->FilteredDownsampleTexture(sTex, dTex, factor, *clamp_min, *dRect); }
static void       sw_dev_render_hw(GSDevice* d, GSHWDrawConfig* config) { SWDEV(d)->RenderHW(*config); }
};
#undef SWDEV

static const struct gs_device_ops s_sw_device_ops = {
	GSDeviceSW_ops_access::sw_dev_free,
	GSDeviceSW_ops_access::sw_dev_create,
	GSDeviceSW_ops_access::sw_dev_destroy,
	NULL, /* reset_api_state */
	NULL, /* restore_api_state */
	GSDeviceSW_ops_access::sw_dev_get_render_api,
	GSDeviceSW_ops_access::sw_dev_begin_present,
	GSDeviceSW_ops_access::sw_dev_end_present,
	GSDeviceSW_ops_access::sw_dev_create_surface,
	GSDeviceSW_ops_access::sw_dev_create_download_texture,
	GSDeviceSW_ops_access::sw_dev_do_merge,
	GSDeviceSW_ops_access::sw_dev_do_interlace,
	GSDeviceSW_ops_access::sw_dev_copy_rect,
	GSDeviceSW_ops_access::sw_dev_stretch_rect,
	GSDeviceSW_ops_access::sw_dev_stretch_rect_mask,
	GSDeviceSW_ops_access::sw_dev_present_rect,
	NULL, /* draw_multi_stretch_rects */
	GSDeviceSW_ops_access::sw_dev_update_clut_texture,
	GSDeviceSW_ops_access::sw_dev_convert_to_indexed_texture,
	GSDeviceSW_ops_access::sw_dev_filtered_downsample_texture,
	NULL, /* move_texels: the software renderer moves in local memory */
	GSDeviceSW_ops_access::sw_dev_render_hw,
	NULL  /* clear_sampler_cache */
};

GSDeviceSW::GSDeviceSW()
{
	m_ops = &s_sw_device_ops;
}

GSDeviceSW::~GSDeviceSW()
{
	memalign_free(m_present_buffer);
	m_present_buffer = NULL;
}

bool GSDeviceSW::Create()
{
	if (!CreateBase())
		return false;

	AcquireWindow();

	/* The SW renderer reads these to decide what to support. We have
	 * no GPU features at all - it's all CPU - so every advanced flag
	 * stays false. The SW renderer doesn't actually check most of
	 * these (they're HW-renderer concerns); zero defaults are right. */
	m_features = FeatureSupport();

	return true;
}

void GSDeviceSW::Destroy()
{
	DestroyBase();
	memalign_free(m_present_buffer);
	m_present_buffer = NULL;
	m_present_buffer = nullptr;
	m_present_buffer_capacity = 0;
	m_present_pitch = 0;
	m_present_width = 0;
	m_present_height = 0;
	m_direct_rendering_checked = false;
	m_direct_rendering_supported = false;
	m_direct_render_buffer = nullptr;
	m_direct_render_pitch = 0;
	m_present_pending = false;
}

GSTexture* GSDeviceSW::CreateSurface(GSTexture::Type type, int width, int height, int /*levels*/, GSTexture::Format format)
{
	return new GSTextureSW(type, width, height, format);
}

GSDownloadTexturePtr GSDeviceSW::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTexturePtr(new GSDownloadTextureSW(width, height, format));
}

void GSDeviceSW::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const bool /*linear*/)
{
	if (!dTex)
		return;

	GSTextureSW* dst = static_cast<GSTextureSW*>(dTex);
	const int dst_w  = dst->GetWidth();
	const int dst_h  = dst->GetHeight();
	const int dst_p  = dst->GetPitch();
	u8* dst_data     = dst->GetPointer();
	if (!dst_data)
		return;

	/* PS2 BGCOLOR is u32 0x00BBGGRR with alpha from PMODE.ALP packed
	 * by the caller into bits 24..31. Treat the contents of GSTextureSW
	 * as PS2-native ABGR (R in byte 0), matching what the SW renderer
	 * wrote when it Update()d circuit textures from m_output. */
	const u32 bg_abgr = (c & 0x00FFFFFFu) | 0xFF000000u;
	ClearRectPx(dst_data, dst_p, dst_w, dst_h, bg_abgr);

	/* Feedback writes are an EXTBUF/PCRTC loopback path the HW
	 * backends route through dTex for one frame then read back. The
	 * SW renderer doesn't exercise this path in practice (Xenosaga
	 * cutscenes use it, but go through GetFeedbackOutput which is a
	 * separate SW-side hook). Leave unhandled for stage 2 - if a
	 * game depends on it under the CPU path we'll see it as a
	 * specific visual regression and can address then. */
	(void)EXTBUF;

	/* Circuit 1: copy with no blend (background already filled). */
	if (sTex[1] && PMODE.SLBG == 0)
	{
		GSTextureSW* src = static_cast<GSTextureSW*>(sTex[1]);
		const int dx0 = static_cast<int>(dRect[1].x);
		const int dy0 = static_cast<int>(dRect[1].y);
		const int dx1 = static_cast<int>(dRect[1].z);
		const int dy1 = static_cast<int>(dRect[1].w);
		NearestCopyPS2(
			src->GetPointer(), src->GetPitch(), src->GetWidth(), src->GetHeight(),
			sRect[1].x, sRect[1].y, sRect[1].z, sRect[1].w,
			dst_data, dst_p,
			dx0, dy0, dx1, dy1,
			dst_w, dst_h);
	}

	/* Circuit 0: alpha-blended composite over whatever is in dTex. */
	if (sTex[0])
	{
		GSTextureSW* src = static_cast<GSTextureSW*>(sTex[0]);
		const int dx0 = static_cast<int>(dRect[0].x);
		const int dy0 = static_cast<int>(dRect[0].y);
		const int dx1 = static_cast<int>(dRect[0].z);
		const int dy1 = static_cast<int>(dRect[0].w);
		const bool constant_alpha = (PMODE.MMOD == 1);
		const u8 alpha8 = static_cast<u8>((c >> 24) & 0xFFu);
		BlendOverPS2(
			src->GetPointer(), src->GetPitch(), src->GetWidth(), src->GetHeight(),
			sRect[0].x, sRect[0].y, sRect[0].z, sRect[0].w,
			dst_data, dst_p,
			dx0, dy0, dx1, dy1,
			dst_w, dst_h,
			constant_alpha, alpha8);
	}
}

void GSDeviceSW::DoInterlace(GSTexture* sTex, const GSVector4& /*sRect*/, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, bool /*linear*/, const InterlaceConstantBuffer& cb)
{
	if (!sTex || !dTex)
		return;
	GSTextureSW* src = static_cast<GSTextureSW*>(sTex);
	GSTextureSW* dst = static_cast<GSTextureSW*>(dTex);
	if (!src->GetPointer() || !dst->GetPointer())
		return;

	/* cb.ZrH layout from GSDevice::Interlace:
	 *   x = bufIdx (passed as `field` for weave/blend, 0 for bob)
	 *   y = 1.0 / dst_height (UV stride per line; unused here)
	 *   z = dst_height
	 *   w = rows per frame line, the upscale, positive for half-height
	 *       fields and negative for full-height ones
	 * GSDevice::Interlace casts ZrH.x to int and uses (idx & 1) as
	 * the field. */
	const int field   = static_cast<int>(cb.ZrH.x) & 1;
	const int y_shift = static_cast<int>(std::floor(dRect.y + 0.5f));
	/* A half-height field (FFMD) passes its block positive. */
	const bool half_fields = cb.ZrH.w > 0.0f;
	const int block = pcsx2_max_i(1, static_cast<int>(std::fabs(cb.ZrH.w) + 0.5f));

	const int src_w   = src->GetWidth();
	const int src_h   = src->GetHeight();
	const int dst_w   = dst->GetWidth();
	const int dst_h   = dst->GetHeight();
	const int src_p   = src->GetPitch();
	const int dst_p   = dst->GetPitch();
	const u8* src_buf = src->GetPointer();
	u8* dst_buf       = dst->GetPointer();

	switch (shader)
	{
		case ShaderInterlace::WEAVE:
			if (half_fields)
				WeaveHalfFields(src_buf, src_p, src_w, src_h,
					dst_buf, dst_p, dst_w, dst_h, field, block);
			else
				WeaveCopy(src_buf, src_p, src_w, src_h,
					dst_buf, dst_p, dst_w, dst_h,
					y_shift, field, block);
			break;

		case ShaderInterlace::BOB:
		{
			/* A rect of another height than the source is a slot of the
			 * adaptive buffer, which keeps the rest of it. */
			const int dy0 = static_cast<int>(std::floor(dRect.y + 0.5f));
			const int dy1 = static_cast<int>(std::floor(dRect.w + 0.5f));
			if (dy1 - dy0 != src_h && dy1 > dy0)
				BobScaled(src_buf, src_p, src_w, src_h,
					dst_buf, dst_p, dst_w, dst_h, dy0, dy1);
			else
				BobCopy(src_buf, src_p, src_w, src_h,
					dst_buf, dst_p, dst_w, dst_h,
					y_shift);
			break;
		}

		case ShaderInterlace::BLEND:
			/* GSDevice::Interlace mode 2 runs WEAVE -> dst, then
			 * BLEND -> dst. Our BlendThree handles the second pass:
			 * for each row of dst, average it with its vertical
			 * neighbors. Source==dst in the second pass would
			 * read-after-write; the base class actually allocates
			 * a separate m_blend target and passes (m_weavebob ->
			 * m_blend), so source and destination are distinct.
			 * BlendThree assumes this and reads only from src. */
			BlendThree(src_buf, src_p, src_w, src_h,
				dst_buf, dst_p, dst_w, dst_h, block);
			break;

		case ShaderInterlace::MAD_BUFFER:
		{
			/* FastMAD pass 1: store the current source frame into
			 * one bank of m_mad. Source = m_merge (frame_h tall),
			 * destination = m_mad (2*frame_h tall). cb.ZrH.x is
			 * the bufIdx (0..3) which gives us bank and field.
			 *
			 * GSDevice::Interlace restricts dRect to either the top
			 * (bank 0) or bottom (bank 1) half of m_mad; the bank
			 * choice and the dRect bounds carry equivalent info, but
			 * we read dRect to learn the destination row range and
			 * idx for bank/field. */
			const int idx_int = static_cast<int>(cb.ZrH.x);
			const int bank    = (idx_int >> 1) & 1;
			const int vres    = static_cast<int>(cb.ZrH.z) >> 1;
			const int dy_begin = static_cast<int>(std::floor(dRect.y + 0.5f));
			const int dy_end   = static_cast<int>(std::floor(dRect.w + 0.5f));

			MadBuffer(src_buf, src_p, src_w, src_h,
				dst_buf, dst_p, dst_w, dst_h,
				dy_begin, dy_end, field, bank, vres, block);
			break;
		}

		case ShaderInterlace::MAD_RECONSTRUCT:
		{
			/* FastMAD pass 2: read m_mad and produce m_weavebob.
			 * Per-pixel motion detection across stored frames
			 * chooses between weave (low motion -> use the field
			 * we have) and linear vertical interpolation (high
			 * motion -> average the two adjacent same-field
			 * pixels of the current frame). */
			const int idx_int = static_cast<int>(cb.ZrH.x);

			if (half_fields)
			{
				std::vector<u8> scratch(static_cast<size_t>(pcsx2_min_i(src_w, dst_w)) * (4 * 8 + sizeof(float) * 7));
				MadFields(src_buf, src_p, src_w, src_h,
					dst_buf, dst_p, dst_w, dst_h, idx_int, block, scratch.data());
			}
			else
			{
				/* The shader's sensitivity, 0.08 of full scale. */
				MadReconstruct(src_buf, src_p, src_w, src_h,
					dst_buf, dst_p, dst_w, dst_h,
					idx_int, 20, block);
			}
			break;
		}

		default:
			break;
	}
}

void GSDeviceSW::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	if (!sTex || !dTex)
		return;

	GSTextureSW* src = static_cast<GSTextureSW*>(sTex);
	GSTextureSW* dst = static_cast<GSTextureSW*>(dTex);
	if (!src->GetPointer() || !dst->GetPointer())
		return;

	const int sx = r.x;
	const int sy = r.y;
	const int sw = r.width();
	const int sh = r.height();
	if (sw <= 0 || sh <= 0)
		return;

	const int dx_start = static_cast<int>(destX);
	const int dy_start = static_cast<int>(destY);

	for (int row = 0; row < sh; row++)
	{
		const int sy_row = sy + row;
		const int dy_row = dy_start + row;
		if (sy_row < 0 || sy_row >= src->GetHeight()) continue;
		if (dy_row < 0 || dy_row >= dst->GetHeight()) continue;

		const u8* src_row = src->GetPointer() + (size_t)sy_row * src->GetPitch();
		u8* dst_row       = dst->GetPointer() + (size_t)dy_row * dst->GetPitch();

		for (int col = 0; col < sw; col++)
		{
			const int sx_col = sx + col;
			const int dx_col = dx_start + col;
			if (sx_col < 0 || sx_col >= src->GetWidth()) continue;
			if (dx_col < 0 || dx_col >= dst->GetWidth()) continue;
			StorePx(dst_row, dx_col, LoadPx(src_row, sx_col));
		}
	}
}

void GSDeviceSW::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvert shader, bool /*linear*/)
{
	if (!sTex || !dTex)
		return;

	GSTextureSW* src = static_cast<GSTextureSW*>(sTex);
	GSTextureSW* dst = static_cast<GSTextureSW*>(dTex);
	if (!src->GetPointer() || !dst->GetPointer())
		return;

	const int dx0 = static_cast<int>(dRect.x);
	const int dy0 = static_cast<int>(dRect.y);
	const int dx1 = static_cast<int>(dRect.z);
	const int dy1 = static_cast<int>(dRect.w);

	/* The SW renderer's present path only ever uses
	 * ShaderConvert::COPY. Anything more exotic (HDR<->LDR conversion,
	 * YUV, depth packing, etc.) is HW-renderer machinery; for the SW
	 * path we treat any non-COPY shader as a plain copy. If a future
	 * code path needs a real conversion we'll see it as bad output
	 * rather than a crash. */
	(void)shader;

	NearestCopyPS2(
		src->GetPointer(), src->GetPitch(), src->GetWidth(), src->GetHeight(),
		sRect.x, sRect.y, sRect.z, sRect.w,
		dst->GetPointer(), dst->GetPitch(),
		dx0, dy0, dx1, dy1,
		dst->GetWidth(), dst->GetHeight());
}

void GSDeviceSW::StretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	bool /*red*/, bool /*green*/, bool /*blue*/, bool /*alpha*/, ShaderConvert shader)
{
	/* Channel-mask variant - used by HW backends to do partial
	 * channel writes via blend equations. The SW renderer's present
	 * flow doesn't call this; route to the plain stretch so a stray
	 * call doesn't crash. */
	StretchRect(sTex, sRect, dTex, dRect, shader, true);
}

void GSDeviceSW::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
{
	if (!sTex)
		return;

	GSTextureSW* src = static_cast<GSTextureSW*>(sTex);
	if (!src->GetPointer())
		return;

	/* GSRenderer::VSync signals "blank this frame" by passing
	 * sRect = (0,0,0,0). Match HW backends: clear sTex to opaque
	 * black, then sample the now-zero source. */
	const bool blank_frame = (sRect.z == 0.0f && sRect.w == 0.0f);
	if (blank_frame)
	{
		ClearRectPx(src->GetPointer(), src->GetPitch(), src->GetWidth(), src->GetHeight(), 0xFF000000u);
	}

	const float sx0 = blank_frame ? 0.0f : sRect.x;
	const float sy0 = blank_frame ? 0.0f : sRect.y;
	const float sx1 = blank_frame ? 1.0f : sRect.z;
	const float sy1 = blank_frame ? 1.0f : sRect.w;

	if (dTex)
	{
		/* GSTextureSW->GSTextureSW path: no format conversion. */
		GSTextureSW* dst = static_cast<GSTextureSW*>(dTex);
		if (!dst->GetPointer())
			return;
		NearestCopyPS2(
			src->GetPointer(), src->GetPitch(), src->GetWidth(), src->GetHeight(),
			sx0, sy0, sx1, sy1,
			dst->GetPointer(), dst->GetPitch(),
			static_cast<int>(dRect.x), static_cast<int>(dRect.y),
			static_cast<int>(dRect.z), static_cast<int>(dRect.w),
			dst->GetWidth(), dst->GetHeight());
		return;
	}

	/* dTex == nullptr: "draw to backbuffer". Write into either the
	 * frontend's direct-render buffer or our own m_present_buffer.
	 * PS2-ABGR -> XRGB8888 swizzle here - intermediate GSTextureSW
	 * textures stay in PS2 byte order; only the final hand-off to
	 * the frontend needs converting.
	 *
	 * Output dimensions are dRect.{z,w}, matching what HW backends
	 * pass to video_cb. The frontend handles fitting the frame into
	 * its window (letterboxing, scaling). */
	const int dx0 = static_cast<int>(dRect.x);
	const int dy0 = static_cast<int>(dRect.y);
	const int dx1 = static_cast<int>(dRect.z);
	const int dy1 = static_cast<int>(dRect.w);
	const int out_w = dx1 - dx0;
	const int out_h = dy1 - dy0;
	if (out_w <= 0 || out_h <= 0)
		return;

	u8* out = nullptr;
	int out_pitch = 0;

	if (AcquireDirectRenderBuffer(out_w, out_h))
	{
		out       = m_direct_render_buffer;
		out_pitch = static_cast<int>(m_direct_render_pitch);
	}
	else
	{
		EnsurePresentBuffer(out_w, out_h);
		if (!m_present_buffer)
			return;
		out       = m_present_buffer;
		out_pitch = m_present_pitch;
	}

	NearestCopyToXRGB(
		src->GetPointer(), src->GetPitch(), src->GetWidth(), src->GetHeight(),
		sx0, sy0, sx1, sy1,
		out, out_pitch,
		0, 0, out_w, out_h,
		out_w, out_h);

	m_present_width  = out_w;
	m_present_height = out_h;
	m_present_pending = true;
}

void GSDeviceSW::UpdateCLUTTexture(GSTexture* /*sTex*/, float /*sScale*/, u32 /*offsetX*/, u32 /*offsetY*/,
	GSTexture* /*dTex*/, u32 /*dOffset*/, u32 /*dSize*/, u32 /*samples*/)
{
	/* HW-renderer-only path - the SW renderer doesn't call this. */
}

void GSDeviceSW::ConvertToIndexedTexture(GSTexture* /*sTex*/, float /*sScale*/, u32 /*offsetX*/, u32 /*offsetY*/,
	u32 /*SBW*/, u32 /*SPSM*/, GSTexture* /*dTex*/, u32 /*DBW*/, u32 /*DPSM*/)
{
	/* HW-renderer-only path. */
}

void GSDeviceSW::FilteredDownsampleTexture(GSTexture* /*sTex*/, GSTexture* /*dTex*/, u32 /*downsample_factor*/,
	const GSVector2i& /*clamp_min*/, const GSVector4& /*dRect*/)
{
	/* HW-renderer-only path. */
}

void GSDeviceSW::RenderHW(GSHWDrawConfig& /*config*/)
{
	/* GSRendererHW dispatches drawing here. The SW renderer never
	 * does. If this fires, the device was wired up to the wrong
	 * renderer - that's a build/init bug, not a runtime condition we
	 * can recover from. */
	log_cb(RETRO_LOG_ERROR, "GSDeviceSW::RenderHW called - HW renderer is not compatible with the CPU-only device.\n");
}

void GSDeviceSW::ClearSamplerCache()
{
	/* No samplers exist. */
}

GSDevice::PresentResult GSDeviceSW::BeginPresent(bool frame_skip)
{
	m_present_pending = false;
	m_direct_render_buffer = nullptr;
	m_direct_render_pitch = 0;

	if (frame_skip)
		return PresentResult::FrameSkipped;

	return PresentResult::OK;
}

bool GSDeviceSW::AcquireDirectRenderBuffer(int width, int height)
{
	/* The libretro contract requires fetching the framebuffer every
	 * frame if direct rendering is used (the pointer is only valid
	 * for the current retro_run). We do still cache a "supported"
	 * flag to short-circuit the probe path when the frontend has
	 * already told us no.
	 *
	 * Re-probe on dimension change since some frontends only
	 * direct-render at specific size constraints. */
	if (!environ_cb)
		return false;

	const bool dims_changed = (width != m_present_width || height != m_present_height);
	if (!m_direct_rendering_checked || dims_changed)
	{
		m_direct_rendering_supported = true;  /* tentatively yes; fetch below decides */
		m_direct_rendering_checked   = true;
	}
	else if (!m_direct_rendering_supported)
		return false;

	retro_framebuffer fb = {};
	fb.width        = static_cast<unsigned>(width);
	fb.height       = static_cast<unsigned>(height);
	fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) ||
		!fb.data ||
		fb.format != RETRO_PIXEL_FORMAT_XRGB8888 ||
		fb.width  != static_cast<unsigned>(width) ||
		fb.height != static_cast<unsigned>(height))
	{
		m_direct_rendering_supported = false;
		return false;
	}

	m_direct_render_buffer = static_cast<u8*>(fb.data);
	m_direct_render_pitch  = fb.pitch;
	return true;
}

void GSDeviceSW::EnsurePresentBuffer(int width, int height)
{
	const int bpp = 4; /* XRGB8888 */
	const int pitch = static_cast<int>(pcsx2_align_up_pow2_u32(static_cast<u32>(width * bpp), VECTOR_ALIGNMENT));
	const int needed = pitch * height;
	if (needed <= 0)
		return;

	if (needed > m_present_buffer_capacity)
	{
		memalign_free(m_present_buffer);
		m_present_buffer = NULL;
		m_present_buffer = (u8*)memalign_alloc(VECTOR_ALIGNMENT, needed);
		m_present_buffer_capacity = m_present_buffer ? needed : 0;
	}
	m_present_pitch  = pitch;
	m_present_width  = width;
	m_present_height = height;
}

void GSDeviceSW::EndPresent()
{
	if (!m_present_pending)
	{
		/* PresentRect wasn't called this frame (no GSTexture* current
		 * to draw - very-early frames before the SW renderer has
		 * produced anything). Don't deliver an undefined buffer. */
		return;
	}

	if (m_direct_render_buffer && m_direct_render_pitch > 0)
	{
		if (video_cb)
			video_cb(m_direct_render_buffer,
				static_cast<unsigned>(m_present_width),
				static_cast<unsigned>(m_present_height),
				m_direct_render_pitch);
	}
	else if (m_present_buffer && m_present_pitch > 0)
	{
		if (video_cb)
			video_cb(m_present_buffer,
				static_cast<unsigned>(m_present_width),
				static_cast<unsigned>(m_present_height),
				static_cast<size_t>(m_present_pitch));
	}
}
