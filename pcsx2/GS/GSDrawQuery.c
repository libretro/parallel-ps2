/* SPDX-License-Identifier: LGPL-3.0+ */

#include <float.h>
#include <math.h>
#include <string.h>

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"
#include "GSDrawQuery.h"
#include "GSPsm.h"
#include "Renderers/Common/gs_vertex.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || defined(__i386__) || defined(_M_IX86)
#define GS_DQ_X86 1
#endif

/* A float to an int, truncated, as the CPU converts one: past an int it
 * is INT_MIN on x86 and saturates on ARM, never undefined */
static INLINE int gs_dq_f2i(float f)
{
#if defined(GS_DQ_X86)
	return _mm_cvtt_ss2si(_mm_set_ss(f));
#else
	return vcvts_s32_f32(f);
#endif
}

/* An int through a float and back (the C++ took the min and max of ints
 * as floats) */
static INLINE int gs_dq_minf(int a, int b)
{
	const float fa = (float)a, fb = (float)b;
	return gs_dq_f2i((fa < fb) ? fa : fb);
}

static INLINE int gs_dq_maxf(int a, int b)
{
	const float fa = (float)a, fb = (float)b;
	return gs_dq_f2i((fa > fb) ? fa : fb);
}

/* (floorf and ceilf are C99; a float is exact as a double) */
static INLINE float gs_dq_floor(float f) { return (float)floor(f); }
static INLINE float gs_dq_ceil(float f) { return (float)ceil(f); }

/* max(f, 0) as the vector max takes it: a NaN is 0 on x86 (its second
 * operand), NaN on ARM */
static INLINE float gs_dq_max0(float f)
{
#if defined(GS_DQ_X86)
	return (f > 0.0f) ? f : 0.0f;
#else
	return (f != f) ? f : ((f > 0.0f) ? f : 0.0f);
#endif
}

static INLINE void gs_dq_v4f(float out[4], const void* v)
{
	memcpy(out, v, 4 * sizeof(float));
}

/* The highest bit set, false for none */
static INLINE bool gs_dq_bsr(unsigned long* msb, u32 v)
{
	int i;
	if (!v)
		return false;
	for (i = 31; !(v & (1u << i)); i--)
	{
	}
	*msb = (unsigned long)i;
	return true;
}

/* Whether region repeat changes any of min..max, and the least and most
 * (exclusive) it makes of them */
static bool gs_dq_uses_region_repeat(int fix, int msk, int min, int max, int* min_out, int* max_out)
{
	int cleared_bits, set_bits, variable_bits, always_set, sometimes_set;
	int overwritten, min_zeros, max_ones;
	bool sets_bits, clears_bits;
	unsigned long msb;

	if ((min < 0) != (max < 0))
	{
		/* Incrementing overflows on the -1 to 0 crossing; and crossing
		 * zero uses the whole range anyway */
		*min_out = fix;
		*max_out = (fix | msk) + 1;
		return true;
	}

	cleared_bits  = ~msk & ~fix; /* always cleared by msk and fix */
	set_bits      = fix;         /* always set */
	variable_bits = min ^ max;
	if (gs_dq_bsr(&msb, (u32)variable_bits))
		variable_bits |= (1 << msb) - 1; /* and every bit below */

	always_set    = min & ~variable_bits; /* set in every value of min..max */
	sometimes_set = min | variable_bits;  /* set in at least one */

	sets_bits   = (set_bits | always_set) != always_set;
	clears_bits = (cleared_bits & sometimes_set) != 0;

	/* A variable bit 0 in min becomes 1 somewhere, clearing the bits
	 * below it; where that 1 is overwritten the value drops. The least
	 * is min with the bits below the top overwritten variable 0 cleared,
	 * the most max with those below the top overwritten 1 set. */
	overwritten = (cleared_bits | set_bits) & variable_bits;
	min_zeros   = ~min & overwritten;
	if (gs_dq_bsr(&msb, (u32)min_zeros))
		min = (int)((u32)min & (~0u << msb));
	max_ones = max & overwritten;
	if (gs_dq_bsr(&msb, (u32)max_ones))
		max |= (1 << msb) - 1;

	*min_out = (msk & min) | fix;
	*max_out = ((msk & max) | fix) + 1;

	return sets_bits || clears_bits;
}

