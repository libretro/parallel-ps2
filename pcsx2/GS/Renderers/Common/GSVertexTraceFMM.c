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

/* The min/max finders, one per kind of draw, compiled once per
 * instruction set (sse4, avx, avx2 on x86; native elsewhere). */

#include <float.h>
#include <stddef.h>
#include <string.h>

#include "GSVertexTraceVec.h"
#include "gs_vertex.h"

#ifdef MULTI_ISA_UNSHARED_COMPILATION
#include "common/CpuFeatures.h"
#endif

#define VT_CAT3(a, b, c) a##b##c
#define VT_XCAT3(a, b, c) VT_CAT3(a, b, c)
#ifdef MULTI_ISA_UNSHARED_COMPILATION
#define VT_ISA MULTI_ISA_UNSHARED_COMPILATION
#else
#define VT_ISA isa_native
#endif

#define VT_POINT GS_POINT_CLASS
#define VT_LINE GS_LINE_CLASS
#define VT_TRIANGLE GS_TRIANGLE_CLASS
#define VT_SPRITE GS_SPRITE_CLASS

/* Two vertices into the running ranges: two of one primitive where it
 * has two (n == 2), or one each of two where it has three; @final is
 * whether they are the ones that colour a flat primitive. */
GS_VT_INLINE void gs_vt_take(const union gs_vertex* v0, const union gs_vertex* v1, int final,
	int primclass, int n, int iip, int tme, int fst, int color, int flat_swapped,
	vt_f* tmin, vt_f* tmax, vt_i* cmin, vt_i* cmax, vt_i* pmin, vt_i* pmax)
{
	const vt_i xyzf0 = vt_iload(&v0->m[1]);
	const vt_i xyzf1 = vt_iload(&v1->m[1]);
	const vt_i zf0   = vt_ywyw(xyzf0);
	const vt_i zf1   = vt_ywyw(xyzf1);
	const vt_i p0    = vt_blend_zw(vt_upl16(xyzf0), primclass == VT_SPRITE ? zf1 : zf0);
	const vt_i p1    = vt_blend_zw(vt_upl16(xyzf1), zf1);

	if (color)
	{
		u32 rgba0, rgba1;
		vt_i c0, c1;
		memcpy(&rgba0, &v0->f.RGBAQ, 4);
		memcpy(&rgba1, &v1->f.RGBAQ, 4);
		c0 = vt_iload32(rgba0);
		c1 = vt_iload32(rgba1);
		if (iip || final)
		{
			*cmin = vt_min_u8(*cmin, vt_min_u8(c0, c1));
			*cmax = vt_max_u8(*cmax, vt_max_u8(c0, c1));
		}
		else if (n == 2)
		{
			/* (two of one primitive: the provoking one's) */
			const vt_i c = flat_swapped ? c0 : c1;
			*cmin = vt_min_u8(*cmin, c);
			*cmax = vt_max_u8(*cmax, c);
		}
	}

	if (tme)
	{
		if (!fst)
		{
			vt_f stq0 = vt_fcast(vt_iload(&v0->m[0]));
			vt_f stq1 = vt_fcast(vt_iload(&v1->m[0]));
			/* (a sprite's vertices are its indices: no table lookup) */
			const vt_f q  = primclass == VT_SPRITE ? VT_SHUF(stq1, stq1, 3, 3, 3, 3) : VT_SHUF(stq0, stq1, 3, 3, 3, 3);
			const vt_f st = vt_div(VT_SHUF(stq0, stq1, 0, 1, 0, 1), q);
			/* (z is RGBA, often a denormal; it is carried, never computed) */
			stq0 = primclass == VT_SPRITE ? VT_SHUF(st, stq1, 0, 1, 3, 3) : VT_SHUF(st, stq0, 0, 1, 3, 3);
			stq1 = VT_SHUF(st, stq1, 2, 3, 3, 3);
			*tmin = vt_fmin(*tmin, vt_fmin(stq0, stq1));
			*tmax = vt_fmax(*tmax, vt_fmax(stq0, stq1));
		}
		else
		{
			const vt_f uv0 = vt_cvt(vt_uph16(xyzf0));
			const vt_f uv1 = vt_cvt(vt_uph16(xyzf1));
			const vt_f st0 = VT_SHUF(uv0, uv0, 0, 1, 0, 1);
			const vt_f st1 = VT_SHUF(uv1, uv1, 0, 1, 0, 1);
			*tmin = vt_fmin(*tmin, vt_fmin(st0, st1));
			*tmax = vt_fmax(*tmax, vt_fmax(st0, st1));
		}
	}

	*pmin = vt_min_u32(*pmin, vt_min_u32(p0, p1));
	*pmax = vt_max_u32(*pmax, vt_max_u32(p0, p1));
}

