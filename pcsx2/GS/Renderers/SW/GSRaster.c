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

/* Every value is what GSVector4 made of it: the same IEEE operations in
 * the same order, and min, max, rounding and conversions to int by the
 * instructions it used, NaN and signed zeros included. Built without
 * fused multiply-adds, as the rest of the renderer is. */

#include <math.h>
#include <string.h>

#include <memalign.h>

#include "GSRaster.h"
#include "../../GSRegs.h"
#include "../../gs_cvt.h"

#ifdef GS_CVT_X86
typedef __m128 rs_v;
#define rs_vload(p) _mm_load_ps(p)
#define rs_vstore(p, v) _mm_store_ps(p, v)
#define rs_vsplat(f) _mm_set1_ps(f)
#define rs_vadd(a, b) _mm_add_ps(a, b)
#define rs_vsub(a, b) _mm_sub_ps(a, b)
#define rs_vmul(a, b) _mm_mul_ps(a, b)
#define rs_vdiv(a, b) _mm_div_ps(a, b)
#define RS_ROUND(f, mode) _mm_cvtss_f32(_mm_round_ss(_mm_setzero_ps(), _mm_set_ss(f), (mode) | _MM_FROUND_NO_EXC))
static INLINE float rs_ceil(float f) { return RS_ROUND(f, _MM_FROUND_TO_POS_INF); }
static INLINE float rs_trunc(float f) { return RS_ROUND(f, _MM_FROUND_TO_ZERO); }
static INLINE float rs_rint(float f) { return RS_ROUND(f, _MM_FROUND_TO_NEAREST_INT); }
static INLINE double rs_rint64(double d)
{
	return _mm_cvtsd_f64(_mm_round_sd(_mm_setzero_pd(), _mm_set_sd(d), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
}
/* minps and maxps: the second operand unless the first is less (more) */
static INLINE float rs_min(float a, float b) { return _mm_cvtss_f32(_mm_min_ss(_mm_set_ss(a), _mm_set_ss(b))); }
static INLINE float rs_max(float a, float b) { return _mm_cvtss_f32(_mm_max_ss(_mm_set_ss(a), _mm_set_ss(b))); }
#else
typedef float32x4_t rs_v;
#define rs_vload(p) vld1q_f32(p)
#define rs_vstore(p, v) vst1q_f32(p, v)
#define rs_vsplat(f) vdupq_n_f32(f)
#define rs_vadd(a, b) vaddq_f32(a, b)
#define rs_vsub(a, b) vsubq_f32(a, b)
#define rs_vmul(a, b) vmulq_f32(a, b)
#define rs_vdiv(a, b) vdivq_f32(a, b)
static INLINE float rs_ceil(float f) { return vgetq_lane_f32(vrndpq_f32(vdupq_n_f32(f)), 0); }
static INLINE float rs_trunc(float f) { return vgetq_lane_f32(vrndq_f32(vdupq_n_f32(f)), 0); }
static INLINE float rs_rint(float f) { return vgetq_lane_f32(vrndnq_f32(vdupq_n_f32(f)), 0); }
static INLINE double rs_rint64(double d) { return vget_lane_f64(vrndn_f64(vdup_n_f64(d)), 0); }
static INLINE float rs_min(float a, float b) { return vgetq_lane_f32(vminq_f32(vdupq_n_f32(a), vdupq_n_f32(b)), 0); }
static INLINE float rs_max(float a, float b) { return vgetq_lane_f32(vmaxq_f32(vdupq_n_f32(a), vdupq_n_f32(b)), 0); }
#endif

static INLINE u32 rs_bits(float f)
{
	u32 u;
	memcpy(&u, &f, 4);
	return u;
}

static INLINE float rs_float(u32 u)
{
	float f;
	memcpy(&f, &u, 4);
	return f;
}

static INLINE float rs_abs(float f) { return rs_float(rs_bits(f) & 0x7fffffffu); }

/* (vertex arithmetic: x and y, the double, and the eight floats of t and c) */

static INLINE void rs_copy(GSVertexSW* d, const GSVertexSW* s)
{
	d->p = s->p;
	d->t = s->t;
	d->c = s->c;
}

static INLINE void rs_add(GSVertexSW* d, const GSVertexSW* a, const GSVertexSW* b)
{
	d->p.F32[0] = a->p.F32[0] + b->p.F32[0];
	d->p.F32[1] = a->p.F32[1] + b->p.F32[1];
	d->p.F64[1] = a->p.F64[1] + b->p.F64[1];
	rs_vstore(d->t.F32, rs_vadd(rs_vload(a->t.F32), rs_vload(b->t.F32)));
	rs_vstore(d->c.F32, rs_vadd(rs_vload(a->c.F32), rs_vload(b->c.F32)));
}

static INLINE void rs_sub(GSVertexSW* d, const GSVertexSW* a, const GSVertexSW* b)
{
	d->p.F32[0] = a->p.F32[0] - b->p.F32[0];
	d->p.F32[1] = a->p.F32[1] - b->p.F32[1];
	d->p.F64[1] = a->p.F64[1] - b->p.F64[1];
	rs_vstore(d->t.F32, rs_vsub(rs_vload(a->t.F32), rs_vload(b->t.F32)));
	rs_vstore(d->c.F32, rs_vsub(rs_vload(a->c.F32), rs_vload(b->c.F32)));
}

static INLINE void rs_mul(GSVertexSW* d, const GSVertexSW* a, float s)
{
	const rs_v v = rs_vsplat(s);
	d->p.F32[0] = a->p.F32[0] * s;
	d->p.F32[1] = a->p.F32[1] * s;
	d->p.F64[1] = a->p.F64[1] * s;
	rs_vstore(d->t.F32, rs_vmul(rs_vload(a->t.F32), v));
	rs_vstore(d->c.F32, rs_vmul(rs_vload(a->c.F32), v));
}

static INLINE void rs_div(GSVertexSW* d, const GSVertexSW* a, float s)
{
	const rs_v v = rs_vsplat(s);
	d->p.F32[0] = a->p.F32[0] / s;
	d->p.F32[1] = a->p.F32[1] / s;
	d->p.F64[1] = a->p.F64[1] / s;
	rs_vstore(d->t.F32, rs_vdiv(rs_vload(a->t.F32), v));
	rs_vstore(d->c.F32, rs_vdiv(rs_vload(a->c.F32), v));
}

/* a + b * s */
static INLINE void rs_madd(GSVertexSW* d, const GSVertexSW* a, const GSVertexSW* b, float s)
{
	GSVertexSW m;
	rs_mul(&m, b, s);
	rs_add(d, a, &m);
}

static const GSVertexSW rs_zero = {{{0.0f, 0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}},
	{{0.0f, 0.0f, 0.0f, 0.0f}}, {{0.0f, 0.0f, 0.0f, 0.0f}}};

static const u16 rs_index012[3] = {0, 1, 2};

/* (u32 arithmetic where it may pass an int's range: what the vector
 * code's was) */
#define RS_ISUB(a, b) ((int)((u32)(a) - (u32)(b)))
#define RS_IADD(a, b) ((int)((u32)(a) + (u32)(b)))

static INLINE int rs_mine(const gs_raster* r, int top)
{
	return r->scanline[top >> r->thread_height] != 0;
}

static INLINE int rs_next_mine(const gs_raster* r, int top)
{
	int i = top >> r->thread_height;
	if (r->scanline[i] == 0)
	{
		while (r->scanline[++i] == 0)
			;
		top = i << r->thread_height;
	}
	return top;
}

static INLINE void rs_info(GSVertexSW* e, int pixels, int left, int top)
{
	e->_pad.I32[0] = pixels;
	e->_pad.I32[1] = left;
	e->_pad.I32[2] = top;
}

static INLINE void rs_add_scanline(GSVertexSW* e, int pixels, int left, int top, const GSVertexSW* scan)
{
	rs_copy(e, scan);
	rs_info(e, pixels, left, top);
}

/* SCANMSK skips native lines; the 2x drawing's line is half its row. */
static INLINE int rs_masked(const gs_raster* r, int top)
{
	return (r->scanmsk_value & 2)
		&& (r->scanmsk_value & 1) == ((top >> ((r->sel & GS_RASTER_SEL_HIRES) ? 1 : 0)) & 1);
}

static INLINE void rs_scanline(gs_raster* r, int pixels, int left, int top, const GSVertexSW* scan)
{
	if (!rs_masked(r, top))
		r->draw_scanline(pixels, left, top, scan, r->local);
}

/* The GS steps texture coordinates and Z along a line of pixels as a
 * DDA does: the n-th pixel takes the start plus n steps, exactly,
 * however many pixels the scanline code handles at once. U and V step
 * in 16.16 by their truncated step. S, T, Q and Z are floating point:
 * the step here, and each scanline's start where the scanline code
 * takes it, snap to a power-of-two grid of the primitive's own, fine
 * enough that every value along its lines is a multiple of it the
 * format holds, so every sum is exact. count is the number of vertices
 * the values lie between, 0 for lines of one pixel only. */
static void rs_setup(gs_raster* r, const GSVertexSW* vertex, const u16* index, const GSVertexSW* dscan, int count)
{
	GSVertexSW d;
	int i, k;

	if (count == 0)
	{
		r->setup_prim(vertex, index, dscan, r->local);
		return;
	}

	d = *dscan;

	if (r->sel & GS_RASTER_SEL_TEX_GRID)
	{
		if (r->sel & GS_RASTER_SEL_FST)
		{
			/* Whole 16.16 units, to 21 significant bits so that eight
			 * steps are exact too. */
			d.t.U32[0] = rs_bits(rs_trunc(d.t.F32[0])) & ~7u;
			d.t.U32[1] = rs_bits(rs_trunc(d.t.F32[1])) & ~7u;
		}
		else
		{
			/* The vertices' magnitudes and eight steps add up past any
			 * value a lane reaches; the grid holds that below 2^23 steps. */
			float m[4];
			u32 grid[4], rcp[4];

			for (k = 0; k < 4; k++)
				m[k] = rs_abs(d.t.F32[k]) * 8.0f;
			for (i = 0; i < count; i++)
				for (k = 0; k < 4; k++)
					m[k] = m[k] + rs_abs(vertex[index[i]].t.F32[k]);
			for (k = 0; k < 4; k++)
			{
				u32 e = rs_bits(m[k]) & 0x7f800000u;
				if (e < (24u << 23))
					e = 24u << 23;
				grid[k] = e - (22u << 23);
				rcp[k]  = (254u << 23) - grid[k];
			}
			memcpy(r->tgrid, grid, sizeof(grid));
			memcpy(r->tgrid_rcp, rcp, sizeof(rcp));
			for (k = 0; k < 3; k++)
				d.t.F32[k] = rs_rint(d.t.F32[k] * rs_float(rcp[k])) * rs_float(grid[k]);
		}
	}

	if (r->sel & GS_RASTER_SEL_Z_GRID)
	{
		/* The same with doubles: below 2^52 steps. */
		double m = 8.0 * fabs(d.p.F64[1]);
		double grid, rcp;
		u64 e;

		for (i = 0; i < count; i++)
			m += fabs(vertex[index[i]].p.F64[1]);

		memcpy(&e, &m, sizeof(e));
		e = (e >> 52) & 0x7ff;
		if (e < 53)
			e = 53;
		e = (e - 51) << 52;
		memcpy(&grid, &e, sizeof(e));
		e = ((u64)2046 << 52) - e;
		memcpy(&rcp, &e, sizeof(e));
		r->zgrid[0] = grid;
		r->zgrid[1] = rcp;
		d.p.F64[1] = rs_rint64(d.p.F64[1] * rcp) * grid;
	}

	r->setup_prim(vertex, index, &d, r->local);
}

static void rs_flush(gs_raster* r, const GSVertexSW* vertex, const u16* index, const GSVertexSW* dscan, int edge)
{
	const int count = r->edge_count;
	const GSVertexSW* e;
	const GSVertexSW* ee;

	if (count <= 0)
		return;

	rs_setup(r, vertex, index, dscan, edge ? 0 : (r->sel & GS_RASTER_SEL_TRIANGLE) ? 3 : 2);

	e  = r->edge;
	ee = e + count;
	do
	{
		if (!rs_masked(r, e->_pad.I32[2]))
			(edge ? r->draw_edge : r->draw_scanline)(e->_pad.I32[0], e->_pad.I32[1], e->_pad.I32[2], e, r->local);
	} while (++e < ee);

	r->edge_count = 0;
}

/* A point at 2x covers its native pixel's 2x2. */
static void rs_point2x(gs_raster* r, const GSVertexSW* vertex, const u16* index)
{
	const GSVertexSW* v = &vertex[*index];
	const int x = (int)((u32)gs_f2i(v->p.F32[0] * 0.5f + 0.5f) << 1);
	const int y = (int)((u32)gs_f2i(v->p.F32[1] * 0.5f + 0.5f) << 1);
	int i;

	if (x < r->scissor.x || x >= r->scissor.z || y < r->scissor.y || y >= r->scissor.w)
		return;

	for (i = y; i < y + 2; i++)
	{
		if (rs_mine(r, i))
		{
			r->setup_prim(vertex, index, &rs_zero, r->local);
			rs_scanline(r, 2, x, i, v);
		}
	}
}

static void rs_point(gs_raster* r, const GSVertexSW* vertex, int vertex_count, const u16* index, int index_count,
	int scissor_test)
{
	static const u16 tmp_index[1] = {0};
	int i;

	if (r->sel & GS_RASTER_SEL_HIRES)
	{
		if (index)
		{
			for (i = 0; i < index_count; i++)
				rs_point2x(r, vertex, index + i);
		}
		else
		{
			for (i = 0; i < vertex_count; i++)
				rs_point2x(r, vertex + i, tmp_index);
		}
		return;
	}

	for (i = 0; i < (index ? index_count : vertex_count); i++)
	{
		const GSVertexSW* base = index ? vertex : vertex + i;
		const u16* idx         = index ? index + i : tmp_index;
		const GSVertexSW* v    = &base[*idx];
		const int x            = gs_f2i(v->p.F32[0] + 0.5f);
		const int y            = gs_f2i(v->p.F32[1] + 0.5f);

		if (!scissor_test || (r->scissor.x <= x && x < r->scissor.z && r->scissor.y <= y && y < r->scissor.w))
		{
			if (rs_mine(r, y))
			{
				r->setup_prim(base, idx, &rs_zero, r->local);
				rs_scanline(r, 1, x, y, v);
			}
		}
	}
}

/* An antialiased edge: a pixel a row (orientation) or a column, its
 * coverage in p.x. side: the top or left edge. */
static void rs_edge(gs_raster* r, const GSVertexSW* v0, const GSVertexSW* v1, const GSVertexSW* dv, int orientation,
	int side)
{
	GSVertexSW* e = r->edge + r->edge_count;
	GSVertexSW edge, dedge;
	const float* fs = orientation ? r->fscissor_y : r->fscissor_x;
	/* (y while stepping rows, x while stepping columns) */
	const int a = orientation ? 1 : 0;
	const float c0 = rs_ceil(v0->p.F32[a]);
	const float c1 = rs_ceil(v1->p.F32[a]);
	int from, to, f, df;

	if (dv->p.F32[a] >= 0.0f)
	{
		from = gs_f2i(rs_max(c0, fs[0]));
		to   = gs_f2i(rs_min(c1, fs[3]));
		if (from >= to)
			return;
		rs_copy(&edge, v0);
		rs_div(&dedge, dv, dv->p.F32[a]);
		rs_madd(&edge, &edge, &dedge, rs_max(c0, fs[0]) - edge.p.F32[a]);
	}
	else
	{
		from = gs_f2i(rs_max(c1, fs[2]));
		to   = gs_f2i(rs_min(c0, fs[1]));
		if (from >= to)
			return;
		rs_copy(&edge, v1);
		rs_div(&dedge, dv, dv->p.F32[a]);
		rs_madd(&edge, &edge, &dedge, rs_max(c1, fs[2]) - edge.p.F32[a]);
	}

	/* the other coordinate in 16.16 */
	f  = gs_f2i(edge.p.F32[1 - a] * 65536.0f);
	df = gs_f2i(dedge.p.F32[1 - a] * 65536.0f);

	for (;;)
	{
		const int fi  = (f >> 16) + (side ? 0 : 1);
		const int ff  = f & 0xffff;
		const int x   = orientation ? fi : from;
		const int y   = orientation ? from : fi;
		const int lo  = orientation ? r->scissor.x : r->scissor.y;
		const int hi  = orientation ? r->scissor.z : r->scissor.w;

		if (lo <= fi && fi < hi && rs_mine(r, y))
		{
			rs_add_scanline(e, 1, x, y, &edge);
			e->p.U32[0] = side ? ((0x10000 - ff) & 0xffff) : (u32)ff;
			e++;
		}

		if (++from >= to)
			break;

		rs_add(&edge, &edge, &dedge);
		f = RS_IADD(f, df);
	}

	r->edge_count += (int)(e - (r->edge + r->edge_count));
}

static void rs_line_impl(gs_raster* r, const GSVertexSW* vertex, const u16* index)
{
	const GSVertexSW* v0 = &vertex[index[0]];
	const GSVertexSW* v1 = &vertex[index[1]];
	GSVertexSW dv;
	float dp[2];
	int i, dpi_x, dpi_y, steps;

	rs_sub(&dv, v1, v0);
	dp[0] = rs_abs(dv.p.F32[0]);
	dp[1] = rs_abs(dv.p.F32[1]);
	i     = dp[0] < dp[1]; /* |dx| <= |dy| */

	if (r->draw_edge)
	{
		/* Each edge is flushed on its own: one is at most a pixel per
		 * scissor row or column, which is what the buffer holds. */
		rs_edge(r, v0, v1, &dv, i, 0);
		rs_flush(r, vertex, index, &rs_zero, 1);
		rs_edge(r, v0, v1, &dv, i, 1);
		rs_flush(r, vertex, index, &rs_zero, 1);
		return;
	}

	dpi_x = gs_f2i(dp[0]);
	dpi_y = gs_f2i(dp[1]);

	if (dpi_y == 0)
	{
		/* a horizontal line: one scanline */
		if (dpi_x > 0)
		{
			const int swap         = v0->p.F32[0] > v1->p.F32[0];
			const GSVertexSW* from = swap ? v1 : v0;
			const GSVertexSW* to   = swap ? v0 : v1;
			const int y            = gs_f2i(from->p.F32[1]);

			if (r->scissor.y <= y && y < r->scissor.w && rs_mine(r, y))
			{
				const float l     = rs_max(rs_ceil(from->p.F32[0]), r->fscissor_x[0]);
				const int left    = gs_f2i(l);
				const int right   = gs_f2i(rs_min(rs_ceil(to->p.F32[0]), r->fscissor_x[1]));
				const int pixels  = RS_ISUB(right, left);

				if (pixels > 0)
				{
					GSVertexSW scan, dscan;

					rs_copy(&scan, from);
					rs_div(&dscan, &dv, dv.p.F32[0]);
					rs_madd(&scan, &scan, &dscan, l - scan.p.F32[0]);
					rs_setup(r, vertex, index, &dscan, 2);
					rs_scanline(r, pixels, left, y, &scan);
				}
			}
		}
		return;
	}

	steps = i ? dpi_y : dpi_x;

	if (steps > 0)
	{
		GSVertexSW edge, dedge;
		GSVertexSW* e = r->edge;

		rs_copy(&edge, v0);
		rs_div(&dedge, &dv, dp[i]);

		for (;;)
		{
			const int x = gs_f2i(edge.p.F32[0]);
			const int y = gs_f2i(edge.p.F32[1]);

			if (r->scissor.x <= x && x < r->scissor.z && r->scissor.y <= y && y < r->scissor.w && rs_mine(r, y))
				rs_add_scanline(e++, 1, x, y, &edge);

			if (--steps == 0)
				break;

			rs_add(&edge, &edge, &dedge);
		}

		r->edge_count = (int)(e - r->edge);
		rs_flush(r, vertex, index, &rs_zero, 0);
	}
}

/* A line at 2x is two rows (or columns) thick, as its native pixels'
 * 2x2s are: the line and itself a row (column) further across. */
static void rs_line(gs_raster* r, const GSVertexSW* vertex, const u16* index)
{
	if ((r->sel & GS_RASTER_SEL_HIRES) && !r->draw_edge)
	{
		GSVertexSW v[2];
		const float dx  = rs_abs(vertex[index[1]].p.F32[0] - vertex[index[0]].p.F32[0]);
		const float dy  = rs_abs(vertex[index[1]].p.F32[1] - vertex[index[0]].p.F32[1]);
		const float ox  = (dx < dy) ? 1.0f : 0.0f;
		const float oy  = (dx < dy) ? 0.0f : 1.0f;
		int k;

		rs_copy(&v[0], &vertex[index[0]]);
		rs_copy(&v[1], &vertex[index[1]]);
		rs_line_impl(r, v, rs_index012);
		for (k = 0; k < 2; k++)
		{
			v[k].p.F32[0] = v[k].p.F32[0] + ox;
			v[k].p.F32[1] = v[k].p.F32[1] + oy;
		}
		rs_line_impl(r, v, rs_index012);
		return;
	}

	rs_line_impl(r, vertex, index);
}

/* The scanlines of a triangle's rows top to bottom, between the edge
 * at x and the one at y of edge.p, which dedge.p steps a row */
static void rs_section(gs_raster* r, int top, int bottom, const GSVertexSW* edge, const GSVertexSW* dedge,
	const GSVertexSW* dscan, const gs_raster_lane* p0)
{
	GSVertexSW* e = r->edge + r->edge_count;
	const rs_v et = rs_vload(edge->t.F32), ec = rs_vload(edge->c.F32);
	const rs_v dt = rs_vload(dedge->t.F32), dc = rs_vload(dedge->c.F32);
	const rs_v st = rs_vload(dscan->t.F32), sc = rs_vload(dscan->c.F32);

	top = rs_next_mine(r, top);

	while (top < bottom)
	{
		const float dy   = (float)top - p0->F32[1];
		const float l    = rs_max(rs_ceil(edge->p.F32[0] + dedge->p.F32[0] * dy), r->fscissor_x[0]);
		const int left   = gs_f2i(l);
		const int right  = gs_f2i(rs_min(rs_ceil(edge->p.F32[1] + dedge->p.F32[1] * dy), r->fscissor_x[1]));
		const int pixels = RS_ISUB(right, left);

		if (pixels > 0)
		{
			const float prestep = l - p0->F32[0];
			const rs_v vdy      = rs_vsplat(dy);
			const rs_v vpre     = rs_vsplat(prestep);

			e->p.F64[1] = edge->p.F64[1] + dedge->p.F64[1] * dy + dscan->p.F64[1] * prestep;
			rs_vstore(e->t.F32, rs_vadd(rs_vadd(et, rs_vmul(dt, vdy)), rs_vmul(st, vpre)));
			rs_vstore(e->c.F32, rs_vadd(rs_vadd(ec, rs_vmul(dc, vdy)), rs_vmul(sc, vpre)));
			rs_info(e++, pixels, left, top);
		}

		top++;

		if (!rs_mine(r, top))
			top += (r->threads - 1) << r->thread_height;
	}

	r->edge_count += (int)(e - (r->edge + r->edge_count));
}

static const u8 rs_ysort[8][4] = {
	{0, 1, 2, 0}, /* y0 <= y1 <= y2 */
	{1, 0, 2, 0}, /* y1 < y0 <= y2 */
	{0, 0, 0, 0},
	{1, 2, 0, 0}, /* y1 <= y2 < y0 */
	{0, 2, 1, 0}, /* y0 <= y2 < y1 */
	{0, 0, 0, 0},
	{2, 0, 1, 0}, /* y2 < y0 <= y1 */
	{2, 1, 0, 0}, /* y2 < y1 < y0 */
};

static void rs_triangle(gs_raster* r, const GSVertexSW* vertex, const u16* index)
{
	const GSVertexSW *v0, *v1, *v2;
	GSVertexSW dv0, dv1, dv2, dscan, dedge, edge, a, b;
	float y0, y1, y2, cross, dx[4], dy[4], q[4], ddx[3][4], dxy01c[4];
	int m1, m2, k, i[3], tb[4];

	y0 = vertex[index[0]].p.F32[1];
	y1 = vertex[index[1]].p.F32[1];
	y2 = vertex[index[2]].p.F32[1];
	m1 = (y0 > y1) | ((y0 > y2) << 1) | ((y1 > y2) << 2);

	i[0] = index[rs_ysort[m1][0]];
	i[1] = index[rs_ysort[m1][1]];
	i[2] = index[rs_ysort[m1][2]];

	v0 = &vertex[i[0]];
	v1 = &vertex[i[1]];
	v2 = &vertex[i[2]];

	y0 = v0->p.F32[1];
	y1 = v1->p.F32[1];
	y2 = v2->p.F32[1];
	/* 0: y0 < y1 < y2, 1: y0 == y1 < y2, 4: y0 < y1 == y2 */
	m1 = (y0 == y1) | ((y0 == y2) << 1) | ((y1 == y2) << 2);

	if (m1 == 7)
		return;

	/* max(y0, t) max(y1, t) min(y1, b) min(y2, b) */
	tb[0] = gs_f2i(rs_max(rs_ceil(y0), r->fscissor_y[0]));
	tb[1] = gs_f2i(rs_max(rs_ceil(y1), r->fscissor_y[2]));
	tb[2] = gs_f2i(rs_min(rs_ceil(y1), r->fscissor_y[1]));
	tb[3] = gs_f2i(rs_min(rs_ceil(y2), r->fscissor_y[3]));

	rs_sub(&dv0, v1, v0);
	rs_sub(&dv1, v2, v0);
	rs_sub(&dv2, v2, v1);

	/* the negated cross product */
	cross = dv0.p.F32[1] * dv1.p.F32[0] - dv0.p.F32[0] * dv1.p.F32[1];

	if (cross == 0.0f)
		return;

	m2 = (int)(rs_bits(cross) >> 31);

	dx[0] = dv0.p.F32[0];
	dx[1] = dv1.p.F32[0];
	dx[2] = dv2.p.F32[0];
	dx[3] = dv2.p.F32[1];
	dy[0] = dv0.p.F32[1];
	dy[1] = dv1.p.F32[1];
	dy[2] = dv2.p.F32[1];
	dy[3] = dv2.p.F32[0];

	for (k = 0; k < 4; k++)
		q[k] = dx[k] / dy[k];
	ddx[0][0] = q[0]; ddx[0][1] = q[1]; ddx[0][2] = q[2]; ddx[0][3] = q[3];
	ddx[1][0] = q[1]; ddx[1][1] = q[0]; ddx[1][2] = q[2]; ddx[1][3] = q[3];
	ddx[2][0] = q[0]; ddx[2][1] = q[2]; ddx[2][2] = q[1]; ddx[2][3] = q[3];

	/* Precision is important here. Don't use reciprocal, it will break Jak3/Xenosaga1 */
	dxy01c[0] = dv0.p.F32[0] / cross;
	dxy01c[1] = dv0.p.F32[1] / cross;
	dxy01c[2] = dv1.p.F32[0] / cross;
	dxy01c[3] = dv1.p.F32[1] / cross;

	rs_mul(&a, &dv1, dxy01c[1]);
	rs_mul(&b, &dv0, dxy01c[3]);
	rs_sub(&dscan, &a, &b);
	rs_mul(&a, &dv0, dxy01c[2]);
	rs_mul(&b, &dv1, dxy01c[0]);
	rs_sub(&dedge, &a, &b);

	if (m1 & 1)
	{
		if (tb[1] < tb[3])
		{
			const float* d = ddx[m2 ? 0 : 2];

			rs_copy(&edge, &vertex[i[1 - m2]]);
			edge.p.F32[1]  = vertex[i[m2]].p.F32[0];
			dedge.p.F32[0] = d[1];
			dedge.p.F32[1] = d[2];
			rs_section(r, tb[0], tb[3], &edge, &dedge, &dscan, &vertex[i[1 - m2]].p);
		}
	}
	else
	{
		if (tb[0] < tb[2])
		{
			rs_copy(&edge, v0);
			edge.p.F32[1]  = edge.p.F32[0];
			dedge.p.F32[0] = ddx[m2][0];
			dedge.p.F32[1] = ddx[m2][1];
			rs_section(r, tb[0], tb[2], &edge, &dedge, &dscan, &v0->p);
		}

		if (tb[1] < tb[3])
		{
			const float* d = ddx[m2 ? 0 : 2];

			rs_copy(&edge, v1);
			edge.p.F32[0]  = v0->p.F32[0] + ddx[m2][0] * dv0.p.F32[1];
			edge.p.F32[1]  = v0->p.F32[0] + ddx[m2][1] * dv0.p.F32[1];
			dedge.p.F32[0] = d[1];
			dedge.p.F32[1] = d[2];
			rs_section(r, tb[1], tb[3], &edge, &dedge, &dscan, &v1->p);
		}
	}

	rs_flush(r, vertex, index, &dscan, 0);

	if (r->draw_edge)
	{
		/* |dx| <= |dy|; and that, or dx < 0, against the longest's x < 0 */
		const int c = cross < 0.0f;
		int orientation = 0, side;

		for (k = 0, side = 0; k < 4; k++)
		{
			const int ak = rs_abs(dx[k]) < rs_abs(dy[k]);
			orientation |= ak << k;
			side |= (((ak | (dx[k] < 0.0f)) ^ c) & 1) << k;
		}
		side ^= 2;

		/* Each edge is flushed on its own: see rs_line_impl. */
		rs_edge(r, v0, v1, &dv0, orientation & 1, side & 1);
		rs_flush(r, vertex, index, &rs_zero, 1);
		rs_edge(r, v0, v2, &dv1, orientation & 2, side & 2);
		rs_flush(r, vertex, index, &rs_zero, 1);
		rs_edge(r, v1, v2, &dv2, orientation & 4, side & 4);
		rs_flush(r, vertex, index, &rs_zero, 1);
	}
}

static void rs_sprite(gs_raster* r, const GSVertexSW* vertex, const u16* index)
{
	const GSVertexSW* s0 = &vertex[index[0]];
	const GSVertexSW* s1 = &vertex[index[1]];
	const int mx = s0->p.F32[0] < s1->p.F32[0];
	const int my = s0->p.F32[1] < s1->p.F32[1];
	GSVertexSW scan, dscan;
	gs_recti rc;
	float lo[4], hi[4], dt[2];

	/* the top left corner, and the bottom right's x and y, s and t */
	scan.p        = s1->p;
	scan.t        = s1->t;
	scan.c        = s1->c;
	scan.p.U32[0] = mx ? s0->p.U32[0] : s1->p.U32[0];
	scan.p.U32[1] = my ? s0->p.U32[1] : s1->p.U32[1];
	scan.t.U32[0] = mx ? s0->t.U32[0] : s1->t.U32[0];
	scan.t.U32[1] = my ? s0->t.U32[1] : s1->t.U32[1];
	lo[0]         = scan.p.F32[0];
	lo[1]         = scan.p.F32[1];
	lo[2]         = scan.t.F32[0];
	lo[3]         = scan.t.F32[1];
	hi[0]         = mx ? s1->p.F32[0] : s0->p.F32[0];
	hi[1]         = my ? s1->p.F32[1] : s0->p.F32[1];
	hi[2]         = mx ? s1->t.F32[0] : s0->t.F32[0];
	hi[3]         = my ? s1->t.F32[1] : s0->t.F32[1];

	rc.x = gs_f2i(rs_ceil(lo[0]));
	rc.y = gs_f2i(rs_ceil(lo[1]));
	rc.z = gs_f2i(rs_ceil(hi[0]));
	rc.w = gs_f2i(rs_ceil(hi[1]));
	gs_recti_clamp_into(&rc, &r->scissor);

	if (!(rc.x < rc.z && rc.y < rc.w))
		return;

	/* The rect fill addresses local memory's own swizzle. */
	if ((r->scanmsk_value & 2) == 0 && (r->sel & GS_RASTER_SEL_SOLID_RECT) && !(r->sel & GS_RASTER_SEL_HIRES))
	{
		if (r->threads == 1)
			r->draw_rect(&rc, &scan, r->local);
		else
		{
			const int band = 1 << r->thread_height;
			int top        = rs_next_mine(r, rc.y);
			const int bottom = rc.w;

			while (top < bottom)
			{
				const int next = (top + band) & ~(band - 1);
				rc.y = top;
				rc.w = (next < bottom) ? next : bottom;
				r->draw_rect(&rc, &scan, r->local);
				top = rc.w + ((r->threads - 1) << r->thread_height);
			}
		}
		return;
	}

	dt[0] = (hi[2] - lo[2]) / (hi[0] - lo[0]);
	dt[1] = (hi[3] - lo[3]) / (hi[1] - lo[1]);

	dscan.p = rs_zero.p;
	dscan.c = rs_zero.c;
	dscan.t = rs_zero.t;
	dscan.t.F32[0] = dt[0];

	scan.t.F32[0] = scan.t.F32[0] + dt[0] * ((float)rc.x - scan.p.F32[0]);
	scan.t.F32[1] = scan.t.F32[1] + dt[1] * ((float)rc.y - scan.p.F32[1]);

	rs_setup(r, vertex, index, &dscan, 2);

	for (;;)
	{
		if (rs_mine(r, rc.y))
			rs_scanline(r, rc.z - rc.x, rc.x, rc.y, &scan);

		if (++rc.y >= rc.w)
			break;

		/* (+0 into the other lanes, as the vector add did) */
		scan.t.F32[0] = scan.t.F32[0] + 0.0f;
		scan.t.F32[1] = scan.t.F32[1] + dt[1];
		scan.t.F32[2] = scan.t.F32[2] + 0.0f;
		scan.t.F32[3] = scan.t.F32[3] + 0.0f;
	}
}

bool gs_raster_init(gs_raster* r, int id, int threads, int thread_height)
{
	/* 4096 lines and columns: the 2x drawing's. */
	const int rows = (4096 >> thread_height) + 16;
	int i;

	memset(r, 0, sizeof(*r));
	r->edge         = (GSVertexSW*)memalign_alloc(32, sizeof(GSVertexSW) * 4096);
	r->scanline_own = (u8*)memalign_alloc(64, 2 * rows);
	if (!r->edge || !r->scanline_own)
	{
		gs_raster_free(r);
		return false;
	}
	r->scanline_all  = r->scanline_own + rows;
	r->scanline      = r->scanline_own;
	r->threads       = threads;
	r->threads_own   = threads;
	r->thread_height = thread_height;

	for (i = 0; i < rows; i++)
	{
		r->scanline_own[i] = (i % threads) == id ? 1 : 0;
		r->scanline_all[i] = 1;
	}
	return true;
}

void gs_raster_free(gs_raster* r)
{
	memalign_free(r->scanline_own);
	memalign_free(r->edge);
	r->scanline_own = NULL;
	r->edge         = NULL;
}

void gs_raster_draw_bands(gs_raster* r, const gs_raster_draw* d)
{
	const GSVertexSW* vertex = d->vertex;
	const u16* index         = d->index;
	gs_recti bbox            = d->bbox;
	int scissor_test;

	if ((vertex && d->vertex_count == 0) || (index && d->index_count == 0))
		return;

	r->sel           = d->sel;
	r->scanmsk_value = d->scanmsk_value;
	r->setup_prim    = d->setup_prim;
	r->draw_scanline = d->draw_scanline;
	r->draw_edge     = d->draw_edge;
	r->scissor       = d->scissor;
	r->fscissor_x[0] = r->fscissor_x[2] = (float)d->scissor.x;
	r->fscissor_x[1] = r->fscissor_x[3] = (float)d->scissor.z;
	r->fscissor_y[0] = r->fscissor_y[2] = (float)d->scissor.y;
	r->fscissor_y[1] = r->fscissor_y[3] = (float)d->scissor.w;

	gs_recti_clamp_into(&bbox, &d->scissor);
	scissor_test = bbox.x != d->bbox.x || bbox.y != d->bbox.y || bbox.z != d->bbox.z || bbox.w != d->bbox.w;

	switch (d->primclass)
	{
		case GS_POINT_CLASS:
			rs_point(r, vertex, d->vertex_count, index, d->index_count, scissor_test);
			break;

		case GS_LINE_CLASS:
			if (index)
			{
				const u16* end = index + d->index_count;
				do
				{
					rs_line(r, vertex, index);
					index += 2;
				} while (index < end);
			}
			else
			{
				const GSVertexSW* end = vertex + d->vertex_count;
				do
				{
					rs_line(r, vertex, rs_index012);
					vertex += 2;
				} while (vertex < end);
			}
			break;

		case GS_TRIANGLE_CLASS:
			if (index)
			{
				const u16* end = index + d->index_count;
				do
				{
					rs_triangle(r, vertex, index);
					index += 3;
				} while (index < end);
			}
			else
			{
				const GSVertexSW* end = vertex + d->vertex_count;
				do
				{
					rs_triangle(r, vertex, rs_index012);
					vertex += 3;
				} while (vertex < end);
			}
			break;

		case GS_SPRITE_CLASS:
			if (index)
			{
				const u16* end = index + d->index_count;
				do
				{
					rs_sprite(r, vertex, index);
					index += 2;
				} while (index < end);
			}
			else
			{
				const GSVertexSW* end = vertex + d->vertex_count;
				do
				{
					rs_sprite(r, vertex, rs_index012);
					vertex += 2;
				} while (vertex < end);
			}
			break;

		default:
			break;
	}
}

void gs_raster_draw_whole(gs_raster* r, const gs_raster_draw* d)
{
	r->scanline = r->scanline_all;
	r->threads  = 1;
	gs_raster_draw_bands(r, d);
	r->scanline = r->scanline_own;
	r->threads  = r->threads_own;
}