/* The clamp modes cut to what uv reads, for one axis: lo..hi of vr from
 * the texel range a..b of uv */
static void gs_dq_cut_axis(int mode, int* lo, int* hi, int a, int b, int mask, int inclusive, int minr, int maxr,
	int size_log2, u8* uses_border, u8 border_bits)
{
	switch (mode)
	{
		case CLAMP_REPEAT:
			if ((a & ~mask) == (b & ~mask))
			{
				*lo = gs_dq_maxf(*lo, a & mask);
				*hi = gs_dq_minf(*hi, (b & mask) + inclusive);
			}
			break;
		case CLAMP_CLAMP:
		case CLAMP_REGION_CLAMP:
			if (*lo < a)
				*lo = gs_dq_minf(a, *hi - 1);
			if (*hi > (b + 1))
			{
				const float fa = (float)b, fb = (float)*lo;
				*hi = gs_dq_f2i(((fa > fb) ? fa : fb) + (float)inclusive);
			}
			break;
		case CLAMP_REGION_REPEAT:
			if (gs_dq_uses_region_repeat(maxr, minr, a, b, lo, hi) || maxr >= size_log2)
				*uses_border |= border_bits;
			break;
		default:
			break;
	}
}

/* Whether the texture coordinates of the first vertex run the other way
 * to its position, against the second (and, for triangles, the third).
 * The first-vs-second compare divides both sides by the first vertex's
 * Q and the first-vs-third gives each its own: upstream's, kept (for a
 * sprite, whose Q is flat, it compares raw S). */
static bool gs_dq_swap(const gs_draw_query* q, int axis)
{
	const union gs_vertex* v  = (const union gs_vertex*)q->vertex;
	const union gs_vertex* v0 = &v[q->index[0]];
	const union gs_vertex* v1 = &v[q->index[1]];
	const union gs_vertex* v2 = &v[q->index[2]];
	bool uv_forward, pos_forward;

	if (axis == 0)
	{
		uv_forward  = q->prim->FST ? (v0->f.U < v1->f.U) : ((v0->f.ST.S / v0->f.RGBAQ.Q) < (v1->f.ST.S / v0->f.RGBAQ.Q));
		pos_forward = v0->f.XYZ.X < v1->f.XYZ.X;
		if (q->vt->m_primclass == GS_TRIANGLE_CLASS)
		{
			uv_forward |= q->prim->FST ? (v0->f.U < v2->f.U) : ((v0->f.ST.S / v0->f.RGBAQ.Q) < (v2->f.ST.S / v2->f.RGBAQ.Q));
			pos_forward |= v0->f.XYZ.X < v2->f.XYZ.X;
		}
	}
	else
	{
		uv_forward  = q->prim->FST ? (v0->f.V < v1->f.V) : ((v0->f.ST.T / v0->f.RGBAQ.Q) < (v1->f.ST.T / v0->f.RGBAQ.Q));
		pos_forward = v0->f.XYZ.Y < v1->f.XYZ.Y;
		if (q->vt->m_primclass == GS_TRIANGLE_CLASS)
		{
			uv_forward |= q->prim->FST ? (v0->f.V < v2->f.V) : ((v0->f.ST.T / v0->f.RGBAQ.Q) < (v2->f.ST.T / v2->f.RGBAQ.Q));
			pos_forward |= v0->f.XYZ.Y < v2->f.XYZ.Y;
		}
	}
	return uv_forward != pos_forward;
}

