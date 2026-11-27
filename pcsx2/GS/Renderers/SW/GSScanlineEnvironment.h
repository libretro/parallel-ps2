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

#pragma once

/* The scanline code's environment: what a draw hands its primitives'
 * setup and scanlines, typed for C (gs_vector) and C++ (GSVector) alike.
 * The layout is the generated code's (GSSwJitLayout.h holds it to
 * GSSwJit.h's offsets) and differs by tier: AVX2 keeps eight lanes. */

#include "common/VectorIntrin.h"
#include "../../GSLocalMemory.h"
#include "../../gs_rect.h"
#include "../../gs_vector.h"
#ifdef __cplusplus
#include "../../GSVector.h"
#endif

/* A double rounded to the nearest integer, ties to even, whatever the
 * rounding mode. */
#if defined(ARCH_ARM64) || defined(__aarch64__) || defined(_M_ARM64)
#define GS_ROUND_NEAREST64(v) vget_lane_f64(vrndn_f64(vdup_n_f64(v)), 0)
#else
#define GS_ROUND_NEAREST64(v) _mm_cvtsd_f64(_mm_round_sd(_mm_setzero_pd(), _mm_set_sd(v), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC))
#endif

/* The vectors of the environment, as each language names them: four
 * lanes (GSVector4i, GSVector4: gs_vec4i, floats carried in it) and, on
 * the AVX2 tier, eight. */
#define GS_SL_V4I PCSX2_LANG_TYPE(GSVector4i, gs_vec4i)
#define GS_SL_V4F PCSX2_LANG_TYPE(GSVector4, gs_vec4i)
#define GS_SL_V8I PCSX2_LANG_TYPE(GSVector8i, __m256i)
#define GS_SL_V8F PCSX2_LANG_TYPE(GSVector8, __m256)

typedef union GSScanlineSelector
{
	struct
	{
		u32 fpsm  : 2; /* 0 */
		u32 zpsm  : 2; /* 2 */
		u32 ztst  : 2; /* 4 (0: off, 1: write, 2: test (ge), 3: test (g)) */
		u32 atst  : 3; /* 6 */
		u32 afail : 2; /* 9 */
		u32 iip   : 1; /* 11 */
		u32 tfx   : 3; /* 12 */
		u32 tcc   : 1; /* 15 */
		u32 fst   : 1; /* 16 */
		u32 ltf   : 1; /* 17 */
		u32 tlu   : 1; /* 18 */
		u32 fge   : 1; /* 19 */
		u32 date  : 1; /* 20 */
		u32 abe   : 1; /* 21 */
		u32 aba   : 2; /* 22 */
		u32 abb   : 2; /* 24 */
		u32 abc   : 2; /* 26 */
		u32 abd   : 2; /* 28 */
		u32 pabe  : 1; /* 30 */
		u32 aa1   : 1; /* 31 */

		u32 fwrite    : 1; /* 32 */
		u32 ftest     : 1; /* 33 */
		u32 rfb       : 1; /* 34 */
		u32 zwrite    : 1; /* 35 */
		u32 ztest     : 1; /* 36 */
		u32 zoverflow : 1; /* 37 (z max >= 0x80000000) */
		u32 zclamp    : 1; /* 38 */
		u32 wms       : 2; /* 39 */
		u32 wmt       : 2; /* 41 */
		u32 datm      : 1; /* 43 */
		u32 colclamp  : 1; /* 44 */
		u32 fba       : 1; /* 45 */
		u32 dthe      : 1; /* 46 */
		u32 prim      : 2; /* 47 */

		u32 edge   : 1; /* 49 */
		u32 tw     : 3; /* 50 (encodes values between 3 -> 10, texture cache makes sure it is at least 3) */
		u32 lcm    : 1; /* 53 */
		u32 mmin   : 2; /* 54 */
		u32 notest : 1; /* 55 (no ztest, no atest, no date, no scissor test, and horizontally aligned to 4 pixels) */
		u32 zequal : 1; /* 56 */
		u32 hires  : 1; /* 57 (drawing into the 2x memory, four times the size of local memory) */
		u32 tw_hi  : 1; /* 58 (tw + 8: a 2x texture 2048 or 4096 texels wide) */
	};

	struct
	{
		u32 _pad1  : 22;
		u32 ababcd :  8;
		u32 _pad2  :  2;

		u32 fb    : 2;
		u32 _pad3 : 1;
		u32 zb    : 2;
	};

	struct
	{
		u32 lo;
		u32 hi;
	};

	u64 key;

#ifdef __cplusplus
	GSScanlineSelector() = default;
	GSScanlineSelector(u64 k)
		: key(k)
	{
	}

	operator u32() const { return lo; }
	operator u64() const { return key; }

	bool IsSolidRect() const
	{
		return prim == GS_SPRITE_CLASS && iip == 0 && tfx == TFX_NONE && abe == 0 && ztst <= 1 && atst <= 1 && date == 0 && fge == 0;
	}
#endif
} GSScanlineSelector;

