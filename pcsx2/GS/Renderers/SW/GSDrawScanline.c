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

/* A draw's scanline code (GSDrawScanline.h). On x86 its functions are
 * generated (GSSwJit.h) and kept by selector; elsewhere the C rasterizer
 * below draws: slower than generated code, written against gs_vector,
 * whose NEON lanes behave as the generated code's do. C89, compiled once
 * a tier. */

#include <stddef.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GSDrawScanline.h"
#include "GSRasterizer.h"
#ifdef GS_DS_JIT
#include "common/CpuFeatures.h"
#include "../Common/GSCodeReserve.h"
#include "GSSwJit.h"
#endif
/* (the generated code's offsets, held to these structs) */
#include "GSSwJitLayout.h"

#if _M_SSE >= 0x501
#include <immintrin.h>
#endif

#if !defined(MULTI_ISA_UNSHARED_COMPILATION) || MULTI_ISA_IS_FIRST
/* (one copy, of the first tier: the tables are the same in each) */
const GSScanlineConstantData g_const = {
	{
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
	},
	{
		{ 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f  , 8.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f  , 5.0f  , 6.0f  , 7.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f  , 5.0f  , 6.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f  , 5.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f  , 4.0f},
		{ -4.0f , -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -5.0f , -4.0f , -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -6.0f , -5.0f , -4.0f , -3.0f , -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -7.0f , -6.0f , -5.0f , -4.0f , -3.0f , -2.0f , -1.0f , 0.0f},
	},
	{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
	{
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0x00000000, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0x00000000, 0x00000000},
		{0xffffffff, 0xffffffff, 0xffffffff, 0x00000000},
		{0x00000000, 0xffffffff, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0xffffffff, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0xffffffff},
		{0x00000000, 0x00000000, 0x00000000, 0x00000000},
	},
	{
		{ 4.0f  , 4.0f  , 4.0f  , 4.0f},
		{ 0.0f  , 1.0f  , 2.0f  , 3.0f},
		{ -1.0f , 0.0f  , 1.0f  , 2.0f},
		{ -2.0f , -1.0f , 0.0f  , 1.0f},
		{ -3.0f , -2.0f , -1.0f , 0.0f},
	},
	{1.0f, 1.0f, 1.0f, 1.0f},
};
#endif

void GS_SW(gs_draw_scanline_begin_draw)(const gs_rasterizer_data* data, GSScanlineLocalData* local)
{
	const GSScanlineGlobalData* global = &data->global;
	local->gd = global;

	if (global->sel.mmin && global->sel.lcm)
	{
		gs_vec4i v;
#if _M_SSE >= 0x501
		/* (the low lane of the eight; see GCC bug 80286 for why it is read
		 * as an int and not extracted) */
		const int lod_x = _mm_cvtsi128_si32(_mm256_castsi256_si128(global->lod.i));
		v = gs_v4i_srl16n(global->t.minmax, lod_x);
		v = gs_v4i_upl16v(v, v);
		/* (each half the four lanes, as the class's assignment does) */
		local->temp.uv_minmax[0] = _mm256_broadcastsi128_si256(gs_v4i_upl32v(v, v));
		local->temp.uv_minmax[1] = _mm256_broadcastsi128_si256(gs_v4i_uph32v(v, v));
#else
		v = gs_v4i_srl16n(global->t.minmax, GS_V4I_EXTRACT32(global->lod.i, 0));
		v = gs_v4i_upl16v(v, v);
		local->temp.uv_minmax[0] = gs_v4i_upl32v(v, v);
		local->temp.uv_minmax[1] = gs_v4i_uph32v(v, v);
#endif
	}
}

#ifdef GS_DS_JIT

/* (the scanline's z overflow bias, GSVector4::m_xc1e00000000fffff, which
 * the generated code reads) */
static const PCSX2_ALIGN(32) u64 s_zbias[4] = {
	0xc1e00000000fffffull, 0xc1e00000000fffffull, 0xc1e00000000fffffull, 0xc1e00000000fffffull};

/* The generators of this tier's code, for the maps */
static void fill_target(gs_swj_target* t, u64 key)
{
	t->key        = key;
	t->avx        = CPU_HAS_AVX;
	t->avx2       = CPU_HAS_AVX2;
	t->ymm        = _M_SSE >= 0x501;
	t->win64      = GS_SWJ_WIN64;
	t->shift_128b = g_const.m_shift_128b;
	t->shift_256b = g_const.m_shift_256b;
	t->consts     = &g_const;
	t->zbias      = s_zbias;
}

static size_t gen_setup_prim(u64 key, void* code, size_t room)
{
	gs_swj_target t;
	fill_target(&t, key);
	return gs_swj_setup_prim(&t, code, room);
}

static size_t gen_draw_scanline(u64 key, void* code, size_t room)
{
	gs_swj_target t;
	fill_target(&t, key);
	return gs_swj_draw_scanline(&t, code, room);
}

void GS_SW(gs_draw_scanline_init)(gs_draw_scanline* ds)
{
	gs_code_map_init(&ds->sp_map, gen_setup_prim, 2048);
	gs_code_map_init(&ds->ds_map, gen_draw_scanline, 8192);
	code_reserve_allow_modification(&g_gs_code_reserve.reserve);
	gs_code_reserve_reset();
}

void GS_SW(gs_draw_scanline_free)(gs_draw_scanline* ds)
{
	code_reserve_forbid_modification(&g_gs_code_reserve.reserve);
	gs_code_map_free(&ds->sp_map);
	gs_code_map_free(&ds->ds_map);
}

void GS_SW(gs_draw_scanline_reset_code_cache)(gs_draw_scanline* ds)
{
	gs_code_map_clear(&ds->sp_map);
	gs_code_map_clear(&ds->ds_map);
	gs_code_reserve_reset();
}

bool GS_SW(gs_draw_scanline_setup_draw)(gs_draw_scanline* ds, gs_rasterizer_data* data)
{
	const GSScanlineGlobalData* global = &data->global;
	GSScanlineSelector sel;

	data->draw_scanline = (gs_raster_scanline_fn)gs_code_map_get(&ds->ds_map, global->sel.key);
	if (unlikely(!data->draw_scanline))
		return false;

	if (global->sel.aa1)
	{
		sel.key = global->sel.key;
		sel.zwrite = 0;
		sel.edge = 1;

		data->draw_edge = (gs_raster_scanline_fn)gs_code_map_get(&ds->ds_map, sel.key);
		if (unlikely(!data->draw_edge))
			return false;
	}
	else
	{
		data->draw_edge = NULL;
	}

	/* doesn't need all bits => less functions generated */

	sel.key = 0;

	sel.iip = global->sel.iip;
	sel.tfx = global->sel.tfx;
	sel.tcc = global->sel.tcc;
	sel.fst = global->sel.fst;
	sel.fge = global->sel.fge;
	sel.prim = global->sel.prim;
	sel.fb = global->sel.fb;
	sel.zb = global->sel.zb;
	sel.zoverflow = global->sel.zoverflow;
	sel.zequal = global->sel.zequal;
	sel.notest = global->sel.notest;

	data->setup_prim = (gs_raster_setup_prim_fn)gs_code_map_get(&ds->sp_map, sel.key);
	return data->setup_prim != NULL;
}

