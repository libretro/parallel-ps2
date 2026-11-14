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

#include <string.h>

#include "GSVertexTraceVec.h"
#include "gs_vertex.h"
#include "../../GSOptionEnums.h"

/* log2 of each lane, sign ignored: the exponent plus a degree 3
 * polynomial of the mantissa (GSVector4::log2(3)). */
static vt_f gs_vt_log2_3(vt_f v)
{
	const vt_f one = vt_splat(1.0f);
	const vt_i i   = vt_icast(v);
	const vt_f e   = vt_cvt(vt_isub(vt_srl(vt_shl(i, 1), 24), vt_srl(vt_iones(), 25)));
	const vt_f m   = vt_for(vt_fcast(vt_srl(vt_shl(i, 9), 9)), one);
	vt_f p         = vt_splat(0.204446009836232697516f);
	p = vt_add(vt_mul(p, m), vt_splat(-1.04913055217340124191f));
	p = vt_add(vt_mul(p, m), vt_splat(2.28330284476918490682f));
	p = vt_mul(p, vt_sub(m, one));
	return vt_add(p, e);
}

void gs_vertex_trace_update(struct gs_vertex_trace* vt, const void* vertex, const u16* index, int v_count,
	int i_count, GS_PRIM_CLASS primclass, const struct gs_vt_regs* regs)
{
	const GIFRegPRIM* prim = regs->prim;
	u32 color;

	if (i_count == 0)
		return;

	vt->m_primclass = primclass;
	color = !(prim->TME && regs->tex0->TFX == TFX_DECAL && regs->tex0->TCC);
	vt->m_fmm[color][prim->FST][prim->TME][prim->IIP][primclass](vt, vertex, index, i_count, regs);

	/* A Q this big makes 1/Q zero: the slower division instead. */
	if (!prim->FST && !vt->m_accurate_stq)
	{
		float tz;
		memcpy(&tz, (const char*)&vt->m_min.t + 8, sizeof(tz));
		if ((double)tz > 1e30)
			vt->m_accurate_stq = true;
	}

	vt->m_eq.value = vt_imask_eq(vt->m_min.c, vt->m_max.c)
		| (vt_fmask_eq(vt->m_min.p, vt->m_max.p) << 16)
		| (vt_fmask_eq(vt->m_min.t, vt->m_max.t) << 20);

	vt->m_alpha.valid = false;

	/* (only where the draw tests depth: the cost is not known) */
	if (regs->test->ZTE == 1 && regs->test->ZTST > ZTST_ALWAYS)
		gs_vertex_trace_correct_depth(vt, vertex, v_count);

	if (prim->TME)
	{
		const GIFRegTEX1* tex1 = regs->tex1;
		u32 mmag   = tex1->MMAG;
		u32 mmin   = (tex1->MMIN == 1) || (tex1->MMIN & 4);
		u32 linear, opt;

		if (tex1->MXL == 0) /* MMIN is ignored: tested on a PS2 */
			linear = mmag;
		else
		{
			const float k = (float)tex1->K / 16;

			if (tex1->LCM == 0 && prim->FST == 0) /* FST == 1: Q is not interpolated */
			{
				/* LOD = log2(1/|Q|) * (1 << L) + K */
				vt_storel(&vt->m_lod, vt_add(vt_mul(vt_neg(gs_vt_log2_3(vt_uph(vt->m_max.t, vt->m_min.t))),
					vt_splat((float)(1 << tex1->L))), vt_splat(k)));
				if (vt->m_lod.x > vt->m_lod.y)
				{
					const float tmp = vt->m_lod.x;
					vt->m_lod.x     = vt->m_lod.y;
					vt->m_lod.y     = tmp;
				}
			}
			else
			{
				vt->m_lod.x = k;
				vt->m_lod.y = k;
			}

			if (vt->m_lod.y <= 0)
				linear = mmag;
			else if (vt->m_lod.x > 0)
				linear = mmin;
			else
				linear = mmag | mmin;
		}

		switch (regs->filtering)
		{
			case GS_BIFILTER_NEAREST:
				opt = 0;
				break;
			case GS_BIFILTER_FORCED_BUT_SPRITE:
				/* fewer glitches when upscaled */
				opt = (primclass == GS_SPRITE_CLASS) ? linear : 1;
				break;
			case GS_BIFILTER_FORCED:
				opt = 1;
				break;
			case GS_BIFILTER_PS2:
			default:
				opt = linear;
				break;
		}
		vt->m_filter.value = (vt->m_filter.value & ~0xfu) | mmag | (mmin << 1) | (linear << 2) | (opt << 3);
	}
}

void gs_vertex_trace_correct_depth(struct gs_vertex_trace* vt, const void* vertex, int count)
{
	const union gs_vertex* v = (const union gs_vertex*)vertex;
	const int step           = (vt->m_primclass == GS_SPRITE_CLASS) ? 1 : 0;
	u32 z;
	int i;

	if (!(vt->m_eq.value & GS_VT_EQ_Z))
		return;

	/* The finder drops Z's low bit; a Z constant but for it is not. Hardly
	 * ever reached (Xenosaga 3). */
	z = v[step].f.XYZ.Z;
	if (z & 1)
	{
		for (i = step; i < count; i += step + 1)
			z &= v[i].f.XYZ.Z;
	}
	else
	{
		for (i = step; i < count; i += step + 1)
			z |= v[i].f.XYZ.Z;
	}

	if (z != v[step].f.XYZ.Z)
		vt->m_eq.value &= ~GS_VT_EQ_Z;
}