gs_tex_minmax gs_draw_texture_min_max(const gs_draw_query* q, GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear,
	bool clamp_to_tsize)
{
	const struct gs_vertex_trace* vt = q->vt;
	const int tw      = TEX0.TW;
	const int th      = TEX0.TH;
	const int tw_mask = (1 << tw) - 1;
	const int th_mask = (1 << th) - 1;
	const int wms     = CLAMP.WMS;
	const int wmt     = CLAMP.WMT;
	const int minu    = (int)CLAMP.MINU;
	const int minv    = (int)CLAMP.MINV;
	const int maxu    = (int)CLAMP.MAXU;
	const int maxv    = (int)CLAMP.MAXV;
	float tmin[4], tmax[4], pmin[4], pmax[4];
	gs_recti tr, vr;
	u8 uses_border = 0;
	gs_tex_minmax res;

	gs_dq_v4f(tmin, &vt->m_min.t);
	gs_dq_v4f(tmax, &vt->m_max.t);
	gs_dq_v4f(pmin, &vt->m_min.p);
	gs_dq_v4f(pmax, &vt->m_max.p);

	tr.x = 0;
	tr.y = 0;
	tr.z = 1 << tw;
	tr.w = 1 << th;
	vr   = tr;

	switch (wms)
	{
		case CLAMP_REGION_CLAMP:
			vr.x = minu;
			vr.z = maxu + 1;
			break;
		case CLAMP_REGION_REPEAT:
			vr.x = maxu;
			vr.z = (maxu | minu) + 1;
			break;
		default:
			break;
	}

	switch (wmt)
	{
		case CLAMP_REGION_CLAMP:
			vr.y = minv;
			vr.w = maxv + 1;
			break;
		case CLAMP_REGION_REPEAT:
			vr.y = maxv;
			vr.w = (maxv | minv) + 1;
			break;
		default:
			break;
	}

	/* The software renderer's TEX0 holds MAXU/MAXV in TW/TH; the hardware
	 * one's does not, and its texture cache handles that, so not here */
	if (clamp_to_tsize)
		gs_recti_clamp_into(&vr, &tr);
	else
	{
		if (vr.x < tr.x) tr.x = vr.x;
		if (vr.y < tr.y) tr.y = vr.y;
		if (vr.z > tr.z) tr.z = vr.z;
		if (vr.w > tr.w) tr.w = vr.w;
	}

	if (tmax[0] >= FLT_MAX || tmin[0] <= -FLT_MAX || tmax[1] >= FLT_MAX || tmin[1] <= -FLT_MAX)
	{
		/* not to be relied on: the whole texture */
		uses_border = 0xF;
	}
	else
	{
		/* only the part of the texture read */
		float st[4];
		float range_x, range_y, grad_x, grad_y;
		int uv[4];
		int inc_x, inc_y;

		st[0] = tmin[0];
		st[1] = tmin[1];
		st[2] = tmax[0];
		st[3] = tmax[1];
		if (linear)
		{
			st[0] += -0.5f;
			st[1] += -0.5f;
			st[2] += 0.5f;
			st[3] += 0.5f;

			/* At the start of the texture, with only the half texel past
			 * it, clamped to 0: repeat would take the whole texture */
			if (!clamp_to_tsize && gs_dq_floor(tmin[0]) == 0.0f && gs_dq_floor(tmin[1]) == 0.0f)
			{
				int i;
				for (i = 0; i < 4; i++)
					st[i] = gs_dq_max0(st[i]);
			}
		}

		/* the scissored part of a sprite draw, its coordinates linear */
		range_x = pmax[0] - pmin[0];
		range_y = pmax[1] - pmin[1];
		if (!(range_x > 1.0f))
			range_x = 1.0f;
		if (!(range_y > 1.0f))
			range_y = 1.0f;
		grad_x = (tmax[0] - tmin[0]) / range_x;
		grad_y = (tmax[1] - tmin[1]) / range_y;
		if (vt->m_primclass == GS_SPRITE_CLASS && q->prim->FST == 1 && !q->gaps_found)
		{
			/* Fractional coordinates draw to the right and bottom: the
			 * ceiling, not the floor */
			gs_recti rc, sc;
			rc.x = gs_dq_f2i(gs_dq_ceil(pmin[0]));
			rc.y = gs_dq_f2i(gs_dq_ceil(pmin[1]));
			rc.z = gs_dq_f2i(gs_dq_floor(pmax[0]));
			rc.w = gs_dq_f2i(gs_dq_floor(pmax[1]));
			sc = rc;
			gs_recti_clamp_into(&sc, &q->ctx->scissor.in);
			if (rc.x != sc.x || rc.y != sc.y || rc.z != sc.z || rc.w != sc.w)
			{
				float n[4];
				bool swap;

				memcpy(n, st, sizeof(n));
				swap = gs_dq_swap(q, 0);
				if (rc.x < sc.x)
				{
					const float d = gs_dq_floor((float)(sc.x - rc.x) * grad_x);
					if (!swap)
						n[0] += d;
					else
						n[2] -= d;
				}
				if (rc.z > sc.z)
				{
					const float d = gs_dq_floor((float)(rc.z - sc.z) * grad_x);
					if (!swap)
						n[2] -= d;
					else
						n[0] += d;
				}
				/* not where it repeats over the part not clipped */
				if (wms != CLAMP_REGION_REPEAT &&
					(wms != CLAMP_REPEAT || (gs_dq_f2i(n[0]) & ~tw_mask) == (gs_dq_f2i(n[2] - 1) & ~tw_mask)))
				{
					st[0] = n[0];
					st[2] = n[2];
				}

				swap = gs_dq_swap(q, 1);
				if (rc.y < sc.y)
				{
					const float d = gs_dq_floor((float)(sc.y - rc.y) * grad_y);
					if (!swap)
						n[1] += d;
					else
						n[3] -= d;
				}
				if (rc.w > sc.w)
				{
					const float d = gs_dq_floor((float)(rc.w - sc.w) * grad_y);
					if (!swap)
						n[3] -= d;
					else
						n[1] += d;
				}
				if (wmt != CLAMP_REGION_REPEAT &&
					(wmt != CLAMP_REPEAT || (gs_dq_f2i(n[1]) & ~th_mask) == (gs_dq_f2i(n[3] - 1) & ~th_mask)))
				{
					st[1] = n[1];
					st[3] = n[3];
				}
			}
		}

		uv[0] = gs_dq_f2i(gs_dq_floor(st[0]));
		uv[1] = gs_dq_f2i(gs_dq_floor(st[1]));
		uv[2] = gs_dq_f2i(gs_dq_floor(st[2]));
		uv[3] = gs_dq_f2i(gs_dq_floor(st[3]));
		uses_border = (u8)((uv[0] < vr.x ? GS_USES_BOUNDARY_LEFT : 0) | (uv[1] < vr.y ? GS_USES_BOUNDARY_TOP : 0) |
			(uv[2] >= vr.z ? GS_USES_BOUNDARY_RIGHT : 0) | (uv[3] >= vr.w ? GS_USES_BOUNDARY_BOTTOM : 0));

		/* Not oversampled, which troubles the texture lookup: this may be
		 * off for some draws, but always one more is wrong too. Software
		 * still takes the one more. */
		inc_x = 1;
		inc_y = 1;
		if (q->hw)
		{
			inc_x = ((vt->m_primclass < GS_TRIANGLE_CLASS) ||
				(grad_x < 1.0f || (grad_x == 1.0f && pmax[0] != gs_dq_floor(pmax[0])))) ? 1 : 0;
			inc_y = ((vt->m_primclass < GS_TRIANGLE_CLASS) ||
				(grad_y < 1.0f || (grad_y == 1.0f && pmax[1] != gs_dq_floor(pmax[1])))) ? 1 : 0;
		}

		/* the read cut down to what is used */
		gs_dq_cut_axis(wms, &vr.x, &vr.z, uv[0], uv[2], tw_mask, inc_x, minu, maxu, tw, &uses_border, GS_USES_BOUNDARY_U);
		gs_dq_cut_axis(wmt, &vr.y, &vr.w, uv[1], uv[3], th_mask, inc_y, minv, maxv, th, &uses_border, GS_USES_BOUNDARY_V);
	}

	gs_recti_clamp_into(&vr, &tr);

	/* All of the coordinates outside the texture, or a clamp region of
	 * nothing: the texture cache still has to update something, as the
	 * sampler takes a texel of the border (THPS, NFSMW, Lupin 3rd) */
	if (!(vr.x < vr.z && vr.y < vr.w))
	{
		const bool ix = vr.x < tr.z;
		const bool iy = vr.y < tr.w;
		vr.x += ix ? 0 : -1;
		vr.y += iy ? 0 : -1;
		vr.z += ix ? 1 : 0;
		vr.w += iy ? 1 : 0;
		gs_recti_clamp_into(&vr, &tr);
	}
	else if (!(vr.x < vr.z))
	{
		const bool ix = vr.x < tr.z;
		vr.x += ix ? 0 : -1;
		vr.z += ix ? 1 : 0;
		gs_recti_clamp_into(&vr, &tr);
	}
	else if (!(vr.y < vr.w))
	{
		const bool iy = vr.y < tr.w;
		vr.y += iy ? 0 : -1;
		vr.w += iy ? 1 : 0;
		gs_recti_clamp_into(&vr, &tr);
	}

	res.coverage      = vr;
	res.uses_boundary = uses_border;
	return res;
}

