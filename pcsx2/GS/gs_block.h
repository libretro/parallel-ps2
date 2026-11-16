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

#ifndef GS_BLOCK_H
#define GS_BLOCK_H

/*
 * The GS block swizzle, in C: a 256-byte block of local memory to and
 * from rows of pixels, a column (two rows of 32 and 16 bits, four of 8 and
 * 4) or the whole block at a time; the high bits of 32-bit pixels read
 * out as 8- and 4-bit indexes; blocks expanded to 32- and 16-bit colours
 * (24 and 16 bits by TEXA, indexes by palette); and 24-bit and high
 * 8/4-bit pixels unpacked into 32-bit blocks.
 *
 * Header only, static inline: each caller is compiled for its own
 * instruction set (the multi-ISA files once per tier) and takes the AVX2
 * path where it was built with AVX2, the 128-bit one elsewhere (SSE4.1,
 * and NEON on arm64). The column number, alignment, write mask, AEM and
 * shifts are constants at every call, so each inlined copy is the
 * specialisation the C++ templates were.
 *
 * Blocks are 32-byte aligned; row pointers are as the callers have them
 * (16-byte aligned for the reads' stores).
 */

#include <string.h>

#include "common/Pcsx2Types.h"
#include "gs_vector.h"

#if defined(__AVX2__)
#define GS_BLOCK_AVX2 1
#endif
/* (from AVX on, unaligned loads cost what aligned ones do) */
#if defined(__AVX__)
#define GS_BLOCK_FAST_UNALIGNED 1
#endif

#define GS_BLOCK_INLINE static GS_VEC_INLINE

/* The byte shuffles, sixteen indexes each */
typedef union gs_block_mask
{
	u8 b[16];
	gs_vec4i v;
} gs_block_mask;

static const gs_block_mask gs_block_r16mask = {{0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15}};
static const gs_block_mask gs_block_r8mask = {{0, 4, 2, 6, 8, 12, 10, 14, 1, 5, 3, 7, 9, 13, 11, 15}};
static const gs_block_mask gs_block_r4mask = {{0, 8, 4, 12, 1, 9, 5, 13, 2, 10, 6, 14, 3, 11, 7, 15}};
static const gs_block_mask gs_block_w4mask = {{0, 4, 8, 12, 2, 6, 10, 14, 1, 5, 9, 13, 3, 7, 11, 15}};
static const gs_block_mask gs_block_palvec_mask = {{0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15}};
static const gs_block_mask gs_block_avx2_r8mask1 = {{0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15}};
static const gs_block_mask gs_block_avx2_r8mask2 = {{1, 5, 9, 13, 0, 4, 8, 12, 3, 7, 11, 15, 2, 6, 10, 14}};
static const gs_block_mask gs_block_avx2_w8mask1 = {{0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15}};
static const gs_block_mask gs_block_avx2_w8mask2 = {{4, 0, 12, 8, 5, 1, 13, 9, 6, 2, 14, 10, 7, 3, 15, 11}};

/* _MM_SHUFFLE immediates, by the lanes they take */
#define GS_BLOCK_XZYW 0xD8 /* (3, 1, 2, 0) */
#define GS_BLOCK_YWXZ 0x8D /* (2, 0, 3, 1) */
#define GS_BLOCK_ZXWY 0x72 /* (1, 3, 0, 2) */
#define GS_BLOCK_YXWZ 0xB1 /* (2, 3, 0, 1) */
#define GS_BLOCK_XZXZ 0x88 /* (2, 0, 2, 0) */
#define GS_BLOCK_YWYW 0xDD /* (3, 1, 3, 1) */
#define GS_BLOCK_ZXZX 0x22 /* (0, 2, 0, 2) */
#define GS_BLOCK_WYWY 0x77 /* (1, 3, 1, 3) */

/* ---- 128 bits: the swaps of four vectors' lanes ---- */

#define GS_BLOCK_SW(a, b, c, d, lo, hi) \
	do                                  \
	{                                   \
		const gs_vec4i e_ = (a);        \
		const gs_vec4i f_ = (c);        \
		(a)               = lo(e_, (b)); \
		(c)               = hi(e_, (b)); \
		(b)               = lo(f_, (d)); \
		(d)               = hi(f_, (d)); \
	} while (0)
#define GS_BLOCK_SW8(a, b, c, d) GS_BLOCK_SW(a, b, c, d, gs_v4i_upl8v, gs_v4i_uph8v)
#define GS_BLOCK_SW16(a, b, c, d) GS_BLOCK_SW(a, b, c, d, gs_v4i_upl16v, gs_v4i_uph16v)
#define GS_BLOCK_SW32(a, b, c, d) GS_BLOCK_SW(a, b, c, d, gs_v4i_upl32v, gs_v4i_uph32v)
#define GS_BLOCK_SW64(a, b, c, d) GS_BLOCK_SW(a, b, c, d, gs_v4i_upl64v, gs_v4i_uph64v)

/* The nibbles of a and b interleaved: a's low with b's low, a's high with
 * b's high */
#define GS_BLOCK_MIX4(a, b)                                                                     \
	do                                                                                          \
	{                                                                                           \
		const gs_vec4i m_ = gs_v4i_set32(0x0f0f0f0f);                                           \
		const gs_vec4i c_ = gs_v4i_select(GS_V4I_SLL32((b), 4), (a), m_);                       \
		const gs_vec4i d_ = gs_v4i_select((b), GS_V4I_SRL32((a), 4), m_);                       \
		(a)               = c_;                                                                 \
		(b)               = d_;                                                                 \
	} while (0)
#define GS_BLOCK_SW4(a, b, c, d)    \
	do                              \
	{                               \
		GS_BLOCK_MIX4(a, b);        \
		GS_BLOCK_MIX4(c, d);        \
		GS_BLOCK_SW8(a, b, c, d);   \
	} while (0)
#define GS_BLOCK_SW32_INV(a, b, c, d)                          \
	do                                                         \
	{                                                          \
		const gs_vec4i a_ = (a), b_ = (b), c_ = (c), d_ = (d); \
		(a)               = GS_V4I_SHUFFLE2(a_, c_, GS_BLOCK_XZXZ); \
		(b)               = GS_V4I_SHUFFLE2(a_, c_, GS_BLOCK_YWYW); \
		(c)               = GS_V4I_SHUFFLE2(b_, d_, GS_BLOCK_XZXZ); \
		(d)               = GS_V4I_SHUFFLE2(b_, d_, GS_BLOCK_YWYW); \
	} while (0)

/* Each 32-bit lane shifted right by a count that is a constant only once
 * inlined (the immediate form wants a literal) */
#if defined(GS_VEC_X86)
#define GS_BLOCK_SRL32V(v, n) _mm_srl_epi32((v), _mm_cvtsi32_si128(n))
#else
#define GS_BLOCK_SRL32V(v, n) vreinterpretq_s32_u32(vshlq_u32(vreinterpretq_u32_s32(v), vdupq_n_s32(-(n))))
#endif

/* d with v where mask has bits (GSVector4i::smartblend: mask is a
 * constant, so all but one branch folds away) */
GS_BLOCK_INLINE gs_vec4i gs_block_blend_mask(gs_vec4i d, gs_vec4i v, u32 mask)
{
	if (mask == 0)
		return d;
	if (mask == 0xffffffffu)
		return v;
	if (mask == 0x0000ffffu)
		return GS_V4I_BLEND16(d, v, 0x55);
	if (mask == 0xffff0000u)
		return GS_V4I_BLEND16(d, v, 0xaa);
	if (((mask & 0xff) != 0 && (mask & 0xff) != 0xff) || ((mask >> 8 & 0xff) != 0 && (mask >> 8 & 0xff) != 0xff)
		|| ((mask >> 16 & 0xff) != 0 && (mask >> 16 & 0xff) != 0xff)
		|| ((mask >> 24) != 0 && (mask >> 24) != 0xff))
		return gs_v4i_select(d, v, gs_v4i_set32((int32_t)mask));
	return gs_v4i_blend8(d, v, gs_v4i_set32((int32_t)mask));
}

/* ---- 256 bits ---- */

#ifdef GS_BLOCK_AVX2

#define GS_BLOCK_ACBD(v) _mm256_permute4x64_epi64((v), 0xD8)
#define GS_BLOCK_PAIR(lo, hi) _mm256_inserti128_si256(_mm256_castsi128_si256(lo), (hi), 1)
#define GS_BLOCK_BCAST(m) _mm256_broadcastsi128_si256((m).v)
#define GS_BLOCK_STOREL8(p, v) _mm_store_si128((__m128i*)(p), _mm256_castsi256_si128(v))
#define GS_BLOCK_STOREH8(p, v) _mm_store_si128((__m128i*)(p), _mm256_extracti128_si256((v), 1))
#define GS_BLOCK_SHUF2_8(a, b, i) \
	_mm256_castps_si256(_mm256_shuffle_ps(_mm256_castsi256_ps(a), _mm256_castsi256_ps(b), (i)))

#define GS_BLOCK_SW_8(a, b, lo, hi) \
	do                              \
	{                               \
		const __m256i c_ = (a);     \
		const __m256i d_ = (b);     \
		(a)              = lo(c_, d_); \
		(b)              = hi(c_, d_); \
	} while (0)
#define GS_BLOCK_SW8_8(a, b) GS_BLOCK_SW_8(a, b, _mm256_unpacklo_epi8, _mm256_unpackhi_epi8)
#define GS_BLOCK_SW16_8(a, b) GS_BLOCK_SW_8(a, b, _mm256_unpacklo_epi16, _mm256_unpackhi_epi16)
#define GS_BLOCK_SW32_8(a, b) GS_BLOCK_SW_8(a, b, _mm256_unpacklo_epi32, _mm256_unpackhi_epi32)
#define GS_BLOCK_SW64_8(a, b) GS_BLOCK_SW_8(a, b, _mm256_unpacklo_epi64, _mm256_unpackhi_epi64)
#define GS_BLOCK_SW32_INV_8(a, b)                        \
	do                                                   \
	{                                                    \
		const __m256i c_ = (a);                          \
		const __m256i d_ = (b);                          \
		(a)              = GS_BLOCK_SHUF2_8(c_, d_, GS_BLOCK_XZXZ); \
		(b)              = GS_BLOCK_SHUF2_8(c_, d_, GS_BLOCK_YWYW); \
	} while (0)
#define GS_BLOCK_MIX4_8(a, b)                                                                            \
	do                                                                                                   \
	{                                                                                                    \
		const __m256i m_ = _mm256_set1_epi32(0x0f0f0f0f);                                                \
		const __m256i c_ = _mm256_or_si256(_mm256_andnot_si256(m_, _mm256_slli_epi32((b), 4)),          \
			_mm256_and_si256(m_, (a)));                                                                  \
		const __m256i d_ = _mm256_or_si256(_mm256_andnot_si256(m_, (b)),                                \
			_mm256_and_si256(m_, _mm256_srli_epi32((a), 4)));                                            \
		(a)              = c_;                                                                           \
		(b)              = d_;                                                                           \
	} while (0)

GS_BLOCK_INLINE __m256i gs_block_blend_mask8(__m256i d, __m256i v, u32 mask)
{
	if (mask == 0)
		return d;
	if (mask == 0xffffffffu)
		return v;
	if (mask == 0x0000ffffu)
		return _mm256_blend_epi16(d, v, 0x55);
	if (mask == 0xffff0000u)
		return _mm256_blend_epi16(d, v, 0xaa);
	if (((mask & 0xff) != 0 && (mask & 0xff) != 0xff) || ((mask >> 8 & 0xff) != 0 && (mask >> 8 & 0xff) != 0xff)
		|| ((mask >> 16 & 0xff) != 0 && (mask >> 16 & 0xff) != 0xff)
		|| ((mask >> 24) != 0 && (mask >> 24) != 0xff))
	{
		const __m256i m = _mm256_set1_epi32((int)mask);
		return _mm256_or_si256(_mm256_andnot_si256(m, d), _mm256_and_si256(m, v));
	}
	return _mm256_blendv_epi8(d, v, _mm256_set1_epi32((int)mask));
}

#endif

/* ---- writing a column: rows of pixels into the block ---- */

/* Column i (0-3) of a 32-bit block from two rows; mask: the bits of each
 * pixel written (the rest kept). aligned: the rows are 16-byte aligned. */
