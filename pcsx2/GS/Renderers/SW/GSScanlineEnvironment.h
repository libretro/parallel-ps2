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

#include "../../GSLocalMemory.h"
#include "../../GSVector.h"

/* A double rounded to the nearest integer, ties to even, whatever the
 * rounding mode. */
#if defined(ARCH_ARM64)
#define GS_ROUND_NEAREST64(v) vget_lane_f64(vrndn_f64(vdup_n_f64(v)), 0)
#else
#define GS_ROUND_NEAREST64(v) _mm_cvtsd_f64(_mm_round_sd(_mm_setzero_pd(), _mm_set_sd(v), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC))
#endif

union GSScanlineSelector
{
	struct
	{
		u32 fpsm  : 2; // 0
		u32 zpsm  : 2; // 2
		u32 ztst  : 2; // 4 (0: off, 1: write, 2: test (ge), 3: test (g))
		u32 atst  : 3; // 6
		u32 afail : 2; // 9
		u32 iip   : 1; // 11
		u32 tfx   : 3; // 12
		u32 tcc   : 1; // 15
		u32 fst   : 1; // 16
		u32 ltf   : 1; // 17
		u32 tlu   : 1; // 18
		u32 fge   : 1; // 19
		u32 date  : 1; // 20
		u32 abe   : 1; // 21
		u32 aba   : 2; // 22
		u32 abb   : 2; // 24
		u32 abc   : 2; // 26
		u32 abd   : 2; // 28
		u32 pabe  : 1; // 30
		u32 aa1   : 1; // 31

		u32 fwrite    : 1; // 32
		u32 ftest     : 1; // 33
		u32 rfb       : 1; // 34
		u32 zwrite    : 1; // 35
		u32 ztest     : 1; // 36
		u32 zoverflow : 1; // 37 (z max >= 0x80000000)
		u32 zclamp    : 1; // 38
		u32 wms       : 2; // 39
		u32 wmt       : 2; // 41
		u32 datm      : 1; // 43
		u32 colclamp  : 1; // 44
		u32 fba       : 1; // 45
		u32 dthe      : 1; // 46
		u32 prim      : 2; // 47

		u32 edge   : 1; // 49
		u32 tw     : 3; // 50 (encodes values between 3 -> 10, texture cache makes sure it is at least 3)
		u32 lcm    : 1; // 53
		u32 mmin   : 2; // 54
		u32 notest : 1; // 55 (no ztest, no atest, no date, no scissor test, and horizontally aligned to 4 pixels)
		// TODO: 1D texture flag? could save 2 texture reads and 4 lerps with bilinear, and also the texture coordinate clamp/wrap code in one direction
		u32 zequal : 1; // 56
		u32 hires  : 1; // 57 (drawing into the 2x memory, four times the size of local memory)
		u32 tw_hi  : 1; // 58 (tw + 8: a 2x texture 2048 or 4096 texels wide)
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
};

struct alignas(32) GSScanlineGlobalData // per batch variables, this is like a pixel shader constant buffer
{
	GSScanlineSelector sel;

	// - the data of vm, tex may change, multi-threaded drawing must be finished before that happens, clut and dimx are copies
	// - tex is a cached texture, it may be recycled to free up memory, its absolute address cannot be compiled into code
	// - row and column pointers are allocated once and never change or freed, thier address can be used directly

	void* vm;
	const void* tex[7];
	u32* clut;
	GSVector4i* dimx;

	GSOffset fbo;
	GSOffset zbo;
	const GSVector2i* fzbr;
	const GSVector2i* fzbc;

	GSVector4i aref;
	GSVector4i afix;
	struct { GSVector4i min, max, minmax, mask, invmask; } t; // [u] x 4 [v] x 4

#if _M_SSE >= 0x501

	u32 fm, zm;
	u32 frb, fga;
	GSVector8 mxl;
	GSVector8 l; // TEX1.L * -0x10000
	/* -log2(Q) * (1 << L) + K, times 0x10000, as a cubic in Q's mantissa
	 * plus its exponent times l (see GS_SCANLINE_SET_LOD). */
	GSVector8 lod_c3, lod_c2, lod_c1, lod_c0;
	struct { GSVector8i i, f; } lod; // lcm == 1

#else

