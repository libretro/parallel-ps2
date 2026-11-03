// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <float.h> /* FLT_MIN, for the zero-Q substitution */

#include "GS/GSVector.h"
#include "GS/GSRegs.h"

/* Packed-vertex parse kernels, lifted verbatim out of GSState's
 * GIFPackedRegHandler bodies so they can be pinned by tests and optimized
 * without touching the state machine around them.
 *
 * The bodies are the original vector sequences, unchanged: same blend8
 * FLT_MIN substitution for a zero Q, same upl16/srl<4> subtexel unpack, same
 * masks. Nothing here reads or writes GSState, so a caller keeps full control
 * of when m_v is stored.
 *
 * Shape note: written in the C89 form the emitters in this tree use --
 * declarations at the head of each block, no mixed declarations, no early
 * returns in the middle of a body, block comments -- so MSVC's C frontend and
 * its C++ one produce the same straight-line shape, and so the sequences read
 * the same as the emitter code they mirror.
 */

namespace GSVertexKernels
{
	/* The two halves of a parsed vertex, returned by value: the out-parameter
	 * form cost the register allocator an extra shuffle in a couple of the
	 * fused instantiations. */
	/* The native 128-bit lane type the vertex struct stores (m_v.m). Writing
	 * through it keeps the stores exactly where the inline code had them. */
#if _M_SSE >= 0x200 || defined(_M_X86)
	typedef __m128i GSVertexNative;
#else
	typedef int32x4_t GSVertexNative;
#endif

	/* STQ + RGBA + XYZF2, the fused three-register packed vertex.
	 *
	 * uv    : the current m_v.UV word, folded into the XY quad as-is.
	 * out_m0: ST in the low half, RGBA and Q in the high half.
	 * out_m1: XY (subtexel-expanded) with UV, then Z (24-bit) and F (8-bit).
	 */
	__forceinline void ParsePackedSTQRGBAXYZF2(const GIFPackedReg* RESTRICT r, u32 uv,
		GSVertexNative* RESTRICT out)
	{
		GSVector4i st;
		GSVector4i q;
		GSVector4i rgba;
		GSVector4i xy;
		GSVector4i zf;

		st = GSVector4i::loadl(&r[0].U64[0]);
		q = GSVector4i::loadl(&r[0].U64[1]);
		rgba = (GSVector4i::load<false>(&r[1]) & GSVector4i::x000000ff()).ps32().pu16();

		/* see GIFPackedRegHandlerSTQ: a zero Q is substituted with FLT_MIN */
		q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());

		out[0] = (GSVertexNative)st.upl64(rgba.upl32(q));

		xy = GSVector4i::loadl(&r[2].U64[0]);
		zf = GSVector4i::loadl(&r[2].U64[1]);
		xy = xy.upl16(xy.srl<4>()).upl32(GSVector4i::load((int)uv));
		zf = zf.srl32<4>() & GSVector4i::x00ffffff().upl32(GSVector4i::x000000ff());