GS_VT_INLINE void gs_vt_fmm(struct gs_vertex_trace* vt, const void* vertex, const u16* index, int count,
	const struct gs_vt_regs* regs, int primclass, int iip, int tme, int fst, int color, int flat_swapped)
{
	const union gs_vertex* v = (const union gs_vertex*)vertex;
	const int n = primclass == VT_POINT ? 1 : (primclass == VT_TRIANGLE ? 3 : 2);
	const vt_f lim = vt_splat(2147483520.0f);
	vt_f tmin = vt_splat(FLT_MAX);
	vt_f tmax = vt_splat(-FLT_MAX);
	vt_i cmin = vt_iones();
	vt_i cmax = vt_izero();
	vt_i pmin = vt_iones();
	vt_i pmax = vt_izero();
	vt_f o, s;
	int i;

#define VT_TAKE(a, b, f) gs_vt_take(&v[index[a]], &v[index[b]], f, primclass, n, iip, tme, fst, color, \
	flat_swapped, &tmin, &tmax, &cmin, &cmax, &pmin, &pmax)
	if (n == 2)
	{
		for (i = 0; i < count; i += 2)
			VT_TAKE(i, i + 1, 0);
	}
	else if (iip || n == 1)
	{
		/* (every vertex counts the same) */
		for (i = 0; i < count - 1; i += 2)
			VT_TAKE(i, i + 1, 1);
		if (count & 1)
			VT_TAKE(i, i, 1);
	}
	else
	{
		/* a vertex of each of two triangles at a time */
		for (i = 0; i < count - 3; i += 6)
		{
			VT_TAKE(i + 0, i + 3, flat_swapped);
			VT_TAKE(i + 1, i + 4, 0);
			VT_TAKE(i + 2, i + 5, !flat_swapped);
		}
		if (count & 1)
		{
			if (flat_swapped)
			{
				VT_TAKE(i + 1, i + 2, 0);
				VT_TAKE(i + 0, i + 0, 1);
			}
			else
			{
				VT_TAKE(i + 0, i + 1, 0);
				VT_TAKE(i + 2, i + 2, 1);
			}
		}
	}
#undef VT_TAKE

	o = vt_set((float)(int)regs->xyoffset->OFX, (float)(int)regs->xyoffset->OFY, 0.0f, 0.0f);
	s = vt_set(1.0f / 16, 1.0f / 16, 2.0f, 1.0f);
	/* (Z as an unsigned, which the signed conversion is not) */
	vt->m_min.p = vt_insert_z(vt_mul(vt_sub(vt_cvt(pmin), o), s), vt_load_ss((float)vt_extract_z(pmin)));
	vt->m_max.p = vt_insert_z(vt_mul(vt_sub(vt_cvt(pmax), o), s), vt_load_ss((float)vt_extract_z(pmax)));

	if (tme)
	{
		if (fst)
			s = vt_set(1.0f / 16, 1.0f / 16, 1.0f, 1.0f);
		else
			s = vt_set((float)(1 << regs->tex0->TW), (float)(1 << regs->tex0->TH), 1.0f, 1.0f);
		/* Every consumer turns the range back into integers, which x86
		 * and arm64 do differently outside int32 (+inf, NaN, 1e30): NaN
		 * becomes 0, the rest saturates to the largest float below 2^31,
		 * which converts exactly on both. */
		vt->m_min.t = vt_denan_sat(vt_mul(tmin, s), vt_neg(lim), lim);
		vt->m_max.t = vt_denan_sat(vt_mul(tmax, s), vt_neg(lim), lim);
	}
	else
	{
		vt->m_min.t = vt_fzero();
		vt->m_max.t = vt_fzero();
	}

	if (color)
	{
		vt->m_min.c = vt_u8to32(cmin);
		vt->m_max.c = vt_u8to32(cmax);
	}
	else
	{
		vt->m_min.c = vt_izero();
		vt->m_max.c = vt_izero();
	}
}

/* The finders, by primitive class, IIP, TME, FST, colour and whether a
 * flat primitive's colour is its first vertex's: only those a draw can
 * select (a sprite is never Gouraud-interpolated, FST means nothing
 * untextured, and only flat lines and triangles have the choice). */