GS_BLOCK_INLINE void gs_block_write_column32(int i, int aligned, u32 mask, u8* dst, const u8* src, int srcpitch)
{
	const u8* s0 = &src[srcpitch * 0];
	const u8* s1 = &src[srcpitch * 1];
#ifdef GS_BLOCK_AVX2
	__m256i v0 = GS_BLOCK_ACBD(_mm256_loadu_si256((const __m256i*)s0));
	__m256i v1 = GS_BLOCK_ACBD(_mm256_loadu_si256((const __m256i*)s1));
	__m256i* d = (__m256i*)dst;
	(void)aligned;
	GS_BLOCK_SW64_8(v0, v1);
	d[i * 2 + 0] = gs_block_blend_mask8(d[i * 2 + 0], v0, mask);
	d[i * 2 + 1] = gs_block_blend_mask8(d[i * 2 + 1], v1, mask);
#else
	gs_vec4i v0, v1, v2, v3;
	gs_vec4i* d = (gs_vec4i*)dst;
#ifdef GS_BLOCK_FAST_UNALIGNED
	(void)aligned;
	v0 = gs_v4i_loadu(&s0[0]);
	v1 = gs_v4i_loadu(&s0[16]);
	v2 = gs_v4i_loadu(&s1[0]);
	v3 = gs_v4i_loadu(&s1[16]);
	GS_BLOCK_SW64(v0, v2, v1, v3);
#else
	if (aligned)
	{
		v0 = gs_v4i_load(&s0[0]);
		v1 = gs_v4i_load(&s0[16]);
		v2 = gs_v4i_load(&s1[0]);
		v3 = gs_v4i_load(&s1[16]);
		GS_BLOCK_SW64(v0, v2, v1, v3);
	}
	else
	{
		v0 = gs_v4i_upl64v(gs_v4i_loadl(&s0[0]), gs_v4i_loadl(&s1[0]));
		v1 = gs_v4i_upl64v(gs_v4i_loadl(&s0[8]), gs_v4i_loadl(&s1[8]));
		v2 = gs_v4i_upl64v(gs_v4i_loadl(&s0[16]), gs_v4i_loadl(&s1[16]));
		v3 = gs_v4i_upl64v(gs_v4i_loadl(&s0[24]), gs_v4i_loadl(&s1[24]));
	}
#endif
	d[i * 4 + 0] = gs_block_blend_mask(d[i * 4 + 0], v0, mask);
	d[i * 4 + 1] = gs_block_blend_mask(d[i * 4 + 1], v1, mask);
	d[i * 4 + 2] = gs_block_blend_mask(d[i * 4 + 2], v2, mask);
	d[i * 4 + 3] = gs_block_blend_mask(d[i * 4 + 3], v3, mask);
#endif
}

GS_BLOCK_INLINE void gs_block_write_column16(int i, int aligned, u8* dst, const u8* src, int srcpitch)
{
	const u8* s0 = &src[srcpitch * 0];
	const u8* s1 = &src[srcpitch * 1];
#ifdef GS_BLOCK_AVX2
	/* (two halves loaded, not one load and a lane swap) */
	__m256i v0 = GS_BLOCK_PAIR(_mm_loadu_si128((const __m128i*)s0), _mm_loadu_si128((const __m128i*)s1));
	__m256i v1 = GS_BLOCK_PAIR(_mm_loadu_si128((const __m128i*)s0 + 1), _mm_loadu_si128((const __m128i*)s1 + 1));
	(void)aligned;
	GS_BLOCK_SW16_8(v0, v1);
	((__m256i*)dst)[i * 2 + 0] = GS_BLOCK_ACBD(v0);
	((__m256i*)dst)[i * 2 + 1] = GS_BLOCK_ACBD(v1);
#else
	gs_vec4i v0, v1, v2, v3;
#ifdef GS_BLOCK_FAST_UNALIGNED
	(void)aligned;
	v0 = gs_v4i_loadu(&s0[0]);
	v1 = gs_v4i_loadu(&s0[16]);
	v2 = gs_v4i_loadu(&s1[0]);
	v3 = gs_v4i_loadu(&s1[16]);
	GS_BLOCK_SW16(v0, v1, v2, v3);
	GS_BLOCK_SW64(v0, v1, v2, v3);
#else
	if (aligned)
	{
		v0 = gs_v4i_load(&s0[0]);
		v1 = gs_v4i_load(&s0[16]);
		v2 = gs_v4i_load(&s1[0]);
		v3 = gs_v4i_load(&s1[16]);
		GS_BLOCK_SW16(v0, v1, v2, v3);
		GS_BLOCK_SW64(v0, v1, v2, v3);
	}
	else
	{
		v0 = gs_v4i_upl16v(gs_v4i_loadl(&s0[0]), gs_v4i_loadl(&s0[16]));
		v2 = gs_v4i_upl16v(gs_v4i_loadl(&s0[8]), gs_v4i_loadl(&s0[24]));
		v1 = gs_v4i_upl16v(gs_v4i_loadl(&s1[0]), gs_v4i_loadl(&s1[16]));
		v3 = gs_v4i_upl16v(gs_v4i_loadl(&s1[8]), gs_v4i_loadl(&s1[24]));
		GS_BLOCK_SW64(v0, v1, v2, v3);
	}
#endif
	((gs_vec4i*)dst)[i * 4 + 0] = v0;
	((gs_vec4i*)dst)[i * 4 + 1] = v2;
	((gs_vec4i*)dst)[i * 4 + 2] = v1;
	((gs_vec4i*)dst)[i * 4 + 3] = v3;
#endif
}

GS_BLOCK_INLINE void gs_block_write_column8(int i, int aligned, u8* dst, const u8* src, int srcpitch)
{
#ifdef GS_BLOCK_AVX2
	const __m128i v4 = _mm_loadu_si128((const __m128i*)&src[srcpitch * 0]);
	const __m128i v5 = _mm_loadu_si128((const __m128i*)&src[srcpitch * 1]);
	const __m128i v6 = _mm_loadu_si128((const __m128i*)&src[srcpitch * 2]);
	const __m128i v7 = _mm_loadu_si128((const __m128i*)&src[srcpitch * 3]);
	const __m256i v0 = GS_BLOCK_PAIR(v4, v5);
	const __m256i v1 = GS_BLOCK_PAIR(v6, v7);
	const __m256i v2 =
		GS_BLOCK_ACBD(_mm256_shuffle_epi8(_mm256_blend_epi32(v0, v1, 0xaa), GS_BLOCK_BCAST(gs_block_avx2_w8mask1)));
	const __m256i v3 =
		GS_BLOCK_ACBD(_mm256_shuffle_epi8(_mm256_blend_epi32(v1, v0, 0xaa), GS_BLOCK_BCAST(gs_block_avx2_w8mask2)));
	(void)aligned;
	((__m256i*)dst)[i * 2 + 0] = (i & 1) == 0 ? v2 : v3;
	((__m256i*)dst)[i * 2 + 1] = (i & 1) == 0 ? v3 : v2;
#else
	gs_vec4i v0 = aligned ? gs_v4i_load(&src[srcpitch * 0]) : gs_v4i_loadu(&src[srcpitch * 0]);
	gs_vec4i v1 = aligned ? gs_v4i_load(&src[srcpitch * 1]) : gs_v4i_loadu(&src[srcpitch * 1]);
	gs_vec4i v2 = aligned ? gs_v4i_load(&src[srcpitch * 2]) : gs_v4i_loadu(&src[srcpitch * 2]);
	gs_vec4i v3 = aligned ? gs_v4i_load(&src[srcpitch * 3]) : gs_v4i_loadu(&src[srcpitch * 3]);
	if ((i & 1) == 0)
	{
		v2 = GS_V4I_SHUFFLE32(v2, GS_BLOCK_YXWZ);
		v3 = GS_V4I_SHUFFLE32(v3, GS_BLOCK_YXWZ);
	}
	else
	{
		v0 = GS_V4I_SHUFFLE32(v0, GS_BLOCK_YXWZ);
		v1 = GS_V4I_SHUFFLE32(v1, GS_BLOCK_YXWZ);
	}
	GS_BLOCK_SW8(v0, v2, v1, v3);
	GS_BLOCK_SW16(v0, v1, v2, v3);
	GS_BLOCK_SW64(v0, v1, v2, v3);
	((gs_vec4i*)dst)[i * 4 + 0] = v0;
	((gs_vec4i*)dst)[i * 4 + 1] = v2;
	((gs_vec4i*)dst)[i * 4 + 2] = v1;
	((gs_vec4i*)dst)[i * 4 + 3] = v3;
#endif
}

GS_BLOCK_INLINE void gs_block_write_column4(int i, int aligned, u8* dst, const u8* src, int srcpitch)
{
#ifdef GS_BLOCK_AVX2
	__m256i v0 = GS_BLOCK_PAIR(_mm_loadu_si128((const __m128i*)&src[srcpitch * 0]),
		_mm_loadu_si128((const __m128i*)&src[srcpitch * 1]));
	__m256i v1 = GS_BLOCK_PAIR(_mm_loadu_si128((const __m128i*)&src[srcpitch * 2]),
		_mm_loadu_si128((const __m128i*)&src[srcpitch * 3]));
	(void)aligned;
	v0 = GS_BLOCK_ACBD(_mm256_shuffle_epi8(v0, GS_BLOCK_BCAST(gs_block_w4mask)));
	v1 = GS_BLOCK_ACBD(_mm256_shuffle_epi8(v1, GS_BLOCK_BCAST(gs_block_w4mask)));
	if ((i & 1) == 0)
	{
		v0 = _mm256_shuffle_epi32(v0, GS_BLOCK_XZYW);
		v1 = _mm256_shuffle_epi32(v1, GS_BLOCK_YWXZ);
	}
	else
	{
		v0 = _mm256_shuffle_epi32(v0, GS_BLOCK_YWXZ);
		v1 = _mm256_shuffle_epi32(v1, GS_BLOCK_XZYW);
	}
	GS_BLOCK_MIX4_8(v0, v1);
	GS_BLOCK_SW32_8(v0, v1);
	((__m256i*)dst)[i * 2 + 0] = v0;
	((__m256i*)dst)[i * 2 + 1] = v1;
#else
	gs_vec4i v0 = aligned ? gs_v4i_load(&src[srcpitch * 0]) : gs_v4i_loadu(&src[srcpitch * 0]);
	gs_vec4i v1 = aligned ? gs_v4i_load(&src[srcpitch * 1]) : gs_v4i_loadu(&src[srcpitch * 1]);
	gs_vec4i v2 = aligned ? gs_v4i_load(&src[srcpitch * 2]) : gs_v4i_loadu(&src[srcpitch * 2]);
	gs_vec4i v3 = aligned ? gs_v4i_load(&src[srcpitch * 3]) : gs_v4i_loadu(&src[srcpitch * 3]);
	if ((i & 1) == 0)
	{
		v2 = GS_V4I_SHUFFLE16LH(v2, GS_BLOCK_YXWZ);
		v3 = GS_V4I_SHUFFLE16LH(v3, GS_BLOCK_YXWZ);
	}
	else
	{
		v0 = GS_V4I_SHUFFLE16LH(v0, GS_BLOCK_YXWZ);
		v1 = GS_V4I_SHUFFLE16LH(v1, GS_BLOCK_YXWZ);
	}
	GS_BLOCK_SW4(v0, v2, v1, v3);
	GS_BLOCK_SW8(v0, v1, v2, v3);
	GS_BLOCK_SW8(v0, v2, v1, v3);
	GS_BLOCK_SW64(v0, v2, v1, v3);
	((gs_vec4i*)dst)[i * 4 + 0] = v0;
	((gs_vec4i*)dst)[i * 4 + 1] = v1;
	((gs_vec4i*)dst)[i * 4 + 2] = v2;
	((gs_vec4i*)dst)[i * 4 + 3] = v3;
#endif
}

/* (the column of row y) */
GS_BLOCK_INLINE void gs_block_write_column32_y(int y, int aligned, u32 mask, u8* dst, const u8* src, int srcpitch)
{
	switch ((y >> 1) & 3)
	{
		case 0: gs_block_write_column32(0, aligned, mask, dst, src, srcpitch); break;
		case 1: gs_block_write_column32(1, aligned, mask, dst, src, srcpitch); break;
		case 2: gs_block_write_column32(2, aligned, mask, dst, src, srcpitch); break;
		default: gs_block_write_column32(3, aligned, mask, dst, src, srcpitch); break;
	}
}

GS_BLOCK_INLINE void gs_block_write_column16_y(int y, int aligned, u8* dst, const u8* src, int srcpitch)
{
	switch ((y >> 1) & 3)
	{
		case 0: gs_block_write_column16(0, aligned, dst, src, srcpitch); break;
		case 1: gs_block_write_column16(1, aligned, dst, src, srcpitch); break;
		case 2: gs_block_write_column16(2, aligned, dst, src, srcpitch); break;
		default: gs_block_write_column16(3, aligned, dst, src, srcpitch); break;
	}
}

GS_BLOCK_INLINE void gs_block_write_column8_y(int y, int aligned, u8* dst, const u8* src, int srcpitch)
{
	switch ((y >> 2) & 3)
	{
		case 0: gs_block_write_column8(0, aligned, dst, src, srcpitch); break;
		case 1: gs_block_write_column8(1, aligned, dst, src, srcpitch); break;
		case 2: gs_block_write_column8(2, aligned, dst, src, srcpitch); break;
		default: gs_block_write_column8(3, aligned, dst, src, srcpitch); break;
	}
}

GS_BLOCK_INLINE void gs_block_write_column4_y(int y, int aligned, u8* dst, const u8* src, int srcpitch)
{
	switch ((y >> 2) & 3)
	{
		case 0: gs_block_write_column4(0, aligned, dst, src, srcpitch); break;
		case 1: gs_block_write_column4(1, aligned, dst, src, srcpitch); break;
		case 2: gs_block_write_column4(2, aligned, dst, src, srcpitch); break;
		default: gs_block_write_column4(3, aligned, dst, src, srcpitch); break;
	}
}

