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

#ifndef GS_DRAWING_CONTEXT_H
#define GS_DRAWING_CONTEXT_H

#include "GSRegs.h"
#include "gs_rect.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The registers of a drawing context and its scissor, as C sees the
 * start of GSDrawingContext */
typedef struct gs_drawing_regs
{
	GIFRegXYOFFSET XYOFFSET;
	GIFRegTEX0     TEX0;
	GIFRegTEX1     TEX1;
	GIFRegCLAMP    CLAMP;
	GIFRegMIPTBP1  MIPTBP1;
	GIFRegMIPTBP2  MIPTBP2;
	GIFRegSCISSOR  SCISSOR;
	GIFRegALPHA    ALPHA;
	GIFRegTEST     TEST;
	GIFRegFBA      FBA;
	GIFRegFRAME    FRAME;
	GIFRegZBUF     ZBUF;
	struct
	{
		gs_recti in;
		gs_recti cull;
		gs_recti xyof;
	} scissor;
} gs_drawing_regs;

/* The scissor as a rectangle with its far edges exclusive, the same in
 * 12.4 fixed point for culling, and the offset culling subtracts from a
 * vertex (zw with the subpixels rounded down) */
void gs_ctx_scissor(const GIFRegSCISSOR* scissor, const GIFRegXYOFFSET* xyoffset,
	int in[4], int cull[4], int xyof[4]);

/* TEX0 with TW and TH cut to what the texture coordinates st (the
 * vertex trace's min uv in xy, max in zw) can reach, and widened where a
 * region mode can reach past them; sides past 1024 texels are one texel.
 * With mipmapping it is left as it is but for that. */
GIFRegTEX0 gs_ctx_size_fixed_tex0(const GIFRegTEX0* tex0, const GIFRegCLAMP* clamp,
	const float st[4], int linear, int mipmap);

#ifdef __cplusplus
}
#endif

#include "GSLocalMemory.h"
#include "gs_vector.h"

/* A context's registers, its scissor as vectors and the offsets into local
 * memory of its frame and Z buffer. The C++ renderers read it through
 * GSDrawingContext, which only adds methods. */
typedef struct PCSX2_ALIGN(32) gs_drawing_context
{
	GIFRegXYOFFSET XYOFFSET;
	GIFRegTEX0     TEX0;
	GIFRegTEX1     TEX1;
	GIFRegCLAMP    CLAMP;
	GIFRegMIPTBP1  MIPTBP1;
	GIFRegMIPTBP2  MIPTBP2;
	GIFRegSCISSOR  SCISSOR;
	GIFRegALPHA    ALPHA;
	GIFRegTEST     TEST;
	GIFRegFBA      FBA;
	GIFRegFRAME    FRAME;
	GIFRegZBUF     ZBUF;

	struct
	{
		PCSX2_LANG_TYPE(GSVector4i, gs_vec4i) in;
		PCSX2_LANG_TYPE(GSVector4i, gs_vec4i) cull;
		PCSX2_LANG_TYPE(GSVector4i, gs_vec4i) xyof;
	} scissor;

	struct
	{
		PCSX2_LANG_TYPE(GSOffset, gs_offset) fb;
		PCSX2_LANG_TYPE(GSOffset, gs_offset) zb;
		GSPixelOffset4* fzb4; /* NULL: the pool was full (gs_state_refetch_pixel_offsets) */
		u32 fzbase[2];        /* the frame and Z buffer's bases, to add to fzb4's */
	} offset;
} gs_drawing_context;

/* The registers back to zero; the scissor and offsets are left */
static INLINE void gs_ctx_reset(gs_drawing_context* c)
{
	memset(c, 0, offsetof(gs_drawing_context, scissor));
}

static INLINE void gs_ctx_update_scissor(gs_drawing_context* c)
{
	gs_ctx_scissor(&c->SCISSOR, &c->XYOFFSET, (int*)&c->scissor.in, (int*)&c->scissor.cull, (int*)&c->scissor.xyof);
}

static INLINE const gs_drawing_regs* gs_ctx_regs(const gs_drawing_context* c)
{
	return (const gs_drawing_regs*)c;
}

#ifdef __cplusplus
class GSDrawingContext : public gs_drawing_context
{
public:
	void Reset() { gs_ctx_reset(this); }

	void UpdateScissor() { gs_ctx_update_scissor(this); }

	GIFRegTEX0 GetSizeFixedTEX0(const GSVector4& st, bool linear, bool mipmap = false) const
	{
		return gs_ctx_size_fixed_tex0(&TEX0, &CLAMP, st.v, linear, mipmap);
	}

	const gs_drawing_regs* Regs() const { return gs_ctx_regs(this); }
};

static_assert(offsetof(GSDrawingContext, ZBUF) == offsetof(gs_drawing_regs, ZBUF)
	&& offsetof(GSDrawingContext, scissor) == offsetof(gs_drawing_regs, scissor)
	&& sizeof(GSDrawingContext::scissor) == sizeof(gs_drawing_regs::scissor), "gs_drawing_regs is GSDrawingContext's start");
static_assert(sizeof(GSDrawingContext) == sizeof(gs_drawing_context), "GSDrawingContext adds no members");
#endif

#endif