#else /* !GS_DS_JIT */

/* --- the C rasterizer ------------------------------------------------------
 *
 * Four pixels at a time, in gs_vec4i; floats are carried in it (gs_v4i_f*),
 * doubles two at a time (gs_v4i_d*). */

#define VI gs_vec4i

static GS_VEC_INLINE VI v_i(int x) { return gs_v4i_set32(x); }
static GS_VEC_INLINE VI v_x(VI v) { return GS_V4I_SHUFFLE32(v, 0x00); }
static GS_VEC_INLINE VI v_y(VI v) { return GS_V4I_SHUFFLE32(v, 0x55); }
static GS_VEC_INLINE VI v_z(VI v) { return GS_V4I_SHUFFLE32(v, 0xaa); }
static GS_VEC_INLINE VI v_w(VI v) { return GS_V4I_SHUFFLE32(v, 0xff); }
/* each 32-bit lane's low 16 bits twice (xxzzlh), its high twice (yywwlh) */
static GS_VEC_INLINE VI v_xxzzlh(VI v) { return GS_V4I_SHUFFLE16LH(v, 0xa0); }
static GS_VEC_INLINE VI v_yywwlh(VI v) { return GS_V4I_SHUFFLE16LH(v, 0xf5); }
static GS_VEC_INLINE VI v_not(VI v) { return gs_v4i_xor(v, gs_v4i_ones()); }
static GS_VEC_INLINE VI v_ne(VI a, VI b) { return v_not(gs_v4i_eq32(a, b)); }
/* a * f << shift, in 16 bits (modulate16<shift>) */
static GS_VEC_INLINE VI v_mod16_0(VI a, VI f) { return gs_v4i_mul16hs(GS_V4I_SLL16(a, 1), f); }
static GS_VEC_INLINE VI v_mod16_1(VI a, VI f) { return gs_v4i_mul16hs(GS_V4I_SLL16(a, 2), f); }
/* this + (a - this) * f >> 4 (lerp16_4), and << 0 + 1 (lerp16<0>) */
static GS_VEC_INLINE VI v_lerp16_4(VI t, VI a, VI f)
{
	return gs_v4i_add16(t, GS_V4I_SRA16(gs_v4i_mul16l(gs_v4i_sub16(a, t), f), 4));
}
static GS_VEC_INLINE VI v_lerp16_0(VI t, VI a, VI f) { return gs_v4i_add16(t, v_mod16_0(gs_v4i_sub16(a, t), f)); }
static GS_VEC_INLINE VI v_clamp8(VI v) { return gs_v4i_upl8(gs_v4i_pu16(v, v)); }
static GS_VEC_INLINE VI v_mix16(VI v, VI a) { return GS_V4I_BLEND16(v, a, 0xaa); }
static GS_VEC_INLINE VI v_sat_i16(VI v, VI lo, VI hi) { return gs_v4i_min_i16(gs_v4i_max_i16(v, lo), hi); }
/* the 16-bit lanes of the 32-bit halves of v, split: low halves (rb), high (ga) */
static GS_VEC_INLINE VI v_lo8(VI c) { return GS_V4I_SRL16(GS_V4I_SLL16(c, 8), 8); }
static GS_VEC_INLINE VI v_hi8(VI c) { return GS_V4I_SRL16(c, 8); }
/* bits of a where mask's are set, of v where clear (blend) */
static GS_VEC_INLINE VI v_blend(VI v, VI a, VI mask) { return gs_v4i_or(gs_v4i_andnot(mask, v), gs_v4i_and(mask, a)); }
static GS_VEC_INLINE VI v_ps32(VI v) { return gs_v4i_ps32(v, v); }
static GS_VEC_INLINE VI v_pu32(VI v) { return gs_v4i_pu32(v, v); }

/* the doubles of z, by the double at p */
static GS_VEC_INLINE VI v_f(float f) { return gs_v4i_fset(f); }

#define LOCAL_STEP local->d4
#define VLEN 4

static const GSScanlineGlobalData* global_of(const GSScanlineLocalData* local)
{
	return local->gd;
}