bool gs_draw_is_coverage_alpha(const gs_draw_query* q)
{
	return !q->prim->ABE && q->prim->AA1 &&
		(q->vt->m_primclass == GS_LINE_CLASS || q->vt->m_primclass == GS_TRIANGLE_CLASS);
}

void gs_draw_calc_alpha_min_max(const gs_draw_query* q, int tex_alpha_min, int tex_alpha_max)
{
	struct gs_vertex_trace* vt = q->vt;
	int min, max;

	if (vt->m_alpha.valid && tex_alpha_min == 0 && tex_alpha_max == 255)
		return;

	/* Forced, the alpha of the texture now known; the max cut to 255, as
	 * 500 is sent for not known */
	min = tex_alpha_min;
	max = (tex_alpha_max < 255) ? tex_alpha_max : 255;

	if (gs_draw_is_coverage_alpha(q))
	{
		/* No AA in the hardware renderer: its min is 128 (were it to
		 * come, with ROV or FB fetch, this changes) */
		min = q->hw ? 128 : 0;
		max = 128;
	}
	else
	{
		const gs_drawing_regs* ctx = q->ctx;
		int cmin[4], cmax[4];
		int ax, ay, az, aw;

		memcpy(cmin, &vt->m_min.c, sizeof(cmin));
		memcpy(cmax, &vt->m_max.c, sizeof(cmax));
		ax = ay = cmin[3];
		az = aw = cmax[3];
		if (q->prim->TME && ctx->TEX0.TCC)
		{
			const GIFRegTEXA* texa = q->texa;
			const bool known       = tex_alpha_max < GS_INVALID_ALPHA_MINMAX;

			switch (gs_psm[ctx->TEX0.PSM].fmt)
			{
				case 0:
					ay = min;
					aw = max;
					break;
				case 1:
					/* AEM and TA0 are in the texture's alpha already
					 * (ReadAndExpandBlock24) */
					ay = known ? min : (texa->AEM ? 0 : (int)texa->TA0);
					aw = known ? max : (int)texa->TA0;
					break;
				case 2:
					/* and TA1 too (ReadAndExpandBlock16) */
					ay = known ? min : (texa->AEM ? 0 : (int)((texa->TA0 < texa->TA1) ? texa->TA0 : texa->TA1));
					aw = known ? max : (int)((texa->TA0 > texa->TA1) ? texa->TA0 : texa->TA1);
					break;
				case 3:
					if (known)
					{
						ay = min;
						aw = max;
					}
					else
						gs_clut_alpha_minmax(q->clut, &ay, &aw);
					break;
				default:
					break;
			}

			switch (ctx->TEX0.TFX)
			{
				case TFX_MODULATE:
					ax = (ax * ay) >> 7;
					az = (az * aw) >> 7;
					if (ax > 0xff)
						ax = 0xff;
					if (az > 0xff)
						az = 0xff;
					break;
				case TFX_DECAL:
				case TFX_HIGHLIGHT2:
					ax = ay;
					az = aw;
					break;
				case TFX_HIGHLIGHT:
					ax = ax + ay;
					az = az + aw;
					if (ax > 0xff)
						ax = 0xff;
					if (az > 0xff)
						az = 0xff;
					break;
				default:
					break;
			}
		}
		min = ax;
		max = az;
	}

	vt->m_alpha.min   = min;
	vt->m_alpha.max   = max;
	vt->m_alpha.valid = true;
}