#define VT_FN(P, IIP, TME, FST, C, SW) VT_XCAT3(gs_vt_fmm_##P##_##IIP##TME##FST##C##SW##_, VT_ISA, )
#define VT_DEF(P, IIP, TME, FST, C, SW) \
	static void VT_FN(P, IIP, TME, FST, C, SW)(struct gs_vertex_trace* vt, const void* vertex, \
		const u16* index, int count, const struct gs_vt_regs* regs) \
	{ \
		gs_vt_fmm(vt, vertex, index, count, regs, VT_##P, IIP, TME, FST, C, SW); \
	}
#define VT_DEF_C(P, IIP, TME, FST, SW) VT_DEF(P, IIP, TME, FST, 0, SW) VT_DEF(P, IIP, TME, FST, 1, SW)
#define VT_DEF_T(P, IIP, SW) VT_DEF_C(P, IIP, 0, 0, SW) VT_DEF_C(P, IIP, 1, 0, SW) VT_DEF_C(P, IIP, 1, 1, SW)

VT_DEF_T(POINT, 0, 0)
VT_DEF_T(POINT, 1, 0)
VT_DEF_T(LINE, 0, 0)
VT_DEF_T(LINE, 0, 1)
VT_DEF_T(LINE, 1, 0)
VT_DEF_T(TRIANGLE, 0, 0)
VT_DEF_T(TRIANGLE, 0, 1)
VT_DEF_T(TRIANGLE, 1, 0)
VT_DEF_T(SPRITE, 0, 0)

static gs_vt_fmm_fn gs_vt_pick(int p, int iip, int tme, int fst, int color, int sw)
{
#define VT_PICK(P, IIP, TME, FST, C, SW) \
	if (p == VT_##P && iip == IIP && tme == TME && fst == FST && color == C && sw == SW) \
		return VT_FN(P, IIP, TME, FST, C, SW);
#define VT_PICK_C(P, IIP, TME, FST, SW) VT_PICK(P, IIP, TME, FST, 0, SW) VT_PICK(P, IIP, TME, FST, 1, SW)
#define VT_PICK_T(P, IIP, SW) VT_PICK_C(P, IIP, 0, 0, SW) VT_PICK_C(P, IIP, 1, 0, SW) VT_PICK_C(P, IIP, 1, 1, SW)
	VT_PICK_T(POINT, 0, 0)
	VT_PICK_T(POINT, 1, 0)
	VT_PICK_T(LINE, 0, 0)
	VT_PICK_T(LINE, 0, 1)
	VT_PICK_T(LINE, 1, 0)
	VT_PICK_T(TRIANGLE, 0, 0)
	VT_PICK_T(TRIANGLE, 0, 1)
	VT_PICK_T(TRIANGLE, 1, 0)
	VT_PICK_T(SPRITE, 0, 0)
#undef VT_PICK_T
#undef VT_PICK_C
#undef VT_PICK
	return NULL;
}

void VT_XCAT3(gs_vertex_trace_populate_, VT_ISA, )(struct gs_vertex_trace* vt, int provoking_vertex_first)
{
	int p, iip, tme, fst, color;
	for (p = 0; p < 4; p++)
		for (iip = 0; iip < 2; iip++)
			for (tme = 0; tme < 2; tme++)
				for (fst = 0; fst < 2; fst++)
					for (color = 0; color < 2; color++)
					{
						const int real_iip = p == VT_SPRITE ? 0 : iip;
						const int real_fst = tme ? fst : 0;
						const int sw       = (p == VT_LINE || p == VT_TRIANGLE) && !iip && provoking_vertex_first;
						vt->m_fmm[color][fst][tme][iip][p] = gs_vt_pick(p, real_iip, tme, real_fst, color, sw);
					}
}

/* (once: in the first tier's build, or the native one) */
#if !defined(MULTI_ISA_UNSHARED_COMPILATION) || MULTI_ISA_IS_FIRST
void gs_vertex_trace_init(struct gs_vertex_trace* vt, int provoking_vertex_first)
{
	/* Not the tail padding: C++ may keep a derived class's member there. */
	memset(vt, 0, offsetof(struct gs_vertex_trace, m_lod) + sizeof(vt->m_lod));
	vt->m_primclass = GS_INVALID_CLASS;
#ifdef MULTI_ISA_UNSHARED_COMPILATION
	if (CPU_HAS_AVX2)
		gs_vertex_trace_populate_isa_avx2(vt, provoking_vertex_first);
	else if (CPU_HAS_AVX)
		gs_vertex_trace_populate_isa_avx(vt, provoking_vertex_first);
	else
		gs_vertex_trace_populate_isa_sse4(vt, provoking_vertex_first);
#else
	gs_vertex_trace_populate_isa_native(vt, provoking_vertex_first);
#endif
}
#endif