/* ---- writing a block: all four columns ---- */

GS_BLOCK_INLINE void gs_block_write_block32(int aligned, u32 mask, u8* dst, const u8* src, int srcpitch)
{
	gs_block_write_column32(0, aligned, mask, dst, src, srcpitch);
	src += srcpitch * 2;
	gs_block_write_column32(1, aligned, mask, dst, src, srcpitch);
	src += srcpitch * 2;
	gs_block_write_column32(2, aligned, mask, dst, src, srcpitch);
	src += srcpitch * 2;
	gs_block_write_column32(3, aligned, mask, dst, src, srcpitch);
}

GS_BLOCK_INLINE void gs_block_write_block16(int aligned, u8* dst, const u8* src, int srcpitch)
{
	gs_block_write_column16(0, aligned, dst, src, srcpitch);
	src += srcpitch * 2;
	gs_block_write_column16(1, aligned, dst, src, srcpitch);
	src += srcpitch * 2;
	gs_block_write_column16(2, aligned, dst, src, srcpitch);
	src += srcpitch * 2;
	gs_block_write_column16(3, aligned, dst, src, srcpitch);
}

GS_BLOCK_INLINE void gs_block_write_block8(int aligned, u8* dst, const u8* src, int srcpitch)
{
	gs_block_write_column8(0, aligned, dst, src, srcpitch);
	src += srcpitch * 4;
	gs_block_write_column8(1, aligned, dst, src, srcpitch);
	src += srcpitch * 4;
	gs_block_write_column8(2, aligned, dst, src, srcpitch);
	src += srcpitch * 4;
	gs_block_write_column8(3, aligned, dst, src, srcpitch);
}

GS_BLOCK_INLINE void gs_block_write_block4(int aligned, u8* dst, const u8* src, int srcpitch)
{
	gs_block_write_column4(0, aligned, dst, src, srcpitch);
	src += srcpitch * 4;
	gs_block_write_column4(1, aligned, dst, src, srcpitch);
	src += srcpitch * 4;
	gs_block_write_column4(2, aligned, dst, src, srcpitch);
	src += srcpitch * 4;
	gs_block_write_column4(3, aligned, dst, src, srcpitch);
}

/* ---- reading a column: the block out to rows (16-byte aligned) ---- */

GS_BLOCK_INLINE void gs_block_read_column32(int i, const u8* src, u8* dst, int dstpitch)
{
#ifdef GS_BLOCK_AVX2
	const __m128i* s = (const __m128i*)src;
	__m256i v0       = GS_BLOCK_PAIR(_mm_load_si128(&s[i * 4 + 0]), _mm_load_si128(&s[i * 4 + 2]));
	__m256i v1       = GS_BLOCK_PAIR(_mm_load_si128(&s[i * 4 + 1]), _mm_load_si128(&s[i * 4 + 3]));
	GS_BLOCK_SW64_8(v0, v1);
	_mm256_store_si256((__m256i*)&dst[dstpitch * 0], v0);
	_mm256_store_si256((__m256i*)&dst[dstpitch * 1], v1);
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	gs_vec4i v0       = s[i * 4 + 0];
	gs_vec4i v1       = s[i * 4 + 1];
	gs_vec4i v2       = s[i * 4 + 2];
	gs_vec4i v3       = s[i * 4 + 3];
	gs_vec4i* d0      = (gs_vec4i*)&dst[dstpitch * 0];
	gs_vec4i* d1      = (gs_vec4i*)&dst[dstpitch * 1];
	GS_BLOCK_SW64(v0, v1, v2, v3);
	gs_v4i_store(&d0[0], v0);
	gs_v4i_store(&d0[1], v1);
	gs_v4i_store(&d1[0], v2);
	gs_v4i_store(&d1[1], v3);
#endif
}

GS_BLOCK_INLINE void gs_block_read_column16(int i, const u8* src, u8* dst, int dstpitch)
{
#ifdef GS_BLOCK_AVX2
	const __m256i* s   = (const __m256i*)src;
	const __m256i mask = GS_BLOCK_BCAST(gs_block_r16mask);
	const __m256i v0   = GS_BLOCK_ACBD(_mm256_shuffle_epi8(s[i * 2 + 0], mask));
	const __m256i v1   = GS_BLOCK_ACBD(_mm256_shuffle_epi8(s[i * 2 + 1], mask));
	_mm256_store_si256((__m256i*)&dst[dstpitch * 0], GS_BLOCK_SHUF2_8(v0, v1, GS_BLOCK_XZXZ));
	_mm256_store_si256((__m256i*)&dst[dstpitch * 1], GS_BLOCK_SHUF2_8(v0, v1, GS_BLOCK_YWYW));
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	gs_vec4i v0       = s[i * 4 + 0];
	gs_vec4i v1       = s[i * 4 + 1];
	gs_vec4i v2       = s[i * 4 + 2];
	gs_vec4i v3       = s[i * 4 + 3];
	gs_vec4i* d0      = (gs_vec4i*)&dst[dstpitch * 0];
	gs_vec4i* d1      = (gs_vec4i*)&dst[dstpitch * 1];
	GS_BLOCK_SW16(v0, v1, v2, v3);
	GS_BLOCK_SW32(v0, v1, v2, v3);
	GS_BLOCK_SW16(v0, v2, v1, v3);
	gs_v4i_store(&d0[0], v0);
	gs_v4i_store(&d0[1], v1);
	gs_v4i_store(&d1[0], v2);
	gs_v4i_store(&d1[1], v3);
#endif
}

GS_BLOCK_INLINE void gs_block_read_column8(int i, const u8* src, u8* dst, int dstpitch)
{
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i a  = (i & 1) == 0 ? s[i * 2 + 0] : s[i * 2 + 1];
	const __m256i b  = (i & 1) == 0 ? s[i * 2 + 1] : s[i * 2 + 0];
	const __m256i v2 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(a), GS_BLOCK_BCAST(gs_block_avx2_r8mask1));
	const __m256i v3 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(b), GS_BLOCK_BCAST(gs_block_avx2_r8mask2));
	const __m256i v0 = _mm256_blend_epi32(v2, v3, 0xaa);
	const __m256i v1 = _mm256_blend_epi32(v3, v2, 0xaa);
	GS_BLOCK_STOREL8(&dst[dstpitch * 0], v0);
	GS_BLOCK_STOREH8(&dst[dstpitch * 1], v0);
	GS_BLOCK_STOREL8(&dst[dstpitch * 2], v1);
	GS_BLOCK_STOREH8(&dst[dstpitch * 3], v1);
#else
	const gs_vec4i* s  = (const gs_vec4i*)src;
	const gs_vec4i msk = gs_block_r8mask.v;
	const int o        = (i & 1) == 0 ? 0 : 2;
	gs_vec4i v0        = gs_v4i_shuffle8(s[i * 4 + (0 ^ o)], msk);
	gs_vec4i v1        = gs_v4i_shuffle8(s[i * 4 + (1 ^ o)], msk);
	gs_vec4i v2        = gs_v4i_shuffle8(s[i * 4 + (2 ^ o)], msk);
	gs_vec4i v3        = gs_v4i_shuffle8(s[i * 4 + (3 ^ o)], msk);
	GS_BLOCK_SW16(v0, v1, v2, v3);
	GS_BLOCK_SW32(v0, v1, v3, v2);
	gs_v4i_store(&dst[dstpitch * 0], v0);
	gs_v4i_store(&dst[dstpitch * 1], v3);
	gs_v4i_store(&dst[dstpitch * 2], v1);
	gs_v4i_store(&dst[dstpitch * 3], v2);
#endif
}

GS_BLOCK_INLINE void gs_block_read_column4(int i, const u8* src, u8* dst, int dstpitch)
{
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	__m256i v0       = s[i * 2 + 0];
	__m256i v1       = s[i * 2 + 1];
	GS_BLOCK_SW32_INV_8(v0, v1);
	GS_BLOCK_MIX4_8(v0, v1);
	if ((i & 1) == 0)
	{
		v0 = _mm256_shuffle_epi32(v0, GS_BLOCK_XZYW);
		v1 = _mm256_shuffle_epi32(v1, GS_BLOCK_ZXWY);
	}
	else
	{
		v0 = _mm256_shuffle_epi32(v0, GS_BLOCK_ZXWY);
		v1 = _mm256_shuffle_epi32(v1, GS_BLOCK_XZYW);
	}
	v0 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(v0), GS_BLOCK_BCAST(gs_block_r4mask));
	v1 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(v1), GS_BLOCK_BCAST(gs_block_r4mask));
	GS_BLOCK_STOREL8(&dst[dstpitch * 0], v0);
	GS_BLOCK_STOREH8(&dst[dstpitch * 1], v0);
	GS_BLOCK_STOREL8(&dst[dstpitch * 2], v1);
	GS_BLOCK_STOREH8(&dst[dstpitch * 3], v1);
#else
	const gs_vec4i* s  = (const gs_vec4i*)src;
	const gs_vec4i msk = gs_block_r4mask.v;
	gs_vec4i v0        = s[i * 4 + 0];
	gs_vec4i v1        = s[i * 4 + 1];
	gs_vec4i v2        = s[i * 4 + 2];
	gs_vec4i v3        = s[i * 4 + 3];
	gs_vec4i r0, r1, r2, r3;
	GS_BLOCK_SW32_INV(v0, v1, v2, v3);
	GS_BLOCK_MIX4(v0, v1);
	GS_BLOCK_MIX4(v2, v3);
	if ((i & 1) == 0)
	{
		r0 = GS_V4I_SHUFFLE2(v0, v2, GS_BLOCK_XZXZ);
		r1 = GS_V4I_SHUFFLE2(v0, v2, GS_BLOCK_YWYW);
		r2 = GS_V4I_SHUFFLE2(v1, v3, GS_BLOCK_ZXZX);
		r3 = GS_V4I_SHUFFLE2(v1, v3, GS_BLOCK_WYWY);
	}
	else
	{
		r0 = GS_V4I_SHUFFLE2(v0, v2, GS_BLOCK_ZXZX);
		r1 = GS_V4I_SHUFFLE2(v0, v2, GS_BLOCK_WYWY);
		r2 = GS_V4I_SHUFFLE2(v1, v3, GS_BLOCK_XZXZ);
		r3 = GS_V4I_SHUFFLE2(v1, v3, GS_BLOCK_YWYW);
	}
	gs_v4i_store(&dst[dstpitch * 0], gs_v4i_shuffle8(r0, msk));
	gs_v4i_store(&dst[dstpitch * 1], gs_v4i_shuffle8(r1, msk));
	gs_v4i_store(&dst[dstpitch * 2], gs_v4i_shuffle8(r2, msk));
	gs_v4i_store(&dst[dstpitch * 3], gs_v4i_shuffle8(r3, msk));
#endif
}

GS_BLOCK_INLINE void gs_block_read_column32_y(int y, const u8* src, u8* dst, int dstpitch)
{
	switch ((y >> 1) & 3)
	{
		case 0: gs_block_read_column32(0, src, dst, dstpitch); break;
		case 1: gs_block_read_column32(1, src, dst, dstpitch); break;
		case 2: gs_block_read_column32(2, src, dst, dstpitch); break;
		default: gs_block_read_column32(3, src, dst, dstpitch); break;
	}
}

GS_BLOCK_INLINE void gs_block_read_column16_y(int y, const u8* src, u8* dst, int dstpitch)
{
	switch ((y >> 1) & 3)
	{
		case 0: gs_block_read_column16(0, src, dst, dstpitch); break;
		case 1: gs_block_read_column16(1, src, dst, dstpitch); break;
		case 2: gs_block_read_column16(2, src, dst, dstpitch); break;
		default: gs_block_read_column16(3, src, dst, dstpitch); break;
	}
}

GS_BLOCK_INLINE void gs_block_read_column8_y(int y, const u8* src, u8* dst, int dstpitch)
{
	switch ((y >> 2) & 3)
	{
		case 0: gs_block_read_column8(0, src, dst, dstpitch); break;
		case 1: gs_block_read_column8(1, src, dst, dstpitch); break;
		case 2: gs_block_read_column8(2, src, dst, dstpitch); break;
		default: gs_block_read_column8(3, src, dst, dstpitch); break;
	}
}

GS_BLOCK_INLINE void gs_block_read_column4_y(int y, const u8* src, u8* dst, int dstpitch)
{
	switch ((y >> 2) & 3)
	{
		case 0: gs_block_read_column4(0, src, dst, dstpitch); break;
		case 1: gs_block_read_column4(1, src, dst, dstpitch); break;
		case 2: gs_block_read_column4(2, src, dst, dstpitch); break;
		default: gs_block_read_column4(3, src, dst, dstpitch); break;
	}
}

/* ---- reading a block ---- */