const struct gs_vt_alpha* gs_draw_alpha_min_max(const gs_draw_query* q)
{
	if (!q->vt->m_alpha.valid)
		gs_draw_calc_alpha_min_max(q, 0, GS_INVALID_ALPHA_MINMAX);
	return &q->vt->m_alpha;
}

static INLINE int gs_dq_min(int a, int b) { return (a < b) ? a : b; }
static INLINE int gs_dq_max(int a, int b) { return (a > b) ? a : b; }

void gs_draw_correct_ate_alpha_min_max(const gs_draw_query* q, u32 atst, int aref)
{
	const struct gs_vt_alpha* a = gs_draw_alpha_min_max(q);
	int amin = a->min;
	int amax = a->max;

	switch (atst)
	{
		case ATST_LESS:
			amin = gs_dq_min(amin, gs_dq_max(aref - 1, amin));
			amax = gs_dq_min(amax, gs_dq_max(aref - 1, amin));
			break;
		case ATST_LEQUAL:
			amin = gs_dq_min(amin, gs_dq_max(aref, amin));
			amax = gs_dq_min(amax, gs_dq_max(aref, amin));
			break;
		case ATST_EQUAL:
			amax = aref;
			amin = aref;
			break;
		case ATST_GEQUAL:
			amax = gs_dq_max(amax, gs_dq_min(aref, amax));
			amin = gs_dq_max(amin, gs_dq_min(aref, amax));
			break;
		case ATST_GREATER:
			amax = gs_dq_max(amax, gs_dq_min(aref + 1, amax));
			amin = gs_dq_max(amin, gs_dq_min(aref + 1, amax));
			break;
		default:
			break;
	}

	q->vt->m_alpha.min = amin;
	q->vt->m_alpha.max = amax;
}