/* A sprite of one colour, nothing tested but the depth write: filled a
 * rectangle at a time (GSRaster.c) */
static INLINE int gs_sl_sel_is_solid_rect(GSScanlineSelector sel)
{
	return sel.prim == GS_SPRITE_CLASS && sel.iip == 0 && sel.tfx == TFX_NONE && sel.abe == 0 && sel.ztst <= 1
		&& sel.atst <= 1 && sel.date == 0 && sel.fge == 0;
}

typedef struct PCSX2_ALIGN(32) GSScanlineGlobalData /* per batch variables, this is like a pixel shader constant buffer */
{
	GSScanlineSelector sel;

	/* - the data of vm, tex may change, multi-threaded drawing must be finished before that happens, clut and dimx are copies
	 * - tex is a cached texture, it may be recycled to free up memory, its absolute address cannot be compiled into code
	 * - row and column pointers are allocated once and never change or freed, thier address can be used directly */

	void* vm;
	const void* tex[7];
	u32* clut;
	GS_SL_V4I* dimx;

	PCSX2_LANG_TYPE(GSOffset, gs_offset) fbo;
	PCSX2_LANG_TYPE(GSOffset, gs_offset) zbo;
	/* the frame's address in x, the z buffer's in y, by row and column */
	const PCSX2_LANG_TYPE(GSVector2i, gs_point)* fzbr;
	const PCSX2_LANG_TYPE(GSVector2i, gs_point)* fzbc;
	u32 fbase, zbase; /* added to a row and a column of fzbr and fzbc */

	GS_SL_V4I aref;
	GS_SL_V4I afix;
	struct { GS_SL_V4I min, max, minmax, mask, invmask; } t; /* [u] x 4 [v] x 4 */

#if _M_SSE >= 0x501

	u32 fm, zm;
	u32 frb, fga;
	GS_SL_V8F mxl;
	GS_SL_V8F l; /* TEX1.L * -0x10000 */
	/* -log2(Q) * (1 << L) + K, times 0x10000, as a cubic in Q's mantissa
	 * plus its exponent times l (see GS_SCANLINE_SET_LOD). */
	GS_SL_V8F lod_c3, lod_c2, lod_c1, lod_c0;
	struct { GS_SL_V8I i, f; } lod; /* lcm == 1 */

#else

	GS_SL_V4I fm, zm;
	GS_SL_V4I frb, fga;
	GS_SL_V4F mxl;
	GS_SL_V4F l; /* TEX1.L * -0x10000 */
	/* -log2(Q) * (1 << L) + K, times 0x10000, as a cubic in Q's mantissa
	 * plus its exponent times l (see GS_SCANLINE_SET_LOD). */
	GS_SL_V4F lod_c3, lod_c2, lod_c1, lod_c0;
	struct { GS_SL_V4I i, f; } lod; /* lcm == 1 */

#endif
} GSScanlineGlobalData;

/* The LOD the scanline code computes from Q for TEX1.L l_log2 and K k
 * (K * 0x10000): log2 of a mantissa m in [1, 2) is p(m) * (m - 1), p a
 * minimax fit of log2(m) / (m - 1), here times l and expanded into a
 * cubic that evaluates in two short chains. l is a power of two, so
 * every product with it is exact. */
#define GS_SCANLINE_LOD_P2 0.204446009836232697516f
#define GS_SCANLINE_LOD_P1 -1.04913055217340124191f
#define GS_SCANLINE_LOD_P0 2.28330284476918490682f
#ifdef __cplusplus
#define GS_SCANLINE_SET_LOD(gd, l_log2, k) \
	do \
	{ \
		const float lod_l_ = (float)(-(0x10000 << (l_log2))); \
		(gd).l = GSVector4(lod_l_); \
		(gd).lod_c3 = GSVector4(lod_l_ * GS_SCANLINE_LOD_P2); \
		(gd).lod_c2 = GSVector4(lod_l_ * (GS_SCANLINE_LOD_P1 - GS_SCANLINE_LOD_P2)); \
		(gd).lod_c1 = GSVector4(lod_l_ * (GS_SCANLINE_LOD_P0 - GS_SCANLINE_LOD_P1)); \
		(gd).lod_c0 = GSVector4((float)(k) - lod_l_ * GS_SCANLINE_LOD_P0); \
	} while (0)