static void c_setup_prim(const GSVertexSW* vertex, const u16* index, const GSVertexSW* dscan, GSScanlineLocalData* local)
{
	const GSScanlineGlobalData* global = global_of(local);
	const GSScanlineSelector sel = global->sel;
	const int has_z = sel.zb != 0;
	const int has_f = sel.fb && sel.fge;
	const int has_t = sel.fb && sel.tfx != TFX_NONE;
	const int has_c = sel.fb && !(sel.tfx == TFX_DECAL && sel.tcc);
	const float (*shift)[4] = g_const.m_shift_128b;
	const VI step_shift = gs_v4i_load(shift[0]);
	const VI dt = gs_v4i_load(&dscan->t);
	const VI tstep = gs_v4i_fmul(dt, step_shift);
	int i, j;

	if (has_z || has_f)
	{
		if (sel.prim != GS_SPRITE_CLASS)
		{
			if (has_f)
			{
				const VI df = v_w(dt);

				local->d4.f = v_w(GS_V4I_SHUFFLE16H(gs_v4i_cvtt_i32(tstep), 0xaa));

				for (i = 0; i < VLEN; i++)
					local->d[i].f = v_xxzzlh(gs_v4i_cvtt_i32(gs_v4i_fmul(df, gs_v4i_load(shift[1 + i]))));
			}

			if (has_z && !sel.zequal)
			{
				const VI dz = gs_v4i_dbroadcast(&dscan->p.F64[1]);
				local->d4.z = gs_v4i_dmul(dz, gs_v4i_f32to64p(shift[0]));
				for (i = 0; i < VLEN; i++)
				{
					local->d[i].z0 = gs_v4i_dmul(dz, gs_v4i_f32to64p(&shift[i + 1][0]));
					local->d[i].z1 = gs_v4i_dmul(dz, gs_v4i_f32to64p(&shift[i + 1][2]));
				}
			}
		}
		else
		{
			if (has_f)
				local->p.f = v_z(GS_V4I_SHUFFLE16H(gs_v4i_cvtt_i32(gs_v4i_load(&vertex[index[1]].p)), 0xaa));

			if (has_z)
				local->p.z = gs_v4i_set32((int)vertex[index[1]].t.U32[3]); /* u32 z is bypassed in t.w */
		}
	}

	if (has_t)
	{
		const int k = sel.fst ? 2 : 3;

		if (sel.fst)
			LOCAL_STEP.stq = gs_v4i_cvtt_i32(tstep);
		else
			LOCAL_STEP.stq = tstep;

		for (j = 0; j < k; j++)
		{
			const VI dstq = j == 0 ? v_x(dt) : j == 1 ? v_y(dt) : v_z(dt);

			for (i = 0; i < VLEN; i++)
			{
				const VI v = gs_v4i_fmul(dstq, gs_v4i_load(shift[1 + i]));

				if (sel.fst)
				{
					if (j == 0)
						local->d[i].s = gs_v4i_cvtt_i32(v);
					else
						local->d[i].t = gs_v4i_cvtt_i32(v);
				}
				else
				{
					if (j == 0)
						local->d[i].s = v;
					else if (j == 1)
						local->d[i].t = v;
					else
						local->d[i].q = v;
				}
			}
		}
	}

	if (has_c)
	{
		if (sel.iip)
		{
			const VI mask16 = v_i(0xffff);
			const VI dc = gs_v4i_load(&dscan->c);
			const VI dr = v_x(dc), db = v_z(dc), dg = v_y(dc), da = v_w(dc);

			local->d4.c = v_pu32(GS_V4I_SHUFFLE32(gs_v4i_and(gs_v4i_cvtt_i32(gs_v4i_fmul(dc, step_shift)), mask16), 0xd8));

			for (i = 0; i < VLEN; i++)
			{
				const VI r = v_pu32(gs_v4i_and(gs_v4i_cvtt_i32(gs_v4i_fmul(dr, gs_v4i_load(shift[1 + i]))), mask16));
				const VI b = v_pu32(gs_v4i_and(gs_v4i_cvtt_i32(gs_v4i_fmul(db, gs_v4i_load(shift[1 + i]))), mask16));

				local->d[i].rb = gs_v4i_upl16v(r, b);
			}

			for (i = 0; i < VLEN; i++)
			{
				const VI g = v_pu32(gs_v4i_and(gs_v4i_cvtt_i32(gs_v4i_fmul(dg, gs_v4i_load(shift[1 + i]))), mask16));
				const VI a = v_pu32(gs_v4i_and(gs_v4i_cvtt_i32(gs_v4i_fmul(da, gs_v4i_load(shift[1 + i]))), mask16));

				local->d[i].ga = gs_v4i_upl16v(g, a);
			}
		}
		else
		{
			int last = 0;
			VI c;

			switch (sel.prim)
			{
				case GS_POINT_CLASS:    last = 0; break;
				case GS_LINE_CLASS:     last = 1; break;
				case GS_TRIANGLE_CLASS: last = 2; break;
				case GS_SPRITE_CLASS:   last = 1; break;
			}

			c = gs_v4i_cvtt_i32(gs_v4i_load(&vertex[index[last]].c));

			c = gs_v4i_upl16v(c, GS_V4I_SHUFFLE32(c, 0x4e));

			if (sel.tfx == TFX_NONE)
				c = GS_V4I_SRL16(c, 7);

			local->c.rb = v_x(c);
			local->c.ga = v_z(c);
		}
	}
}

/* -log2(Q) * (1 << L) + K, times 0x10000, from Q's mantissa m and
 * exponent e: (m * c3 + c2) * (m * m) + (m * c1 + (e * L + c0)), each
 * product and sum rounded, as the generated code computes it. */
static GS_VEC_INLINE VI lod_from_q(VI q, const GSScanlineGlobalData* global)
{
	const VI e = gs_v4i_cvt_f32(gs_v4i_sub32(GS_V4I_SRL32(GS_V4I_SLL32(q, 1), 24), v_i(0x7f)));
	const VI m = gs_v4i_or(GS_V4I_SRL32(GS_V4I_SLL32(q, 9), 9), v_i(GS_V4I_F32_ONE));
	const VI a = gs_v4i_fadd(gs_v4i_fmul(m, global->lod_c1), gs_v4i_fadd(gs_v4i_fmul(e, global->l), global->lod_c0));
	const VI b = gs_v4i_fmul(gs_v4i_fadd(gs_v4i_fmul(m, global->lod_c3), global->lod_c2), gs_v4i_fmul(m, m));
	return gs_v4i_fadd(a, b);
}

/* Whether any pixel is left after the alpha test: the failing ones are
 * added to test, or to the masks, by AFAIL */
static GS_VEC_INLINE int test_alpha(VI* test, VI* fm, VI* zm, VI ga, const GSScanlineGlobalData* global)
{
	const GSScanlineSelector sel = global->sel;
	VI t;

	switch (sel.afail)
	{
		case AFAIL_FB_ONLY:
			if (!sel.zwrite)
				return 1;
			break;

		case AFAIL_ZB_ONLY:
			if (!sel.fwrite)
				return 1;
			break;

		case AFAIL_RGB_ONLY:
			if (!sel.zwrite && sel.fpsm == 1)
				return 1;
			break;
	}

	switch (sel.atst)
	{
		case ATST_NEVER:
			t = gs_v4i_ones();
			break;

		case ATST_ALWAYS:
			return 1;

		case ATST_LESS:
		case ATST_LEQUAL:
			t = gs_v4i_gt32(GS_V4I_SRL32(ga, 16), global->aref);
			break;

		case ATST_EQUAL:
			t = v_ne(GS_V4I_SRL32(ga, 16), global->aref);
			break;

		case ATST_GEQUAL:
		case ATST_GREATER:
			t = gs_v4i_lt32(GS_V4I_SRL32(ga, 16), global->aref);
			break;

		case ATST_NOTEQUAL:
		default:
			t = gs_v4i_eq32(GS_V4I_SRL32(ga, 16), global->aref);
			break;
	}

	switch (sel.afail)
	{
		case AFAIL_KEEP:
			*test = gs_v4i_or(*test, t);
			if (gs_v4i_alltrue(*test))
				return 0;
			break;

		case AFAIL_FB_ONLY:
			*zm = gs_v4i_or(*zm, t);
			break;

		case AFAIL_ZB_ONLY:
			*fm = gs_v4i_or(*fm, t);
			break;

		case AFAIL_RGB_ONLY:
		default:
			*zm = gs_v4i_or(*zm, t);
			*fm = gs_v4i_or(*fm, gs_v4i_and(t, v_i((int)0xff000000))); /* fpsm 16 bit => & 0xffff8000? */
			break;
	}

	return 1;
}