bool gs_draw_try_alpha_test(const gs_draw_query* q, u32* fm, u32* zm)
{
	const gs_drawing_regs* ctx = q->ctx;
	u32 framemask, framemaskalpha, fail_type;
	bool pass = true;

	if (ctx->TEST.ATST == ATST_ALWAYS)
		return true;

	framemask      = gs_psm[ctx->FRAME.PSM].fmsk;
	framemaskalpha = framemask & 0xFF000000;
	/* (FB only for RGB only where the frame buffer is not 32-bit) */
	fail_type = (ctx->TEST.AFAIL == AFAIL_RGB_ONLY && (ctx->FRAME.PSM & 0xF) != 0) ? (u32)AFAIL_FB_ONLY : (u32)ctx->TEST.AFAIL;

	/* The test only decides the write of some channels: with those masked
	 * already it does nothing */
	switch (fail_type)
	{
		case AFAIL_FB_ONLY:
			if (*zm == 0xFFFFFFFF)
				return true;
			break;
		case AFAIL_ZB_ONLY:
			if ((*fm & framemask) == framemask)
				return true;
			break;
		case AFAIL_RGB_ONLY:
			if (*zm == 0xFFFFFFFF && (*fm & framemaskalpha) == framemaskalpha)
				return true;
			break;
		default:
			break;
	}

	if (ctx->TEST.ATST == ATST_NEVER)
		pass = false;
	else
	{
		const struct gs_vt_alpha* a = gs_draw_alpha_min_max(q);
		const int amin = a->min;
		const int amax = a->max;
		const int aref = ctx->TEST.AREF;

		switch (ctx->TEST.ATST)
		{
			case ATST_LESS:
				if (amax < aref)
					pass = true;
				else if (amin >= aref)
					pass = false;
				else
					return false;
				break;
			case ATST_LEQUAL:
				if (amax <= aref)
					pass = true;
				else if (amin > aref)
					pass = false;
				else
					return false;
				break;
			case ATST_EQUAL:
				if (amin == aref && amax == aref)
					pass = true;
				else if (amin > aref || amax < aref)
					pass = false;
				else
					return false;
				break;
			case ATST_GEQUAL:
				if (amin >= aref)
					pass = true;
				else if (amax < aref)
					pass = false;
				else
					return false;
				break;
			case ATST_GREATER:
				if (amin > aref)
					pass = true;
				else if (amax <= aref)
					pass = false;
				else
					return false;
				break;
			case ATST_NOTEQUAL:
				if (amin == aref && amax == aref)
					pass = false;
				else if (amin > aref || amax < aref)
					pass = true;
				else
					return false;
				break;
			default:
				break;
		}
	}

	if (!pass)
	{
		switch (fail_type)
		{
			case AFAIL_KEEP:
				*fm = *zm = 0xffffffff;
				break;
			case AFAIL_FB_ONLY:
				*zm = 0xffffffff;
				break;
			case AFAIL_ZB_ONLY:
				*fm = 0xffffffff;
				break;
			case AFAIL_RGB_ONLY:
				*fm |= 0xff000000;
				*zm = 0xffffffff;
				break;
			default:
				break;
		}
	}

	return true;
}