GS_BLOCK_INLINE void gs_block_read_block32(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_column32(0, src, dst, dstpitch);
	dst += dstpitch * 2;
	gs_block_read_column32(1, src, dst, dstpitch);
	dst += dstpitch * 2;
	gs_block_read_column32(2, src, dst, dstpitch);
	dst += dstpitch * 2;
	gs_block_read_column32(3, src, dst, dstpitch);
}

GS_BLOCK_INLINE void gs_block_read_block16(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_column16(0, src, dst, dstpitch);
	dst += dstpitch * 2;
	gs_block_read_column16(1, src, dst, dstpitch);
	dst += dstpitch * 2;
	gs_block_read_column16(2, src, dst, dstpitch);
	dst += dstpitch * 2;
	gs_block_read_column16(3, src, dst, dstpitch);
}

GS_BLOCK_INLINE void gs_block_read_block8(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_column8(0, src, dst, dstpitch);
	dst += dstpitch * 4;
	gs_block_read_column8(1, src, dst, dstpitch);
	dst += dstpitch * 4;
	gs_block_read_column8(2, src, dst, dstpitch);
	dst += dstpitch * 4;
	gs_block_read_column8(3, src, dst, dstpitch);
}

GS_BLOCK_INLINE void gs_block_read_block4(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_column4(0, src, dst, dstpitch);
	dst += dstpitch * 4;
	gs_block_read_column4(1, src, dst, dstpitch);
	dst += dstpitch * 4;
	gs_block_read_column4(2, src, dst, dstpitch);
	dst += dstpitch * 4;
	gs_block_read_column4(3, src, dst, dstpitch);
}

/* A 4-bit block read out a byte a texel (the palette index), 32 rows of
 * 32 bytes... as eight rows of 32 */
GS_BLOCK_INLINE void gs_block_read_block4p(const u8* src, u8* dst, int dstpitch)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s    = (const __m256i*)src;
	const __m256i shuf  = GS_BLOCK_BCAST(gs_block_palvec_mask);
	const __m256i mask  = _mm256_set1_epi32(0x0f0f0f0f);
	__m256i v0, v1;
	for (i = 0; i < 2; i++)
	{
		/* columns 0 and 2 */
		v0 = s[i * 4 + 0];
		v1 = s[i * 4 + 1];
		GS_BLOCK_SW8_8(v0, v1);
		v0 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(_mm256_shuffle_epi32(v0, GS_BLOCK_XZYW)), shuf);
		v1 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(_mm256_shuffle_epi32(v1, GS_BLOCK_XZYW)), shuf);
		_mm256_store_si256((__m256i*)(dst + dstpitch * 0), _mm256_and_si256(v0, mask));
		_mm256_store_si256((__m256i*)(dst + dstpitch * 1), _mm256_and_si256(v1, mask));
		_mm256_store_si256((__m256i*)(dst + dstpitch * 2),
			_mm256_and_si256(_mm256_srli_epi32(_mm256_shuffle_epi32(v0, GS_BLOCK_YXWZ), 4), mask));
		_mm256_store_si256((__m256i*)(dst + dstpitch * 3),
			_mm256_and_si256(_mm256_srli_epi32(_mm256_shuffle_epi32(v1, GS_BLOCK_YXWZ), 4), mask));
		dst += dstpitch * 4;

		/* columns 1 and 3 */
		v0 = s[i * 4 + 2];
		v1 = s[i * 4 + 3];
		GS_BLOCK_SW8_8(v0, v1);
		v0 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(_mm256_shuffle_epi32(v0, GS_BLOCK_XZYW)), shuf);
		v1 = _mm256_shuffle_epi8(GS_BLOCK_ACBD(_mm256_shuffle_epi32(v1, GS_BLOCK_XZYW)), shuf);
		_mm256_store_si256((__m256i*)(dst + dstpitch * 0), _mm256_and_si256(_mm256_shuffle_epi32(v0, GS_BLOCK_YXWZ), mask));
		_mm256_store_si256((__m256i*)(dst + dstpitch * 1), _mm256_and_si256(_mm256_shuffle_epi32(v1, GS_BLOCK_YXWZ), mask));
		_mm256_store_si256((__m256i*)(dst + dstpitch * 2), _mm256_and_si256(_mm256_srli_epi32(v0, 4), mask));
		_mm256_store_si256((__m256i*)(dst + dstpitch * 3), _mm256_and_si256(_mm256_srli_epi32(v1, 4), mask));
		dst += dstpitch * 4;
	}
#else
	const gs_vec4i* s   = (const gs_vec4i*)src;
	const gs_vec4i mask = gs_v4i_set32(0x0f0f0f0f);
	gs_vec4i v0, v1, v2, v3;
	for (i = 0; i < 2; i++)
	{
		/* columns 0 and 2 */
		v0 = s[i * 8 + 0];
		v1 = s[i * 8 + 1];
		v2 = s[i * 8 + 2];
		v3 = s[i * 8 + 3];
		GS_BLOCK_SW8(v0, v1, v2, v3);
		GS_BLOCK_SW16(v0, v1, v2, v3);
		GS_BLOCK_SW8(v0, v2, v1, v3);
		gs_v4i_store(&dst[dstpitch * 0 + 0], gs_v4i_and(v0, mask));
		gs_v4i_store(&dst[dstpitch * 0 + 16], gs_v4i_and(v1, mask));
		gs_v4i_store(&dst[dstpitch * 1 + 0], gs_v4i_and(v2, mask));
		gs_v4i_store(&dst[dstpitch * 1 + 16], gs_v4i_and(v3, mask));
		dst += dstpitch * 2;
		gs_v4i_store(&dst[dstpitch * 0 + 0], GS_V4I_SRL32(GS_V4I_SHUFFLE32(gs_v4i_andnot(mask, v0), GS_BLOCK_YXWZ), 4));
		gs_v4i_store(&dst[dstpitch * 0 + 16], GS_V4I_SRL32(GS_V4I_SHUFFLE32(gs_v4i_andnot(mask, v1), GS_BLOCK_YXWZ), 4));
		gs_v4i_store(&dst[dstpitch * 1 + 0], GS_V4I_SRL32(GS_V4I_SHUFFLE32(gs_v4i_andnot(mask, v2), GS_BLOCK_YXWZ), 4));
		gs_v4i_store(&dst[dstpitch * 1 + 16], GS_V4I_SRL32(GS_V4I_SHUFFLE32(gs_v4i_andnot(mask, v3), GS_BLOCK_YXWZ), 4));
		dst += dstpitch * 2;

		/* columns 1 and 3 */
		v0 = s[i * 8 + 4];
		v1 = s[i * 8 + 5];
		v2 = s[i * 8 + 6];
		v3 = s[i * 8 + 7];
		GS_BLOCK_SW8(v0, v1, v2, v3);
		GS_BLOCK_SW16(v0, v1, v2, v3);
		GS_BLOCK_SW8(v0, v2, v1, v3);
		gs_v4i_store(&dst[dstpitch * 0 + 0], GS_V4I_SHUFFLE32(gs_v4i_and(v0, mask), GS_BLOCK_YXWZ));
		gs_v4i_store(&dst[dstpitch * 0 + 16], GS_V4I_SHUFFLE32(gs_v4i_and(v1, mask), GS_BLOCK_YXWZ));
		gs_v4i_store(&dst[dstpitch * 1 + 0], GS_V4I_SHUFFLE32(gs_v4i_and(v2, mask), GS_BLOCK_YXWZ));
		gs_v4i_store(&dst[dstpitch * 1 + 16], GS_V4I_SHUFFLE32(gs_v4i_and(v3, mask), GS_BLOCK_YXWZ));
		dst += dstpitch * 2;
		gs_v4i_store(&dst[dstpitch * 0 + 0], GS_V4I_SRL32(gs_v4i_andnot(mask, v0), 4));
		gs_v4i_store(&dst[dstpitch * 0 + 16], GS_V4I_SRL32(gs_v4i_andnot(mask, v1), 4));
		gs_v4i_store(&dst[dstpitch * 1 + 0], GS_V4I_SRL32(gs_v4i_andnot(mask, v2), 4));
		gs_v4i_store(&dst[dstpitch * 1 + 16], GS_V4I_SRL32(gs_v4i_andnot(mask, v3), 4));
		dst += dstpitch * 2;
	}
#endif
}

/* The top bits of a 32-bit block (shift 24 for 8H and 4HL, 28 for 4HH)
 * read out a byte a pixel, masked (0x0f0f0f0f for 4HL) */
GS_BLOCK_INLINE void gs_block_read_block_hp(int shift, u32 mask, const u8* src, u8* dst, int dstpitch)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s      = (const __m256i*)src;
	const __m256i maskvec = _mm256_set1_epi32((int)mask);
	__m256i v0, v1, v2, v3;
	__m128i v4, v5;
	for (i = 0; i < 2; i++)
	{
		v0 = GS_BLOCK_ACBD(s[i * 4 + 0]);
		v1 = GS_BLOCK_ACBD(s[i * 4 + 1]);
		v2 = GS_BLOCK_ACBD(s[i * 4 + 2]);
		v3 = GS_BLOCK_ACBD(s[i * 4 + 3]);
		v0 = _mm256_packus_epi16(_mm256_packs_epi32(_mm256_srl_epi32(v0, _mm_cvtsi32_si128(shift)), _mm256_srl_epi32(v1, _mm_cvtsi32_si128(shift))),
			_mm256_packs_epi32(_mm256_srl_epi32(v2, _mm_cvtsi32_si128(shift)), _mm256_srl_epi32(v3, _mm_cvtsi32_si128(shift))));
		if (mask != 0xffffffffu)
			v0 = _mm256_and_si256(v0, maskvec);
		v4 = _mm256_castsi256_si128(v0);
		v5 = _mm256_extracti128_si256(v0, 1);
		gs_v4i_storel(&dst[dstpitch * 0], v4);
		gs_v4i_storel(&dst[dstpitch * 1], v5);
		gs_v4i_storeh(&dst[dstpitch * 2], v4);
		gs_v4i_storeh(&dst[dstpitch * 3], v5);
		dst += dstpitch * 4;
	}
#else
	const gs_vec4i* s      = (const gs_vec4i*)src;
	const gs_vec4i maskvec = gs_v4i_set32((int32_t)mask);
	gs_vec4i v0, v1, v2, v3;
	for (i = 0; i < 4; i++)
	{
		v0 = s[i * 4 + 0];
		v1 = s[i * 4 + 1];
		v2 = s[i * 4 + 2];
		v3 = s[i * 4 + 3];
		GS_BLOCK_SW64(v0, v1, v2, v3);
		v0 = gs_v4i_pu16(gs_v4i_ps32(GS_BLOCK_SRL32V(v0, shift), GS_BLOCK_SRL32V(v1, shift)),
			gs_v4i_ps32(GS_BLOCK_SRL32V(v2, shift), GS_BLOCK_SRL32V(v3, shift)));
		if (mask != 0xffffffffu)
			v0 = gs_v4i_and(v0, maskvec);
		gs_v4i_storel(dst, v0);
		dst += dstpitch;
		gs_v4i_storeh(dst, v0);
		dst += dstpitch;
	}
#endif
}

GS_BLOCK_INLINE void gs_block_read_block8hp(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_block_hp(24, 0xffffffffu, src, dst, dstpitch);
}

GS_BLOCK_INLINE void gs_block_read_block4hlp(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_block_hp(24, 0x0f0f0f0fu, src, dst, dstpitch);
}

GS_BLOCK_INLINE void gs_block_read_block4hhp(const u8* src, u8* dst, int dstpitch)
{
	gs_block_read_block_hp(28, 0xffffffffu, src, dst, dstpitch);
}

/* ==== Expanding blocks to 32-bit pixels, and unpacking 24- and 8/4-bit
 * high pixels into a block ==== */

/* The 24- and 16-bit pixel expanders (GSBlock::Expand*to32): TA0/TA1
 * already in the top byte; aem: a pixel of zero colour takes no TA0 */
GS_BLOCK_INLINE gs_vec4i gs_block_expand24(int aem, gs_vec4i c, gs_vec4i ta0)
{
	return gs_v4i_or(c, aem ? gs_v4i_andnot(gs_v4i_eq32(c, gs_v4i_zero()), ta0) : ta0);
}

/* The 16-bit pixel in both halves of each dword */
GS_BLOCK_INLINE gs_vec4i gs_block_expand16(int aem, gs_vec4i c, gs_vec4i ta0, gs_vec4i ta1)
{
	const gs_vec4i o = gs_v4i_or(gs_v4i_or(gs_v4i_and(GS_V4I_SLL32(c, 3), gs_v4i_set32(0x000000f8)),
		gs_v4i_and(GS_V4I_SLL32(c, 6), gs_v4i_set32(0x0000f800))), gs_v4i_and(GS_V4I_SLL32(c, 9), gs_v4i_set32(0x00f80000)));
	const gs_vec4i ta = gs_v4i_blend8(ta0, ta1, c);
	return gs_v4i_or(o, aem ? gs_v4i_andnot(gs_v4i_eq32(c, gs_v4i_zero()), ta) : ta);
}

