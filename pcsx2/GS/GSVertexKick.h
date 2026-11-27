/* SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+ */

#ifndef GS_VERTEX_KICK_H
#define GS_VERTEX_KICK_H

#include <float.h> /* FLT_MIN, for the zero-Q substitution */
#include "GS/gs_vector.h"
#include "GS/GSRegs.h"

/* The packed-vertex parse kernels and the per-primitive cull of the GIF
 * path (GSState.c), as C over gs_vector so tests can pin them without the
 * state machine around them. Nothing here reads or writes the state, so a
 * caller keeps full control of when m_v is stored.
 *
 * The prim arguments are constants at every call: the functions are always
 * inlined, so the per-primitive selection folds away. */

/* STQ + RGBA + XYZF2, the fused three-register packed vertex.
 *
 * uv : the current m_v.UV word, folded into the XY quad as-is.
 * out[0]: ST in the low half, RGBA and Q in the high half.
 * out[1]: XY (subtexel-expanded) with UV, then Z (24-bit) and F (8-bit). */
static GS_VEC_INLINE void gs_vk_parse_stqrgbaxyzf2(const GIFPackedReg* RESTRICT r, u32 uv,
	gs_vec4i* RESTRICT out)
{
	gs_vec4i st, q, rgba, xy, zf;

	st   = gs_v4i_loadl(&r[0].U64[0]);
	q    = gs_v4i_loadl(&r[0].U64[1]);
	rgba = gs_v4i_and(gs_v4i_loadu(&r[1]), gs_v4i_set32(0xff));
	rgba = gs_v4i_ps32(rgba, rgba);
	rgba = gs_v4i_pu16(rgba, rgba);
	/* a zero Q is substituted with FLT_MIN (GIFPackedRegHandlerSTQ) */
	q = gs_v4i_blend8(q, gs_v4i_set32(GS_V4I_F32_MIN_POS), gs_v4i_eq32(q, gs_v4i_zero()));
	out[0] = gs_v4i_upl64v(st, gs_v4i_upl32v(rgba, q));

	xy = gs_v4i_loadl(&r[2].U64[0]);
	zf = gs_v4i_loadl(&r[2].U64[1]);
	xy = gs_v4i_upl32v(gs_v4i_upl16v(xy, GS_V4I_SRL(xy, 4)), gs_v4i_load32((int32_t)uv));
	zf = gs_v4i_and(GS_V4I_SRL32(zf, 4), gs_v4i_set4(0x00ffffff, 0xff, 0x00ffffff, 0xff));
	out[1] = gs_v4i_upl32v(xy, zf);
}

/* STQ + RGBA + XYZ2. As above without the fog field: Z keeps its full 32
 * bits and UV rides in the high half of out[1]. */
static GS_VEC_INLINE void gs_vk_parse_stqrgbaxyz2(const GIFPackedReg* RESTRICT r, const void* uv_ptr,
	gs_vec4i* RESTRICT out)
{
	gs_vec4i st, q, rgba, xy, z;

	st   = gs_v4i_loadl(&r[0].U64[0]);
	q    = gs_v4i_loadl(&r[0].U64[1]);
	rgba = gs_v4i_and(gs_v4i_loadu(&r[1]), gs_v4i_set32(0xff));
	rgba = gs_v4i_ps32(rgba, rgba);
	rgba = gs_v4i_pu16(rgba, rgba);
	q = gs_v4i_blend8(q, gs_v4i_set32(GS_V4I_F32_MIN_POS), gs_v4i_eq32(q, gs_v4i_zero()));
	out[0] = gs_v4i_upl64v(st, gs_v4i_upl32v(rgba, q));

	xy = gs_v4i_loadl(&r[2].U64[0]);
	z  = gs_v4i_loadl(&r[2].U64[1]);
	out[1] = gs_v4i_upl64v(gs_v4i_upl32v(gs_v4i_upl16v(xy, GS_V4I_SRL(xy, 4)), z),
		gs_v4i_loadl(uv_ptr));
}

