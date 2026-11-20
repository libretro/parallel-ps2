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

#include "GSLocalMemory.h"

class alignas(32) GSDrawingContext
{
public:
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
		GSVector4i in;
		GSVector4i cull;
		GSVector4i xyof;
	} scissor;

	struct
	{
		GSOffset fb;
		GSOffset zb;
		GSPixelOffset4* fzb4; /* NULL: the pool was full (GSState::RefetchPixelOffsets) */
		u32 fzbase[2];        /* the frame and Z buffer's bases, to add to fzb4's */
	} offset;

	void Reset()
	{
		memset(&XYOFFSET, 0, sizeof(XYOFFSET));
		memset(&TEX0, 0, sizeof(TEX0));
		memset(&TEX1, 0, sizeof(TEX1));
		memset(&CLAMP, 0, sizeof(CLAMP));
		memset(&MIPTBP1, 0, sizeof(MIPTBP1));
		memset(&MIPTBP2, 0, sizeof(MIPTBP2));
		memset(&SCISSOR, 0, sizeof(SCISSOR));
		memset(&ALPHA, 0, sizeof(ALPHA));
		memset(&TEST, 0, sizeof(TEST));
		memset(&FBA, 0, sizeof(FBA));
		memset(&FRAME, 0, sizeof(FRAME));
		memset(&ZBUF, 0, sizeof(ZBUF));
	}

	void UpdateScissor()
	{
		gs_ctx_scissor(&SCISSOR, &XYOFFSET, scissor.in.v, scissor.cull.v, scissor.xyof.v);
	}

	GIFRegTEX0 GetSizeFixedTEX0(const GSVector4& st, bool linear, bool mipmap = false) const
	{
		return gs_ctx_size_fixed_tex0(&TEX0, &CLAMP, st.v, linear, mipmap);
	}

	const gs_drawing_regs* Regs() const { return reinterpret_cast<const gs_drawing_regs*>(this); }
};

static_assert(offsetof(GSDrawingContext, ZBUF) == offsetof(gs_drawing_regs, ZBUF)
	&& offsetof(GSDrawingContext, scissor) == offsetof(gs_drawing_regs, scissor)
	&& sizeof(GSDrawingContext::scissor) == sizeof(gs_drawing_regs::scissor), "gs_drawing_regs is GSDrawingContext's start");

#endif

#endif