bool gs_draw_is_opaque(const gs_draw_query* q)
{
	const gs_drawing_regs* ctx = q->ctx;
	int amin = 0;
	int amax = 0xff;

	if (q->prim->AA1)
		return false;
	if (!q->prim->ABE)
		return true;

	if (ctx->ALPHA.A != ctx->ALPHA.B)
	{
		if (ctx->ALPHA.C == 0)
		{
			const struct gs_vt_alpha* a = gs_draw_alpha_min_max(q);
			amin = a->min;
			amax = a->max;
		}
		else if (ctx->ALPHA.C == 1)
		{
			if (ctx->FRAME.PSM == PSMCT24 || ctx->FRAME.PSM == PSMZ24)
				amin = amax = 0x80;
		}
		else if (ctx->ALPHA.C == 2)
			amin = amax = ctx->ALPHA.FIX;
	}

	/* (GIFRegALPHA::IsOpaque) */
	return ((ctx->ALPHA.A == ctx->ALPHA.B || amax == 0) && ctx->ALPHA.D == 0) ||
		(ctx->ALPHA.A == 0 && ctx->ALPHA.B == ctx->ALPHA.D && amin == 0x80 && amax == 0x80);
}

bool gs_draw_is_mipmap_draw(const gs_draw_query* q)
{
	return q->ctx->TEX1.MXL > 0 && q->ctx->TEX1.MMIN >= 2 && q->ctx->TEX1.MMIN <= 5 && q->vt->m_lod.y > 0;
}

GIFRegTEX0 gs_draw_tex0_layer(const gs_draw_query* q, u32 lod)
{
	const gs_drawing_regs* ctx = q->ctx;
	GIFRegTEX0 TEX0            = ctx->TEX0;

	if (lod == 0)
		return TEX0;

	switch (lod)
	{
		case 1:
			TEX0.TBP0 = ctx->MIPTBP1.TBP1;
			TEX0.TBW  = ctx->MIPTBP1.TBW1;
			break;
		case 2:
			TEX0.TBP0 = ctx->MIPTBP1.TBP2;
			TEX0.TBW  = ctx->MIPTBP1.TBW2;
			break;
		case 3:
			TEX0.TBP0 = ctx->MIPTBP1.TBP3;
			TEX0.TBW  = ctx->MIPTBP1.TBW3;
			break;
		case 4:
			TEX0.TBP0 = ctx->MIPTBP2.TBP4;
			TEX0.TBW  = ctx->MIPTBP2.TBW4;
			break;
		case 5:
			TEX0.TBP0 = ctx->MIPTBP2.TBP5;
			TEX0.TBW  = ctx->MIPTBP2.TBW5;
			break;
		case 6:
			TEX0.TBP0 = ctx->MIPTBP2.TBP6;
			TEX0.TBW  = ctx->MIPTBP2.TBW6;
			break;
		default:
			log_cb(RETRO_LOG_ERROR, "GS: Invalid guest lod setting. Please report: https://github.com/PCSX2/pcsx2/issues\n");
			break;
	}

	/* the size of the level */
	if (TEX0.TH <= lod)
		TEX0.TH = 0;
	else
		TEX0.TH -= lod;

	if (TEX0.TW <= lod)
		TEX0.TW = 0;
	else
		TEX0.TW -= lod;

	return TEX0;
}