/* The single-register XYZF2 and XYZ2 handlers' unpack. */
static GS_VEC_INLINE gs_vec4i gs_vk_parse_xyzf2(const GIFPackedReg* RESTRICT r, u32 uv)
{
	gs_vec4i xy = gs_v4i_loadl(&r->U64[0]);
	gs_vec4i zf = gs_v4i_loadl(&r->U64[1]);

	xy = gs_v4i_upl32v(gs_v4i_upl16v(xy, GS_V4I_SRL(xy, 4)), gs_v4i_load32((int32_t)uv));
	zf = gs_v4i_and(GS_V4I_SRL32(zf, 4), gs_v4i_set4(0x00ffffff, 0xff, 0x00ffffff, 0xff));
	return gs_v4i_upl32v(xy, zf);
}

static GS_VEC_INLINE gs_vec4i gs_vk_parse_xyz2(const GIFPackedReg* RESTRICT r, const void* uv_ptr)
{
	gs_vec4i xy = gs_v4i_loadl(&r->U64[0]);
	gs_vec4i z  = gs_v4i_loadl(&r->U64[1]);

	return gs_v4i_upl64v(gs_v4i_upl32v(gs_v4i_upl16v(xy, GS_V4I_SRL(xy, 4)), z), gs_v4i_loadl(uv_ptr));
}

/* Bounding box of the primitive's vertices. Kept apart from the cull test
 * because the caller also feeds it to the draw rect. */
static GS_VEC_INLINE void gs_vk_prim_bbox(u32 prim, gs_vec4i v0, gs_vec4i v1, gs_vec4i v2,
	gs_vec4i* pmin, gs_vec4i* pmax)
{
	switch (prim)
	{
		case GS_POINTLIST:
			*pmin = v0;
			*pmax = v0;
			break;
		case GS_LINELIST:
		case GS_LINESTRIP:
		case GS_SPRITE:
			*pmin = gs_v4i_min_i32(v0, v1);
			*pmax = gs_v4i_max_i32(v0, v1);
			break;
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			*pmin = gs_v4i_min_i32(v0, gs_v4i_min_i32(v1, v2));
			*pmax = gs_v4i_max_i32(v0, gs_v4i_max_i32(v1, v2));
			break;
		default:
			break;
	}
}

/* Non-zero when the primitive is to be skipped: its bbox wholly outside
 * the cull rect, a degenerate extent covering no pixel (native resolution
 * only -- upscaled, a primitive spanning no pixel at 1x may span several),
 * or, for triangles, two coincident vertices. */
static GS_VEC_INLINE u32 gs_vk_cull_skip(u32 prim, gs_vec4i pmin, gs_vec4i pmax,
	gs_vec4i v0, gs_vec4i v1, gs_vec4i v2, gs_vec4i cull_min, gs_vec4i cull_max, int nativeres)
{
	gs_vec4i test = gs_v4i_or(gs_v4i_lt32(pmax, cull_min), gs_v4i_gt32(pmin, cull_max));

	switch (prim)
	{
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
		case GS_SPRITE:
		{
			/* At native resolution the integer positions are the pixel grid */
			const gs_vec4i degen = gs_v4i_eq32(pmin, pmax);
			const gs_vec4i degen_zw = (gs_vec4i)GS_V4I_SHUFFLE32(degen, 0xee);
			test = gs_v4i_or(test, nativeres ? degen_zw : degen);
		}
		break;
		default:
			break;
	}
	switch (prim)
	{
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			test = gs_v4i_or(gs_v4i_or(test, gs_v4i_eq64(v0, v1)),
				gs_v4i_or(gs_v4i_eq64(v1, v2), gs_v4i_eq64(v0, v2)));
			break;
		default:
			break;
	}
#if defined(GS_VEC_NEON)
	/* A byte mask is slow on ARM: only the first eight bytes' top bits
	 * count, so test them in place */
	return (u32)((vgetq_lane_u64(vreinterpretq_u64_s32(test), 0) & UINT64_C(0x8080808080808080)) != 0);
#else
	/* Only xy has to pass; zw holds the offset coordinates the native cull
	 * reads */
	return (u32)(gs_v4i_mask(test) & 0xff);
#endif
}

#endif