	GSVector4i fm, zm;
	GSVector4i frb, fga;
	GSVector4 mxl;
	GSVector4 l; // TEX1.L * -0x10000
	/* -log2(Q) * (1 << L) + K, times 0x10000, as a cubic in Q's mantissa
	 * plus its exponent times l (see GS_SCANLINE_SET_LOD). */
	GSVector4 lod_c3, lod_c2, lod_c1, lod_c0;
	struct { GSVector4i i, f; } lod; // lcm == 1

#endif
};

/* The LOD the scanline code computes from Q for TEX1.L l_log2 and K k
 * (K * 0x10000): log2 of a mantissa m in [1, 2) is p(m) * (m - 1), p a
 * minimax fit of log2(m) / (m - 1), here times l and expanded into a
 * cubic that evaluates in two short chains. l is a power of two, so
 * every product with it is exact. */
#define GS_SCANLINE_LOD_P2 0.204446009836232697516f
#define GS_SCANLINE_LOD_P1 -1.04913055217340124191f
#define GS_SCANLINE_LOD_P0 2.28330284476918490682f
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

struct alignas(32) GSScanlineLocalData // per prim variables, each thread has its own
{
#if _M_SSE >= 0x501

	struct skip { GSVector8 z0, z1, s, t, q; GSVector8i rb, ga, f; } d[8]; /* z0, z1: four doubles each */
	struct step { GSVector4 stq; struct { u32 rb, ga; } c; struct { u64 z; u32 f; } p; } d8;
	struct { u32 z, f; } p;
	struct { GSVector8i rb, ga; } c;
	/* The primitive's grid for S, T, Q and Z, and its reciprocals: a
	 * scanline's start snaps to it (see GSRasterizer::SetupPrim). */
	GSVector4 tgrid, tgrid_rcp;
	struct { double grid, rcp; } zgrid;

	// these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack)

	struct
	{
		GSVector8 z0, z1;
		GSVector8i f;
		GSVector8 s, t, q;
		GSVector8i rb, ga;
		GSVector8i zs, zd;
		GSVector8i uf, vf;
		GSVector8i cov;

		// mipmapping

		struct { GSVector8i i, f; } lod;
		GSVector8i uv[2];
		GSVector8i uv_minmax[2];
		GSVector8i trb, tga;
		GSVector8i test;
	} temp;

#else

	struct skip { GSVector4 z0, z1, s, t, q; GSVector4i rb, ga, f; } d[4]; /* z0, z1: two doubles each */
	struct step { GSVector4 z, stq; GSVector4i c, f; } d4;
	struct { GSVector4i rb, ga; } c;
	struct { GSVector4i z, f; } p;
	/* The primitive's grid for S, T, Q and Z, and its reciprocals: a
	 * scanline's start snaps to it (see GSRasterizer::SetupPrim). */
	GSVector4 tgrid, tgrid_rcp;
	struct { double grid, rcp; } zgrid;

	// these should be stored on stack as normal local variables (no free regs to use, esp cannot be saved to anywhere, and we need an aligned stack)

	struct
	{
		GSVector4 z0, z1;
		GSVector4i f;
		GSVector4 s, t, q;
		GSVector4i rb, ga;
		GSVector4i zs, zd;
		GSVector4i uf, vf;
		GSVector4i cov;

		// mipmapping

		struct { GSVector4i i, f; } lod;
		GSVector4i uv[2];
		GSVector4i uv_minmax[2];
		GSVector4i trb, tga;
		GSVector4i test;
	} temp;

#endif

	//

	const GSScanlineGlobalData* gd;
};

// Constant shared by all threads (to reduce cache miss)
struct GSScanlineConstantData : public GSAlignedClass<32>
{
	alignas(32) u8 m_test_256b[16][8] = {
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00},
		{0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00},
		{0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00},
		{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00},
		{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00},
		{0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
		{0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
		{0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff},
		{0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff},
		{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
	};
	alignas(32) float m_shift_256b[9][8] = {
		{ 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f  , 5.0f  , 6.0f  , 7.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f  , 5.0f  , 6.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f  , 5.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f},
		{ -4.0f , -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -5.0f , -4.0f , -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -6.0f , -5.0f , -4.0f , -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -7.0f , -6.0f , -5.0f , -4.0f , -3.0f , -2.0f , -1.0f , 0.0f},
	};
	alignas(32) float m_one_256b[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

	alignas(16) u32 m_test_128b[8][4] = {
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	};
	alignas(16) float m_shift_128b[5][4] = {
		{ 4.0f  , 4.0f  , 4.0f  , 4.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f},
	};
	alignas(16) float m_one_128b[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	constexpr GSScanlineConstantData() {}
};

extern const GSScanlineConstantData g_const;