/* The 16-bit pixel in the low half of each dword */
GS_BLOCK_INLINE gs_vec4i gs_block_expand16l(int aem, gs_vec4i c, gs_vec4i ta0, gs_vec4i ta1)
{
	const gs_vec4i o = gs_v4i_or(gs_v4i_or(gs_v4i_and(GS_V4I_SLL32(c, 3), gs_v4i_set32(0x000000f8)),
		gs_v4i_and(GS_V4I_SLL32(c, 6), gs_v4i_set32(0x0000f800))), gs_v4i_and(GS_V4I_SLL32(c, 9), gs_v4i_set32(0x00f80000)));
	const gs_vec4i t = aem ? gs_v4i_andnot(gs_v4i_eq32(o, gs_v4i_zero()), ta0) : ta0;
	return gs_v4i_or(o, gs_v4i_blend8(t, ta1, GS_V4I_SLL32(c, 16)));
}

/* The 16-bit pixel in the high half of each dword */
GS_BLOCK_INLINE gs_vec4i gs_block_expand16h(int aem, gs_vec4i c, gs_vec4i ta0, gs_vec4i ta1)
{
	const gs_vec4i o = gs_v4i_or(gs_v4i_or(gs_v4i_and(GS_V4I_SRL32(c, 13), gs_v4i_set32(0x000000f8)),
		gs_v4i_and(GS_V4I_SRL32(c, 10), gs_v4i_set32(0x0000f800))), gs_v4i_and(GS_V4I_SRL32(c, 7), gs_v4i_set32(0x00f80000)));
	const gs_vec4i t = aem ? gs_v4i_andnot(gs_v4i_eq32(o, gs_v4i_zero()), ta0) : ta0;
	return gs_v4i_or(o, gs_v4i_blend8(t, ta1, c));
}

/* A 16-entry palette's 32-bit colours as four vectors of their bytes
 * (byte k of every entry in p[k]), for byte shuffles to look up */
GS_BLOCK_INLINE void gs_block_pal_vecs(const u32* pal, gs_vec4i* p0, gs_vec4i* p1, gs_vec4i* p2, gs_vec4i* p3)
{
	gs_vec4i a = gs_v4i_shuffle8(gs_v4i_load(pal + 0), gs_block_palvec_mask.v);
	gs_vec4i b = gs_v4i_shuffle8(gs_v4i_load(pal + 4), gs_block_palvec_mask.v);
	gs_vec4i c = gs_v4i_shuffle8(gs_v4i_load(pal + 8), gs_block_palvec_mask.v);
	gs_vec4i d = gs_v4i_shuffle8(gs_v4i_load(pal + 12), gs_block_palvec_mask.v);
	GS_BLOCK_SW32(a, b, c, d);
	GS_BLOCK_SW64(a, b, c, d);
	*p0 = a;
	*p1 = c;
	*p2 = b;
	*p3 = d;
}

/* Sixteen 4-bit indexes (one a byte) to their 32-bit colours, four
 * vectors of them */
#define GS_BLOCK_CLUT4(p0, p1, p2, p3, idx, d0, d1, d2, d3)        \
	do                                                             \
	{                                                              \
		gs_vec4i r0_ = gs_v4i_shuffle8((p0), (idx));               \
		gs_vec4i r1_ = gs_v4i_shuffle8((p1), (idx));               \
		gs_vec4i r2_ = gs_v4i_shuffle8((p2), (idx));               \
		gs_vec4i r3_ = gs_v4i_shuffle8((p3), (idx));               \
		GS_BLOCK_SW8(r0_, r1_, r2_, r3_);                          \
		GS_BLOCK_SW16(r0_, r1_, r2_, r3_);                         \
		(d0) = r0_;                                                \
		(d1) = r2_;                                                \
		(d2) = r1_;                                                \
		(d3) = r3_;                                                \
	} while (0)

/* Four palette lookups of 32 bits by the bytes of idx from byte k on */
#define GS_BLOCK_GATHER32_8(pal, b, k) \
	gs_v4i_set4((int32_t)(pal)[(b)[(k) + 0]], (int32_t)(pal)[(b)[(k) + 1]], (int32_t)(pal)[(b)[(k) + 2]], (int32_t)(pal)[(b)[(k) + 3]])

/* The bytes of a vector, for scalar lookups by them */
typedef union gs_block_bytes
{
	u8 b[16];
	u32 u[4];
	gs_vec4i v;
} gs_block_bytes;

#ifdef GS_BLOCK_AVX2

#define GS_BLOCK_GATHER8(pal, idx) _mm256_i32gather_epi32((const int*)(pal), (idx), 4)

/* a: the low halves of two 32-byte rows, b: their high halves (as one
 * load of each and a lane swap) */
#define GS_BLOCK_LOADSW128(a, b, s0, s1)                                                                  \
	do                                                                                                    \
	{                                                                                                     \
		(a) = GS_BLOCK_PAIR(_mm_load_si128((const __m128i*)(s0)), _mm_load_si128((const __m128i*)(s1)));         \
		(b) = GS_BLOCK_PAIR(_mm_load_si128((const __m128i*)(s0) + 1), _mm_load_si128((const __m128i*)(s1) + 1)); \
	} while (0)

GS_BLOCK_INLINE __m256i gs_block_expand24_8(int aem, __m256i c, __m256i ta0)
{
	return _mm256_or_si256(c, aem ? _mm256_andnot_si256(_mm256_cmpeq_epi32(c, _mm256_setzero_si256()), ta0) : ta0);
}

GS_BLOCK_INLINE __m256i gs_block_rgb15_8(__m256i c)
{
	return _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(_mm256_slli_epi32(c, 3), _mm256_set1_epi32(0x000000f8)),
		_mm256_and_si256(_mm256_slli_epi32(c, 6), _mm256_set1_epi32(0x0000f800))),
		_mm256_and_si256(_mm256_slli_epi32(c, 9), _mm256_set1_epi32(0x00f80000)));
}

GS_BLOCK_INLINE __m256i gs_block_expand16_8(int aem, __m256i c, __m256i ta0, __m256i ta1)
{
	const __m256i ta = _mm256_blendv_epi8(ta0, ta1, c);
	return _mm256_or_si256(gs_block_rgb15_8(c), aem ? _mm256_andnot_si256(_mm256_cmpeq_epi32(c, _mm256_setzero_si256()), ta) : ta);
}

GS_BLOCK_INLINE __m256i gs_block_expand16l_8(int aem, __m256i c, __m256i ta0, __m256i ta1)
{
	const __m256i o = gs_block_rgb15_8(c);
	const __m256i t = aem ? _mm256_andnot_si256(_mm256_cmpeq_epi32(o, _mm256_setzero_si256()), ta0) : ta0;
	return _mm256_or_si256(o, _mm256_blendv_epi8(t, ta1, _mm256_slli_epi32(c, 16)));
}

GS_BLOCK_INLINE __m256i gs_block_expand16h_8(int aem, __m256i c, __m256i ta0, __m256i ta1)
{
	const __m256i o = _mm256_or_si256(_mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(c, 13), _mm256_set1_epi32(0x000000f8)),
		_mm256_and_si256(_mm256_srli_epi32(c, 10), _mm256_set1_epi32(0x0000f800))),
		_mm256_and_si256(_mm256_srli_epi32(c, 7), _mm256_set1_epi32(0x00f80000)));
	const __m256i t = aem ? _mm256_andnot_si256(_mm256_cmpeq_epi32(o, _mm256_setzero_si256()), ta0) : ta0;
	return _mm256_or_si256(o, _mm256_blendv_epi8(t, ta1, c));
}

GS_BLOCK_INLINE void gs_block_pal_vecs8(const u32* pal, __m256i* p0, __m256i* p1, __m256i* p2, __m256i* p3)
{
	const __m256i m = GS_BLOCK_BCAST(gs_block_palvec_mask);
	__m256i a = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128((const __m128i*)pal + 0)), m);
	__m256i b = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128((const __m128i*)pal + 1)), m);
	__m256i c = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128((const __m128i*)pal + 2)), m);
	__m256i d = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128((const __m128i*)pal + 3)), m);
	__m256i e, f;
	e = a; f = c;
	a = _mm256_unpacklo_epi32(e, b); c = _mm256_unpackhi_epi32(e, b);
	b = _mm256_unpacklo_epi32(f, d); d = _mm256_unpackhi_epi32(f, d);
	e = a; f = c;
	a = _mm256_unpacklo_epi64(e, b); c = _mm256_unpackhi_epi64(e, b);
	b = _mm256_unpacklo_epi64(f, d); d = _mm256_unpackhi_epi64(f, d);
	*p0 = a;
	*p1 = c;
	*p2 = b;
	*p3 = d;
}

#define GS_BLOCK_CLUT4_8(p0, p1, p2, p3, idx, d0, d1, d2, d3)                          \
	do                                                                                 \
	{                                                                                  \
		__m256i r0_ = _mm256_shuffle_epi8((p0), (idx));                                \
		__m256i r1_ = _mm256_shuffle_epi8((p1), (idx));                                \
		__m256i r2_ = _mm256_shuffle_epi8((p2), (idx));                                \
		__m256i r3_ = _mm256_shuffle_epi8((p3), (idx));                                \
		__m256i e_ = r0_, f_ = r2_;                                                    \
		r0_ = _mm256_unpacklo_epi8(e_, r1_); r2_ = _mm256_unpackhi_epi8(e_, r1_);      \
		r1_ = _mm256_unpacklo_epi8(f_, r3_); r3_ = _mm256_unpackhi_epi8(f_, r3_);      \
		e_ = r0_; f_ = r2_;                                                            \
		r0_ = _mm256_unpacklo_epi16(e_, r1_); r2_ = _mm256_unpackhi_epi16(e_, r1_);    \
		r1_ = _mm256_unpacklo_epi16(f_, r3_); r3_ = _mm256_unpackhi_epi16(f_, r3_);    \
		_mm256_store_si256((__m256i*)(d0), r0_);                                       \
		_mm256_store_si256((__m256i*)(d1), r2_);                                       \
		_mm256_store_si256((__m256i*)(d2), r1_);                                       \
		_mm256_store_si256((__m256i*)(d3), r3_);                                       \
	} while (0)

#endif

/* ---- expanding a block in its own layout (GSBlock::ExpandBlock*) ---- */

/* 24-bit pixels: ta0, TEXA.TA0 */
GS_BLOCK_INLINE void gs_block_expand_block24(int aem, const u32* src, u8* dst, int dstpitch, u32 ta0)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i ta = _mm256_set1_epi32((int)(ta0 << 24));
	const __m256i mask = _mm256_set1_epi32(0x00ffffff);
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		const __m256i v0 = _mm256_and_si256(s[i * 2 + 0], mask);
		const __m256i v1 = _mm256_and_si256(s[i * 2 + 1], mask);
		*(__m256i*)&dst[dstpitch * 0] = gs_block_expand24_8(aem, v0, ta);
		*(__m256i*)&dst[dstpitch * 1] = gs_block_expand24_8(aem, v1, ta);
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	const gs_vec4i ta = gs_v4i_set32((int32_t)(ta0 << 24));
	const gs_vec4i mask = gs_v4i_set32(0x00ffffff);
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		gs_vec4i* d0 = (gs_vec4i*)&dst[dstpitch * 0];
		gs_vec4i* d1 = (gs_vec4i*)&dst[dstpitch * 1];
		d0[0] = gs_block_expand24(aem, gs_v4i_and(s[i * 4 + 0], mask), ta);
		d0[1] = gs_block_expand24(aem, gs_v4i_and(s[i * 4 + 1], mask), ta);
		d1[0] = gs_block_expand24(aem, gs_v4i_and(s[i * 4 + 2], mask), ta);
		d1[1] = gs_block_expand24(aem, gs_v4i_and(s[i * 4 + 3], mask), ta);
	}
#endif
}

/* 16-bit pixels: ta0 and ta1, TEXA.TA0 and TA1 */
GS_BLOCK_INLINE void gs_block_expand_block16(int aem, const u16* src, u8* dst, int dstpitch, u32 ta0, u32 ta1)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i t0 = _mm256_set1_epi32((int)(ta0 << 24));
	const __m256i t1 = _mm256_set1_epi32((int)(ta1 << 24));
	for (i = 0; i < 8; i++, dst += dstpitch)
	{
		const __m256i v = GS_BLOCK_ACBD(s[i]);
		((__m256i*)dst)[0] = gs_block_expand16_8(aem, _mm256_unpacklo_epi16(v, v), t0, t1);
		((__m256i*)dst)[1] = gs_block_expand16_8(aem, _mm256_unpackhi_epi16(v, v), t0, t1);
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	const gs_vec4i t0 = gs_v4i_set32((int32_t)(ta0 << 24));
	const gs_vec4i t1 = gs_v4i_set32((int32_t)(ta1 << 24));
	for (i = 0; i < 8; i++, dst += dstpitch)
	{
		const gs_vec4i v0 = s[i * 2 + 0];
		const gs_vec4i v1 = s[i * 2 + 1];
		((gs_vec4i*)dst)[0] = gs_block_expand16(aem, gs_v4i_upl16v(v0, v0), t0, t1);
		((gs_vec4i*)dst)[1] = gs_block_expand16(aem, gs_v4i_uph16v(v0, v0), t0, t1);
		((gs_vec4i*)dst)[2] = gs_block_expand16(aem, gs_v4i_upl16v(v1, v1), t0, t1);
		((gs_vec4i*)dst)[3] = gs_block_expand16(aem, gs_v4i_uph16v(v1, v1), t0, t1);
	}
#endif
}