		out[1] = (GSVertexNative)xy.upl32(zf);
	}

	/* STQ + RGBA + XYZ2. Same as above without the fog field: Z keeps its full
	 * 32 bits and UV rides in the high half of m1.
	 */
	__forceinline void ParsePackedSTQRGBAXYZ2(const GIFPackedReg* RESTRICT r, const void* uv_ptr,
		GSVertexNative* RESTRICT out)
	{
		GSVector4i st;
		GSVector4i q;
		GSVector4i rgba;
		GSVector4i xy;
		GSVector4i z;
		GSVector4i xyz;

		st = GSVector4i::loadl(&r[0].U64[0]);
		q = GSVector4i::loadl(&r[0].U64[1]);
		rgba = (GSVector4i::load<false>(&r[1]) & GSVector4i::x000000ff()).ps32().pu16();

		q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());

		out[0] = (GSVertexNative)st.upl64(rgba.upl32(q));

		xy = GSVector4i::loadl(&r[2].U64[0]);
		z = GSVector4i::loadl(&r[2].U64[1]);
		xyz = xy.upl16(xy.srl<4>()).upl32(z);

		out[1] = (GSVertexNative)xyz.upl64(GSVector4i::loadl(uv_ptr));
	}

	/* The single-register XYZF2/XYZ2 handlers' unpack, same sequences. */
	__forceinline GSVector4i ParseXYZF2(const GIFPackedReg* RESTRICT r, u32 uv)
	{
		GSVector4i xy;
		GSVector4i zf;

		xy = GSVector4i::loadl(&r->U64[0]);
		zf = GSVector4i::loadl(&r->U64[1]);
		xy = xy.upl16(xy.srl<4>()).upl32(GSVector4i::load((int)uv));
		zf = zf.srl32<4>() & GSVector4i::x00ffffff().upl32(GSVector4i::x000000ff());

		return xy.upl32(zf);
	}

	__forceinline GSVector4i ParseXYZ2(const GIFPackedReg* RESTRICT r, const void* uv_ptr)
	{
		GSVector4i xy;
		GSVector4i z;
		GSVector4i xyz;

		xy = GSVector4i::loadl(&r->U64[0]);
		z = GSVector4i::loadl(&r->U64[1]);
		xyz = xy.upl16(xy.srl<4>()).upl32(z);

		return xyz.upl64(GSVector4i::loadl(uv_ptr));
	}

	/* Per-primitive accept/cull decision, lifted out of VertexKick.
	 *
	 * Returns non-zero when the primitive is to be skipped. The three tests are
	 * the original ones, unchanged: the bbox of the primitive lying wholly
	 * outside the cull rect, a degenerate extent that covers no pixel (native
	 * resolution only -- upscaled, a primitive spanning no pixel at 1x may
	 * span several), and, for triangles, two coincident vertices.
	 *
	 * Templated on prim so the per-primitive selection folds away exactly as it
	 * did when this was written inline.
	 */
	/* Bounding box of the primitive's vertices. Kept separate from the cull
	 * test because the caller also feeds it to the draw rect. */
	template <u32 prim>
	__forceinline void BuildPrimBBox(const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2,
		GSVector4i& pmin, GSVector4i& pmax)
	{
		switch (prim)
		{
			case GS_POINTLIST:
				pmin = v0;
				pmax = v0;
				break;
			case GS_LINELIST:
			case GS_LINESTRIP:
			case GS_SPRITE:
				pmin = v0.min_i32(v1);
				pmax = v0.max_i32(v1);
				break;
			case GS_TRIANGLELIST:
			case GS_TRIANGLESTRIP:
			case GS_TRIANGLEFAN:
				pmin = v0.min_i32(v1.min_i32(v2));
				pmax = v0.max_i32(v1.max_i32(v2));
				break;
			default:
				break;
		}
	}

	template <u32 prim>
	__forceinline u32 CullSkipTest(const GSVector4i& pmin, const GSVector4i& pmax,
		const GSVector4i& v0, const GSVector4i& v1, const GSVector4i& v2,
		const GSVector4i& cull_min, const GSVector4i& cull_max, bool nativeres)
	{
		GSVector4i test;

		test = pmax.lt32(cull_min) | pmin.gt32(cull_max);

		switch (prim)
		{
			case GS_TRIANGLELIST:
			case GS_TRIANGLESTRIP:
			case GS_TRIANGLEFAN:
			case GS_SPRITE:
			{
				/* Discard primitives that do not cover a pixel. Only valid at
				 * native resolution, where the integer positions are the pixel
				 * grid. */
				const GSVector4i degen_test = pmin.eq32(pmax);
				test |= nativeres ? degen_test.zwzw() : degen_test;
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
				test = (test | v0.eq64(v1)) | (v1.eq64(v2) | v0.eq64(v2));
				break;
			default:
				break;
		}

#if defined(_M_ARM64) || defined(__aarch64__)
		/* mask() is slow on ARM, so pull the bits out instead; only the first
		 * four bytes matter. */
		return (u32)(((u64)test.extract64<0>() & UINT64_C(0x8080808080808080)) != 0);
#else
		/* Only xy has to pass the skip test; zw holds the offset coordinates
		 * used for native culling. */
		return (u32)(test.mask() & 0xff);
#endif
	}
} // namespace GSVertexKernels