static const int s_offsets[] = {0, 2, 8, 10, 16, 18, 24, 26}; /* gs_column_table16[0] */

static GS_VEC_INLINE void write_pixel(VI src, int addr, int i, u32 psm, const GSScanlineGlobalData* global)
{
	u8* dst = (u8*)global->vm + addr * 2 + s_offsets[i] * 2;
	union gs_v4i_view s;
	u32 d;
	s.v = src;

	switch (psm)
	{
		case 0:
			memcpy(dst, &s.u32[i], 4);
			break;
		case 1:
			memcpy(&d, dst, 4);
			d = (s.u32[i] & 0xffffff) | (d & 0xff000000);
			memcpy(dst, &d, 4);
			break;
		case 2:
			memcpy(dst, &s.u16[i * 2], 2);
			break;
	}
}

/* The texel addresses of four u, v pairs (16.16, shifted by the LOD
 * already): wrapped or clamped into [min, max], the second of each pair
 * one on for bilinear filtering, and the fractions that filter with */
typedef struct sl_texaddr
{
	VI a00, a01, a10, a11;
} sl_texaddr;

static GS_VEC_INLINE void tex_addr(GSScanlineSelector sel, VI uv0, VI minuv, VI maxuv, VI mask, sl_texaddr* a)
{
	const int shift = sel.tw + (sel.tw_hi << 3) + 3;
	VI uv1 = uv0;
	VI y0, x0;

	{
		const VI repeat = gs_v4i_or(gs_v4i_and(uv0, minuv), maxuv);
		const VI clamp = v_sat_i16(uv0, minuv, maxuv);
		uv0 = gs_v4i_blend8(clamp, repeat, mask);
	}

	if (sel.ltf)
	{
		uv1 = gs_v4i_add16(uv1, v_i(0x00010001));
		{
			const VI repeat = gs_v4i_or(gs_v4i_and(uv1, minuv), maxuv);
			const VI clamp = v_sat_i16(uv1, minuv, maxuv);
			uv1 = gs_v4i_blend8(clamp, repeat, mask);
		}
	}

	y0 = gs_v4i_sll32n(gs_v4i_uph16(uv0), shift);
	x0 = gs_v4i_upl16(uv0);

	if (sel.ltf)
	{
		const VI y1 = gs_v4i_sll32n(gs_v4i_uph16(uv1), shift);
		const VI x1 = gs_v4i_upl16(uv1);

		a->a00 = gs_v4i_add32(y0, x0);
		a->a01 = gs_v4i_add32(y0, x1);
		a->a10 = gs_v4i_add32(y1, x0);
		a->a11 = gs_v4i_add32(y1, x1);
	}
	else
	{
		a->a00 = gs_v4i_add32(y0, x0);
	}
}

/* A texel of each lane's own level (lodi), through the CLUT if tlu */
static GS_VEC_INLINE VI fetch(GSScanlineSelector sel, const GSScanlineGlobalData* global, VI lodi, VI addr)
{
	union gs_v4i_view l, a, c;
	int i;
	l.v = lodi;
	a.v = addr;
	for (i = 0; i < 4; i++)
	{
		if (sel.tlu)
			c.u32[i] = global->clut[((const u8*)global->tex[l.u32[i]])[a.u32[i]]];
		else
			c.u32[i] = ((const u32*)global->tex[l.u32[i]])[a.u32[i]];
	}
	return c.v;
}

/* The texels at addresses a, filtered by uf and vf when ltf: rb and ga */
static GS_VEC_INLINE void sample(GSScanlineSelector sel, const GSScanlineGlobalData* global, VI lodi, const sl_texaddr* a,
	VI uf, VI vf, VI* rb, VI* ga)
{
	if (sel.ltf)
	{
		const VI c00 = fetch(sel, global, lodi, a->a00);
		const VI c01 = fetch(sel, global, lodi, a->a01);
		const VI c10 = fetch(sel, global, lodi, a->a10);
		const VI c11 = fetch(sel, global, lodi, a->a11);
		const VI rb00 = v_lerp16_4(v_lo8(c00), v_lo8(c01), uf);
		const VI ga00 = v_lerp16_4(v_hi8(c00), v_hi8(c01), uf);
		const VI rb10 = v_lerp16_4(v_lo8(c10), v_lo8(c11), uf);
		const VI ga10 = v_lerp16_4(v_hi8(c10), v_hi8(c11), uf);

		*rb = v_lerp16_4(rb00, rb10, vf);
		*ga = v_lerp16_4(ga00, ga10, vf);
	}
	else
	{
		const VI c00 = fetch(sel, global, lodi, a->a00);

		*rb = v_lo8(c00);
		*ga = v_hi8(c00);
	}
}