/* 8-bit indexes to 32-bit colours */
GS_BLOCK_INLINE void gs_block_expand_block8_32(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	int j, k;
	for (j = 0; j < 16; j++, dst += dstpitch)
	{
		for (k = 0; k < 4; k++)
		{
			const u8* s = src + j * 16 + k * 4;
			((gs_vec4i*)dst)[k] = gs_v4i_set4((int32_t)pal[s[0]], (int32_t)pal[s[1]], (int32_t)pal[s[2]], (int32_t)pal[s[3]]);
		}
	}
}

/* 8-bit indexes to 16-bit colours (each entry's low half) */
GS_BLOCK_INLINE void gs_block_expand_block8_16(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	int j;
	for (j = 0; j < 16; j++, dst += dstpitch)
	{
		const u8* s = src + j * 16;
		((gs_vec4i*)dst)[0] = gs_v4i_set16((int)pal[s[0]], (int)pal[s[1]], (int)pal[s[2]], (int)pal[s[3]],
			(int)pal[s[4]], (int)pal[s[5]], (int)pal[s[6]], (int)pal[s[7]]);
		((gs_vec4i*)dst)[1] = gs_v4i_set16((int)pal[s[8]], (int)pal[s[9]], (int)pal[s[10]], (int)pal[s[11]],
			(int)pal[s[12]], (int)pal[s[13]], (int)pal[s[14]], (int)pal[s[15]]);
	}
}

/* 4-bit pixel pairs (a byte) to two 32-bit colours at once */
GS_BLOCK_INLINE void gs_block_expand_block4_32(const u8* src, u8* dst, int dstpitch, const u64* pal)
{
	int j, k;
	for (j = 0; j < 16; j++, dst += dstpitch)
	{
		const u8* s = src + j * 16;
		for (k = 0; k < 8; k++)
			((gs_vec4i*)dst)[k] = gs_v4i_set64(pal[s[k * 2 + 0]], pal[s[k * 2 + 1]]);
	}
}

/* ... and to two 16-bit colours (the low half of each pair) */
GS_BLOCK_INLINE void gs_block_expand_block4_16(const u8* src, u8* dst, int dstpitch, const u64* pal)
{
	int j, k;
	for (j = 0; j < 16; j++, dst += dstpitch)
	{
		const u8* s = src + j * 16;
		for (k = 0; k < 4; k++)
			((gs_vec4i*)dst)[k] = gs_v4i_set4((int32_t)pal[s[k * 4 + 0]], (int32_t)pal[s[k * 4 + 1]],
				(int32_t)pal[s[k * 4 + 2]], (int32_t)pal[s[k * 4 + 3]]);
	}
}

/* The high bits of 32-bit pixels (>> shift, & mask) as indexes, to 32-bit
 * colours */
GS_BLOCK_INLINE void gs_block_expand_block_h_32(int shift, u32 mask, const u32* src, u8* dst, int dstpitch, const u32* pal)
{
	int j;
	const gs_vec4i* s = (const gs_vec4i*)src;
	for (j = 0; j < 8; j++, dst += dstpitch)
	{
		gs_block_bytes v0, v1;
		v0.v = GS_BLOCK_SRL32V(s[j * 2 + 0], shift);
		v1.v = GS_BLOCK_SRL32V(s[j * 2 + 1], shift);
		if (mask != 0xff)
		{
			v0.v = gs_v4i_and(v0.v, gs_v4i_set32((int32_t)mask));
			v1.v = gs_v4i_and(v1.v, gs_v4i_set32((int32_t)mask));
		}
		((gs_vec4i*)dst)[0] = gs_v4i_set4((int32_t)pal[v0.u[0]], (int32_t)pal[v0.u[1]], (int32_t)pal[v0.u[2]], (int32_t)pal[v0.u[3]]);
		((gs_vec4i*)dst)[1] = gs_v4i_set4((int32_t)pal[v1.u[0]], (int32_t)pal[v1.u[1]], (int32_t)pal[v1.u[2]], (int32_t)pal[v1.u[3]]);
	}
}

/* ... to 16-bit colours */
GS_BLOCK_INLINE void gs_block_expand_block_h_16(int shift, u32 mask, const u32* src, u8* dst, int dstpitch, const u32* pal)
{
	int j;
	const gs_vec4i* s = (const gs_vec4i*)src;
	for (j = 0; j < 8; j++, dst += dstpitch)
	{
		gs_block_bytes v0, v1;
		gs_vec4i c0, c1;
		v0.v = GS_BLOCK_SRL32V(s[j * 2 + 0], shift);
		v1.v = GS_BLOCK_SRL32V(s[j * 2 + 1], shift);
		if (mask != 0xff)
		{
			v0.v = gs_v4i_and(v0.v, gs_v4i_set32((int32_t)mask));
			v1.v = gs_v4i_and(v1.v, gs_v4i_set32((int32_t)mask));
		}
		c0 = gs_v4i_set4((int32_t)pal[v0.u[0]], (int32_t)pal[v0.u[1]], (int32_t)pal[v0.u[2]], (int32_t)pal[v0.u[3]]);
		c1 = gs_v4i_set4((int32_t)pal[v1.u[0]], (int32_t)pal[v1.u[1]], (int32_t)pal[v1.u[2]], (int32_t)pal[v1.u[3]]);
		((gs_vec4i*)dst)[0] = gs_v4i_pu32(c0, c1);
	}
}

GS_BLOCK_INLINE void gs_block_expand_block8h_32(const u32* src, u8* dst, int dstpitch, const u32* pal) { gs_block_expand_block_h_32(24, 0xff, src, dst, dstpitch, pal); }
GS_BLOCK_INLINE void gs_block_expand_block8h_16(const u32* src, u8* dst, int dstpitch, const u32* pal) { gs_block_expand_block_h_16(24, 0xff, src, dst, dstpitch, pal); }
GS_BLOCK_INLINE void gs_block_expand_block4hl_32(const u32* src, u8* dst, int dstpitch, const u32* pal) { gs_block_expand_block_h_32(24, 0x0f, src, dst, dstpitch, pal); }
GS_BLOCK_INLINE void gs_block_expand_block4hl_16(const u32* src, u8* dst, int dstpitch, const u32* pal) { gs_block_expand_block_h_16(24, 0x0f, src, dst, dstpitch, pal); }
GS_BLOCK_INLINE void gs_block_expand_block4hh_32(const u32* src, u8* dst, int dstpitch, const u32* pal) { gs_block_expand_block_h_32(28, 0xff, src, dst, dstpitch, pal); }
GS_BLOCK_INLINE void gs_block_expand_block4hh_16(const u32* src, u8* dst, int dstpitch, const u32* pal) { gs_block_expand_block_h_16(28, 0xff, src, dst, dstpitch, pal); }

/* ---- unpacking into a block (GSBlock::UnpackAndWriteBlock*) ---- */

/* Twelve bytes of 24-bit pixels (from byte 0 of v) to four in 32 bits:
 * the colour of each, the top byte left as it came */
#define GS_BLOCK_U24(v) \
	gs_v4i_upl64v(gs_v4i_upl32v((v), GS_V4I_SRL((v), 3)), gs_v4i_upl32v(GS_V4I_SRL((v), 6), GS_V4I_SRL((v), 9)))

/* Eight rows of eight 24-bit pixels into a 32-bit block, its alpha kept */
GS_BLOCK_INLINE void gs_block_unpack_and_write_block24(const u8* src, int srcpitch, u8* dst)
{
#ifdef GS_BLOCK_AVX2
	const __m256i mask = _mm256_set1_epi32(0x00ffffff);
	int h;
	for (h = 0; h < 2; h++, src += srcpitch * 4)
	{
		const u8* s0 = &src[srcpitch * 0];
		const u8* s1 = &src[srcpitch * 1];
		const u8* s2 = &src[srcpitch * 2];
		const u8* s3 = &src[srcpitch * 3];
		__m256i* d = (__m256i*)dst + h * 4;
		__m256i v0, v1, v2, v3, v4, v5, v6;
		v4 = GS_BLOCK_PAIR(gs_v4i_upl64v(gs_v4i_loadl(s0), gs_v4i_loadl(s0 + 8)), gs_v4i_upl64v(gs_v4i_loadl(s2), gs_v4i_loadl(s2 + 8)));
		v5 = GS_BLOCK_PAIR(gs_v4i_upl64v(gs_v4i_loadl(s0 + 16), gs_v4i_loadl(s1)), gs_v4i_upl64v(gs_v4i_loadl(s2 + 16), gs_v4i_loadl(s3)));
		v6 = GS_BLOCK_PAIR(gs_v4i_upl64v(gs_v4i_loadl(s1 + 8), gs_v4i_loadl(s1 + 16)), gs_v4i_upl64v(gs_v4i_loadl(s3 + 8), gs_v4i_loadl(s3 + 16)));
#define GS_BLOCK_U24_8(v) GS_BLOCK_ACBD(_mm256_unpacklo_epi64(_mm256_unpacklo_epi32((v), _mm256_srli_si256((v), 3)), \
	_mm256_unpacklo_epi32(_mm256_srli_si256((v), 6), _mm256_srli_si256((v), 9))))
		v0 = GS_BLOCK_U24_8(v4);
		v4 = _mm256_alignr_epi8(v5, v4, 12);
		v1 = GS_BLOCK_U24_8(v4);
		v4 = _mm256_alignr_epi8(v6, v5, 8);
		v2 = GS_BLOCK_U24_8(v4);
		v4 = _mm256_srli_si256(v6, 4);
		v3 = GS_BLOCK_U24_8(v4);
#undef GS_BLOCK_U24_8
		{
			const __m256i e = v0, f = v1;
			v0 = _mm256_unpacklo_epi64(e, v2);
			v1 = _mm256_unpackhi_epi64(e, v2);
			v2 = _mm256_unpacklo_epi64(f, v3);
			v3 = _mm256_unpackhi_epi64(f, v3);
		}
		d[0] = _mm256_blendv_epi8(d[0], v0, mask);
		d[1] = _mm256_blendv_epi8(d[1], v2, mask);
		d[2] = _mm256_blendv_epi8(d[2], v1, mask);
		d[3] = _mm256_blendv_epi8(d[3], v3, mask);
	}
#else
	const gs_vec4i mask = gs_v4i_set32(0x00ffffff);
	gs_vec4i* d = (gs_vec4i*)dst;
	int i;
	for (i = 0; i < 4; i++, src += srcpitch * 2)
	{
		gs_vec4i v0, v1, v2, v3, v4, v5, v6;
		v4 = gs_v4i_loadu(src);
		v5 = gs_v4i_upl64v(gs_v4i_loadl(src + 16), gs_v4i_loadl(src + srcpitch));
		v6 = gs_v4i_loadu(src + srcpitch + 8);
		v0 = GS_BLOCK_U24(v4);
		v4 = GS_V4I_ALIGNR(v5, v4, 12);
		v1 = GS_BLOCK_U24(v4);
		v4 = GS_V4I_ALIGNR(v6, v5, 8);
		v2 = GS_BLOCK_U24(v4);
		v4 = GS_V4I_SRL(v6, 4);
		v3 = GS_BLOCK_U24(v4);
		GS_BLOCK_SW64(v0, v2, v1, v3);
		d[i * 4 + 0] = gs_v4i_blend8(d[i * 4 + 0], v0, mask);
		d[i * 4 + 1] = gs_v4i_blend8(d[i * 4 + 1], v1, mask);
		d[i * 4 + 2] = gs_v4i_blend8(d[i * 4 + 2], v2, mask);
		d[i * 4 + 3] = gs_v4i_blend8(d[i * 4 + 3], v3, mask);
	}
#endif
}

static const gs_block_mask gs_block_uw8hmask[4] = {
	{{0, 0, 0, 0, 1, 1, 1, 1, 8, 8, 8, 8, 9, 9, 9, 9}},
	{{2, 2, 2, 2, 3, 3, 3, 3, 10, 10, 10, 10, 11, 11, 11, 11}},
	{{4, 4, 4, 4, 5, 5, 5, 5, 12, 12, 12, 12, 13, 13, 13, 13}},
	{{6, 6, 6, 6, 7, 7, 7, 7, 14, 14, 14, 14, 15, 15, 15, 15}}};

/* Eight rows of eight 8-bit (mask 0xff000000) or 4-bit (0x0f000000 low
 * nibble, 0xf0000000 high) pixels into the top bits of a 32-bit block */