#endif

/* The steps a primitive's setup leaves for its scanlines */
#if _M_SSE >= 0x501
typedef struct gs_sl_skip { GS_SL_V8F z0, z1, s, t, q; GS_SL_V8I rb, ga, f; } gs_sl_skip; /* z0, z1: four doubles each */
typedef struct gs_sl_step { GS_SL_V4F stq; struct { u32 rb, ga; } c; struct { u64 z; u32 f; } p; } gs_sl_step;
#else
typedef struct gs_sl_skip { GS_SL_V4F z0, z1, s, t, q; GS_SL_V4I rb, ga, f; } gs_sl_skip; /* z0, z1: two doubles each */
typedef struct gs_sl_step { GS_SL_V4F z, stq; GS_SL_V4I c, f; } gs_sl_step;
#endif

typedef struct PCSX2_ALIGN(32) GSScanlineLocalData /* per prim variables, each thread has its own */
{
#if _M_SSE >= 0x501

	gs_sl_skip d[8];
	gs_sl_step d8;
	struct { u32 z, f; } p;
	struct { GS_SL_V8I rb, ga; } c;
	/* The primitive's grid for S, T, Q and Z, and its reciprocals: a
	 * scanline's start snaps to it (see GSRasterizer::SetupPrim). */
	GS_SL_V4F tgrid, tgrid_rcp;
	struct { double grid, rcp; } zgrid;

	/* these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack) */

	struct
	{
		GS_SL_V8F z0, z1;
		GS_SL_V8I f;
		GS_SL_V8F s, t, q;
		GS_SL_V8I rb, ga;
		GS_SL_V8I zs, zd;
		GS_SL_V8I uf, vf;
		GS_SL_V8I cov;

		/* mipmapping */

		struct { GS_SL_V8I i, f; } lod;
		GS_SL_V8I uv[2];
		GS_SL_V8I uv_minmax[2];
		GS_SL_V8I trb, tga;
		GS_SL_V8I test;
	} temp;

#else

	gs_sl_skip d[4];
	gs_sl_step d4;
	struct { GS_SL_V4I rb, ga; } c;
	struct { GS_SL_V4I z, f; } p;
	/* The primitive's grid for S, T, Q and Z, and its reciprocals: a
	 * scanline's start snaps to it (see GSRasterizer::SetupPrim). */
	GS_SL_V4F tgrid, tgrid_rcp;
	struct { double grid, rcp; } zgrid;

	/* these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack) */

	struct
	{
		GS_SL_V4F z0, z1;
		GS_SL_V4I f;
		GS_SL_V4F s, t, q;
		GS_SL_V4I rb, ga;
		GS_SL_V4I zs, zd;
		GS_SL_V4I uf, vf;
		GS_SL_V4I cov;

		/* mipmapping */

		struct { GS_SL_V4I i, f; } lod;
		GS_SL_V4I uv[2];
		GS_SL_V4I uv_minmax[2];
		GS_SL_V4I trb, tga;
		GS_SL_V4I test;
	} temp;

#endif

	const GSScanlineGlobalData* gd;
} GSScanlineLocalData;

/* Constant shared by all threads (to reduce cache miss); g_const is in
 * GSDrawScanline.c */
typedef struct PCSX2_ALIGN(32) GSScanlineConstantData
{
	PCSX2_ALIGN(32) u8 m_test_256b[16][8];
	PCSX2_ALIGN(32) float m_shift_256b[9][8];
	PCSX2_ALIGN(32) float m_one_256b[8];
	PCSX2_ALIGN(16) u32 m_test_128b[8][4];
	PCSX2_ALIGN(16) float m_shift_128b[5][4];
	PCSX2_ALIGN(16) float m_one_128b[4];
} GSScanlineConstantData;

#ifdef __cplusplus
extern "C" {
#endif
extern const GSScanlineConstantData g_const;
#ifdef __cplusplus
}
#endif