static void c_draw_scanline_sel(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData* local, GSScanlineSelector sel)
{
	const GSScanlineGlobalData* global = global_of(local);
	const u32 (*const_test)[4] = g_const.m_test_128b;
	const gs_point* fza_base;
	const gs_point* fza_offset;
	VI test = gs_v4i_zero();
	VI z0 = gs_v4i_zero(), z1 = gs_v4i_zero();
	VI f = gs_v4i_zero();
	VI s = gs_v4i_zero(), t = gs_v4i_zero(), q = gs_v4i_zero();
	VI uf = gs_v4i_zero(), vf = gs_v4i_zero();
	VI rbf = gs_v4i_zero(), gaf = gs_v4i_zero();
	VI cov = gs_v4i_zero();
	const VI scan_t = gs_v4i_load(&scan->t);
	const u32 vm_mask = (sel.hires ? 4 * HALF_VM_SIZE : HALF_VM_SIZE) - 1;
	int skip, steps;

	/* Init */

	if (!sel.notest)
	{
		skip = left & (VLEN - 1);
		steps = pixels + skip - VLEN;
		left -= skip;
		test = gs_v4i_or(gs_v4i_load(const_test[skip]), gs_v4i_load(const_test[7 + (steps & (steps >> 31))]));
	}
	else
	{
		skip = 0;
		steps = pixels - VLEN;
	}

	fza_base = &global->fzbr[top];
	fza_offset = &global->fzbc[left >> 2];

	if (sel.prim != GS_SPRITE_CLASS)
	{
		if (sel.fwrite && sel.fge)
			f = gs_v4i_add16(v_z(GS_V4I_SHUFFLE16H(gs_v4i_cvtt_i32(scan_t), 0xaa)), local->d[skip].f);

		if (sel.zb)
		{
			if (sel.zequal)
			{
				z0 = v_i((int)(u32)scan->p.F64[1]);
			}
			else
			{
				double z = scan->p.F64[1];
				VI zbase;
				if (sel.prim != GS_POINT_CLASS && !sel.edge)
					z = GS_ROUND_NEAREST64(z * local->zgrid.rcp) * local->zgrid.grid;
				zbase = gs_v4i_dbroadcast(&z);
				z0 = gs_v4i_dadd(zbase, local->d[skip].z0);
				z1 = gs_v4i_dadd(zbase, local->d[skip].z1);
			}
		}
	}

	if (sel.fb)
	{
		if (sel.edge)
			cov = GS_V4I_SRL16(v_x(GS_V4I_SHUFFLE16L(gs_v4i_load(&scan->p), 0x00)), 9);

		if (sel.tfx != TFX_NONE)
		{
			if (sel.fst)
			{
				const VI vt = gs_v4i_cvtt_i32(scan_t);
				const VI u = gs_v4i_add32(v_x(vt), local->d[skip].s);
				VI v = v_y(vt);

				if (sel.prim != GS_SPRITE_CLASS || sel.mmin)
					v = gs_v4i_add32(v, local->d[skip].t);
				else if (sel.ltf)
					vf = GS_V4I_SRL16(v_xxzzlh(v), 12);

				s = u;
				t = v;
			}
			else
			{
				VI st = scan_t;
				if (sel.prim != GS_POINT_CLASS && !sel.edge)
					st = gs_v4i_fmul(gs_v4i_fround(gs_v4i_fmul(st, local->tgrid_rcp)), local->tgrid);
				s = gs_v4i_fadd(v_x(st), local->d[skip].s);
				t = gs_v4i_fadd(v_y(st), local->d[skip].t);
				q = gs_v4i_fadd(v_z(st), local->d[skip].q);
			}
		}

		if (!(sel.tfx == TFX_DECAL && sel.tcc))
		{
			if (sel.iip)
			{
				VI c = gs_v4i_cvtt_i32(gs_v4i_load(&scan->c));

				c = gs_v4i_upl16v(c, GS_V4I_SHUFFLE32(c, 0x4e));

				rbf = gs_v4i_add16(v_x(c), local->d[skip].rb);
				gaf = gs_v4i_add16(v_z(c), local->d[skip].ga);
			}
			else
			{
				rbf = local->c.rb;
				gaf = local->c.ga;
			}
		}
	}

	for (;;)
	{
		do
		{
			int fa = 0, za = 0;
			int fzm = 0;
			VI fd = gs_v4i_zero(), zs = gs_v4i_zero(), zd = gs_v4i_zero();
			VI fm = gs_v4i_zero(), zm = gs_v4i_zero();
			VI rb = gs_v4i_zero(), ga = gs_v4i_zero();

			/* TestZ */

			if (sel.zb)
			{
				/* (unsigned and masked, as the generated code does) */
				za = (int)(((u32)fza_base->y + (u32)fza_offset->y + global->zbase) & vm_mask);

				if (sel.prim != GS_SPRITE_CLASS)
				{
					if (sel.zequal)
					{
						zs = z0;
					}
					else if (sel.zoverflow)
					{
						/* SSE only has double to int32 conversion, no double to uint32
						 * Work around this by subtracting 0x80000000 before converting, then adding it back after
						 * Since we've subtracted 0x80000000, truncating now rounds up for numbers less than 0x80000000
						 * So approximate the truncation by subtracting an extra (0.5 - ulp) and rounding instead */
						const VI bias = gs_v4i_set64(0xc1e00000000fffffull, 0xc1e00000000fffffull);
						const VI zl = gs_v4i_f64toi32(gs_v4i_dadd(z0, bias), 0);
						const VI zh = gs_v4i_f64toi32(gs_v4i_dadd(z1, bias), 0);
						zs = gs_v4i_upl64v(zl, zh);
						zs = gs_v4i_add32(zs, v_i((int)0x80000000));
					}
					else
					{
						zs = gs_v4i_upl64v(gs_v4i_f64toi32(z0, 1), gs_v4i_f64toi32(z1, 1));
					}

					if (sel.zclamp)
						zs = gs_v4i_min_u32(zs, v_i((int)(0xFFFFFFFFu >> (sel.zpsm * 8))));
				}
				else
				{
					zs = local->p.z;
				}

				if (sel.ztest)
				{
					VI zso, zdo;

					zd = gs_v4i_loadlh((u8*)global->vm + za * 2, (u8*)global->vm + za * 2 + 16);

					zso = zs;
					zdo = zd;

					switch (sel.zpsm)
					{
						case 1: zdo = GS_V4I_SRL32(GS_V4I_SLL32(zdo, 8), 8); break;
						case 2: zdo = GS_V4I_SRL32(GS_V4I_SLL32(zdo, 16), 16); break;
						default: break;
					}

					if (sel.zpsm == 0)
					{
						zso = gs_v4i_sub32(zso, v_i((int)0x80000000));
						zdo = gs_v4i_sub32(zdo, v_i((int)0x80000000));
					}

					switch (sel.ztst)
					{
						case ZTST_GEQUAL:  test = gs_v4i_or(test, gs_v4i_lt32(zso, zdo)); break;
						case ZTST_GREATER: test = gs_v4i_or(test, v_not(gs_v4i_gt32(zso, zdo))); break;
					}

					if (gs_v4i_alltrue(test))
						continue;
				}
			}

			/* SampleTexture */

			if (sel.fb && sel.tfx != TFX_NONE)
			{
				VI u, v, uv[2];
				VI lodi = gs_v4i_zero(), lodf = gs_v4i_zero();
				VI minuv, maxuv;
				sl_texaddr a;

				if (sel.mmin)
				{
					if (!sel.fst)
					{
						u = gs_v4i_cvtt_i32(gs_v4i_fdiv(s, q));
						v = gs_v4i_cvtt_i32(gs_v4i_fdiv(t, q));
					}
					else
					{
						u = s;
						v = t;
					}

					if (!sel.lcm)
					{
						const VI tmp = lod_from_q(q, global); /* (-log2(Q) * (1 << L) + K) * 0x10000 */
						VI lod = gs_v4i_cvtn_i32(gs_v4i_fsat(tmp, gs_v4i_zero(), global->mxl));
						union gs_v4i_view li;
						VI aabb, ccdd, aaxx, xxbb, ccxx, xxdd, acac, bdbd, v0, v1, v2, v3;

						if (sel.mmin == 1) /* round-off mode */
							lod = gs_v4i_add32(lod, v_i(0x8000));

						lodi = GS_V4I_SRL32(lod, 16);

						if (sel.mmin == 2) /* trilinear mode */
							lodf = v_xxzzlh(lod);

						/* shift u/v by (int)lod */

						li.v = lodi;
						aabb = gs_v4i_upl32v(u, v);
						ccdd = gs_v4i_uph32v(u, v);

						aaxx = gs_v4i_sra32n(aabb, li.i32[0]);
						xxbb = gs_v4i_sra32n(aabb, li.i32[1]);
						ccxx = gs_v4i_sra32n(ccdd, li.i32[2]);
						xxdd = gs_v4i_sra32n(ccdd, li.i32[3]);

						acac = gs_v4i_upl32v(aaxx, ccxx);
						bdbd = gs_v4i_uph32v(xxbb, xxdd);

						u = gs_v4i_upl32v(acac, bdbd);
						v = gs_v4i_uph32v(acac, bdbd);

						uv[0] = u;
						uv[1] = v;

						v0 = gs_v4i_srl16n(global->t.minmax, li.i32[0]);
						v1 = gs_v4i_srl16n(global->t.minmax, li.i32[1]);
						v2 = gs_v4i_srl16n(global->t.minmax, li.i32[2]);
						v3 = gs_v4i_srl16n(global->t.minmax, li.i32[3]);

						v0 = gs_v4i_upl16v(v0, v1);
						v2 = gs_v4i_upl16v(v2, v3);

						minuv = gs_v4i_upl32v(v0, v2);
						maxuv = gs_v4i_uph32v(v0, v2);
					}
					else
					{
						const int lx = GS_V4I_EXTRACT32(global->lod.i, 0);

						lodi = global->lod.i;

						u = gs_v4i_sra32n(u, lx);
						v = gs_v4i_sra32n(v, lx);

						uv[0] = u;
						uv[1] = v;

						minuv = local->temp.uv_minmax[0];
						maxuv = local->temp.uv_minmax[1];
					}

					if (sel.ltf)
					{
						u = gs_v4i_sub32(u, v_i(0x8000));
						v = gs_v4i_sub32(v, v_i(0x8000));

						uf = GS_V4I_SRL16(v_xxzzlh(u), 12);
						vf = GS_V4I_SRL16(v_xxzzlh(v), 12);
					}

					tex_addr(sel, gs_v4i_ps32(GS_V4I_SRA32(u, 16), GS_V4I_SRA32(v, 16)), minuv, maxuv, global->t.mask, &a);
					sample(sel, global, lodi, &a, uf, vf, &rb, &ga);

					if (sel.mmin != 1) /* !round-off mode */
					{
						VI rb2, ga2;

						lodi = gs_v4i_add32(lodi, v_i(1));

						u = GS_V4I_SRA32(uv[0], 1);
						v = GS_V4I_SRA32(uv[1], 1);

						minuv = GS_V4I_SRL16(minuv, 1);
						maxuv = GS_V4I_SRL16(maxuv, 1);

						if (sel.ltf)
						{
							u = gs_v4i_sub32(u, v_i(0x8000));
							v = gs_v4i_sub32(v, v_i(0x8000));

							uf = GS_V4I_SRL16(v_xxzzlh(u), 12);
							vf = GS_V4I_SRL16(v_xxzzlh(v), 12);
						}

						tex_addr(sel, gs_v4i_ps32(GS_V4I_SRA32(u, 16), GS_V4I_SRA32(v, 16)), minuv, maxuv, global->t.mask, &a);
						sample(sel, global, lodi, &a, uf, vf, &rb2, &ga2);

						if (sel.lcm)
							lodf = global->lod.f;

						lodf = GS_V4I_SRL16(lodf, 1);

						rb = v_lerp16_0(rb, rb2, lodf);
						ga = v_lerp16_0(ga, ga2, lodf);
					}
				}
				else
				{
					if (!sel.fst)
					{
						u = gs_v4i_cvtt_i32(gs_v4i_fdiv(s, q));
						v = gs_v4i_cvtt_i32(gs_v4i_fdiv(t, q));

						if (sel.ltf)
						{
							u = gs_v4i_sub32(u, v_i(0x8000));
							v = gs_v4i_sub32(v, v_i(0x8000));
						}
					}
					else
					{
						u = s;
						v = t;
					}

					if (sel.ltf)
					{
						uf = GS_V4I_SRL16(v_xxzzlh(u), 12);

						if (sel.prim != GS_SPRITE_CLASS)
							vf = GS_V4I_SRL16(v_xxzzlh(v), 12);
					}

					/* (one texture: the lanes' level is 0) */
					tex_addr(sel, gs_v4i_ps32(GS_V4I_SRA32(u, 16), GS_V4I_SRA32(v, 16)), global->t.min, global->t.max, global->t.mask, &a);
					sample(sel, global, gs_v4i_zero(), &a, uf, vf, &rb, &ga);
				}
			}

			/* AlphaTFX */

			if (sel.fb)
			{
				switch (sel.tfx)
				{
					case TFX_MODULATE:
						ga = v_clamp8(v_mod16_1(ga, gaf));
						if (!sel.tcc)
							ga = v_mix16(ga, GS_V4I_SRL16(gaf, 7));
						break;
					case TFX_DECAL:
						if (!sel.tcc)
							ga = v_mix16(ga, GS_V4I_SRL16(gaf, 7));
						break;
					case TFX_HIGHLIGHT:
						ga = v_mix16(ga, !sel.tcc ? GS_V4I_SRL16(gaf, 7) : gs_v4i_addus8(ga, GS_V4I_SRL16(gaf, 7)));
						break;
					case TFX_HIGHLIGHT2:
						if (!sel.tcc)
							ga = v_mix16(ga, GS_V4I_SRL16(gaf, 7));
						break;
					case TFX_NONE:
						ga = sel.iip ? GS_V4I_SRL16(gaf, 7) : gaf;
						break;
				}

				if (sel.aa1)
				{
					const VI x00800080 = v_i(0x00800080);
					const VI a = sel.edge ? cov : x00800080;

					if (!sel.abe)
						ga = v_mix16(ga, a);
					else
						ga = gs_v4i_blend8(ga, a, GS_V4I_SLL32(GS_V4I_SRL32(gs_v4i_eq16(ga, x00800080), 16), 16));
				}
			}

			/* ReadMask */

			if (sel.fwrite)
				fm = global->fm;

			if (sel.zwrite)
				zm = global->zm;

			/* TestAlpha */

			if (!test_alpha(&test, &fm, &zm, ga, global))
				continue;

			/* ColorTFX */

			if (sel.fwrite)
			{
				VI af;

				switch (sel.tfx)
				{
					case TFX_MODULATE:
						rb = v_clamp8(v_mod16_1(rb, rbf));
						break;
					case TFX_DECAL:
						break;
					case TFX_HIGHLIGHT:
					case TFX_HIGHLIGHT2:
						af = GS_V4I_SRL16(v_yywwlh(gaf), 7);
						rb = v_clamp8(gs_v4i_add16(v_mod16_1(rb, rbf), af));
						ga = v_mix16(v_clamp8(gs_v4i_add16(v_mod16_1(ga, gaf), af)), ga);
						break;
					case TFX_NONE:
						rb = sel.iip ? GS_V4I_SRL16(rbf, 7) : rbf;
						break;
				}
			}

			/* Fog */

			if (sel.fwrite && sel.fge)
			{
				const VI fog = sel.prim != GS_SPRITE_CLASS ? f : local->p.f;

				rb = v_lerp16_0(global->frb, rb, fog);
				ga = v_mix16(v_lerp16_0(global->fga, ga, fog), ga);
			}

			/* ReadFrame */

			if (sel.fb)
			{
				fa = (int)(((u32)fza_base->x + (u32)fza_offset->x + global->fbase) & vm_mask);

				if (sel.rfb)
					fd = gs_v4i_loadlh((u8*)global->vm + fa * 2, (u8*)global->vm + fa * 2 + 16);
			}

			/* TestDestAlpha */

			if (sel.date && (sel.fpsm == 0 || sel.fpsm == 2))
			{
				if (sel.datm)
				{
					if (sel.fpsm == 2)
						test = gs_v4i_or(test, gs_v4i_eq32(GS_V4I_SRA32(GS_V4I_SLL32(fd, 16), 31), gs_v4i_zero()));
					else
						test = gs_v4i_or(test, GS_V4I_SRA32(v_not(fd), 31));
				}
				else
				{
					if (sel.fpsm == 2)
						test = gs_v4i_or(test, GS_V4I_SRA32(GS_V4I_SLL32(fd, 16), 31));
					else
						test = gs_v4i_or(test, GS_V4I_SRA32(fd, 31));
				}

				if (gs_v4i_alltrue(test))
					continue;
			}

			/* WriteMask */

			if (!sel.notest)
			{
				if (sel.fwrite)
					fm = gs_v4i_or(fm, test);

				if (sel.zwrite)
					zm = gs_v4i_or(zm, test);

				if (sel.fwrite && sel.zwrite)
					fzm = ~gs_v4i_mask(gs_v4i_ps32(gs_v4i_eq32(fm, gs_v4i_ones()), gs_v4i_eq32(zm, gs_v4i_ones())));
				else if (sel.fwrite)
					fzm = ~gs_v4i_mask(v_ps32(gs_v4i_eq32(fm, gs_v4i_ones())));
				else if (sel.zwrite)
					fzm = ~gs_v4i_mask(v_ps32(gs_v4i_eq32(zm, gs_v4i_ones())));
			}

			/* WriteZBuf */

			if (sel.zwrite)
			{
				const int fast = sel.ztest ? sel.zpsm < 2 : sel.zpsm == 0 && sel.notest;

				if (sel.ztest && sel.zpsm < 2)
					zs = gs_v4i_blend8(zs, zd, zm);

				if (sel.notest)
				{
					if (fast)
					{
						gs_v4i_storel((u8*)global->vm + za * 2, zs);
						gs_v4i_storeh((u8*)global->vm + za * 2 + 16, zs);
					}
					else
					{
						write_pixel(zs, za, 0, sel.zpsm, global);
						write_pixel(zs, za, 1, sel.zpsm, global);
						write_pixel(zs, za, 2, sel.zpsm, global);
						write_pixel(zs, za, 3, sel.zpsm, global);
					}
				}
				else
				{
					if (fast)
					{
						if (fzm & 0x0f00) gs_v4i_storel((u8*)global->vm + za * 2, zs);
						if (fzm & 0xf000) gs_v4i_storeh((u8*)global->vm + za * 2 + 16, zs);
					}
					else
					{
						if (fzm & 0x00000300) write_pixel(zs, za, 0, sel.zpsm, global);
						if (fzm & 0x00000c00) write_pixel(zs, za, 1, sel.zpsm, global);
						if (fzm & 0x00003000) write_pixel(zs, za, 2, sel.zpsm, global);
						if (fzm & 0x0000c000) write_pixel(zs, za, 3, sel.zpsm, global);
					}
				}
			}

			/* AlphaBlend */

			if (sel.fwrite && (sel.abe || sel.aa1))
			{
				const VI rbs = rb, gas = ga;
				VI rbd = gs_v4i_zero(), gad = gs_v4i_zero(), a = gs_v4i_zero(), mask = gs_v4i_zero();

				if ((sel.aba != sel.abb && (sel.aba == 1 || sel.abb == 1 || sel.abc == 1)) || sel.abd == 1)
				{
					switch (sel.fpsm)
					{
						case 0:
						case 1:
							rbd = v_lo8(fd);
							gad = v_hi8(fd);
							break;
						case 2:
							rbd = gs_v4i_or(GS_V4I_SLL32(gs_v4i_and(fd, v_i(0x7c00)), 9), GS_V4I_SLL32(gs_v4i_and(fd, v_i(0x001f)), 3));
							gad = gs_v4i_or(GS_V4I_SLL32(gs_v4i_and(fd, v_i(0x8000)), 8), GS_V4I_SRL32(gs_v4i_and(fd, v_i(0x03e0)), 2));
							break;
					}
				}

				if (sel.aba != sel.abb)
				{
					switch (sel.aba)
					{
						case 0: break;
						case 1: rb = rbd; break;
						case 2: rb = gs_v4i_zero(); break;
					}

					switch (sel.abb)
					{
						case 0: rb = gs_v4i_sub16(rb, rbs); break;
						case 1: rb = gs_v4i_sub16(rb, rbd); break;
						case 2: break;
					}

					if (!(sel.fpsm == 1 && sel.abc == 1))
					{
						switch (sel.abc)
						{
							case 0: a = GS_V4I_SLL16(v_yywwlh(gas), 7); break;
							case 1: a = GS_V4I_SLL16(v_yywwlh(gad), 7); break;
							case 2: a = global->afix; break;
						}

						rb = v_mod16_1(rb, a);
					}

					switch (sel.abd)
					{
						case 0: rb = gs_v4i_add16(rb, rbs); break;
						case 1: rb = gs_v4i_add16(rb, rbd); break;
						case 2: break;
					}
				}
				else
				{
					switch (sel.abd)
					{
						case 0: break;
						case 1: rb = rbd; break;
						case 2: rb = gs_v4i_zero(); break;
					}
				}

				if (sel.pabe)
				{
					mask = GS_V4I_SRA32(GS_V4I_SLL32(gas, 8), 31);

					rb = gs_v4i_blend8(rbs, rb, mask);
				}

				if (sel.aba != sel.abb)
				{
					switch (sel.aba)
					{
						case 0: break;
						case 1: ga = gad; break;
						case 2: ga = gs_v4i_zero(); break;
					}

					switch (sel.abb)
					{
						case 0: ga = gs_v4i_sub16(ga, gas); break;
						case 1: ga = gs_v4i_sub16(ga, gad); break;
						case 2: break;
					}

					if (!(sel.fpsm == 1 && sel.abc == 1))
						ga = v_mod16_1(ga, a);

					switch (sel.abd)
					{
						case 0: ga = gs_v4i_add16(ga, gas); break;
						case 1: ga = gs_v4i_add16(ga, gad); break;
						case 2: break;
					}
				}
				else
				{
					switch (sel.abd)
					{
						case 0: break;
						case 1: ga = gad; break;
						case 2: ga = gs_v4i_zero(); break;
					}
				}

				if (sel.pabe)
				{
					ga = gs_v4i_blend8(gas, ga, GS_V4I_SRL32(mask, 16));
				}
				else
				{
					if (sel.fpsm != 1)
						ga = v_mix16(ga, gas);
				}
			}

			/* WriteFrame */

			if (sel.fwrite)
			{
				VI fs;
				int fast;

				if (sel.fpsm == 2 && sel.dthe)
				{
					const int y = (top & 3) << 1;

					rb = gs_v4i_add16(rb, global->dimx[0 + y]);
					ga = gs_v4i_add16(ga, global->dimx[1 + y]);
				}

				if (sel.colclamp == 0)
				{
					rb = gs_v4i_and(rb, v_i(0x00ff00ff));
					ga = gs_v4i_and(ga, v_i(0x00ff00ff));
				}

				fs = gs_v4i_pu16(gs_v4i_upl16v(rb, ga), gs_v4i_uph16v(rb, ga));

				if (sel.fba && sel.fpsm != 1)
					fs = gs_v4i_or(fs, v_i((int)0x80000000));

				if (sel.fpsm == 2)
				{
					const VI rb5 = gs_v4i_and(fs, v_i(0x00f800f8));
					const VI ga5 = gs_v4i_and(fs, v_i((int)0x8000f800));

					fs = gs_v4i_or(gs_v4i_or(GS_V4I_SRL32(ga5, 16), GS_V4I_SRL32(rb5, 9)),
						gs_v4i_or(GS_V4I_SRL32(ga5, 6), GS_V4I_SRL32(rb5, 3)));
				}

				if (sel.rfb)
					fs = v_blend(fs, fd, fm);

				fast = sel.rfb ? sel.fpsm < 2 : sel.fpsm == 0 && sel.notest;

				if (sel.notest)
				{
					if (fast)
					{
						gs_v4i_storel((u8*)global->vm + fa * 2, fs);
						gs_v4i_storeh((u8*)global->vm + fa * 2 + 16, fs);
					}
					else
					{
						write_pixel(fs, fa, 0, sel.fpsm, global);
						write_pixel(fs, fa, 1, sel.fpsm, global);
						write_pixel(fs, fa, 2, sel.fpsm, global);
						write_pixel(fs, fa, 3, sel.fpsm, global);
					}
				}
				else
				{
					if (fast)
					{
						if (fzm & 0x000f) gs_v4i_storel((u8*)global->vm + fa * 2, fs);
						if (fzm & 0x00f0) gs_v4i_storeh((u8*)global->vm + fa * 2 + 16, fs);
					}
					else
					{
						if (fzm & 0x00000003) write_pixel(fs, fa, 0, sel.fpsm, global);
						if (fzm & 0x0000000c) write_pixel(fs, fa, 1, sel.fpsm, global);
						if (fzm & 0x00000030) write_pixel(fs, fa, 2, sel.fpsm, global);
						if (fzm & 0x000000c0) write_pixel(fs, fa, 3, sel.fpsm, global);
					}
				}
			}
		} while (0);

		if (sel.edge)
			break;

		if (steps <= 0)
			break;

		/* Step */

		steps -= VLEN;

		fza_offset += VLEN / 4;

		if (sel.prim != GS_SPRITE_CLASS)
		{
			if (sel.zb && !sel.zequal)
			{
				const VI add = local->d4.z;
				z0 = gs_v4i_dadd(z0, add);
				z1 = gs_v4i_dadd(z1, add);
			}

			if (sel.fwrite && sel.fge)
				f = gs_v4i_add16(f, local->d4.f);
		}

		if (sel.fb)
		{
			if (sel.tfx != TFX_NONE)
			{
				const VI stq = LOCAL_STEP.stq;

				if (sel.fst)
				{
					s = gs_v4i_add32(s, v_x(stq));

					if (sel.prim != GS_SPRITE_CLASS || sel.mmin)
						t = gs_v4i_add32(t, v_y(stq));
				}
				else
				{
					s = gs_v4i_fadd(s, v_x(stq));
					t = gs_v4i_fadd(t, v_y(stq));
					q = gs_v4i_fadd(q, v_z(stq));
				}
			}
		}

		if (!(sel.tfx == TFX_DECAL && sel.tcc))
		{
			if (sel.iip)
			{
				const VI c = local->d4.c;
				rbf = gs_v4i_max_i16(gs_v4i_add16(rbf, v_x(c)), gs_v4i_zero());
				gaf = gs_v4i_max_i16(gs_v4i_add16(gaf, v_y(c)), gs_v4i_zero());
			}
		}

		if (!sel.notest)
			test = gs_v4i_load(const_test[7 + (steps & (steps >> 31))]);
	}
}

static void c_draw_scanline(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData* local)
{
	c_draw_scanline_sel(pixels, left, top, scan, local, global_of(local)->sel);
}

static void c_draw_edge(int pixels, int left, int top, const GSVertexSW* scan, GSScanlineLocalData* local)
{
	GSScanlineSelector sel = global_of(local)->sel;
	sel.zwrite = 0;
	sel.edge = 1;
	c_draw_scanline_sel(pixels, left, top, scan, local, sel);
}

void GS_SW(gs_draw_scanline_init)(gs_draw_scanline* ds)
{
	ds->unused = 0;
}

void GS_SW(gs_draw_scanline_free)(gs_draw_scanline* ds)
{
	(void)ds;
}

void GS_SW(gs_draw_scanline_reset_code_cache)(gs_draw_scanline* ds)
{
	(void)ds;
}

bool GS_SW(gs_draw_scanline_setup_draw)(gs_draw_scanline* ds, gs_rasterizer_data* data)
{
	(void)ds;
	data->setup_prim = c_setup_prim;
	data->draw_scanline = c_draw_scanline;
	data->draw_edge = data->global.sel.aa1 ? c_draw_edge : NULL;
	return true;
}

#endif /* GS_DS_JIT */