GS_BLOCK_INLINE void gs_block_unpack_and_write_block_h(u32 mask, const u8* src, int srcpitch, u8* dst)
{
	gs_vec4i v4, v5, v6, v7;
	int i;
#ifdef GS_BLOCK_AVX2
	__m256i* d = (__m256i*)dst;
	for (i = 0; i < 2; i++, src += srcpitch * 4, d += 4)
	{
		if (mask == 0xff000000u)
		{
			v4 = gs_v4i_upl16v(gs_v4i_loadl(&src[srcpitch * 0]), gs_v4i_loadl(&src[srcpitch * 1]));
			v5 = gs_v4i_upl16v(gs_v4i_loadl(&src[srcpitch * 2]), gs_v4i_loadl(&src[srcpitch * 3]));
		}
		else
		{
			int w0, w1, w2, w3;
			memcpy(&w0, &src[srcpitch * 0], 4);
			memcpy(&w1, &src[srcpitch * 1], 4);
			memcpy(&w2, &src[srcpitch * 2], 4);
			memcpy(&w3, &src[srcpitch * 3], 4);
			v4 = gs_v4i_upl32v(_mm_cvtsi32_si128(w0), _mm_cvtsi32_si128(w2));
			v5 = gs_v4i_upl32v(_mm_cvtsi32_si128(w1), _mm_cvtsi32_si128(w3));
			if (mask == 0x0f000000u)
			{
				v6 = gs_v4i_upl8v(v4, GS_V4I_SRL32(v4, 4));
				v7 = gs_v4i_upl8v(v5, GS_V4I_SRL32(v5, 4));
			}
			else
			{
				v6 = gs_v4i_upl8v(GS_V4I_SLL32(v4, 4), v4);
				v7 = gs_v4i_upl8v(GS_V4I_SLL32(v5, 4), v5);
			}
			v4 = gs_v4i_upl16v(v6, v7);
			v5 = gs_v4i_uph16v(v6, v7);
		}
		d[0] = gs_block_blend_mask8(d[0], _mm256_slli_epi32(_mm256_cvtepu8_epi32(v4), 24), mask);
		d[1] = gs_block_blend_mask8(d[1], _mm256_slli_epi32(_mm256_cvtepu8_epi32(GS_V4I_SHUFFLE32(v4, 0xEE)), 24), mask);
		d[2] = gs_block_blend_mask8(d[2], _mm256_slli_epi32(_mm256_cvtepu8_epi32(v5), 24), mask);
		d[3] = gs_block_blend_mask8(d[3], _mm256_slli_epi32(_mm256_cvtepu8_epi32(GS_V4I_SHUFFLE32(v5, 0xEE)), 24), mask);
	}
#else
	gs_vec4i* d = (gs_vec4i*)dst;
	for (i = 0; i < 2; i++, src += srcpitch * 4, d += 8)
	{
		u32 w0, w1, w2, w3;
		if (mask == 0xff000000u)
		{
			v4 = gs_v4i_upl64v(gs_v4i_loadl(src + srcpitch * 0), gs_v4i_loadl(src + srcpitch * 1));
			v5 = gs_v4i_upl64v(gs_v4i_loadl(src + srcpitch * 2), gs_v4i_loadl(src + srcpitch * 3));
		}
		else
		{
			memcpy(&w0, &src[srcpitch * 0], 4);
			memcpy(&w1, &src[srcpitch * 1], 4);
			memcpy(&w2, &src[srcpitch * 2], 4);
			memcpy(&w3, &src[srcpitch * 3], 4);
			v4 = gs_v4i_set4((int32_t)w0, (int32_t)w1, 0, 0);
			v5 = gs_v4i_set4((int32_t)w2, (int32_t)w3, 0, 0);
			if (mask == 0x0f000000u)
			{
				v4 = gs_v4i_upl8v(v4, GS_V4I_SRL32(v4, 4));
				v5 = gs_v4i_upl8v(v5, GS_V4I_SRL32(v5, 4));
			}
			else
			{
				v4 = gs_v4i_upl8v(GS_V4I_SLL32(v4, 4), v4);
				v5 = gs_v4i_upl8v(GS_V4I_SLL32(v5, 4), v5);
			}
		}
		d[0] = gs_block_blend_mask(d[0], gs_v4i_shuffle8(v4, gs_block_uw8hmask[0].v), mask);
		d[1] = gs_block_blend_mask(d[1], gs_v4i_shuffle8(v4, gs_block_uw8hmask[1].v), mask);
		d[2] = gs_block_blend_mask(d[2], gs_v4i_shuffle8(v4, gs_block_uw8hmask[2].v), mask);
		d[3] = gs_block_blend_mask(d[3], gs_v4i_shuffle8(v4, gs_block_uw8hmask[3].v), mask);
		d[4] = gs_block_blend_mask(d[4], gs_v4i_shuffle8(v5, gs_block_uw8hmask[0].v), mask);
		d[5] = gs_block_blend_mask(d[5], gs_v4i_shuffle8(v5, gs_block_uw8hmask[1].v), mask);
		d[6] = gs_block_blend_mask(d[6], gs_v4i_shuffle8(v5, gs_block_uw8hmask[2].v), mask);
		d[7] = gs_block_blend_mask(d[7], gs_v4i_shuffle8(v5, gs_block_uw8hmask[3].v), mask);
	}
	(void)v6;
	(void)v7;
#endif
}

GS_BLOCK_INLINE void gs_block_unpack_and_write_block8h(const u8* src, int srcpitch, u8* dst) { gs_block_unpack_and_write_block_h(0xff000000u, src, srcpitch, dst); }
GS_BLOCK_INLINE void gs_block_unpack_and_write_block4hl(const u8* src, int srcpitch, u8* dst) { gs_block_unpack_and_write_block_h(0x0f000000u, src, srcpitch, dst); }
GS_BLOCK_INLINE void gs_block_unpack_and_write_block4hh(const u8* src, int srcpitch, u8* dst) { gs_block_unpack_and_write_block_h(0xf0000000u, src, srcpitch, dst); }

/* ---- reading a block straight to 32-bit colours
 * (GSBlock::ReadAndExpandBlock*) ---- */

GS_BLOCK_INLINE void gs_block_read_and_expand_block24(int aem, const u8* src, u8* dst, int dstpitch, u32 ta0)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i ta = _mm256_set1_epi32((int)(ta0 << 24));
	const __m256i mask = _mm256_set1_epi32(0x00ffffff);
	for (i = 0; i < 2; i++, dst += dstpitch * 4)
	{
		__m256i v0 = _mm256_and_si256(s[i * 4 + 0], mask);
		__m256i v1 = _mm256_and_si256(s[i * 4 + 1], mask);
		__m256i v2 = _mm256_and_si256(s[i * 4 + 2], mask);
		__m256i v3 = _mm256_and_si256(s[i * 4 + 3], mask);
		__m256i e;
		e = v0; v0 = _mm256_permute2x128_si256(e, v1, 0x20); v1 = _mm256_permute2x128_si256(e, v1, 0x31);
		GS_BLOCK_SW64_8(v0, v1);
		e = v2; v2 = _mm256_permute2x128_si256(e, v3, 0x20); v3 = _mm256_permute2x128_si256(e, v3, 0x31);
		GS_BLOCK_SW64_8(v2, v3);
		*(__m256i*)&dst[dstpitch * 0] = gs_block_expand24_8(aem, v0, ta);
		*(__m256i*)&dst[dstpitch * 1] = gs_block_expand24_8(aem, v1, ta);
		*(__m256i*)&dst[dstpitch * 2] = gs_block_expand24_8(aem, v2, ta);
		*(__m256i*)&dst[dstpitch * 3] = gs_block_expand24_8(aem, v3, ta);
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	const gs_vec4i ta = gs_v4i_set32((int32_t)(ta0 << 24));
	const gs_vec4i mask = gs_v4i_set32(0x00ffffff);
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		gs_vec4i v0 = s[i * 4 + 0];
		gs_vec4i v1 = s[i * 4 + 1];
		gs_vec4i v2 = s[i * 4 + 2];
		gs_vec4i v3 = s[i * 4 + 3];
		gs_vec4i* d0 = (gs_vec4i*)&dst[dstpitch * 0];
		gs_vec4i* d1 = (gs_vec4i*)&dst[dstpitch * 1];
		GS_BLOCK_SW64(v0, v1, v2, v3);
		d0[0] = gs_block_expand24(aem, gs_v4i_and(v0, mask), ta);
		d0[1] = gs_block_expand24(aem, gs_v4i_and(v1, mask), ta);
		d1[0] = gs_block_expand24(aem, gs_v4i_and(v2, mask), ta);
		d1[1] = gs_block_expand24(aem, gs_v4i_and(v3, mask), ta);
	}
#endif
}

GS_BLOCK_INLINE void gs_block_read_and_expand_block16(int aem, const u8* src, u8* dst, int dstpitch, u32 ta0, u32 ta1)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i t0 = _mm256_set1_epi32((int)(ta0 << 24));
	const __m256i t1 = _mm256_set1_epi32((int)(ta1 << 24));
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		__m256i v0, v1;
		__m256i* d0 = (__m256i*)&dst[dstpitch * 0];
		__m256i* d1 = (__m256i*)&dst[dstpitch * 1];
		GS_BLOCK_LOADSW128(v0, v1, &s[i * 2 + 0], &s[i * 2 + 1]);
		GS_BLOCK_SW64_8(v0, v1);
		d0[0] = gs_block_expand16l_8(aem, v0, t0, t1);
		d0[1] = gs_block_expand16h_8(aem, v0, t0, t1);
		d1[0] = gs_block_expand16l_8(aem, v1, t0, t1);
		d1[1] = gs_block_expand16h_8(aem, v1, t0, t1);
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	const gs_vec4i t0 = gs_v4i_set32((int32_t)(ta0 << 24));
	const gs_vec4i t1 = gs_v4i_set32((int32_t)(ta1 << 24));
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		gs_vec4i v0 = s[i * 4 + 0];
		gs_vec4i v1 = s[i * 4 + 1];
		gs_vec4i v2 = s[i * 4 + 2];
		gs_vec4i v3 = s[i * 4 + 3];
		gs_vec4i* d0 = (gs_vec4i*)&dst[dstpitch * 0];
		gs_vec4i* d1 = (gs_vec4i*)&dst[dstpitch * 1];
		GS_BLOCK_SW64(v0, v1, v2, v3);
		d0[0] = gs_block_expand16l(aem, v0, t0, t1);
		d0[1] = gs_block_expand16l(aem, v1, t0, t1);
		d0[2] = gs_block_expand16h(aem, v0, t0, t1);
		d0[3] = gs_block_expand16h(aem, v1, t0, t1);
		d1[0] = gs_block_expand16l(aem, v2, t0, t1);
		d1[1] = gs_block_expand16l(aem, v3, t0, t1);
		d1[2] = gs_block_expand16h(aem, v2, t0, t1);
		d1[3] = gs_block_expand16h(aem, v3, t0, t1);
	}
#endif
}

/* 8-bit indexes through a block read first: for AVX2 parts with a slow
 * vpgatherdd (Haswell, Zen 1-4) */
GS_BLOCK_INLINE void gs_block_read_and_expand_block8_32hsw(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	union
	{
		u8 b[256];
#ifdef GS_BLOCK_AVX2
		__m256i y[8];
#endif
		gs_vec4i v[16];
	} block;
	gs_block_read_block8(src, block.b, 16);
	gs_block_expand_block8_32(block.b, dst, dstpitch, pal);
}

GS_BLOCK_INLINE void gs_block_read_and_expand_block8_32(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i mask = _mm256_set1_epi32(0xff);
	for (i = 0; i < 2; i++)
	{
		int h;
		for (h = 0; h < 2; h++, dst += dstpitch * 4)
		{
			__m256i v0, v1;
			__m256i* d0 = (__m256i*)(dst + dstpitch * 0);
			__m256i* d1 = (__m256i*)(dst + dstpitch * 1);
			__m256i* d2 = (__m256i*)(dst + dstpitch * 2);
			__m256i* d3 = (__m256i*)(dst + dstpitch * 3);
			if (h == 0)
				GS_BLOCK_LOADSW128(v0, v1, &s[i * 4 + 0], &s[i * 4 + 1]);
			else
				GS_BLOCK_LOADSW128(v0, v1, &s[i * 4 + 3], &s[i * 4 + 2]);
			GS_BLOCK_SW64_8(v0, v1);
			d0[0] = GS_BLOCK_GATHER8(pal, _mm256_and_si256(v0, mask));
			d0[1] = GS_BLOCK_GATHER8(pal, _mm256_and_si256(_mm256_srli_epi32(v0, 16), mask));
			d1[0] = GS_BLOCK_GATHER8(pal, _mm256_and_si256(v1, mask));
			d1[1] = GS_BLOCK_GATHER8(pal, _mm256_and_si256(_mm256_srli_epi32(v1, 16), mask));
			v0 = _mm256_permute4x64_epi64(v0, 0x4E);
			v1 = _mm256_permute4x64_epi64(v1, 0x4E);
			d2[0] = GS_BLOCK_GATHER8(pal, _mm256_and_si256(_mm256_srli_epi32(v0, 8), mask));
			d2[1] = GS_BLOCK_GATHER8(pal, _mm256_srli_epi32(v0, 24));
			d3[0] = GS_BLOCK_GATHER8(pal, _mm256_and_si256(_mm256_srli_epi32(v1, 8), mask));
			d3[1] = GS_BLOCK_GATHER8(pal, _mm256_srli_epi32(v1, 24));
		}
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	const gs_vec4i mask = gs_block_r8mask.v;
	for (i = 0; i < 2; i++)
	{
		int h;
		for (h = 0; h < 2; h++)
		{
			gs_block_bytes v0, v1, v2, v3;
			if (h == 0)
			{
				v0.v = gs_v4i_shuffle8(s[i * 8 + 0], mask);
				v1.v = gs_v4i_shuffle8(s[i * 8 + 1], mask);
				v2.v = gs_v4i_shuffle8(s[i * 8 + 2], mask);
				v3.v = gs_v4i_shuffle8(s[i * 8 + 3], mask);
			}
			else
			{
				v2.v = gs_v4i_shuffle8(s[i * 8 + 4], mask);
				v3.v = gs_v4i_shuffle8(s[i * 8 + 5], mask);
				v0.v = gs_v4i_shuffle8(s[i * 8 + 6], mask);
				v1.v = gs_v4i_shuffle8(s[i * 8 + 7], mask);
			}
			GS_BLOCK_SW16(v0.v, v1.v, v2.v, v3.v);
			GS_BLOCK_SW32(v0.v, v1.v, v3.v, v2.v);
#define GS_BLOCK_ROW8(v)                                                    \
	do                                                                      \
	{                                                                       \
		((gs_vec4i*)dst)[0] = GS_BLOCK_GATHER32_8(pal, (v).b, 0);           \
		((gs_vec4i*)dst)[1] = GS_BLOCK_GATHER32_8(pal, (v).b, 4);           \
		((gs_vec4i*)dst)[2] = GS_BLOCK_GATHER32_8(pal, (v).b, 8);           \
		((gs_vec4i*)dst)[3] = GS_BLOCK_GATHER32_8(pal, (v).b, 12);          \
		dst += dstpitch;                                                    \
	} while (0)
			GS_BLOCK_ROW8(v0);
			GS_BLOCK_ROW8(v3);
			GS_BLOCK_ROW8(v1);
			GS_BLOCK_ROW8(v2);
#undef GS_BLOCK_ROW8
		}
	}
#endif
}

GS_BLOCK_INLINE void gs_block_read_and_expand_block4_32(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i shuf = GS_BLOCK_BCAST(gs_block_palvec_mask);
	const __m256i mask = _mm256_set1_epi32(0x0f0f0f0f);
	__m256i p0, p1, p2, p3;
	gs_block_pal_vecs8(pal, &p0, &p1, &p2, &p3);
	for (i = 0; i < 2; i++)
	{
		int h;
		for (h = 0; h < 2; h++, dst += dstpitch * 4)
		{
			__m256i v0, v1;
			__m256i* d0 = (__m256i*)(dst + dstpitch * 0);
			__m256i* d1 = (__m256i*)(dst + dstpitch * 1);
			__m256i* d2 = (__m256i*)(dst + dstpitch * 2);
			__m256i* d3 = (__m256i*)(dst + dstpitch * 3);
			if (h == 0)
				GS_BLOCK_LOADSW128(v0, v1, &s[i * 4 + 0], &s[i * 4 + 1]);
			else
				GS_BLOCK_LOADSW128(v0, v1, &s[i * 4 + 3], &s[i * 4 + 2]);
			GS_BLOCK_SW64_8(v0, v1);
			v0 = _mm256_shuffle_epi8(v0, shuf);
			v1 = _mm256_shuffle_epi8(v1, shuf);
			GS_BLOCK_CLUT4_8(p0, p1, p2, p3, _mm256_and_si256(v0, mask), d0 + 0, d0 + 1, d0 + 2, d0 + 3);
			GS_BLOCK_CLUT4_8(p0, p1, p2, p3, _mm256_and_si256(v1, mask), d1 + 0, d1 + 1, d1 + 2, d1 + 3);
			v0 = _mm256_srli_epi32(_mm256_permute4x64_epi64(v0, 0x4E), 4);
			v1 = _mm256_srli_epi32(_mm256_permute4x64_epi64(v1, 0x4E), 4);
			GS_BLOCK_CLUT4_8(p0, p1, p2, p3, _mm256_and_si256(v0, mask), d2 + 0, d2 + 1, d2 + 2, d2 + 3);
			GS_BLOCK_CLUT4_8(p0, p1, p2, p3, _mm256_and_si256(v1, mask), d3 + 0, d3 + 1, d3 + 2, d3 + 3);
		}
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	const gs_vec4i mask = gs_v4i_set32(0x0f0f0f0f);
	gs_vec4i p0, p1, p2, p3;
	gs_block_pal_vecs(pal, &p0, &p1, &p2, &p3);
	for (i = 0; i < 2; i++)
	{
		int h;
		for (h = 0; h < 2; h++, dst += dstpitch * 4)
		{
			/* the second four rows take the column pairs the other way */
			const int x = h;
			gs_vec4i* d0 = (gs_vec4i*)(dst + dstpitch * 0);
			gs_vec4i* d1 = (gs_vec4i*)(dst + dstpitch * 1);
			gs_vec4i* d2 = (gs_vec4i*)(dst + dstpitch * 2);
			gs_vec4i* d3 = (gs_vec4i*)(dst + dstpitch * 3);
			gs_vec4i v0 = s[i * 8 + h * 4 + 0];
			gs_vec4i v1 = s[i * 8 + h * 4 + 1];
			gs_vec4i v2 = s[i * 8 + h * 4 + 2];
			gs_vec4i v3 = s[i * 8 + h * 4 + 3];
			GS_BLOCK_SW64(v0, v1, v2, v3);
			v0 = gs_v4i_shuffle8(v0, gs_block_palvec_mask.v);
			v1 = gs_v4i_shuffle8(v1, gs_block_palvec_mask.v);
			v2 = gs_v4i_shuffle8(v2, gs_block_palvec_mask.v);
			v3 = gs_v4i_shuffle8(v3, gs_block_palvec_mask.v);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(v0, mask), d0[x + 0], d0[x + 2], d0[x + 4], d0[x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(GS_V4I_SRL32(v0, 4), mask), d2[1 - x + 0], d2[1 - x + 2], d2[1 - x + 4], d2[1 - x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(v1, mask), d0[1 - x + 0], d0[1 - x + 2], d0[1 - x + 4], d0[1 - x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(GS_V4I_SRL32(v1, 4), mask), d2[x + 0], d2[x + 2], d2[x + 4], d2[x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(v2, mask), d1[x + 0], d1[x + 2], d1[x + 4], d1[x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(GS_V4I_SRL32(v2, 4), mask), d3[1 - x + 0], d3[1 - x + 2], d3[1 - x + 4], d3[1 - x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(v3, mask), d1[1 - x + 0], d1[1 - x + 2], d1[1 - x + 4], d1[1 - x + 6]);
			GS_BLOCK_CLUT4(p0, p1, p2, p3, gs_v4i_and(GS_V4I_SRL32(v3, 4), mask), d3[x + 0], d3[x + 2], d3[x + 4], d3[x + 6]);
		}
	}
#endif
}

/* The top byte of 32-bit pixels as 8-bit indexes, one lookup each (also
 * the AVX2 path where vpgatherdd is slow) */
GS_BLOCK_INLINE void gs_block_read_and_expand_block8h_32hsw(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	int i;
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		const u8* s = src + i * 64;
		gs_vec4i* d0 = (gs_vec4i*)(dst + dstpitch * 0);
		gs_vec4i* d1 = (gs_vec4i*)(dst + dstpitch * 1);
		d0[0] = gs_v4i_set4((int32_t)pal[s[3]], (int32_t)pal[s[7]], (int32_t)pal[s[19]], (int32_t)pal[s[23]]);
		d0[1] = gs_v4i_set4((int32_t)pal[s[35]], (int32_t)pal[s[39]], (int32_t)pal[s[51]], (int32_t)pal[s[55]]);
		d1[0] = gs_v4i_set4((int32_t)pal[s[11]], (int32_t)pal[s[15]], (int32_t)pal[s[27]], (int32_t)pal[s[31]]);
		d1[1] = gs_v4i_set4((int32_t)pal[s[43]], (int32_t)pal[s[47]], (int32_t)pal[s[59]], (int32_t)pal[s[63]]);
	}
}

GS_BLOCK_INLINE void gs_block_read_and_expand_block8h_32(const u8* src, u8* dst, int dstpitch, const u32* pal)
{
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	int i;
	for (i = 0; i < 4; i++)
	{
		__m256i v0, v1;
		GS_BLOCK_LOADSW128(v0, v1, &s[i * 2 + 0], &s[i * 2 + 1]);
		GS_BLOCK_SW64_8(v0, v1);
		*(__m256i*)dst = GS_BLOCK_GATHER8(pal, _mm256_srli_epi32(v0, 24));
		dst += dstpitch;
		*(__m256i*)dst = GS_BLOCK_GATHER8(pal, _mm256_srli_epi32(v1, 24));
		dst += dstpitch;
	}
#else
	gs_block_read_and_expand_block8h_32hsw(src, dst, dstpitch, pal);
#endif
}

static const gs_block_mask gs_block_r4hmask = {{0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15}};
static const gs_block_mask gs_block_r4hmask_avx2 = {{0, 1, 8, 9, 2, 3, 10, 11, 4, 5, 12, 13, 6, 7, 14, 15}};

/* The top bits of 32-bit pixels (>> shift, & mask) as 4-bit indexes */
GS_BLOCK_INLINE void gs_block_read_and_expand_block4h_32(int shift, u32 mask, const u8* src, u8* dst, int dstpitch, const u32* pal)
{
	int i;
#ifdef GS_BLOCK_AVX2
	const __m256i* s = (const __m256i*)src;
	const __m256i shufvec = GS_BLOCK_BCAST(gs_block_r4hmask_avx2);
	const __m128i sh = _mm_cvtsi32_si128(shift);
	__m256i p0, p1, p2, p3;
	gs_block_pal_vecs8(pal, &p0, &p1, &p2, &p3);
	for (i = 0; i < 2; i++, dst += dstpitch * 4)
	{
		const __m256i v0 = _mm256_srl_epi32(s[i * 4 + 0], sh);
		const __m256i v1 = _mm256_srl_epi32(s[i * 4 + 1], sh);
		const __m256i v2 = _mm256_srl_epi32(s[i * 4 + 2], sh);
		const __m256i v3 = _mm256_srl_epi32(s[i * 4 + 3], sh);
		__m256i all = _mm256_packus_epi16(_mm256_packs_epi32(v0, v1), _mm256_packs_epi32(v2, v3));
		all = _mm256_shuffle_epi8(GS_BLOCK_ACBD(_mm256_shuffle_epi32(all, GS_BLOCK_XZYW)), shufvec);
		if (mask != 0xffffffffu)
			all = _mm256_and_si256(all, _mm256_set1_epi32((int)mask));
		GS_BLOCK_CLUT4_8(p0, p1, p2, p3, all, dst, dst + dstpitch, dst + dstpitch * 2, dst + dstpitch * 3);
	}
#else
	const gs_vec4i* s = (const gs_vec4i*)src;
	gs_vec4i p0, p1, p2, p3;
	gs_block_pal_vecs(pal, &p0, &p1, &p2, &p3);
	for (i = 0; i < 4; i++, dst += dstpitch * 2)
	{
		gs_vec4i* d0 = (gs_vec4i*)dst;
		gs_vec4i* d1 = (gs_vec4i*)(dst + dstpitch);
		const gs_vec4i v0 = GS_BLOCK_SRL32V(s[i * 4 + 0], shift);
		const gs_vec4i v1 = GS_BLOCK_SRL32V(s[i * 4 + 1], shift);
		const gs_vec4i v2 = GS_BLOCK_SRL32V(s[i * 4 + 2], shift);
		const gs_vec4i v3 = GS_BLOCK_SRL32V(s[i * 4 + 3], shift);
		gs_vec4i all = gs_v4i_shuffle8(gs_v4i_pu16(gs_v4i_ps32(v0, v1), gs_v4i_ps32(v2, v3)), gs_block_r4hmask.v);
		if (mask != 0xffffffffu)
			all = gs_v4i_and(all, gs_v4i_set32((int32_t)mask));
		GS_BLOCK_CLUT4(p0, p1, p2, p3, all, d0[0], d0[1], d1[0], d1[1]);
	}
#endif
}

GS_BLOCK_INLINE void gs_block_read_and_expand_block4hl_32(const u8* src, u8* dst, int dstpitch, const u32* pal) { gs_block_read_and_expand_block4h_32(24, 0x0f0f0f0fu, src, dst, dstpitch, pal); }
GS_BLOCK_INLINE void gs_block_read_and_expand_block4hh_32(const u8* src, u8* dst, int dstpitch, const u32* pal) { gs_block_read_and_expand_block4h_32(28, 0xffffffffu, src, dst, dstpitch, pal); }

#endif
