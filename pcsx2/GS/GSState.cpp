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

#include <string.h> /* memset/memcpy */
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include <float.h> /* FLT_MAX */
#include <math.h>

#include "common/MathUtils.h"

#include <retro_atomic.h>


#include "GSState.h"
#include "GSUtil.h"
#include "GSVertexKick.h"

int GSState::s_n = 0;
int GSState::s_last_transfer_draw_n = 0;
int GSState::s_transfer_n = 0;

/* Provide this to avoid C++20 bit_cast */
template <class T2, class T1>
static T2 cpp11_bit_cast(T1 t1) {
  T2 t2;
  memcpy(&t2, &t1, sizeof(T1));
  return t2;
}

static __fi bool IsFirstProvokingVertex()
{
	return (GSIsHardwareRenderer() && !g_gs_device->Features().provoking_vertex_last);
}

int GSState::GetSaveStateSize()
{
	int size = 0;

	size += sizeof(STATE_VERSION);
	size += sizeof(m_env.PRIM);
	size += sizeof(m_env.PRMODECONT);
	size += sizeof(m_env.TEXCLUT);
	size += sizeof(m_env.SCANMSK);
	size += sizeof(m_env.TEXA);
	size += sizeof(m_env.FOGCOL);
	size += sizeof(m_env.DIMX);
	size += sizeof(m_env.DTHE);
	size += sizeof(m_env.COLCLAMP);
	size += sizeof(m_env.PABE);
	size += sizeof(m_env.BITBLTBUF);
	size += sizeof(m_env.TRXDIR);
	size += sizeof(m_env.TRXPOS);
	size += sizeof(m_env.TRXREG);
	size += sizeof(m_env.TRXREG); // obsolete

	for (int i = 0; i < 2; i++)
	{
		size += sizeof(m_env.CTXT[i].XYOFFSET);
		size += sizeof(m_env.CTXT[i].TEX0);
		size += sizeof(m_env.CTXT[i].TEX1);
		size += sizeof(m_env.CTXT[i].CLAMP);
		size += sizeof(m_env.CTXT[i].MIPTBP1);
		size += sizeof(m_env.CTXT[i].MIPTBP2);
		size += sizeof(m_env.CTXT[i].SCISSOR);
		size += sizeof(m_env.CTXT[i].ALPHA);
		size += sizeof(m_env.CTXT[i].TEST);
		size += sizeof(m_env.CTXT[i].FBA);
		size += sizeof(m_env.CTXT[i].FRAME);
		size += sizeof(m_env.CTXT[i].ZBUF);
	}

	size += sizeof(m_v.RGBAQ);
	size += sizeof(m_v.ST);
	size += sizeof(m_v.UV);
	size += sizeof(m_v.FOG);
	size += sizeof(m_v.XYZ);
	size += sizeof(GIFReg); // obsolete

	size += sizeof(m_tr.x);
	size += sizeof(m_tr.y);
	size += GSLocalMemory::m_vmsize;
	size += (sizeof(GIFPath::tag) + sizeof(GIFPath::reg)) * 4; /* the four paths of m_path */
	size += sizeof(m_q);

	return size;
}

/* The trace of a draw, from the registers it was drawn with. */
void GSVertexTrace::Update(const void* vertex, const u16* index, int v_count, int i_count, GS_PRIM_CLASS primclass)
{
	struct gs_vt_regs regs;
	regs.prim      = m_state->PRIM;
	regs.xyoffset  = &m_state->m_context->XYOFFSET;
	regs.tex0      = &m_state->m_context->TEX0;
	regs.tex1      = &m_state->m_context->TEX1;
	regs.test      = &m_state->m_context->TEST;
	regs.filtering = GSConfig.TextureFiltering;
	gs_vertex_trace_update(this, vertex, index, v_count, i_count, primclass, &regs);
}

/* What the present asks the renderer (GSPresent.c) */
static gs_texture* present_get_output(void* gs, int i, float* scale, int* y_offset)
{
	GSState* s = static_cast<GSState*>(gs);
	return (s->m_ops && s->m_ops->get_output) ? s->m_ops->get_output(s, i, scale, y_offset) : NULL;
}

static gs_texture* present_get_feedback_output(void* gs, float* scale)
{
	GSState* s = static_cast<GSState*>(gs);
	return (s->m_ops && s->m_ops->get_feedback_output) ? s->m_ops->get_feedback_output(s, scale) : NULL;
}

static bool present_get_vr_depth(void* gs, gs_texture* color, gs_texture** depth, bool* encoded, float* z_scale)
{
	GSState* s = static_cast<GSState*>(gs);
	GSTexture* d = NULL;
	if (!s->m_ops || !s->m_ops->get_vr_depth ||
		!s->m_ops->get_vr_depth(s, static_cast<GSTexture*>(color), &d, encoded, z_scale))
		return false;
	*depth = d;
	return true;
}

static float present_get_upscale_multiplier(void* gs)
{
	return static_cast<GSState*>(gs)->GetUpscaleMultiplier();
}

static const gs_present_ops s_present_ops = {
	present_get_output,
	present_get_feedback_output,
	present_get_vr_depth,
	present_get_upscale_multiplier
};

GSState::GSState()
	: m_vt(this, IsFirstProvokingVertex())
{
	// m_nativeres seems to be a hack. Unfortunately it impacts draw call number which make debug painful in the replayer.
	// Let's keep it disabled to ease debug.
	m_nativeres = GSConfig.UpscaleMultiplier == 1.0f;
	m_mipmap = GSConfig.Mipmap;

	s_n = 0;
	s_transfer_n = 0;

	gs_present_init(&m_present, this, &s_present_ops, &PCRTCDisplays, &m_scanmask_used, &s_n, &s_transfer_n);

	memset(&m_v, 0, sizeof(m_v));
	memset(&m_vertex, 0, sizeof(m_vertex));
	memset(&m_index, 0, sizeof(m_index));
	memset(m_mem.m_vm8, 0, m_mem.m_vmsize);

	m_v.RGBAQ.Q = 1.0f;
	gs_array_init(&m_draw_transfers, sizeof(GSUploadQueue));
	gs_array_init(&m_drawlist, sizeof(u32));
	/* (room for the runs of a full vertex buffer of sprites, made now) */
	if (!gs_array_reserve(&m_drawlist, 2048))
		abort();

	GrowVertexBuffer();

	PRIM = &m_env.PRIM;
	//CSR->rREV = 0x20;
	m_env.PRMODECONT.AC = 1;
	/* The base reset, as it always was here: a constructor never reached a
	 * renderer's own. */
	ResetBase(false);

	ResetHandlers();
}

GSState::~GSState()
{
	if (m_vertex.buff)
		memalign_free(m_vertex.buff);
	if (m_index.buff)
		memalign_free(m_index.buff);
	gs_array_free(&m_draw_transfers);
	gs_array_free(&m_drawlist);
}

void GSState::PushDrawTransfer(const GSUploadQueue& transfer)
{
	GSUploadQueue* const slot = static_cast<GSUploadQueue*>(gs_array_push(&m_draw_transfers));
	if (!slot)
		abort(); /* out of memory, as the vector's push_back was without exceptions */
	*slot = transfer;
}

void GSState::ResetBase(bool hardware_reset)
{
	/* From GSRenderer::Reset, which did this and then called here: clear
	 * the current display texture. The constructor resets too, with
	 * hardware_reset false, so it never reaches this. */
	if (hardware_reset && g_gs_device)
		g_gs_device->ClearCurrent();

	Flush(GSFlushReason::RESET);

	// FIXME: bios logo not shown cut in half after reset, missing graphics in GoW after first FMV
	memset(&m_path, 0, sizeof(m_path));
	memset(&m_v, 0, sizeof(m_v));

	memset(&m_env.PRIM, 0, sizeof(m_env.PRIM));
	memset(&m_env.PRMODE, 0, sizeof(m_env.PRMODE));
	memset(&m_env.PRMODECONT, 0, sizeof(m_env.PRMODECONT));
	memset(&m_env.TEXCLUT, 0, sizeof(m_env.TEXCLUT));
	memset(&m_env.SCANMSK, 0, sizeof(m_env.SCANMSK));
	memset(&m_env.TEXA, 0, sizeof(m_env.TEXA));
	memset(&m_env.FOGCOL, 0, sizeof(m_env.FOGCOL));
	memset(&m_env.DIMX, 0, sizeof(m_env.DIMX));
	memset(&m_env.DTHE, 0, sizeof(m_env.DTHE));
	memset(&m_env.COLCLAMP, 0, sizeof(m_env.COLCLAMP));
	memset(&m_env.PABE, 0, sizeof(m_env.PABE));
	memset(&m_env.BITBLTBUF, 0, sizeof(m_env.BITBLTBUF));
	memset(&m_env.TRXDIR, 0, sizeof(m_env.TRXDIR));
	memset(&m_env.TRXPOS, 0, sizeof(m_env.TRXPOS));
	memset(&m_env.TRXREG, 0, sizeof(m_env.TRXREG));

	m_env.CTXT[0].Reset();
	m_env.CTXT[1].Reset();

	/* The registers a reset leaves are ones the register handlers could
	 * have produced, as a restored state's are: the same machine holds
	 * the same values whether it was reset or loaded. */
	NormalizeRestoredRegs();

	m_mem.m_clut.Reset();

	PRIM = &m_env.PRIM;

	UpdateContext();

	UpdateVertexKick();

	for (u32 i = 0; i < 2; i++)
	{
		m_env.CTXT[i].UpdateScissor();

		// What is this nonsense? Basically, GOW does a 32x448 draw after resetting the GS, thinking the PSM for the framebuffer is going
		// to be set to C24, therefore the alpha bits get left alone. Because of the reset, in PCSX2, it ends up as C32, and the TC gets
		// confused, leading to a later texture load using this render target instead of local memory. It's a problem because the game
		// uploads texture data on startup to the beginning of VRAM, and never overwrites it.
		//
		// In the software renderer, if we let the draw happen, it gets scissored to 1x1 (because the scissor is inclusive of the
		// upper bounds). This doesn't seem to destroy the chest texture, presumably it's further out in memory.
		//
		// Hardware test show that VRAM gets corrupted on CSR reset, but the first page remains intact. We're guessing this has something
		// to do with DRAM refresh, and perhaps the internal counters used for refresh also getting reset. We're obviously not going
		// to emulate this, but to work around the aforementioned issue, in the hardware renderers, we set the scissor to an out of
		// bounds value. This means that draws get skipped until the game sets a proper scissor up, which is definitely going to happen
		// after reset (otherwise it'd only ever render 1x1).
		//
		if (!hardware_reset && emu_gs_hardware_renderer(&GSConfig))
			m_env.CTXT[i].scissor.cull = GSVector4i::xffffffff();

		m_env.CTXT[i].offset.fb = m_mem.GetOffset(m_env.CTXT[i].FRAME.Block(), m_env.CTXT[i].FRAME.FBW, m_env.CTXT[i].FRAME.PSM);
		m_env.CTXT[i].offset.zb = m_mem.GetOffset(m_env.CTXT[i].ZBUF.Block(), m_env.CTXT[i].FRAME.FBW, m_env.CTXT[i].ZBUF.PSM);
		m_env.CTXT[i].offset.fzb4 = m_mem.GetPixelOffset4(m_env.CTXT[i].FRAME, m_env.CTXT[i].ZBUF);
	}

	UpdateScissor();

	m_vertex.head = 0;
	m_vertex.tail = 0;
	m_vertex.next = 0;
	m_index.tail = 0;
	m_scanmask_used = 0;
	m_texflush_flag = false;
	m_channel_shuffle = false;
	m_dirty_gs_regs = 0;
	m_backed_up_ctx = -1;

	memcpy(&m_prev_env, &m_env, sizeof(m_prev_env));
}

/* Both sets, plain and auto-flush: UpdateVertexKick picks by whether the
 * draw state can read what it draws (UpdateAutoFlushKick). */
template<bool index_swap>
void GSState::SetPrimHandlers()
{
#define SetHandlerXYZ(P, af, index_swap) \
	m_fpGIFPackedRegHandlerXYZ[af][P][0] = &GSState::GIFPackedRegHandlerXYZF2<P, 0, af, index_swap>; \
	m_fpGIFPackedRegHandlerXYZ[af][P][1] = &GSState::GIFPackedRegHandlerXYZF2<P, 1, af, index_swap>; \
	m_fpGIFPackedRegHandlerXYZ[af][P][2] = &GSState::GIFPackedRegHandlerXYZ2<P, 0, af, index_swap>; \
	m_fpGIFPackedRegHandlerXYZ[af][P][3] = &GSState::GIFPackedRegHandlerXYZ2<P, 1, af, index_swap>; \
	m_fpGIFRegHandlerXYZ[af][P][0] = &GSState::GIFRegHandlerXYZF2<P, 0, af, index_swap>; \
	m_fpGIFRegHandlerXYZ[af][P][1] = &GSState::GIFRegHandlerXYZF2<P, 1, af, index_swap>; \
	m_fpGIFRegHandlerXYZ[af][P][2] = &GSState::GIFRegHandlerXYZ2<P, 0, af, index_swap>; \
	m_fpGIFRegHandlerXYZ[af][P][3] = &GSState::GIFRegHandlerXYZ2<P, 1, af, index_swap>; \
	m_fpGIFPackedRegHandlerSTQRGBAXYZF2[af][P] = &GSState::GIFPackedRegHandlerSTQRGBAXYZF2<P, af, index_swap>; \
	m_fpGIFPackedRegHandlerSTQRGBAXYZ2[af][P] = &GSState::GIFPackedRegHandlerSTQRGBAXYZ2<P, af, index_swap>;

	SetHandlerXYZ(GS_POINTLIST, 0, false);
	SetHandlerXYZ(GS_LINELIST, 0, index_swap);
	SetHandlerXYZ(GS_LINESTRIP, 0, index_swap);
	SetHandlerXYZ(GS_TRIANGLELIST, 0, index_swap);
	SetHandlerXYZ(GS_TRIANGLESTRIP, 0, index_swap);
	SetHandlerXYZ(GS_TRIANGLEFAN, 0, index_swap);
	SetHandlerXYZ(GS_SPRITE, 0, false);
	SetHandlerXYZ(GS_INVALID, 0, false);

	SetHandlerXYZ(GS_POINTLIST, 1, false);
	SetHandlerXYZ(GS_LINELIST, 1, index_swap);
	SetHandlerXYZ(GS_LINESTRIP, 1, index_swap);
	SetHandlerXYZ(GS_TRIANGLELIST, 1, index_swap);
	SetHandlerXYZ(GS_TRIANGLESTRIP, 1, index_swap);
	SetHandlerXYZ(GS_TRIANGLEFAN, 1, index_swap);
	SetHandlerXYZ(GS_SPRITE, 1, false);
	SetHandlerXYZ(GS_INVALID, 1, false);

#undef SetHandlerXYZ
}

void GSState::ResetHandlers()
{
	for (size_t i = 0; i < C89_ARRAY_SIZE(m_fpGIFPackedRegHandlers); i++)
		m_fpGIFPackedRegHandlers[i] = &GSState::GIFPackedRegHandlerNull;

	m_fpGIFPackedRegHandlers[GIF_REG_PRIM] = (GIFPackedRegHandler)(GIFRegHandler)&GSState::GIFRegHandlerPRIM;
	m_fpGIFPackedRegHandlers[GIF_REG_RGBA] = &GSState::GIFPackedRegHandlerRGBA;
	m_fpGIFPackedRegHandlers[GIF_REG_STQ] = &GSState::GIFPackedRegHandlerSTQ;
	m_fpGIFPackedRegHandlers[GIF_REG_UV] = GSConfig.UserHacks_ForceEvenSpritePosition ? &GSState::GIFPackedRegHandlerUV_Hack : &GSState::GIFPackedRegHandlerUV;
	m_fpGIFPackedRegHandlers[GIF_REG_TEX0_1] = (GIFPackedRegHandler)(GIFRegHandler)&GSState::GIFRegHandlerTEX0<0>;
	m_fpGIFPackedRegHandlers[GIF_REG_TEX0_2] = (GIFPackedRegHandler)(GIFRegHandler)&GSState::GIFRegHandlerTEX0<1>;
	m_fpGIFPackedRegHandlers[GIF_REG_CLAMP_1] = (GIFPackedRegHandler)(GIFRegHandler)&GSState::GIFRegHandlerCLAMP<0>;
	m_fpGIFPackedRegHandlers[GIF_REG_CLAMP_2] = (GIFPackedRegHandler)(GIFRegHandler)&GSState::GIFRegHandlerCLAMP<1>;
	m_fpGIFPackedRegHandlers[GIF_REG_FOG] = &GSState::GIFPackedRegHandlerFOG;
	m_fpGIFPackedRegHandlers[GIF_REG_A_D] = &GSState::GIFPackedRegHandlerA_D;
	m_fpGIFPackedRegHandlers[GIF_REG_NOP] = &GSState::GIFPackedRegHandlerNOP;

	// swap first/last indices when the provoking vertex is the first (D3D/Vulkan)
	IsFirstProvokingVertex() ? SetPrimHandlers<true>() : SetPrimHandlers<false>();

	for (size_t i = 0; i < C89_ARRAY_SIZE(m_fpGIFRegHandlers); i++)
		m_fpGIFRegHandlers[i] = &GSState::GIFRegHandlerNull;

	m_fpGIFRegHandlers[GIF_A_D_REG_PRIM] = &GSState::GIFRegHandlerPRIM;
	m_fpGIFRegHandlers[GIF_A_D_REG_RGBAQ] = &GSState::GIFRegHandlerRGBAQ;
	m_fpGIFRegHandlers[GIF_A_D_REG_RGBAQ + 0x10] = &GSState::GIFRegHandlerRGBAQ;
	m_fpGIFRegHandlers[GIF_A_D_REG_ST] = &GSState::GIFRegHandlerST;
	m_fpGIFRegHandlers[GIF_A_D_REG_UV] = GSConfig.UserHacks_ForceEvenSpritePosition ? &GSState::GIFRegHandlerUV_Hack : &GSState::GIFRegHandlerUV;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEX0_1] = &GSState::GIFRegHandlerTEX0<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEX0_2] = &GSState::GIFRegHandlerTEX0<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_CLAMP_1] = &GSState::GIFRegHandlerCLAMP<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_CLAMP_2] = &GSState::GIFRegHandlerCLAMP<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_FOG] = &GSState::GIFRegHandlerFOG;
	m_fpGIFRegHandlers[GIF_A_D_REG_NOP] = &GSState::GIFRegHandlerNOP;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEX1_1] = &GSState::GIFRegHandlerTEX1<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEX1_2] = &GSState::GIFRegHandlerTEX1<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEX2_1] = &GSState::GIFRegHandlerTEX2<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEX2_2] = &GSState::GIFRegHandlerTEX2<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_XYOFFSET_1] = &GSState::GIFRegHandlerXYOFFSET<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_XYOFFSET_2] = &GSState::GIFRegHandlerXYOFFSET<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_PRMODECONT] = &GSState::GIFRegHandlerPRMODECONT;
	m_fpGIFRegHandlers[GIF_A_D_REG_PRMODE] = &GSState::GIFRegHandlerPRMODE;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEXCLUT] = &GSState::GIFRegHandlerTEXCLUT;
	m_fpGIFRegHandlers[GIF_A_D_REG_SCANMSK] = &GSState::GIFRegHandlerSCANMSK;
	m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP1_1] = &GSState::GIFRegHandlerMIPTBP1<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP1_2] = &GSState::GIFRegHandlerMIPTBP1<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP2_1] = &GSState::GIFRegHandlerMIPTBP2<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_MIPTBP2_2] = &GSState::GIFRegHandlerMIPTBP2<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEXA] = &GSState::GIFRegHandlerTEXA;
	m_fpGIFRegHandlers[GIF_A_D_REG_FOGCOL] = &GSState::GIFRegHandlerFOGCOL;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEXFLUSH] = &GSState::GIFRegHandlerTEXFLUSH;
	m_fpGIFRegHandlers[GIF_A_D_REG_SCISSOR_1] = &GSState::GIFRegHandlerSCISSOR<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_SCISSOR_2] = &GSState::GIFRegHandlerSCISSOR<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_ALPHA_1] = &GSState::GIFRegHandlerALPHA<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_ALPHA_2] = &GSState::GIFRegHandlerALPHA<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_DIMX] = &GSState::GIFRegHandlerDIMX;
	m_fpGIFRegHandlers[GIF_A_D_REG_DTHE] = &GSState::GIFRegHandlerDTHE;
	m_fpGIFRegHandlers[GIF_A_D_REG_COLCLAMP] = &GSState::GIFRegHandlerCOLCLAMP;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEST_1] = &GSState::GIFRegHandlerTEST<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_TEST_2] = &GSState::GIFRegHandlerTEST<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_PABE] = &GSState::GIFRegHandlerPABE;
	m_fpGIFRegHandlers[GIF_A_D_REG_FBA_1] = &GSState::GIFRegHandlerFBA<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_FBA_2] = &GSState::GIFRegHandlerFBA<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_FRAME_1] = &GSState::GIFRegHandlerFRAME<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_FRAME_2] = &GSState::GIFRegHandlerFRAME<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_ZBUF_1] = &GSState::GIFRegHandlerZBUF<0>;
	m_fpGIFRegHandlers[GIF_A_D_REG_ZBUF_2] = &GSState::GIFRegHandlerZBUF<1>;
	m_fpGIFRegHandlers[GIF_A_D_REG_BITBLTBUF] = &GSState::GIFRegHandlerBITBLTBUF;
	m_fpGIFRegHandlers[GIF_A_D_REG_TRXPOS] = &GSState::GIFRegHandlerTRXPOS;
	m_fpGIFRegHandlers[GIF_A_D_REG_TRXREG] = &GSState::GIFRegHandlerTRXREG;
	m_fpGIFRegHandlers[GIF_A_D_REG_TRXDIR] = &GSState::GIFRegHandlerTRXDIR;
	m_fpGIFRegHandlers[GIF_A_D_REG_HWREG] = &GSState::GIFRegHandlerHWREG;

	m_fpGIFRegHandlers[GIF_A_D_REG_SIGNAL] = &GSState::GIFRegHandlerNull;
	m_fpGIFRegHandlers[GIF_A_D_REG_FINISH] = &GSState::GIFRegHandlerNull;
	m_fpGIFRegHandlers[GIF_A_D_REG_LABEL] = &GSState::GIFRegHandlerNull;
}

void GSState::UpdateSettingsBase(const EmuGSOptions& old_config)
{
	m_mipmap = GSConfig.Mipmap;

	if (GSConfig.UserHacks_ForceEvenSpritePosition != old_config.UserHacks_ForceEvenSpritePosition)
	{
		ResetHandlers();
	}
}

__noinline void GSState::CheckFlushes()
{
	if (m_dirty_gs_regs && m_index.tail > 0)
	{
		if (TestDrawChanged())
			Flush(GSFlushReason::CONTEXTCHANGE);
	}
}

void GSState::GIFPackedRegHandlerNull(const GIFPackedReg* RESTRICT r)
{
}

void GSState::GIFPackedRegHandlerRGBA(const GIFPackedReg* RESTRICT r)
{
	const GSVector4i mask = GSVector4i::load(0x0c080400);
	const GSVector4i v = GSVector4i::load<false>(r).shuffle8(mask);

	m_v.RGBAQ.U32[0] = (u32)GSVector4i::store(v);

	m_v.RGBAQ.Q = m_q;
}

void GSState::GIFPackedRegHandlerSTQ(const GIFPackedReg* RESTRICT r)
{
	const GSVector4i st = GSVector4i::loadl(&r->U64[0]);

	GSVector4i q = GSVector4i::loadl(&r->U64[1]);
	GSVector4i::storel(&m_v.ST, st);

	// Vexx (character shadow)
	// q = 0 (st also 0 on the first 16 vertices), setting it to FLT_MIN to avoid div by zero later
	q = q.blend8(GSVector4i::cast(GSVector4(FLT_MIN)), q == GSVector4i::zero());

	// Suikoden 4
	// creates some nan for Q. Let's avoid undefined behavior (See GIFRegHandlerRGBAQ)
	q = GSVector4i::cast(GSVector4::cast(q).replace_nan(GSVector4::m_max));

	GSVector4::store(&m_q, GSVector4::cast(q));
}

void GSState::GIFPackedRegHandlerUV(const GIFPackedReg* RESTRICT r)
{
	const GSVector4i v = GSVector4i::loadl(r) & GSVector4i::x00003fff();

	m_v.UV = (u32)GSVector4i::store(v.ps32(v));
}

void GSState::GIFPackedRegHandlerUV_Hack(const GIFPackedReg* RESTRICT r)
{
	const GSVector4i v = GSVector4i::loadl(r) & GSVector4i::x00003fff();

	m_v.UV = (u32)GSVector4i::store(v.ps32(v));

	m_isPackedUV_HackFlag = true;
}

template <u32 prim, u32 adc, bool auto_flush, bool index_swap>
void GSState::GIFPackedRegHandlerXYZF2(const GIFPackedReg* RESTRICT r)
{
	if (!adc || GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GSUtil::GetPrimClass(m_env.PRIM.PRIM) || (m_dirty_gs_regs & (1 << DIRTY_REG_XYOFFSET)))
		CheckFlushes();

	GSVector4i xy = GSVector4i::loadl(&r->U64[0]);
	GSVector4i zf = GSVector4i::loadl(&r->U64[1]);

	xy = xy.upl16(xy.srl<4>()).upl32(GSVector4i::load((int)m_v.UV));
	zf = zf.srl32<4>() & GSVector4i::x00ffffff().upl32(GSVector4i::x000000ff());

	m_v.m[1] = xy.upl32(zf);

	VertexKick<prim, auto_flush, index_swap>(adc ? 1 : r->XYZF2.Skip(), m_vertex.maxcount);
}

template <u32 prim, u32 adc, bool auto_flush, bool index_swap>
void GSState::GIFPackedRegHandlerXYZ2(const GIFPackedReg* RESTRICT r)
{
	if (!adc || GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GSUtil::GetPrimClass(m_env.PRIM.PRIM) || (m_dirty_gs_regs & (1 << DIRTY_REG_XYOFFSET)))
		CheckFlushes();

	const GSVector4i xy = GSVector4i::loadl(&r->U64[0]);
	const GSVector4i z = GSVector4i::loadl(&r->U64[1]);
	const GSVector4i xyz = xy.upl16(xy.srl<4>()).upl32(z);

	m_v.m[1] = xyz.upl64(GSVector4i::loadl(&m_v.UV));

	VertexKick<prim, auto_flush, index_swap>(adc ? 1 : r->XYZ2.Skip(), m_vertex.maxcount);
}

void GSState::GIFPackedRegHandlerFOG(const GIFPackedReg* RESTRICT r)
{
	m_v.FOG = r->FOG.F;
}

void GSState::GIFPackedRegHandlerA_D(const GIFPackedReg* RESTRICT r)
{
	(this->*m_fpGIFRegHandlers[r->A_D.ADDR & 0x7F])(&r->r);
}

void GSState::GIFPackedRegHandlerNOP(const GIFPackedReg* RESTRICT r)
{
}

template <u32 prim, bool auto_flush, bool index_swap>
void GSState::GIFPackedRegHandlerSTQRGBAXYZF2(const GIFPackedReg* RESTRICT r, u32 size)
{
	bool flushes_checked = false;

	if (GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GSUtil::GetPrimClass(m_env.PRIM.PRIM) || (m_dirty_gs_regs & (1 << DIRTY_REG_XYOFFSET)))
	{
		flushes_checked = true;
		CheckFlushes();
	}

	const GIFPackedReg* RESTRICT r_end = r + size;
	u32 local_maxcount = m_vertex.maxcount;

	while (r < r_end)
	{
		__prefetch_r(r + 6);

		/* TODO: only store the last one */
		GSVertexKernels::ParsePackedSTQRGBAXYZF2(r, m_v.UV, m_v.m);

		const bool skip = r[2].XYZF2.Skip();
		if (!flushes_checked && !skip)
		{
			flushes_checked = true;
			CheckFlushes();
		}
		VertexKick<prim, auto_flush, index_swap>(skip, local_maxcount);

		r += 3;
	}

	m_q = r[-3].STQ.Q; // remember the last one, STQ outputs this to the temp Q each time
}

template <u32 prim, bool auto_flush, bool index_swap>
void GSState::GIFPackedRegHandlerSTQRGBAXYZ2(const GIFPackedReg* RESTRICT r, u32 size)
{
	bool flushes_checked = false;

	if (GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GSUtil::GetPrimClass(m_env.PRIM.PRIM) || (m_dirty_gs_regs & (1 << DIRTY_REG_XYOFFSET)))
	{
		flushes_checked = true;
		CheckFlushes();
	}

	const GIFPackedReg* RESTRICT r_end = r + size;
	u32 local_maxcount = m_vertex.maxcount;

	while (r < r_end)
	{
		__prefetch_r(r + 6);

		/* TODO: only store the last one */
		GSVertexKernels::ParsePackedSTQRGBAXYZ2(r, &m_v.UV, m_v.m);

		const bool skip = r[2].XYZF2.Skip();
		if (!flushes_checked && !skip)
		{
			flushes_checked = true;
			CheckFlushes();
		}
		VertexKick<prim, auto_flush, index_swap>(skip, local_maxcount);

		r += 3;
	}

	m_q = r[-3].STQ.Q; // remember the last one, STQ outputs this to the temp Q each time
}

void GSState::GIFPackedRegHandlerNOP(const GIFPackedReg* RESTRICT r, u32 size)
{
}

void GSState::GIFRegHandlerNull(const GIFReg* RESTRICT r)
{
}

__forceinline void GSState::ApplyPRIM(u32 prim)
{
	if (m_env.PRMODECONT.AC == 1)
	{
		m_env.PRIM.U32[0] = prim;

		UpdateContext();
	}
	else
		m_env.PRIM.PRIM = prim & 0x7;

	if (m_prev_env.PRIM.U32[0] ^ m_env.PRIM.U32[0])
		m_dirty_gs_regs |= (1 << DIRTY_REG_PRIM);
	else
		m_dirty_gs_regs &= ~(1<< DIRTY_REG_PRIM);

	UpdateVertexKick();

	if (m_index.tail == 0)
		m_vertex.next = 0;

	m_vertex.head = m_vertex.tail = m_vertex.next; // remove unused vertices from the end of the vertex buffer
}

void GSState::GIFRegHandlerPRIM(const GIFReg* RESTRICT r)
{
	ALIGN_STACK(32);

	ApplyPRIM(r->PRIM.U32[0]);
}

void GSState::GIFRegHandlerRGBAQ(const GIFReg* RESTRICT r)
{
	const GSVector4i rgbaq = (GSVector4i)r->RGBAQ;

	GSVector4i q = rgbaq.blend8(GSVector4i::cast(GSVector4::m_one), rgbaq == GSVector4i::zero()).yyyy(); // see GIFPackedRegHandlerSTQ

	// Silent Hill output a nan in Q to emulate the flash light. Unfortunately it
	// breaks GSVertexTrace code that rely on min/max.

	q = GSVector4i::cast(GSVector4::cast(q).replace_nan(GSVector4::m_max));

	m_v.RGBAQ = rgbaq.upl32(q);
}

void GSState::GIFRegHandlerST(const GIFReg* RESTRICT r)
{
	m_v.ST = r->ST;
}

void GSState::GIFRegHandlerUV(const GIFReg* RESTRICT r)
{
	m_v.UV = r->UV.U32[0] & 0x3fff3fff;
}

void GSState::GIFRegHandlerUV_Hack(const GIFReg* RESTRICT r)
{
	m_v.UV = r->UV.U32[0] & 0x3fff3fff;

	m_isPackedUV_HackFlag = false;
}

template <u32 prim, u32 adc, bool auto_flush, bool index_swap>
void GSState::GIFRegHandlerXYZF2(const GIFReg* RESTRICT r)
{
	if (!adc || GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GSUtil::GetPrimClass(m_env.PRIM.PRIM) || (m_dirty_gs_regs & (1 << DIRTY_REG_XYOFFSET)))
		CheckFlushes();

	const GSVector4i xyzf = GSVector4i::loadl(&r->XYZF);
	const GSVector4i xyz = xyzf & (GSVector4i::xffffffff().upl32(GSVector4i::x00ffffff()));
	const GSVector4i uvf = GSVector4i::load((int)m_v.UV).upl32(xyzf.srl32<24>().srl<4>());

	m_v.m[1] = xyz.upl64(uvf);

	VertexKick<prim, auto_flush, index_swap>(adc, m_vertex.maxcount);
}

template <u32 prim, u32 adc, bool auto_flush, bool index_swap>
void GSState::GIFRegHandlerXYZ2(const GIFReg* RESTRICT r)
{
	if (!adc || GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GSUtil::GetPrimClass(m_env.PRIM.PRIM) || (m_dirty_gs_regs & (1 << DIRTY_REG_XYOFFSET)))
		CheckFlushes();

	m_v.m[1] = GSVector4i::load(&r->XYZ, &m_v.UV);

	VertexKick<prim, auto_flush, index_swap>(adc, m_vertex.maxcount);
}

template <int i>
void GSState::ApplyTEX0(GIFRegTEX0& TEX0)
{
	// TODO: Paletted Formats
	// 8-bit and 4 bit formats need to be addressed with a buffer width divisible 2.
	// However, not doing so is possible and does have a behavior on the GS.
	// When implementing such code care must be taken not to apply it unless it is
	// used for a draw. Galaxy Angel will send TEX0 with a PSM of T8 and a TBW of 7
	// only to immediately update it to CT32 with TEX2. The old code used to apply a
	// correction on the TEX0 setting which caused the game to draw the CT32 texture
	// with an incorrect buffer width.
	//
	// Bouken Jidai Katsugeki Goemon apparently uses a TBW of 1 but this game is currently
	// extremely broken for the same reasons as MLB Power Pros in that it spams TEX0 with
	// complete garbage making for a nice 1G heap of GSOffset.

	if ((TEX0.PSM & 0x7) >= 3 && m_mem.m_clut.CanLoadCLUT(TEX0))
	{
		m_mem.m_clut.ClearDrawInvalidity();
		m_mem.m_clut.SetNextCLUTTEX0(TEX0.U64);
		CheckCLUTValidity(m_prev_env.PRIM.PRIM);
	}

	// Even if TEX0 did not change, a new palette may have been uploaded and will overwrite the currently queued for drawing.
	const bool wt = m_mem.m_clut.WriteTest(TEX0, m_env.TEXCLUT);

	// No need to flush on CLUT if we aren't texture mapping.
	if (wt)
	{
		if ((m_prev_env.PRIM.TME && (m_prev_env.CTXT[m_prev_env.PRIM.CTXT].TEX0.PSM & 0x7) >= 3) || (m_mem.m_clut.IsInvalid() & 2))
			Flush(GSFlushReason::CLUTCHANGE);
		else
			FlushWrite();

		// Abort any channel shuffle skipping, since this is likely part of a new shuffle.
		// Test case: Tomb Raider series. This is gated by the CBP actually changing, because
		// Urban Chaos writes to the memory backing the CLUT in the middle of a shuffle, and
		// it's unclear whether the CLUT would actually get reloaded in that case.
		if (TEX0.CBP != m_mem.m_clut.GetCLUTCBP())
			m_channel_shuffle = false;
	}

	TEX0.CPSM &= 0xa; // 1010b

	m_env.CTXT[i].TEX0 = TEX0;

	if (wt)
	{
		GIFRegBITBLTBUF BITBLTBUF = {};
		GSVector4i r;
		/* A load the GPU palette path takes (CSM1, CSA within the first
		 * sixteen blocks) is read from a target holding it on the GPU; any
		 * other is read from local memory, which must then hold what the
		 * GPU drew there. */
		const u32 load_csa = TEX0.CSA & ((TEX0.CPSM == PSMCT16 || TEX0.CPSM == PSMCT16S) ? 31u : 15u);
		const bool gpu_palette = (TEX0.CSM == 0) && (load_csa <= 15);

		if (TEX0.CSM == 0)
		{
			BITBLTBUF.SBP = TEX0.CBP;
			BITBLTBUF.SBW = 1;
			BITBLTBUF.SPSM = TEX0.CPSM;

			r.left = 0;
			r.top = 0;
			r.right = GSLocalMemory::m_psm[TEX0.CPSM].bs.x;
			r.bottom = GSLocalMemory::m_psm[TEX0.CPSM].bs.y;

			int blocks = 4;

			if (GSLocalMemory::m_psm[TEX0.CPSM].trbpp == 16)
				blocks >>= 1;

			if (GSLocalMemory::m_psm[TEX0.PSM].trbpp == 4)
				blocks >>= 1;

			// Invalidating videomem is slow, so *only* do it when it's definitely a CLUT draw in HW mode.
			for (int j = 0; j < blocks; j++, BITBLTBUF.SBP++)
				InvalidateLocalMem(BITBLTBUF, r, gpu_palette);
		}
		else
		{
			BITBLTBUF.SBP = TEX0.CBP;
			BITBLTBUF.SBW = m_env.TEXCLUT.CBW;
			BITBLTBUF.SPSM = TEX0.CPSM;

			r.left = m_env.TEXCLUT.COU;
			r.top = m_env.TEXCLUT.COV;
			r.right = r.left + GSLocalMemory::m_psm[TEX0.CPSM].pal;
			r.bottom = r.top + 1;

			InvalidateLocalMem(BITBLTBUF, r, gpu_palette);
		}

		m_mem.m_clut.Write(m_mem.m_vm8, m_env.CTXT[i].TEX0, m_env.TEXCLUT);
	}

	u64 mask = 0x1fffffffffull; // TBP0 TBW PSM TW TH TCC TFX
	if ((TEX0.PSM & 0x7) >= 3)
		mask |= 0x1f78000000000000ull; // CPSM CSA

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].TEX0.TBP0 != m_env.CTXT[i].TEX0.TBP0)
			m_texflush_flag = false;

		if ((m_prev_env.CTXT[i].TEX0.U64 ^ m_env.CTXT[i].TEX0.U64) & mask)
			m_dirty_gs_regs |= (1 << DIRTY_REG_TEX0);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_TEX0);
	}

	if (i == PRIM->CTXT)
		UpdateAutoFlushKick();
}

template <int i>
void GSState::GIFRegHandlerTEX0(const GIFReg* RESTRICT r)
{
	GIFRegTEX0 TEX0 = r->TEX0;
	// Max allowed MTBA size for 32bit swizzled textures (including 8H 4HL etc) is 512, 16bit and normal 8/4bit formats can be 1024
	const u32 maxTex = (GSLocalMemory::m_psm[TEX0.PSM].bpp < 32) ? 10 : 9;

	// Spec max is 10, but bitfield allows for up to 15
	// However STQ calculations expect the written size to be used for denormalization (Simple 2000 Series Vol 105 The Maid)
	// This is clamped to 10 in the FixedTEX0 functions so texture sizes don't exceed 1024x1024, but STQ can calculate properly (with invalid_tex0)
	//
	// Yakuza (minimap)
	// Sets TW/TH to 0
	// Drawn using solid colors, the texture is really a 1x1 white texel,
	// modulated by the vertex color. Cannot change the dimension because S/T are normalized.
	//
	// Tokyo Xtreme Racer Drift 2 (text)
	// Sets TW/TH to 0
	// there used to be a case to force this to 10
	// but GetSizeFixedTEX0 sorts this now
	u32 a   = (TEX0.TW < 0) ? 0 : TEX0.TW;
	u32 b   = (TEX0.TH < 0) ? 0 : TEX0.TH;
	TEX0.TW = (15 < a) ? 15 : a;
	TEX0.TH = (15 < b) ? 15 : b;

	// MTBA loads are triggered by writes to TEX0 (but not TEX2!)
	// Textures MUST be a minimum width of 32 pixels
	// Format must be a color, Z formats do not trigger MTBA (but are valid for Mipmapping)
	if (m_env.CTXT[i].TEX1.MTBA && TEX0.TW >= 5 && TEX0.TW <= maxTex && (TEX0.PSM & 0x30) != 0x30)
	{
		GIFRegMIPTBP1& mip_tbp1 = m_env.CTXT[i].MIPTBP1;
		// NOTE 1: TEX1.MXL must not be automatically set to 3 here and it has no effect on MTBA.
		// NOTE 2: Mipmap levels are packed with a minimum distance between them of 1 block, even down at 4bit textures under 16x16.
		// NOTE 3: Everything is derrived from the width of the texture, TBW and TH are completely ignored (useful for handling non-rectangular ones)
		// NOTE 4: Cartoon Network Racing's menu is VERY sensitive to this as it uses 4bit sized textures for the sky.
		u32 bp = TEX0.TBP0;
		u32 bw = pcsx2_max_i(1u, (1u << TEX0.TW) >> 6);

		// Address is calculated as a 4bit address space, then converted (/8) to 32bit address space
		// ((w * w * bpp) / 8) / 64. No the 'w' is not a typo ;)
		const u32 bpp = GSLocalMemory::m_psm[TEX0.PSM].bpp >> 2;
		u32 tex_size = ((1u << TEX0.TW) * (1u << TEX0.TW) * bpp) >> 9;

		bp += tex_size;
		bw = pcsx2_max_u(bw >> 1, 1);
		tex_size = pcsx2_max_u(tex_size >> 2, 1);

		mip_tbp1.TBP1 = bp;
		mip_tbp1.TBW1 = bw;

		bp += tex_size;
		bw = pcsx2_max_u(bw >> 1, 1);
		tex_size = pcsx2_max_u(tex_size >> 2, 1);

		mip_tbp1.TBP2 = bp;
		mip_tbp1.TBW2 = bw;

		bp += tex_size;
		bw = pcsx2_max_u(bw >> 1, 1);

		mip_tbp1.TBP3 = bp;
		mip_tbp1.TBW3 = bw;

		if (i == m_prev_env.PRIM.CTXT)
		{
			if (m_prev_env.CTXT[i].MIPTBP1.U64 ^ mip_tbp1.U64)
				m_dirty_gs_regs |= (1 << DIRTY_REG_MIPTBP1);
			else
				m_dirty_gs_regs &= ~(1 << DIRTY_REG_MIPTBP1);
		}
	}

	ApplyTEX0<i>(TEX0);
}

template <int i>
void GSState::GIFRegHandlerCLAMP(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].CLAMP = r->CLAMP;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].CLAMP.U64 ^ m_env.CTXT[i].CLAMP.U64)
			m_dirty_gs_regs |= (1 << DIRTY_REG_CLAMP);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_CLAMP);
	}
}

void GSState::GIFRegHandlerFOG(const GIFReg* RESTRICT r)
{
	m_v.FOG = r->FOG.F;
}

void GSState::GIFRegHandlerNOP(const GIFReg* RESTRICT r)
{
}

template <int i>
void GSState::GIFRegHandlerTEX1(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].TEX1 = r->TEX1;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].TEX1.U64 ^ m_env.CTXT[i].TEX1.U64)
			m_dirty_gs_regs |= (1 << DIRTY_REG_TEX1);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_TEX1);
	}
}

template <int i>
void GSState::GIFRegHandlerTEX2(const GIFReg* RESTRICT r)
{
	// TEX2 is a masked write to TEX0, for performing CLUT swaps (palette swaps).
	// It only applies the following fields:
	//    CLD, CSA, CSM, CPSM, CBP, PSM.
	// It ignores these fields (uses existing values in the context):
	//    TFX, TCC, TH, TW, TBW, and TBP0

	constexpr u64 mask = 0xFFFFFFE003F00000ull; // TEX2 bits

	GIFRegTEX0 TEX0{};

	TEX0.U64 = (m_env.CTXT[i].TEX0.U64 & ~mask) | (r->U64 & mask);

	ApplyTEX0<i>(TEX0);
}

template <int i>
void GSState::GIFRegHandlerXYOFFSET(const GIFReg* RESTRICT r)
{
	const u64 r_masked = r->U64 & 0x0000FFFF0000FFFFu;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].XYOFFSET.U64 != r_masked)
			m_dirty_gs_regs |= (1 << DIRTY_REG_XYOFFSET);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_XYOFFSET);
	}

	if (m_env.CTXT[i].XYOFFSET.U64 == r_masked)
		return;

	m_env.CTXT[i].XYOFFSET.U64 = r_masked;

	m_env.CTXT[i].UpdateScissor();

	UpdateScissor();
}

void GSState::GIFRegHandlerPRMODECONT(const GIFReg* RESTRICT r)
{
	m_env.PRMODECONT.AC = r->PRMODECONT.AC;
}

void GSState::GIFRegHandlerPRMODE(const GIFReg* RESTRICT r)
{
	// We're in PRIM mode, need to ignore any writes
	if (m_env.PRMODECONT.AC)
		return;

	const u32 _PRIM = m_env.PRIM.PRIM;
	m_env.PRIM = r->PRMODE;
	m_env.PRIM.PRIM = _PRIM;

	if (m_prev_env.PRIM.U32[0] ^ m_env.PRIM.U32[0])
		m_dirty_gs_regs |= (1 << DIRTY_REG_PRIM);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_PRIM);

	UpdateContext();
}

void GSState::GIFRegHandlerTEXCLUT(const GIFReg* RESTRICT r)
{
	m_env.TEXCLUT = r->TEXCLUT;
}

void GSState::GIFRegHandlerSCANMSK(const GIFReg* RESTRICT r)
{
	m_env.SCANMSK = r->SCANMSK;

	if (m_env.SCANMSK.MSK & 2)
		m_scanmask_used = 2;

	if (m_prev_env.SCANMSK.MSK != m_env.SCANMSK.MSK)
		m_dirty_gs_regs |= (1 << DIRTY_REG_SCANMSK);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_SCANMSK);
}

template <int i>
void GSState::GIFRegHandlerMIPTBP1(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].MIPTBP1 = r->MIPTBP1;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].MIPTBP1.U64 != m_env.CTXT[i].MIPTBP1.U64)
			m_dirty_gs_regs |= (1 << DIRTY_REG_MIPTBP1);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_MIPTBP1);
	}
}

template <int i>
void GSState::GIFRegHandlerMIPTBP2(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].MIPTBP2 = r->MIPTBP2;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].MIPTBP2.U64 != m_env.CTXT[i].MIPTBP2.U64)
			m_dirty_gs_regs |= (1 << DIRTY_REG_MIPTBP2);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_MIPTBP2);
	}
}

void GSState::GIFRegHandlerTEXA(const GIFReg* RESTRICT r)
{
	m_env.TEXA = r->TEXA;

	if (m_prev_env.TEXA != m_env.TEXA)
		m_dirty_gs_regs |= (1 << DIRTY_REG_TEXA);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_TEXA);
}

void GSState::GIFRegHandlerFOGCOL(const GIFReg* RESTRICT r)
{
	m_env.FOGCOL = r->FOGCOL;

	if (m_prev_env.FOGCOL != m_env.FOGCOL)
		m_dirty_gs_regs |= (1 << DIRTY_REG_FOGCOL);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_FOGCOL);
}

void GSState::GIFRegHandlerTEXFLUSH(const GIFReg* RESTRICT r)
{
	// No need to do a flush if TEX0 has changed
	if (!(m_dirty_gs_regs & (1 << DIRTY_REG_TEX0)))
		m_texflush_flag = true;
}

template <int i>
void GSState::GIFRegHandlerSCISSOR(const GIFReg* RESTRICT r)
{
	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].SCISSOR.U64 != r->SCISSOR.U64)
			m_dirty_gs_regs |= (1 << DIRTY_REG_SCISSOR);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_SCISSOR);
	}

	if (m_env.CTXT[i].SCISSOR.U64 == r->SCISSOR.U64)
		return;

	m_env.CTXT[i].SCISSOR = r->SCISSOR;
	m_env.CTXT[i].UpdateScissor();

	UpdateScissor();
}

template <int i>
void GSState::GIFRegHandlerALPHA(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].ALPHA = r->ALPHA;

	// value of 3 is not allowed by the spec
	// acts like 2 on real hw, so just clamp it
	u32 alpha_a           = (r->ALPHA.A < 0) ? 0 : r->ALPHA.A;
	u32 alpha_b           = (r->ALPHA.B < 0) ? 0 : r->ALPHA.B;
	u32 alpha_c           = (r->ALPHA.C < 0) ? 0 : r->ALPHA.C;
	u32 alpha_d           = (r->ALPHA.D < 0) ? 0 : r->ALPHA.D;
	m_env.CTXT[i].ALPHA.A = (2 < alpha_a) ? 2 : alpha_a;
	m_env.CTXT[i].ALPHA.B = (2 < alpha_b) ? 2 : alpha_b;
	m_env.CTXT[i].ALPHA.C = (2 < alpha_c) ? 2 : alpha_c;
	m_env.CTXT[i].ALPHA.D = (2 < alpha_d) ? 2 : alpha_d;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].ALPHA.U64 != m_env.CTXT[i].ALPHA.U64)
			m_dirty_gs_regs |= (1 << DIRTY_REG_ALPHA);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_ALPHA);
	}
}

void GSState::GIFRegHandlerDIMX(const GIFReg* RESTRICT r)
{
	m_env.DIMX = r->DIMX;

	if (m_prev_env.DIMX != m_env.DIMX)
		m_dirty_gs_regs |= (1 << DIRTY_REG_DIMX);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_DIMX);
}

void GSState::GIFRegHandlerDTHE(const GIFReg* RESTRICT r)
{
	m_env.DTHE = r->DTHE;

	if (m_prev_env.DTHE != m_env.DTHE)
		m_dirty_gs_regs |= (1 << DIRTY_REG_DTHE);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_DTHE);
}

void GSState::GIFRegHandlerCOLCLAMP(const GIFReg* RESTRICT r)
{
	m_env.COLCLAMP = r->COLCLAMP;

	if (m_prev_env.COLCLAMP != m_env.COLCLAMP)
		m_dirty_gs_regs |= (1 << DIRTY_REG_COLCLAMP);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_COLCLAMP);
}

template <int i>
void GSState::GIFRegHandlerTEST(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].TEST = r->TEST;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].TEST != m_env.CTXT[i].TEST)
			m_dirty_gs_regs |= (1 << DIRTY_REG_TEST);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_TEST);
	}

	if (i == PRIM->CTXT)
		UpdateAutoFlushKick();
}

void GSState::GIFRegHandlerPABE(const GIFReg* RESTRICT r)
{
	m_env.PABE = r->PABE;

	if (m_prev_env.PABE != m_env.PABE)
		m_dirty_gs_regs |= (1 << DIRTY_REG_PABE);
	else
		m_dirty_gs_regs &= ~(1 << DIRTY_REG_PABE);
}

template <int i>
void GSState::GIFRegHandlerFBA(const GIFReg* RESTRICT r)
{
	m_env.CTXT[i].FBA = r->FBA;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].FBA != m_env.CTXT[i].FBA)
			m_dirty_gs_regs |= (1 << DIRTY_REG_FBA);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_FBA);
	}
}

template <int i>
void GSState::GIFRegHandlerFRAME(const GIFReg* RESTRICT r)
{
	GIFRegFRAME NewFrame = r->FRAME;
	// FBW is clamped to 32
	NewFrame.FBW = (32U < NewFrame.FBW) ? 32U : NewFrame.FBW;

	if ((NewFrame.PSM & 0x30) == 0x30)
		m_env.CTXT[i].ZBUF.PSM &= ~0x30;
	else
		m_env.CTXT[i].ZBUF.PSM |= 0x30;

	if ((m_env.CTXT[i].FRAME.U32[0] ^ NewFrame.U32[0]) & 0x3f3f01ff) // FBP FBW PSM
	{
		m_env.CTXT[i].offset.fb = m_mem.GetOffset(NewFrame.Block(), NewFrame.FBW, NewFrame.PSM);
		m_env.CTXT[i].offset.zb = m_mem.GetOffset(m_env.CTXT[i].ZBUF.Block(), NewFrame.FBW, m_env.CTXT[i].ZBUF.PSM);
		m_env.CTXT[i].offset.fzb4 = m_mem.GetPixelOffset4(NewFrame, m_env.CTXT[i].ZBUF);
	}

	m_env.CTXT[i].FRAME = NewFrame;

	switch (m_env.CTXT[i].FRAME.PSM)
	{
		case PSMT8H:
			// Berserk uses the format to only update the alpha channel
			m_env.CTXT[i].FRAME.PSM = PSMCT32;
			m_env.CTXT[i].FRAME.FBMSK = 0x00FFFFFF;
			break;
		case PSMT4HH: // Not tested. Based on PSMT8H behavior
			m_env.CTXT[i].FRAME.PSM = PSMCT32;
			m_env.CTXT[i].FRAME.FBMSK = 0x0FFFFFFF;
			break;
		case PSMT4HL: // Not tested. Based on PSMT8H behavior
			m_env.CTXT[i].FRAME.PSM = PSMCT32;
			m_env.CTXT[i].FRAME.FBMSK = 0xF0FFFFFF;
			break;
		default:
			break;
	}

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].FRAME != m_env.CTXT[i].FRAME)
			m_dirty_gs_regs |= (1 << DIRTY_REG_FRAME);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_FRAME);
	}

	if (i == PRIM->CTXT)
		UpdateAutoFlushKick();
}

template <int i>
void GSState::GIFRegHandlerZBUF(const GIFReg* RESTRICT r)
{
	GIFRegZBUF ZBUF = r->ZBUF;

	// We tested this on the PS2 and it seems to be that when the FRAME is a Z format,
	// the Z buffer is forced to use color swizzling.
	// Powerdrome relies on this behavior to clear the z buffer by drawing 32 pixel wide strips, skipping 32,
	// causing the FRAME to do one strip and the Z to do the other 32 due to the block arrangement.
	// Other games listed here also hit this Color/Z swap behaviour without masking Z so could be problematic:
	// Black, Driver Parallel Lines, Driv3r, Dropship, DT Racer, Scarface, The Simpsons, THP8
	if ((m_env.CTXT[i].FRAME.PSM & 0x30) == 0x30)
		ZBUF.PSM &= ~0x30;
	else
		ZBUF.PSM |= 0x30;

	if ((m_env.CTXT[i].ZBUF.U32[0] ^ ZBUF.U32[0]) & 0x3f0001ff) // ZBP PSM
	{
		m_env.CTXT[i].offset.zb = m_mem.GetOffset(ZBUF.Block(), m_env.CTXT[i].FRAME.FBW, ZBUF.PSM);
		m_env.CTXT[i].offset.fzb4 = m_mem.GetPixelOffset4(m_env.CTXT[i].FRAME, ZBUF);
	}

	m_env.CTXT[i].ZBUF = ZBUF;

	if (i == m_prev_env.PRIM.CTXT)
	{
		if (m_prev_env.CTXT[i].ZBUF != m_env.CTXT[i].ZBUF)
			m_dirty_gs_regs |= (1 << DIRTY_REG_ZBUF);
		else
			m_dirty_gs_regs &= ~(1 << DIRTY_REG_ZBUF);
	}

	if (i == PRIM->CTXT)
		UpdateAutoFlushKick();
}

void GSState::GIFRegHandlerBITBLTBUF(const GIFReg* RESTRICT r)
{
	// TODO: Paletted formats
	// There is a memory bug on the GS as it relates to the transfering of
	// 8-bit and 4-bit formats needing an even buffer width due to the
	// second half of the page being addressed by TBW/2
	//
	// namcoXcapcom: Apparently uses DBW of 5 and 11 (and refers to them
	// in TEX0 later as 4 and 10 respectively). However I can find no
	// documentation on this problem, nothing in the game to suggest
	// it is broken and the code here for it was likely incorrect to begin with.

	if (r->BITBLTBUF != m_env.BITBLTBUF)
		FlushWrite();

	m_env.BITBLTBUF = r->BITBLTBUF;
}

void GSState::GIFRegHandlerTRXPOS(const GIFReg* RESTRICT r)
{
	if (r->TRXPOS != m_env.TRXPOS)
		FlushWrite();

	m_env.TRXPOS = r->TRXPOS;
}

void GSState::GIFRegHandlerTRXREG(const GIFReg* RESTRICT r)
{
	if (r->TRXREG != m_env.TRXREG)
		FlushWrite();

	m_env.TRXREG = r->TRXREG;
}

void GSState::GIFRegHandlerTRXDIR(const GIFReg* RESTRICT r)
{
	FlushWrite();

	m_env.TRXDIR = r->TRXDIR;

	switch (m_env.TRXDIR.XDIR)
	{
		case 0: // host -> local
			m_tr.Init(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.BITBLTBUF, true);
			break;
		case 1: // local -> host
			m_tr.Init(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.BITBLTBUF, false);
			break;
		case 2: // local -> local
			CheckWriteOverlap(true, true);
			Move();
			break;
		default: // 3 deactivated as stated by manual. Tested on hardware and no transfers happen.
			break;
	}
}

void GSState::GIFRegHandlerHWREG(const GIFReg* RESTRICT r)
{
	// don't bother if not host -> local
	// real hw ignores
	if (m_env.TRXDIR.XDIR != 0)
		return;

	Write(reinterpret_cast<const u8*>(r), 8); // haunting ground
}

void GSState::Flush(GSFlushReason reason)
{
	FlushWrite();

	if (m_index.tail > 0)
	{
		m_state_flush_reason = reason;

		// Used to prompt the current draw that it's modifying its own CLUT.
		CheckCLUTValidity(m_prev_env.PRIM.PRIM);

		if (m_dirty_gs_regs)
		{
			m_draw_env = &m_prev_env;
			PRIM = &m_prev_env.PRIM;
			UpdateContext();

			FlushPrim();

			m_draw_env = &m_env;
			PRIM = &m_env.PRIM;
			UpdateContext();

			m_backed_up_ctx = -1;
		}
		else
		{
			FlushPrim();
		}

		m_dirty_gs_regs = 0;
		temp_draw_rect = GSVector4i::zero();
	}

	m_state_flush_reason = GSFlushReason::UNKNOWN;
}

void GSState::FlushWrite()
{
	if (!m_tr.write)
		return;

	const int len = m_tr.end - m_tr.start;

	if (len <= 0)
		return;

	GSVector4i r;

	r.left = m_env.TRXPOS.DSAX;
	r.top = m_env.TRXPOS.DSAY;
	r.right = r.left + m_env.TRXREG.RRW;
	r.bottom = r.top + m_env.TRXREG.RRH;

	// If the transfer ended before its declared total, the rect describes more
	// than was actually written. Some games (e.g. Star Wars - The Clone Wars)
	// set a huge rect, transfer only part of it, then kill the transfer; nuking
	// the whole declared rect would invalidate texture-cache data that was never
	// overwritten. Shrink the invalidation to the area the data actually covers.
	if (m_tr.end < m_tr.total && GSIsHardwareRenderer())
	{
		const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM];
		// Convert to nibbles then back to bytes after, in case trbpp is 4.
		const u32 in_data_pixel_count = (((len * 2) + ((psm_s.trbpp / 4) - 1)) / (psm_s.trbpp / 4));
		const u32 rect_pixel_count = r.width() * r.height();

		if (rect_pixel_count > in_data_pixel_count)
		{
			const int calculated_height = ((in_data_pixel_count + (r.width() - 1)) / r.width());

			// Just setting the height should be okay...
			r.w = pcsx2_max_i(r.y + calculated_height, psm_s.bs.y);

			if (DrawTransferCount() > 0 && m_env.BITBLTBUF.DBP == LastDrawTransfer().blit.DBP)
				LastDrawTransfer().rect = LastDrawTransfer().rect.runion(r);
		}
	}

	InvalidateVideoMem(m_env.BITBLTBUF, r);

	const GSLocalMemory::writeImage wi = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM].wi;

	wi(m_mem, m_tr.x, m_tr.y, &m_tr.buff[m_tr.start], len, m_env.BITBLTBUF, m_env.TRXPOS, m_env.TRXREG);

	m_tr.start += len;

	s_transfer_n++;
	if (m_tr.start >= m_tr.total)
		m_env.TRXDIR.XDIR = 3;
}

// This function decides if the context has changed in a way which warrants flushing the draw.
inline bool GSState::TestDrawChanged()
{
	// Check if PRIM has changed we need to check if it's just a different triangle or the context is changing.
	if (m_dirty_gs_regs & (1 << DIRTY_REG_PRIM))
	{
		u32 prim_mask = 0x7ff;

		if (GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) == GSUtil::GetPrimClass(m_env.PRIM.PRIM))
			prim_mask &= ~0x7;
		else
			return true;

		if ((m_env.PRIM.U32[0] ^ m_prev_env.PRIM.U32[0]) & prim_mask)
			return true;

		m_dirty_gs_regs &= ~(1 << DIRTY_REG_PRIM);

		// Shortcut, a bunch of games just change the prim reg
		if (!m_dirty_gs_regs)
			return false;
	}

	if ((m_dirty_gs_regs & ((1 << DIRTY_REG_TEST) | (1 << DIRTY_REG_SCISSOR) | (1 << DIRTY_REG_XYOFFSET) | (1 << DIRTY_REG_SCANMSK) | (1 << DIRTY_REG_DTHE))) || ((m_dirty_gs_regs & (1 << DIRTY_REG_DIMX)) && m_prev_env.DTHE.DTHE))
		return true;

	if (m_prev_env.PRIM.ABE && (m_dirty_gs_regs & ((1 << DIRTY_REG_ALPHA) | (1 << DIRTY_REG_PABE))))
		return true;

	if (m_prev_env.PRIM.FGE && (m_dirty_gs_regs & (1 << DIRTY_REG_FOGCOL)))
		return true;

	const int context = m_prev_env.PRIM.CTXT;
	const GSDrawingContext& ctx = m_prev_env.CTXT[context];
	// If the frame is getting updated check the FRAME, otherwise, we can ignore it
	if ((ctx.TEST.ATST != ATST_NEVER) || !ctx.TEST.ATE || (ctx.TEST.AFAIL & 1) || ctx.TEST.DATE)
	{
		if ((m_dirty_gs_regs & ((1 << DIRTY_REG_FRAME) | (1 << DIRTY_REG_COLCLAMP) | (1 << DIRTY_REG_FBA))))
			return true;
	}

	if ((ctx.TEST.ATST != ATST_NEVER) || !ctx.TEST.ATE || ctx.TEST.AFAIL == AFAIL_ZB_ONLY)
	{
		if (m_dirty_gs_regs & (1 << DIRTY_REG_ZBUF))
			return true;
	}

	if (m_prev_env.PRIM.TME)
	{
		if (m_dirty_gs_regs & ((1 << DIRTY_REG_TEX0) | (1 << DIRTY_REG_TEX1) | (1 << DIRTY_REG_CLAMP) | (1 << DIRTY_REG_TEXA)))
			return true;

		if(ctx.TEX1.MXL > 0 && (m_dirty_gs_regs & ((1 << DIRTY_REG_MIPTBP1) | (1 << DIRTY_REG_MIPTBP2))))
			return true;
	}

	m_dirty_gs_regs = 0;

	return false;
}

u32 GSState::CalcMask(int exp, int max_exp)
{
	const int amount = 9 + (max_exp - exp);

	return (1 << pcsx2_min_i(amount, 23)) - 1;
}

void GSState::FlushPrim()
{
	if (m_index.tail > 0)
	{
		// clear texture cache flushed flag, since we're reading from it
		m_texflush_flag = PRIM->TME ? false : m_texflush_flag;

		// internal frame rate detection based on sprite blits to the display framebuffer
		{
			const u32 FRAME_FBP = m_context->FRAME.FBP;
			if ((m_regs->DISP[0].DISPFB.FBP == FRAME_FBP && m_regs->PMODE.EN1) ||
				(m_regs->DISP[1].DISPFB.FBP == FRAME_FBP && m_regs->PMODE.EN2))
				m_disp_fb_sprite_blits++;
		}

		GSVertex buff[2];
		s_n++;

		const u32 head = m_vertex.head;
		const u32 tail = m_vertex.tail;
		const u32 next = m_vertex.next;
		u32 unused = 0;

		if (tail > head)
		{
			switch (PRIM->PRIM)
			{
				case GS_POINTLIST:
					break;
				case GS_LINELIST:
				case GS_LINESTRIP:
				case GS_SPRITE:
					unused = 1;
					buff[0] = m_vertex.buff[tail - 1];
					break;
				case GS_TRIANGLELIST:
				case GS_TRIANGLESTRIP:
					unused = pcsx2_min_u(tail - head, 2);
					memcpy(buff, &m_vertex.buff[tail - unused], sizeof(GSVertex) * 2);
					break;
				case GS_TRIANGLEFAN:
					buff[0] = m_vertex.buff[head];
					unused = 1;
					if (tail - 1 > head)
					{
						buff[1] = m_vertex.buff[tail - 1];
						unused = 2;
					}
					break;
				case GS_INVALID:
				default:
					break;
			}

		}

		m_vt.Update(m_vertex.buff, m_index.buff, m_vertex.tail, m_index.tail, GSUtil::GetPrimClass(PRIM->PRIM));

		// Texel coordinate rounding
		// Helps Manhunt (lights shining through objects).
		// Can help with some alignment issues when upscaling too, and is for both Software and Hardware renderers.
		// Sometimes hardware doesn't get affected, likely due to the difference in how GPU's handle textures (Persona minimap).
		if (PRIM->TME && (GSUtil::GetPrimClass(PRIM->PRIM) == GS_PRIM_CLASS::GS_SPRITE_CLASS || m_vt.m_eq.z))
		{
			if (!PRIM->FST) // STQ's
			{
				const bool is_sprite = GSUtil::GetPrimClass(PRIM->PRIM) == GS_PRIM_CLASS::GS_SPRITE_CLASS;
				// ST's have the lowest 9 bits (or greater depending on exponent difference) rounding down (from hardware tests).
				for (int i = m_index.tail - 1; i >= 0; i--)
				{
					GSVertex* v = &m_vertex.buff[m_index.buff[i]];

					// Only Q on the second vertex is valid
					if (!(i & 1) && is_sprite)
						v->RGBAQ.Q = m_vertex.buff[m_index.buff[i + 1]].RGBAQ.Q;

					int T = cpp11_bit_cast<int>(v->ST.T);
					int Q = cpp11_bit_cast<int>(v->RGBAQ.Q);
					int S = cpp11_bit_cast<int>(v->ST.S);
					const int expS = (S >> 23) & 0xff;
					const int expT = (T >> 23) & 0xff;
					const int expQ = (Q >> 23) & 0xff;
					int max_exp = pcsx2_max_i(expS, expQ);

					u32 mask = CalcMask(expS, max_exp);
					S &= ~mask;
					v->ST.S = cpp11_bit_cast<float>(S);
					max_exp = pcsx2_max_i(expT, expQ);
					mask = CalcMask(expT, max_exp);
					T &= ~mask;
					v->ST.T = cpp11_bit_cast<float>(T);
					Q &= ~0xff;

					if (!is_sprite || (i & 1))
						v->RGBAQ.Q = cpp11_bit_cast<float>(Q);

					m_vt.m_min.t.x = pcsx2_min_f(m_vt.m_min.t.x, (v->ST.S / v->RGBAQ.Q) * (1 << m_context->TEX0.TW));
					m_vt.m_min.t.y = pcsx2_min_f(m_vt.m_min.t.y, (v->ST.T / v->RGBAQ.Q) * (1 << m_context->TEX0.TH));
				}
			}
		}

		// Skip draw if Z test is enabled, but set to fail all pixels.
		const bool skip_draw = (m_context->TEST.ZTE && m_context->TEST.ZTST == ZTST_NEVER);

		if (!skip_draw)
		{
			if (m_af_seg_count && !m_ops->can_draw_runs(this))
				FlushSegments();
			else
				Draw();
		}

		m_index.tail = 0;
		m_vertex.head = 0;
		m_af_pixel_batch = true;
		m_af_skipped = false;
		m_af_seg_count = 0;
		m_af_seg_fresh = true;

		if (unused > 0)
		{
			memcpy(m_vertex.buff, buff, sizeof(GSVertex) * unused);

			m_vertex.tail = unused;
			m_vertex.next = next > head ? next - head : 0;

			// If it's a Triangle fan the XY buffer needs to be updated to point to the correct head vert
			// Jak 3 shadows get spikey (with autoflush) if you don't.
			if (PRIM->PRIM == GS_TRIANGLEFAN)
			{
				for (u32 i = 0; i < unused; i++)
				{
					GSVector4i* RESTRICT vert_ptr = (GSVector4i*)&m_vertex.buff[i];
					GSVector4i v = vert_ptr[1];
					v = v.xxxx().u16to32().sub32(m_xyof);
					v = v.blend32<12>(v.sra32<4>());
					m_vertex.xy[i & 3] = v;
					m_vertex.xy_tail = unused;
				}
			}
		}
		else
		{
			m_vertex.tail = 0;
			m_vertex.next = 0;
		}
	}
}
void GSState::CheckWriteOverlap(bool req_write, bool req_read)
{
	const GIFRegBITBLTBUF& blit = m_env.BITBLTBUF;
	gs_transfer_overlap o;
	int reason;

	o.blit        = &blit;
	o.pos         = &m_env.TRXPOS;
	o.reg         = &m_env.TRXREG;
	o.prev_ctx    = m_prev_env.CTXT[m_prev_env.PRIM.CTXT].Regs();
	o.prev_tme    = m_prev_env.PRIM.TME;
	o.draw_rect   = reinterpret_cast<const gs_recti*>(&temp_draw_rect);
	o.vertex      = m_vertex.buff;
	o.index       = m_index.buff;
	o.index_count = m_index.tail;
	o.fst         = PRIM->FST;
	o.tw          = m_context->TEX0.TW;
	o.th          = m_context->TEX0.TH;

	/* (one flush draws the queue: whatever else it touched is gone) */
	reason = gs_transfer_overlap_flush(&o, req_write, req_read);
	if (reason)
		Flush(static_cast<GSFlushReason>(reason));

	if (req_write)
	{
		/* The CLUT is invalid where the write crosses it */
		const GSVector4i write_rect = GSVector4i(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY,
			m_env.TRXPOS.DSAX + m_env.TRXREG.RRW, m_env.TRXPOS.DSAY + m_env.TRXREG.RRH);
		const u32 write_start_bp = GSLocalMemory::GetStartBlockAddress(blit.DBP, blit.DBW, blit.DPSM, write_rect);
		const u32 write_end_bp = ((GSLocalMemory::GetEndBlockAddress(blit.DBP, blit.DBW, blit.DPSM, write_rect) + 1) + (GS_BLOCKS_PER_PAGE - 1)) & ~(GS_BLOCKS_PER_PAGE - 1);
		m_mem.m_clut.InvalidateRange(write_start_bp, write_end_bp);
	}
}

void GSState::Write(const u8* mem, int len)
{
	if (m_env.TRXDIR.XDIR == 3)
		return;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;

	CheckWriteOverlap(true, false);

	if (!m_tr.Update(w, h, GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM].trbpp, len))
	{
		m_env.TRXDIR.XDIR = 3;
		return;
	}

	GIFRegBITBLTBUF& blit = m_tr.m_blit;
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[blit.DPSM];

	if (m_tr.end == 0)
	{
		GSVector4i r;

		r.left = m_env.TRXPOS.DSAX;
		r.top = m_env.TRXPOS.DSAY;
		r.right = r.left + m_env.TRXREG.RRW;
		r.bottom = r.top + m_env.TRXREG.RRH;

		s_last_transfer_draw_n = s_n;
		// Store the transfer for preloading new RT's.
		if ((DrawTransferCount() > 0 && blit.DBP == LastDrawTransfer().blit.DBP))
		{
			// Same BP, let's update the rect.
			GSUploadQueue& transfer = LastDrawTransfer();
			transfer.rect = transfer.rect.runion(r);
			transfer.draw = s_n;
			transfer.zero_clear = false;
		}
		else
		{
			GSUploadQueue new_transfer = { blit, r, s_n, false };
			PushDrawTransfer(new_transfer);
		}

		if (len >= m_tr.total)
		{
			// received all data in one piece, no need to buffer it
			InvalidateVideoMem(blit, r);

			psm.wi(m_mem, m_tr.x, m_tr.y, mem, m_tr.total, blit, m_env.TRXPOS, m_env.TRXREG);

			m_tr.start = m_tr.end = m_tr.total;

			s_transfer_n++;
			m_env.TRXDIR.XDIR = 3;
			return;
		}
	}

	memcpy(&m_tr.buff[m_tr.end], mem, len);

	m_tr.end += len;

	if (m_tr.end >= m_tr.total)
		FlushWrite();
}

void GSState::InitReadFIFO(u8* mem, int len)
{
	// No size or already a transfer in progress.
	if (len <= 0 || m_tr.total != 0)
		return;

	if (m_env.TRXDIR.XDIR == 3)
		return;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;

	const u16 bpp = GSLocalMemory::m_psm[m_env.BITBLTBUF.SPSM].trbpp;

	CheckWriteOverlap(false, true);

	if (!m_tr.Update(w, h, bpp, len))
		return;

	const int sx = m_env.TRXPOS.SSAX;
	const int sy = m_env.TRXPOS.SSAY;
	const GSVector4i r(sx, sy, sx + w, sy + h);

	if (m_tr.x == sx && m_tr.y == sy)
		InvalidateLocalMem(m_env.BITBLTBUF, r);

	// Read the image all in one go.
	m_mem.ReadImageX(m_tr.x, m_tr.y, m_tr.buff, m_tr.total, m_env.BITBLTBUF, m_env.TRXPOS, m_env.TRXREG);
}

// NOTE: called from outside MTGS
void GSState::Read(u8* mem, int len)
{
	if (len <= 0 || m_tr.total == 0)
		return;

	if (m_env.TRXDIR.XDIR == 3)
		return;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const u16 bpp = GSLocalMemory::m_psm[m_env.BITBLTBUF.SPSM].trbpp;

	CheckWriteOverlap(false, true);

	if (!m_tr.Update(w, h, bpp, len))
		return;

	/* (Update keeps it within the image, which the buffer holds) */
	gs_transfer_read_out(m_tr.buff, &m_tr.end, mem, len);

	if(m_tr.end >= m_tr.total)
		m_env.TRXDIR.XDIR = 3;
}

void GSState::MoveBase()
{
	// ffxii uses this to move the top/bottom of the scrolling menus offscreen and then blends them back over the text to create a shading effect
	// guitar hero copies the far end of the board to do a similar blend too
	s_transfer_n++;

	if (m_env.TRXDIR.XDIR == 3)
		return;

	const int sx = m_env.TRXPOS.SSAX;
	const int sy = m_env.TRXPOS.SSAY;
	const int dx = m_env.TRXPOS.DSAX;
	const int dy = m_env.TRXPOS.DSAY;
	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;

	InvalidateLocalMem(m_env.BITBLTBUF, GSVector4i(sx, sy, sx + w, sy + h));
	InvalidateVideoMem(m_env.BITBLTBUF, GSVector4i(dx, dy, dx + w, dy + h));

	const GSVector4i r(dx, dy, dx + w, dy + h);

	s_last_transfer_draw_n = s_n;
	// Store the transfer for preloading new RT's.
	if ((DrawTransferCount() > 0 && m_env.BITBLTBUF.DBP == LastDrawTransfer().blit.DBP))
	{
		// Same BP, let's update the rect.
		GSUploadQueue& transfer = LastDrawTransfer();
		transfer.rect = transfer.rect.runion(r);
		transfer.draw = s_n;
		transfer.zero_clear = false;
	}
	else
	{
		GSUploadQueue new_transfer = { m_env.BITBLTBUF, r, s_n, false };
		PushDrawTransfer(new_transfer);
	}

	/* the copy itself (GSTransfer.c) */
	gs_local_memory_move(&m_mem, &m_env.BITBLTBUF, &m_env.TRXPOS, &m_env.TRXREG);

	m_env.TRXDIR.XDIR = 3;
}

void GSState::SoftReset(u32 mask)
{
	if (mask & 1)
	{
		memset(&m_path[0], 0, sizeof(GIFPath));
		memset(&m_path[3], 0, sizeof(GIFPath));
	}

	if (mask & 2)
		memset(&m_path[1], 0, sizeof(GIFPath));

	if (mask & 4)
		memset(&m_path[2], 0, sizeof(GIFPath));

	m_env.TRXDIR.XDIR = 3; //-1 ; set it to invalid value

	m_q = 1.0f;
}

void GSState::ReadFIFO(u8* mem, int size)
{
	size *= 16;

	Read(mem, size);
}

void GSState::ReadLocalMemoryUnsync(u8* mem, int qwc, GIFRegBITBLTBUF BITBLTBUF, GIFRegTRXPOS TRXPOS, GIFRegTRXREG TRXREG)
{
	const int w = TRXREG.RRW;
	const int h = TRXREG.RRH;

	const u16 bpp = GSLocalMemory::m_psm[BITBLTBUF.SPSM].trbpp;

	/* (the bookkeeping only: the image is read through m_tr's buffer) */
	gs_transfer tb;

	memset(&tb, 0, sizeof(tb));
	if(m_tr.end >= m_tr.total || m_tr.write == true)
		gs_transfer_init(&tb, TRXPOS.SSAX, TRXPOS.SSAY, &BITBLTBUF, false);

	int len = qwc * 16;
	if (!gs_transfer_update(&tb, w, h, bpp, &len))
		return;

	if (m_tr.start == 0)
	{
		m_mem.ReadImageX(tb.x, tb.y, m_tr.buff, m_tr.total, BITBLTBUF, TRXPOS, TRXREG);
		m_tr.start += m_tr.total;
	}

	/* Read round the buffer from its start where it ends: the end can be
	 * past the image, and past the buffer */
	gs_transfer_read_out(m_tr.buff, &m_tr.end, mem, len);
}

void GSState::Transfer(const u8* mem, u32 size)
{
	GIFPath& path = m_path[3];

	while (size > 0)
	{
		if (path.nloop == 0)
		{
			path.SetTag(mem);

			mem += sizeof(GIFTag);
			size--;

			// eeuser 7.2.2. GIFtag:
			// "... when NLOOP is 0, the GIF does not output anything, and values other than the EOP field are disregarded."
			if (path.nloop > 0)
			{
				m_q = 1.0f;

				if (path.tag.PRE && path.tag.FLG == GIF_FLG_PACKED)
					ApplyPRIM(path.tag.PRIM);
			}
		}
		else
		{
			u32 total;

			switch (path.tag.FLG)
			{
				case GIF_FLG_PACKED:
					// get to the start of the loop
					if (path.reg != 0)
					{
						do
						{
							(this->*m_fpGIFPackedRegHandlers[path.GetReg()])((GIFPackedReg*)mem);

							mem += sizeof(GIFPackedReg);
							size--;
						} while (path.StepReg() && size > 0 && path.reg != 0);
					}

					// all data available? usually is

					total = path.nloop * path.nreg;

					if (size >= total)
					{
						size -= total;

						switch (path.type)
						{
							case GIFPath::TYPE_UNKNOWN:
							{
								u32 reg = 0;

								do
								{
									(this->*m_fpGIFPackedRegHandlers[path.GetReg(reg++)])((GIFPackedReg*)mem);

									mem += sizeof(GIFPackedReg);

									reg = reg & ((int)(reg - path.nreg) >> 31); // resets reg back to 0 when it becomes equal to path.nreg
								} while (--total > 0);
							}
							break;
							case GIFPath::TYPE_ADONLY: // very common
								do
								{
									(this->*m_fpGIFRegHandlers[((GIFPackedReg*)mem)->A_D.ADDR & 0x7F])(&((GIFPackedReg*)mem)->r);

									mem += sizeof(GIFPackedReg);
								} while (--total > 0);

								break;
							case GIFPath::TYPE_STQRGBAXYZF2: // majority of the vertices are formatted like this
								(this->*m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZF2])((GIFPackedReg*)mem, total);

								mem += total * sizeof(GIFPackedReg);

								break;
							case GIFPath::TYPE_STQRGBAXYZ2:
								(this->*m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZ2])((GIFPackedReg*)mem, total);

								mem += total * sizeof(GIFPackedReg);

								break;
							default:
								break;
						}

						path.nloop = 0;
					}
					else
					{
						do
						{
							(this->*m_fpGIFPackedRegHandlers[path.GetReg()])((GIFPackedReg*)mem);

							mem += sizeof(GIFPackedReg);
							size--;
						} while (path.StepReg() && size > 0);
					}

					break;
				case GIF_FLG_REGLIST:
					// TODO: do it similar to packed operation

					size *= 2;

					do
					{
						(this->*m_fpGIFRegHandlers[path.GetReg() & 0x7F])((GIFReg*)mem);

						mem += sizeof(GIFReg);
						size--;
					} while (path.StepReg() && size > 0);

					if (size & 1)
						mem += sizeof(GIFReg);

					size /= 2;

					break;
				case GIF_FLG_IMAGE2:
					// hmmm
					// Fall through here fixes a crash in Wallace and Gromit Project Zoo
					// and according to Pseudonym we shouldn't even land in this code. So hmm indeed. (rama)
				case GIF_FLG_IMAGE:
				{
					const int len = (int)pcsx2_min_i(size, path.nloop);

					switch (m_env.TRXDIR.XDIR)
					{
					case 0:
						Write(mem, len * 16);
						break;
					case 2:
						Move();
						break;
					default: // 1 and 3
						// 1 is invalid because downloads can only be done
						// with a reverse fifo operation (vif)
						// 3 is spec prohibited, it's behavior is not known
						// lets do nothing for now
						break;
					}

					mem += len * 16;
					path.nloop -= len;
					size -= len;

					break;
				}
				default:
					break;
			}
		}
	}
}

template <class T>
static void WriteState(u8*& dst, T* src, size_t len = sizeof(T))
{
	memcpy(dst, src, len);
	dst += len;
}

template <class T>
static void ReadState(T* dst, u8*& src, size_t len = sizeof(T))
{
	memcpy(dst, src, len);
	src += len;
}

int GSState::Freeze(freezeData* fd, bool sizeonly)
{
	if (sizeonly)
	{
		fd->size = GetSaveStateSize();
		return 0;
	}

	if (!fd->data || fd->size < GetSaveStateSize())
		return -1;

	Flush(GSFlushReason::SAVESTATE);

	/* The GPU's part of the state, which local memory does not hold: kept
	 * on the GPU for a load in this instance (runahead), and written into
	 * local memory for any other state, which another instance or another
	 * renderer may load. */
	if (GSSnapshotSave && m_ops && m_ops->snapshot_targets)
		m_ops->snapshot_targets(this, GSSnapshotKey);
	else
		ReadbackTextureCache();

	u8* data = fd->data;
	const u32 version = STATE_VERSION;

	WriteState(data, &version);
	WriteState(data, &m_env.PRIM);
	WriteState(data, &m_env.PRMODECONT);
	WriteState(data, &m_env.TEXCLUT);
	WriteState(data, &m_env.SCANMSK);
	WriteState(data, &m_env.TEXA);
	WriteState(data, &m_env.FOGCOL);
	WriteState(data, &m_env.DIMX);
	WriteState(data, &m_env.DTHE);
	WriteState(data, &m_env.COLCLAMP);
	WriteState(data, &m_env.PABE);
	WriteState(data, &m_env.BITBLTBUF);
	WriteState(data, &m_env.TRXDIR);
	WriteState(data, &m_env.TRXPOS);
	WriteState(data, &m_env.TRXREG);
	WriteState(data, &m_env.TRXREG); // obsolete

	for (int i = 0; i < 2; i++)
	{
		WriteState(data, &m_env.CTXT[i].XYOFFSET);
		WriteState(data, &m_env.CTXT[i].TEX0);
		WriteState(data, &m_env.CTXT[i].TEX1);
		WriteState(data, &m_env.CTXT[i].CLAMP);
		WriteState(data, &m_env.CTXT[i].MIPTBP1);
		WriteState(data, &m_env.CTXT[i].MIPTBP2);
		WriteState(data, &m_env.CTXT[i].SCISSOR);
		WriteState(data, &m_env.CTXT[i].ALPHA);
		WriteState(data, &m_env.CTXT[i].TEST);
		WriteState(data, &m_env.CTXT[i].FBA);
		WriteState(data, &m_env.CTXT[i].FRAME);
		WriteState(data, &m_env.CTXT[i].ZBUF);
	}

	WriteState(data, &m_v.RGBAQ);
	WriteState(data, &m_v.ST);
	WriteState(data, &m_v.UV);
	WriteState(data, &m_v.FOG);
	WriteState(data, &m_v.XYZ);
	memset(data, 0, sizeof(GIFReg)); // obsolete
	data += sizeof(GIFReg);
	WriteState(data, &m_tr.x);
	WriteState(data, &m_tr.y);
	WriteState(data, m_mem.m_vm8, m_mem.m_vmsize);

	for (GIFPath& path : m_path)
	{
		path.tag.NREG = path.nreg;
		path.tag.NLOOP = path.nloop;
		path.tag.REGS = 0;

		for (size_t j = 0; j < C89_ARRAY_SIZE(path.regs.U8); j++)
		{
			path.tag.U32[2 + (j >> 3)] |= path.regs.U8[j] << ((j & 7) << 2);
		}

		WriteState(data, &path.tag);
		WriteState(data, &path.reg);
	}

	WriteState(data, &m_q);

	return 0;
}

int GSState::Defrost(const freezeData* fd)
{
	if (!fd || !fd->data || fd->size == 0)
		return -1;

	if (fd->size < GetSaveStateSize())
		return -1;

	u8* data = fd->data;

	u32 version;

	ReadState(&version, data);

	if (version > STATE_VERSION)
	{
		log_cb(RETRO_LOG_ERROR, "GS: Savestate version is incompatible.  Load aborted.\n");
		return -1;
	}

	Flush(GSFlushReason::LOADSTATE);

	m_state_loading = true;
	Reset(true);
	m_state_loading = false;

	ReadState(&m_env.PRIM, data);

	if (version <= 6)
		data += sizeof(GIFRegPRMODE);

	ReadState(&m_env.PRMODECONT, data);
	ReadState(&m_env.TEXCLUT, data);
	ReadState(&m_env.SCANMSK, data);
	ReadState(&m_env.TEXA, data);
	ReadState(&m_env.FOGCOL, data);
	ReadState(&m_env.DIMX, data);
	ReadState(&m_env.DTHE, data);
	ReadState(&m_env.COLCLAMP, data);
	ReadState(&m_env.PABE, data);
	ReadState(&m_env.BITBLTBUF, data);
	ReadState(&m_env.TRXDIR, data);
	ReadState(&m_env.TRXPOS, data);
	ReadState(&m_env.TRXREG, data);
	ReadState(&m_env.TRXREG, data); // obsolete
	// Technically this value ought to be saved like m_tr.x/y (break
	// compatibility) but so far only a single game (Motocross Mania) really
	// depends on this value (i.e != BITBLTBUF) Savestates are likely done at
	// VSYNC, so not in the middle of a texture transfer, therefore register
	// will be set again properly
	m_tr.m_blit = m_env.BITBLTBUF;

	for (int i = 0; i < 2; i++)
	{
		ReadState(&m_env.CTXT[i].XYOFFSET, data);
		ReadState(&m_env.CTXT[i].TEX0, data);
		ReadState(&m_env.CTXT[i].TEX1, data);

		if (version <= 6)
			data += sizeof(GIFRegTEX2);

		ReadState(&m_env.CTXT[i].CLAMP, data);
		ReadState(&m_env.CTXT[i].MIPTBP1, data);
		ReadState(&m_env.CTXT[i].MIPTBP2, data);
		ReadState(&m_env.CTXT[i].SCISSOR, data);
		ReadState(&m_env.CTXT[i].ALPHA, data);
		ReadState(&m_env.CTXT[i].TEST, data);
		ReadState(&m_env.CTXT[i].FBA, data);
		ReadState(&m_env.CTXT[i].FRAME, data);
		ReadState(&m_env.CTXT[i].ZBUF, data);

		if (version <= 4)
			data += sizeof(u32) * 7; // skip
	}

	ReadState(&m_v.RGBAQ, data);
	ReadState(&m_v.ST, data);
	ReadState(&m_v.UV, data);
	ReadState(&m_v.FOG, data);
	ReadState(&m_v.XYZ, data);
	data += sizeof(GIFReg); // obsolite
	ReadState(&m_tr.x, data);
	ReadState(&m_tr.y, data);
	/* A page at a time: one that already holds what the state does is
	 * left alone, and the renderer hears which ones changed. */
	{
		u32 changed[GS_MAX_PAGES / 32];
		u32 page;

		memset(changed, 0, sizeof(changed));
		for (page = 0; page < GS_MAX_PAGES; page++)
		{
			u8* const dst = m_mem.m_vm8 + page * GS_PAGE_SIZE;
			if (memcmp(dst, data, GS_PAGE_SIZE) != 0)
			{
				memcpy(dst, data, GS_PAGE_SIZE);
				changed[page >> 5] |= 1u << (page & 31);
			}
			data += GS_PAGE_SIZE;
		}
		m_mem_restores++;
		if (m_ops && m_ops->mem_restored)
			m_ops->mem_restored(this, changed);
	}

	m_tr.total = 0; // TODO: restore transfer state

	for (GIFPath& path : m_path)
	{
		ReadState(&path.tag, data);
		ReadState(&path.reg, data);

		path.SetTag(&path.tag); // expand regs

		/* SetTag leaves an idle path's registers as they were, and Freeze
		 * writes them back: an idle path takes the state's too, so the
		 * state saved after a load is the state that was loaded. */
		if (path.nloop == 0)
		{
			const GSVector4i v = GSVector4i::loadl(&path.tag.REGS);
			path.nreg = (path.tag.U32[1] & 0xf0000000) ? (path.tag.U32[1] >> 28) : 16;
			path.regs = v.upl8(v >> 4) & GSVector4i::x0f(path.nreg);
		}
	}

	ReadState(&m_q, data);

	// Before m_prev_env is taken, so the draw environment gets the
	// normalised values too, and before the scissor and offset derivation
	// below, which would otherwise be computed from raw ones.
	NormalizeRestoredRegs();

	m_prev_env = m_env;
	PRIM = &m_env.PRIM;

	UpdateContext();

	UpdateVertexKick();

	for (u32 i = 0; i < 2; i++)
	{
		m_env.CTXT[i].UpdateScissor();

		m_env.CTXT[i].offset.fb = m_mem.GetOffset(m_env.CTXT[i].FRAME.Block(), m_env.CTXT[i].FRAME.FBW, m_env.CTXT[i].FRAME.PSM);
		m_env.CTXT[i].offset.zb = m_mem.GetOffset(m_env.CTXT[i].ZBUF.Block(), m_env.CTXT[i].FRAME.FBW, m_env.CTXT[i].ZBUF.PSM);
		m_env.CTXT[i].offset.fzb4 = m_mem.GetPixelOffset4(m_env.CTXT[i].FRAME, m_env.CTXT[i].ZBUF);
	}

	UpdateScissor();

	// Force CLUT to be reloaded.
	m_mem.m_clut.Reset();
	(PRIM->CTXT == 0) ? ApplyTEX0<0>(m_context->TEX0) : ApplyTEX0<1>(m_context->TEX0);

	ResetPCRTC();

	return 0;
}

/* A Q that a restore brought back, made into one the handlers could have
 * produced: a zero becomes the substitute that handler uses, and a NaN
 * becomes FLT_MAX. The zero test is on the bit pattern, not the value,
 * because that is what the handlers do -- so a negative zero is left alone
 * here exactly as it is there. */
static float ScrubRestoredQ(float q, float zero_substitute)
{
	u32 bits;
	memcpy(&bits, &q, sizeof(bits));
	if (bits == 0)
		return zero_substitute;
	if (q != q)
		return FLT_MAX;
	return q;
}

/* Every GS register a game writes goes through a GIFRegHandler, and a good
 * many of those handlers do more than store the value: they mask a field to
 * its real width, clamp it to a range the rest of the emulator relies on,
 * substitute a value a particular game depends on, or maintain an invariant
 * between two registers. Defrost restores registers with a raw copy, so none
 * of that happens, and every one of those guarantees is silently void for the
 * rest of the session.
 *
 * That is not a theoretical gap. The ALPHA clamp below is the only thing
 * keeping a blend index inside an 81-entry table, and the two Q scrubs exist
 * because named games emit a zero or a NaN there; a state file put all three
 * straight back. Two registers were being re-normalised by hand here, which
 * is how the ALPHA one came to be noticed and the other nine did not.
 *
 * So when a handler transforms a value, this has to transform it the same
 * way, and a new handler that does needs a line here. What it deliberately
 * does NOT do is improve on the handlers: the Q scrubs compare the bit
 * pattern rather than the float, so a negative zero passes through both here
 * and there, and that stays true until someone changes the handler. */
void GSState::NormalizeRestoredRegs()
{
	for (u32 i = 0; i < 2; i++)
	{
		GSDrawingContext& ctx = m_env.CTXT[i];

		// GIFRegHandlerXYOFFSET: OFX/OFY are declared u32, and the header
		// states the handler is what keeps the pad bits clear.
		ctx.XYOFFSET.OFX &= 0xffff;
		ctx.XYOFFSET.OFY &= 0xffff;

		// GIFRegHandlerALPHA: clamped to 2 because GSRendererHW builds a
		// blend index as ((A * 3 + B) * 3 + C) * 3 + D against an 81-entry
		// table. These are 2-bit fields, so a state holding 3 in each
		// reaches 120 and reads thirty-nine entries past the end.
		ctx.ALPHA.A = pcsx2_min_i(ctx.ALPHA.A, 2U);
		ctx.ALPHA.B = pcsx2_min_i(ctx.ALPHA.B, 2U);
		ctx.ALPHA.C = pcsx2_min_i(ctx.ALPHA.C, 2U);
		ctx.ALPHA.D = pcsx2_min_i(ctx.ALPHA.D, 2U);

		// GIFRegHandlerFRAME: FBW is clamped to 32. The field is 6 bits, so
		// a state can ask for 63, and FRAME.FBW * 64 is a target width the
		// texture cache allocates from.
		ctx.FRAME.FBW = (32U < ctx.FRAME.FBW) ? 32U : ctx.FRAME.FBW;

		// GIFRegHandlerFRAME: the three formats that address only part of a
		// 32-bit pixel are rewritten to PSMCT32 with a mask that spares the
		// rest. Berserk depends on the first of them. A saved FRAME should
		// already hold the substituted value, so this is a no-op for any
		// state the handler produced.
		switch (ctx.FRAME.PSM)
		{
			case PSMT8H:
				ctx.FRAME.PSM   = PSMCT32;
				ctx.FRAME.FBMSK = 0x00FFFFFF;
				break;
			case PSMT4HH:
				ctx.FRAME.PSM   = PSMCT32;
				ctx.FRAME.FBMSK = 0x0FFFFFFF;
				break;
			case PSMT4HL:
				ctx.FRAME.PSM   = PSMCT32;
				ctx.FRAME.FBMSK = 0xF0FFFFFF;
				break;
			default:
				break;
		}

		// GIFRegHandlerFRAME and GIFRegHandlerZBUF both maintain this: on
		// hardware, a Z-format FRAME forces the Z buffer to colour
		// swizzling. Powerdrome clears Z by relying on it. Each handler
		// re-establishes it on every write to either register, so the pair
		// is never seen uncoupled -- except after a restore, which brings
		// them back independently and then derives offset.zb and
		// offset.fzb4 from the mismatch.
		if ((ctx.FRAME.PSM & 0x30) == 0x30)
			ctx.ZBUF.PSM &= ~0x30;
		else
			ctx.ZBUF.PSM |= 0x30;

		// ApplyTEX0: CPSM is restricted to 1010b. Defrost calls ApplyTEX0
		// for the active context only, and after m_prev_env is taken, so
		// neither copy of the other context was ever masked.
		ctx.TEX0.CPSM &= 0xa;
	}

	// GIFPackedRegHandlerUV and GIFRegHandlerUV: UV is 10.4 fixed point in
	// 14 bits. GSRegs.h states these handlers are what keeps the pad bits
	// clear, and a raw restore falsifies that.
	m_v.UV &= 0x3fff3fff;

	// GIFRegHandlerRGBAQ and GIFPackedRegHandlerSTQ scrub Q on the way in,
	// both for named games: Silent Hill emits a NaN, which breaks the
	// min/max in GSVertexTrace, and Vexx emits a zero, which becomes a
	// division by zero further down. A state file reintroduced both, past
	// everything downstream that was written to assume they were gone.
	// Substituting on the bit pattern rather than the value, as the two
	// handlers do.
	m_v.RGBAQ.Q = ScrubRestoredQ(m_v.RGBAQ.Q, 1.0f);
	m_q         = ScrubRestoredQ(m_q, FLT_MIN);

	// GIFRegHandlerSCANMSK latches this when the register arrives; Reset
	// cleared it and the raw restore of SCANMSK does not set it again.
	if (m_env.SCANMSK.MSK & 2)
		m_scanmask_used = 2;
}

//

void GSState::UpdateContext()
{
	m_context = const_cast<GSDrawingContext*>(&m_draw_env->CTXT[PRIM->CTXT]);

	UpdateScissor();
	UpdateAutoFlushKick();
}

void GSState::UpdateScissor()
{
	m_scissor_cull_min = m_context->scissor.cull.xyxy();
	m_scissor_cull_max = m_context->scissor.cull.zwzw();
	m_xyof = m_context->scissor.xyof;
	m_scissor_invalid = !m_context->scissor.in.gt32(m_context->scissor.in.zwzw()).allfalse();
}

void GSState::UpdateVertexKick()
{
	const u32 prim = PRIM->PRIM;
	const int af = (m_autoflush_kick = IsAutoFlushDraw()) ? 1 : 0;

	m_fpGIFPackedRegHandlers[GIF_REG_XYZF2] = m_fpGIFPackedRegHandlerXYZ[af][prim][0];
	m_fpGIFPackedRegHandlers[GIF_REG_XYZF3] = m_fpGIFPackedRegHandlerXYZ[af][prim][1];
	m_fpGIFPackedRegHandlers[GIF_REG_XYZ2] = m_fpGIFPackedRegHandlerXYZ[af][prim][2];
	m_fpGIFPackedRegHandlers[GIF_REG_XYZ3] = m_fpGIFPackedRegHandlerXYZ[af][prim][3];

	m_fpGIFRegHandlers[GIF_A_D_REG_XYZF2] = m_fpGIFRegHandlerXYZ[af][prim][0];
	m_fpGIFRegHandlers[GIF_A_D_REG_XYZF3] = m_fpGIFRegHandlerXYZ[af][prim][1];
	m_fpGIFRegHandlers[GIF_A_D_REG_XYZ2] = m_fpGIFRegHandlerXYZ[af][prim][2];
	m_fpGIFRegHandlers[GIF_A_D_REG_XYZ3] = m_fpGIFRegHandlerXYZ[af][prim][3];

	m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZF2] = m_fpGIFPackedRegHandlerSTQRGBAXYZF2[af][prim];
	m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZ2] = m_fpGIFPackedRegHandlerSTQRGBAXYZ2[af][prim];
}

/* After a write to a register IsAutoFlushDraw reads: the handlers change
 * only when the answer does. */
void GSState::UpdateAutoFlushKick()
{
	if (IsAutoFlushDraw() != m_autoflush_kick)
		UpdateVertexKick();
}

void GSState::GrowVertexBuffer()
{
	const u32 maxcount = pcsx2_max_u(m_vertex.maxcount * 3 / 2, 10000);

	GSVertex* vertex = static_cast<GSVertex*>(memalign_alloc(32, sizeof(GSVertex) * maxcount));
	// Worst case index list is a list of points with vs expansion, 6 indices per point
	u16* index = static_cast<u16*>(memalign_alloc(32, sizeof(u16) * maxcount * 6));

	if (m_vertex.buff)
	{
		memcpy(vertex, m_vertex.buff, sizeof(GSVertex) * m_vertex.tail);

		memalign_free(m_vertex.buff);
	}

	if (m_index.buff)
	{
		memcpy(index, m_index.buff, sizeof(u16) * m_index.tail);

		memalign_free(m_index.buff);
	}

	m_vertex.buff = vertex;
	m_vertex.maxcount = maxcount - 3; // -3 to have some space at the end of the buffer before DrawingKick can grow it
	m_index.buff = index;
}

static constexpr u32 NumIndicesForPrim(u32 prim)
{
	switch (prim)
	{
		case GS_POINTLIST:
		case GS_INVALID:
			return 1;
		case GS_LINELIST:
		case GS_SPRITE:
		case GS_LINESTRIP:
			return 2;
		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
			return 3;
		default:
			return 0;
	}
}

static constexpr u32 MaxVerticesForPrim(u32 prim)
{
	switch (prim)
	{
		// Four indices per 1 vertex.
		case GS_POINTLIST:
		case GS_INVALID:

		// Indices are shifted left by 2 to form quads.
		case GS_LINELIST:
		case GS_LINESTRIP:
			return (0xFFFF / 4) - 4;

		// Four indices per two vertices.
		case GS_SPRITE:
			return (0xFFFF / 2) - 2;

		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
		default:
			return (0xFFFF - 3);
	}
}

__forceinline void GSState::CheckCLUTValidity(u32 prim)
{
	if (m_mem.m_clut.IsInvalid() & 2)
		return;

	u32 n = NumIndicesForPrim(prim);

	const GSDrawingContext& ctx = m_prev_env.CTXT[m_prev_env.PRIM.CTXT];
	if ((m_index.tail > 0 || (m_vertex.tail == n - 1)) && (GSLocalMemory::m_psm[ctx.TEX0.PSM].pal == 0 || !m_prev_env.PRIM.TME))
	{
		const GSLocalMemory::psm_t& fpsm = GSLocalMemory::m_psm[ctx.FRAME.PSM];
		const bool frame_needed = !(ctx.TEST.ATE && ctx.TEST.ATST == 0 && ctx.TEST.AFAIL == 2) && ((ctx.FRAME.FBMSK & fpsm.fmsk) != fpsm.fmsk);
		if (frame_needed && GSLocalMemory::m_psm[m_mem.m_clut.GetCLUTCPSM()].bpp == fpsm.bpp)
		{
			const u32 startbp = gs_swizzle_bn(&fpsm.info, temp_draw_rect.x, temp_draw_rect.y, ctx.FRAME.Block(), ctx.FRAME.FBW);

			// If it's a point, then we only have one coord, so the address for start and end will be the same, which is bad for the following check.
			u32 endbp = startbp;
			// otherwise calculate the end.
			if (prim != GS_POINTLIST || (m_index.tail > 1))
				endbp = gs_swizzle_bn(&fpsm.info, temp_draw_rect.z - 1, temp_draw_rect.w - 1, ctx.FRAME.Block(), ctx.FRAME.FBW);

			m_mem.m_clut.InvalidateRange(startbp, endbp, true);
		}
	}
}

/* An overlap found: a batch of primitives all reading only their own
 * pixels takes it in the draw; anything else flushes before the primitive. */
void GSState::AutoFlushOverlap(bool pixel)
{
	if (pixel && m_af_pixel_batch && m_af_seg_count == 0)
	{
		m_af_skipped = true;
		return;
	}
	/* A batch drawn in place cannot take segments read from a copy. */
	if (m_ops && m_ops->can_draw_runs && !m_af_skipped && m_af_seg_count < AF_MAX_SEGMENTS)
	{
		m_af_seg_start[m_af_seg_count++] = m_index.tail;
		m_af_seg_fresh = true;
		m_af_pixel_batch = false;
		return;
	}
	Flush(GSFlushReason::AUTOFLUSH);
	m_af_pixel_batch = pixel;
}

/* The batch one segment at a time, each with its own vertex trace, as
 * flushing at each boundary drew it: a segment's vertices are moved to
 * the front of the buffer and its indices rebased onto them, since a
 * draw may rewrite every vertex it is given. */
void GSState::FlushSegments()
{
	const u32 total = m_index.tail;
	const u32 nverts = m_vertex.tail;
	const u32 segs = m_af_seg_count;
	const GS_PRIM_CLASS pc = GSUtil::GetPrimClass(PRIM->PRIM);
	u16* const saved_index = static_cast<u16*>(malloc(sizeof(u16) * total));
	GSVertex* const saved_vertex = static_cast<GSVertex*>(malloc(sizeof(GSVertex) * nverts));
	u32 start = 0;
	u32 end;
	u32 i;
	u32 k;
	u32 vmin;
	u32 vmax;

	m_af_seg_count = 0;
	if (!saved_index || !saved_vertex)
	{
		free(saved_index);
		free(saved_vertex);
		Draw();
		return;
	}
	memcpy(saved_index, m_index.buff, sizeof(u16) * total);
	memcpy(saved_vertex, m_vertex.buff, sizeof(GSVertex) * nverts);
	for (i = 0; i <= segs; i++)
	{
		end = (i < segs) ? m_af_seg_start[i] : total;
		if (end > start)
		{
			vmin = saved_index[start];
			vmax = saved_index[start];
			for (k = start + 1; k < end; k++)
			{
				if (saved_index[k] < vmin)
					vmin = saved_index[k];
				if (saved_index[k] > vmax)
					vmax = saved_index[k];
			}
			memcpy(m_vertex.buff, saved_vertex + vmin, sizeof(GSVertex) * (vmax - vmin + 1));
			for (k = start; k < end; k++)
				m_index.buff[k - start] = static_cast<u16>(saved_index[k] - vmin);
			m_vertex.head = 0;
			m_vertex.tail = vmax - vmin + 1;
			m_vertex.next = m_vertex.tail;
			m_index.tail = end - start;
			m_vt.Update(m_vertex.buff, m_index.buff, m_vertex.tail, m_index.tail, pc);
			if (start)
				s_n++;
			Draw();
		}
		start = end;
	}
	free(saved_index);
	free(saved_vertex);
}

template<u32 prim, bool index_swap>
__noinline void GSState::HandleAutoFlush()
{
	// To briefly explain what's going on here, what we are checking for is draws over a texture when the source and destination are themselves.
	// Because one page of the texture gets buffered in the Texture Cache (the PS2's one) if any of those pixels are overwritten, you still read the old data.
	// So we need to calculate if a page boundary is being crossed for the format it is in and if the same part of the texture being written and read inside the draw.
	if (IsAutoFlushDraw())
	{
		int  n = 1;
		u32 buff[3];
		const u32 head = m_vertex.head;
		const u32 tail = m_vertex.tail;

		switch (prim)
		{
			case GS_POINTLIST:
				buff[0] = tail - 1;
				n = 1;
				break;
			case GS_LINELIST:
			case GS_LINESTRIP:
			case GS_SPRITE:
				buff[0] = tail - 1;
				n = 2;
				break;
			case GS_TRIANGLELIST:
			case GS_TRIANGLESTRIP:
				buff[0] = tail - 2;
				buff[1] = tail - 1;
				n = 3;
				break;
			case GS_TRIANGLEFAN:
				buff[0] = head;
				buff[1] = tail - 1;
				n = 3;
				break;
			case GS_INVALID:
			default:
				break;
		}

		const bool pixel = m_pixel_feedback_ok && IsPixelFeedbackPrim(prim, buff, n);
		if (m_index.tail == 0)
		{
			m_af_pixel_batch = pixel;
			return;
		}
		if (!pixel)
		{
			/* Overlaps let through earlier are drawn in place, which this
			 * one cannot be: they go first. */
			if (m_af_skipped)
			{
				Flush(GSFlushReason::AUTOFLUSH);
				m_af_pixel_batch = false;
				return;
			}
			m_af_pixel_batch = false;
		}

		// Kind of a cheat, making the assumption that 2 consecutive fan/strip triangles won't overlap each other (*should* be safe)
		if ((m_index.tail & 1) && (prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLEFAN) && !m_texflush_flag)
			return;

		GSVector4i tex_coord;
		// Prepare the currently processed vertex.
		if (PRIM->FST)
		{
			tex_coord.x = m_v.U >> 4;
			tex_coord.y = m_v.V >> 4;
		}
		else
		{
			const float s = pcsx2_min_f((m_v.ST.S / m_v.RGBAQ.Q), 1.0f);
			const float t = pcsx2_min_f((m_v.ST.T / m_v.RGBAQ.Q), 1.0f);

			tex_coord.x = f32_to_s32_sat((1 << m_context->TEX0.TW) * s);
			tex_coord.y = f32_to_s32_sat((1 << m_context->TEX0.TH) * t);
		}

		GSVector4i tex_rect = tex_coord.xyxy();

		const GSLocalMemory::psm_t tex_psm = GSLocalMemory::m_psm[m_context->TEX0.PSM];
		const GSLocalMemory::psm_t frame_psm = GSLocalMemory::m_psm[m_context->FRAME.PSM];
		// Get the rest of the rect.
		for (int i = 0; i < (n - 1); i++)
		{
			const GSVertex* v = &m_vertex.buff[buff[i]];

			if (PRIM->FST)
			{
				tex_coord.x = v->U >> 4;
				tex_coord.y = v->V >> 4;
			}
			else
			{
				const float s = pcsx2_min_f((v->ST.S / v->RGBAQ.Q), 1.0f);
				const float t = pcsx2_min_f((v->ST.T / v->RGBAQ.Q), 1.0f);

				tex_coord.x = f32_to_s32_sat(roundf((1 << m_context->TEX0.TW) * s));
				tex_coord.y = f32_to_s32_sat(roundf((1 << m_context->TEX0.TH) * t));
			}

			tex_rect.x = pcsx2_min_f(tex_rect.x, tex_coord.x);
			tex_rect.z = pcsx2_max_f(tex_rect.z, tex_coord.x);
			tex_rect.y = pcsx2_min_f(tex_rect.y, tex_coord.y);
			tex_rect.w = pcsx2_max_f(tex_rect.w, tex_coord.y);
		}

		// If the draw was 1 line thick, make it larger as rects are exclusive of ends.
		if (tex_rect.x == tex_rect.z)
			tex_rect += GSVector4i::cxpr(0, 0, 1, 0);
		if (tex_rect.y == tex_rect.w)
			tex_rect += GSVector4i::cxpr(0, 0, 0, 1);

		const bool swap_index = index_swap && GSUtil::GetPrimClass(m_prev_env.PRIM.PRIM) != GS_SPRITE_CLASS;
		// Get the last texture position from the last draw.
		const GSVertex* v = &m_vertex.buff[m_index.buff[m_index.tail - (swap_index ? n : 1)]];

		if (PRIM->FST)
		{
			tex_coord.x = v->U >> 4;
			tex_coord.y = v->V >> 4;
		}
		else
		{
			const float s = pcsx2_min_f((v->ST.S / v->RGBAQ.Q), 1.0f);
			const float t = pcsx2_min_f((v->ST.T / v->RGBAQ.Q), 1.0f);

			tex_coord.x = f32_to_s32_sat(roundf((1 << m_context->TEX0.TW) * s));
			tex_coord.y = f32_to_s32_sat(roundf((1 << m_context->TEX0.TH) * t));
		}

		const int clamp_minu = m_context->CLAMP.MINU;
		const int clamp_maxu = m_context->CLAMP.MAXU;
		const int clamp_minv = m_context->CLAMP.MINV;
		const int clamp_maxv = m_context->CLAMP.MAXV;

		switch (m_context->CLAMP.WMS)
		{
			case CLAMP_REGION_CLAMP:
				tex_rect.x = pcsx2_max_f(tex_rect.x, clamp_minu);
				tex_rect.z = pcsx2_max_f(tex_rect.z, clamp_minu);
				tex_coord.x = pcsx2_max_f(tex_coord.x, clamp_minu);
				tex_rect.x = pcsx2_min_f(tex_rect.x, clamp_maxu);
				tex_rect.z = pcsx2_min_f(tex_rect.z, clamp_maxu);
				tex_coord.x = pcsx2_min_f(tex_coord.x, clamp_maxu);
				break;

			case CLAMP_REGION_REPEAT:
				tex_rect.x = pcsx2_max_f(tex_rect.x, clamp_maxu);
				tex_rect.z = pcsx2_max_f(tex_rect.z, clamp_maxu);
				tex_coord.x = pcsx2_max_f(tex_coord.x, clamp_maxu);
				tex_rect.x = pcsx2_min_f(tex_rect.x, (clamp_maxu | clamp_minu));
				tex_rect.z = pcsx2_min_f(tex_rect.z, (clamp_maxu | clamp_minu));
				tex_coord.x = pcsx2_min_f(tex_coord.x, (clamp_maxu | clamp_minu));
				break;
			default:
				break;
		}

		switch (m_context->CLAMP.WMT)
		{
			case CLAMP_REGION_CLAMP:
				tex_rect.y = pcsx2_max_f(tex_rect.y, clamp_minv);
				tex_rect.w = pcsx2_max_f(tex_rect.w, clamp_minv);
				tex_coord.y = pcsx2_max_f(tex_coord.y, clamp_minv);
				tex_rect.y = pcsx2_min_f(tex_rect.y, clamp_maxv);
				tex_rect.w = pcsx2_min_f(tex_rect.w, clamp_maxv);
				tex_coord.y = pcsx2_min_f(tex_coord.y, clamp_maxv);
				break;
			case CLAMP_REGION_REPEAT:
				tex_rect.y = pcsx2_max_f(tex_rect.y, clamp_maxv);
				tex_rect.w = pcsx2_max_f(tex_rect.w, clamp_maxv);
				tex_coord.y = pcsx2_max_f(tex_coord.y, clamp_maxv);
				tex_rect.y = pcsx2_min_f(tex_rect.y, (clamp_maxv | clamp_minv));
				tex_rect.w = pcsx2_min_f(tex_rect.w, (clamp_maxv | clamp_minv));
				tex_coord.y = pcsx2_min_f(tex_coord.y, (clamp_maxv | clamp_minv));
				break;
			default:
				break;
		}

		// Nothing being drawn intersect with the new texture, so no point in checking further.
		// What the segments before this one drew, this one reads after.
		if (tex_psm.depth == frame_psm.depth && tex_rect.rintersect(m_af_seg_draw).rempty())
			return;
		if (m_texflush_flag)
		{
			AutoFlushOverlap(pixel);
			return;
		}

		const int tex_page_mask_x = ~(tex_psm.pgs.x - 1);
		const int tex_page_mask_y = ~(tex_psm.pgs.y - 1);
		const GSVector4i tex_page_mask = { tex_page_mask_x, tex_page_mask_y, tex_page_mask_x, tex_page_mask_y };
		const GSVector4i last_tex_page = tex_coord.xyxy() & tex_page_mask;
		const GSVector4i tex_page = tex_rect.xyxy() & tex_page_mask;

		// Crossed page since last draw end
		if (!tex_page.eq(last_tex_page))
		{
			// Make sure the format matches, otherwise the coordinates aren't gonna match, so the draws won't intersect.
			if (tex_psm.bpp == frame_psm.bpp && (m_context->FRAME.FBW == m_context->TEX0.TBW))
			{
				const GSVector2i offset = GSVector2i(m_context->XYOFFSET.OFX, m_context->XYOFFSET.OFY);
				const GSVector4i scissor = m_context->scissor.in;
				GSVector4i old_draw_rect = GSVector4i::zero();
				int current_draw_end = m_index.tail;
				const int seg_begin = m_af_seg_count ? static_cast<int>(m_af_seg_start[m_af_seg_count - 1]) : 0;

				while (current_draw_end >= seg_begin + n)
				{
					for (int i = current_draw_end - 1; i >= current_draw_end - n; i--)
					{
						const GSVertex* v = &m_vertex.buff[m_index.buff[i]];

						if (prim == GS_SPRITE && (i & 1))
						{
							tex_coord.x = ((static_cast<int>(v->XYZ.X) - offset.x) >> 4) - 1;
							tex_coord.y = ((static_cast<int>(v->XYZ.Y) - offset.y) >> 4) - 1;
						}
						else
						{
							tex_coord.x = (static_cast<int>(v->XYZ.X) - offset.x) >> 4;
							tex_coord.y = (static_cast<int>(v->XYZ.Y) - offset.y) >> 4;
						}

						if (tex_psm.depth != frame_psm.depth)
						{
							tex_coord.x ^= (frame_psm.pgs.x / 2);
							tex_coord.y ^= (frame_psm.pgs.y / 2);
						}

						if (prim == GS_SPRITE && (i & 1))
						{
							tex_coord.x += 1;
							tex_coord.y += 1;
						}

						if (i == (current_draw_end - 1))
						{
							old_draw_rect = tex_coord.xyxy();
						}
						else
						{
							old_draw_rect.x = pcsx2_min_f(old_draw_rect.x, tex_coord.x);
							old_draw_rect.z = pcsx2_max_f(old_draw_rect.z, tex_coord.x);
							old_draw_rect.y = pcsx2_min_f(old_draw_rect.y, tex_coord.y);
							old_draw_rect.w = pcsx2_max_f(old_draw_rect.w, tex_coord.y);
						}
					}

					if (old_draw_rect.x == old_draw_rect.z)
						old_draw_rect += GSVector4i::cxpr(0, 0, 1, 0);
					if (old_draw_rect.y == old_draw_rect.w)
						old_draw_rect += GSVector4i::cxpr(0, 0, 0, 1);

					old_draw_rect = tex_rect.rintersect(old_draw_rect);
					if (!old_draw_rect.rintersect(scissor).rempty())
					{
						AutoFlushOverlap(pixel);
						return;
					}

					current_draw_end -= n;
				}
			}
			else // Storage of the TEX and FRAME/Z is different, so uhh, just fall back to flushing each page. It's slower, sorry.
			{
				const int frame_width = (m_context->FRAME.FBW * 64) / frame_psm.pgs.x;
				const int tex_width = (m_context->TEX0.TBW * 64) / tex_psm.pgs.x;
				if ((frame_width == tex_width) || ((tex_rect.w / tex_psm.pgs.y) <= 1 && frame_width >= tex_width))
				{
					tex_rect += GSVector4i(0, 0, tex_page_mask.z, tex_page_mask.w); // round up to the next page as we will be comparing by page.
					//We know we've changed page, so let's set the dimension to cover the page they're in (for different pixel orders)
					tex_rect &= tex_page_mask;
					tex_rect = GSVector4i(tex_rect.x / tex_psm.pgs.x, tex_rect.y / tex_psm.pgs.y, tex_rect.z / tex_psm.pgs.x, tex_rect.w / tex_psm.pgs.y);
					
					const int frame_page_mask_x = ~(frame_psm.pgs.x - 1);
					const int frame_page_mask_y = ~(frame_psm.pgs.y - 1);
					const GSVector4i frame_page_mask = { frame_page_mask_x, frame_page_mask_y, frame_page_mask_x, frame_page_mask_y };
					GSVector4i area_out = m_af_seg_draw;
					area_out += GSVector4i(0, 0, frame_page_mask.z, frame_page_mask.w); // round up to the next page as we will be comparing by page.
					area_out &= frame_page_mask;
					area_out = GSVector4i(area_out.x / frame_psm.pgs.x, area_out.y / frame_psm.pgs.y, area_out.z / frame_psm.pgs.x, area_out.w / frame_psm.pgs.y);

					if (!area_out.rintersect(tex_rect).rempty())
						AutoFlushOverlap(pixel);
				}
				else // Formats are too different so just flush it.
					AutoFlushOverlap(pixel);
			}
		}
	}
}

template <u32 prim, bool auto_flush, bool index_swap>
__forceinline void GSState::VertexKick(u32 skip, u32& maxcount)
{
	constexpr u32 n = NumIndicesForPrim(prim);

	if constexpr (prim == GS_INVALID)
	{
		m_vertex.tail = m_vertex.head;
		return;
	}

	if (auto_flush && skip == 0 && ((m_vertex.tail + 1) - m_vertex.head) >= n)
	{
		HandleAutoFlush<prim, index_swap>();
	}

	u32 head = m_vertex.head;
	u32 tail = m_vertex.tail;
	u32 next = m_vertex.next;
	u32 xy_tail = m_vertex.xy_tail;

	if (GSConfig.UserHacks_Limit24BitDepth != GS_LIMIT24_DISABLED &&
		GSIsHardwareRenderer() && GSLocalMemory::m_psm[m_context->ZBUF.PSM].bpp == 32)
	{
		if (GSConfig.UserHacks_Limit24BitDepth == GS_LIMIT24_PRIORITIZE_UPPER)
			m_v.XYZ.Z = ((m_v.XYZ.Z >> 8) & ~0xFF) | (m_v.XYZ.Z & 0xFF);
		else if (GSConfig.UserHacks_Limit24BitDepth == GS_LIMIT24_PRIORITIZE_LOWER)
			m_v.XYZ.Z &= 0x00FFFFFF;
	}

	// callers should write XYZUVF to m_v.m[1] in one piece to have this load store-forwarded, either by the cpu or the compiler when this function is inlined

	const GSVector4i new_v1(m_v.m[1]);

	gs_vertex_store((union gs_vertex*)&m_vertex.buff[tail],
		(const union gs_vertex*)&m_v);

	// We maintain the X/Y coordinates for the last 4 vertices, as well as the head for triangle fans, so we can compute
	// the min/max, and cull degenerate triangles, which saves draws in some cases. Why 4? Mod 4 is cheaper than Mod 3.
	// These vertices are a full vector containing <X_Fixed_Point, Y_Fixed_Point, X_Integer, Y_Integer>. We use the
	// integer coordinates for culling at native resolution, and the fixed point for all others. The XY offset has to be
	// applied, then we split it into the fixed/integer portions.
	const GSVector4i xy_ofs = new_v1.xxxx().u16to32().sub32(m_xyof);
	const GSVector4i xy = xy_ofs.blend32<12>(xy_ofs.sra32<4>());
	m_vertex.xy[xy_tail & 3] = xy;

	// Backup head for triangle fans so we can read it later, otherwise it'll get lost after the 4th vertex.
	if (prim == GS_TRIANGLEFAN && tail == head)
		m_vertex.xyhead = xy;

	m_vertex.tail = ++tail;
	m_vertex.xy_tail = ++xy_tail;

	const u32 m = tail - head;

	if (m < n)
		return;


	// Skip draws when scissor is out of range (i.e. bottom-right is less than top-left), since everything will get clipped.
	skip |= static_cast<u32>(m_scissor_invalid);

	GSVector4i pmin, pmax;
	if (skip == 0)
	{
		const GSVector4i v0 = xy; /* reuse register -- same as m_vertex.xy[(xy_tail - 1) & 3] */
		const GSVector4i v1 = m_vertex.xy[(xy_tail - 2) & 3];
		const GSVector4i v2 = (prim == GS_TRIANGLEFAN) ? m_vertex.xyhead : m_vertex.xy[(xy_tail - 3) & 3];

		GSVertexKernels::BuildPrimBBox<prim>(v0, v1, v2, pmin, pmax);
		skip |= GSVertexKernels::CullSkipTest<prim>(pmin, pmax, v0, v1, v2,
			m_scissor_cull_min, m_scissor_cull_max, m_nativeres);
	}

	if (skip != 0)
	{
		switch (prim)
		{
			case GS_POINTLIST:
			case GS_LINELIST:
			case GS_TRIANGLELIST:
			case GS_SPRITE:
				m_vertex.tail = head; // no need to check or grow the buffer length
				break;
			case GS_LINESTRIP:
			case GS_TRIANGLESTRIP:
				m_vertex.head = head + 1;
				// fallthrough
			case GS_TRIANGLEFAN:
				if (tail >= maxcount)
					{ GrowVertexBuffer(); maxcount = m_vertex.maxcount; } // in case too many vertices were skipped
				break;
			default:
				break;
		}

		return;
	}

	if (tail >= maxcount)
		{ GrowVertexBuffer(); maxcount = m_vertex.maxcount; }

	if (m_index.tail == 0 && ((m_backed_up_ctx != m_env.PRIM.CTXT) || m_dirty_gs_regs))
	{
		const int ctx = m_env.PRIM.CTXT;
		memcpy(&m_prev_env, &m_env, 88);
		memcpy(&m_prev_env.CTXT[ctx], &m_env.CTXT[ctx], 96);
		memcpy(&m_prev_env.CTXT[ctx].offset, &m_env.CTXT[ctx].offset, sizeof(m_env.CTXT[ctx].offset));
		memcpy(&m_prev_env.CTXT[ctx].scissor, &m_env.CTXT[ctx].scissor, sizeof(m_env.CTXT[ctx].scissor));
		m_dirty_gs_regs = 0;
		m_backed_up_ctx = m_env.PRIM.CTXT;
	}

	u16* RESTRICT buff = &m_index.buff[m_index.tail];

	switch (prim)
	{
		case GS_POINTLIST:
			buff[0] = static_cast<u16>(head + 0);
			m_vertex.head = head + 1;
			m_vertex.next = head + 1;
			m_index.tail += 1;
			break;
		case GS_LINELIST:
			buff[0] = static_cast<u16>(head + (index_swap ? 1 : 0));
			buff[1] = static_cast<u16>(head + (index_swap ? 0 : 1));
			m_vertex.head = head + 2;
			m_vertex.next = head + 2;
			m_index.tail += 2;
			break;
		case GS_LINESTRIP:
			if (next < head)
			{
				m_vertex.buff[next + 0] = m_vertex.buff[head + 0];
				m_vertex.buff[next + 1] = m_vertex.buff[head + 1];
				head = next;
				m_vertex.tail = next + 2;
			}
			buff[0] = static_cast<u16>(head + (index_swap ? 1 : 0));
			buff[1] = static_cast<u16>(head + (index_swap ? 0 : 1));
			m_vertex.head = head + 1;
			m_vertex.next = head + 2;
			m_index.tail += 2;
			break;
		case GS_TRIANGLELIST:
			buff[0] = static_cast<u16>(head + (index_swap ? 2 : 0));
			buff[1] = static_cast<u16>(head + 1);
			buff[2] = static_cast<u16>(head + (index_swap ? 0 : 2));
			m_vertex.head = head + 3;
			m_vertex.next = head + 3;
			m_index.tail += 3;
			break;
		case GS_TRIANGLESTRIP:
			if (next < head)
			{
				m_vertex.buff[next + 0] = m_vertex.buff[head + 0];
				m_vertex.buff[next + 1] = m_vertex.buff[head + 1];
				m_vertex.buff[next + 2] = m_vertex.buff[head + 2];
				head = next;
				m_vertex.tail = next + 3;
			}
			buff[0] = static_cast<u16>(head + (index_swap ? 2 : 0));
			buff[1] = static_cast<u16>(head + 1);
			buff[2] = static_cast<u16>(head + (index_swap ? 0 : 2));
			m_vertex.head = head + 1;
			m_vertex.next = head + 3;
			m_index.tail += 3;
			break;
		case GS_TRIANGLEFAN:
			// TODO: remove gaps, next == head && head < tail - 3 || next > head && next < tail - 2 (very rare)
			buff[0] = static_cast<u16>(index_swap ? (tail - 1) : (head + 0));
			buff[1] = static_cast<u16>(tail - 2);
			buff[2] = static_cast<u16>(index_swap ? (head + 0) : (tail - 1));
			m_vertex.next = tail;
			m_index.tail += 3;
			break;
		case GS_SPRITE:
			buff[0] = static_cast<u16>(head + 0);
			buff[1] = static_cast<u16>(head + 1);

			// Update the first vert's Q for ease of doing Autoflush
			if (!m_env.PRIM.FST)
				m_vertex.buff[buff[0]].RGBAQ.Q = m_vertex.buff[buff[1]].RGBAQ.Q;

			m_vertex.head = head + 2;
			m_vertex.next = head + 2;
			m_index.tail += 2;
			break;
		default:
			break;
	}

	// Update rectangle for the current draw. We can use the re-integer coordinates from min/max here.
	const GSVector4i draw_min = pmin.zwzw();
	const GSVector4i draw_max = pmax;
	if (m_vertex.tail != n)
		temp_draw_rect = temp_draw_rect.min_i32(draw_min).blend32<12>(temp_draw_rect.max_i32(draw_max));
	else
		temp_draw_rect = draw_min.blend32<12>(draw_max);
	temp_draw_rect = temp_draw_rect.rintersect(m_context->scissor.in);
	if (m_af_seg_fresh || m_vertex.tail == n)
		m_af_seg_draw = draw_min.blend32<12>(draw_max).rintersect(m_context->scissor.in);
	else
		m_af_seg_draw = m_af_seg_draw.min_i32(draw_min).blend32<12>(m_af_seg_draw.max_i32(draw_max)).rintersect(m_context->scissor.in);
	m_af_seg_fresh = false;

	constexpr u32 max_vertices = MaxVerticesForPrim(prim);
	if (max_vertices != 0 && m_vertex.tail >= max_vertices)
		Flush(VERTEXCOUNT);
}

// GSTransferBuffer

GSState::GSTransferBuffer::GSTransferBuffer()
{
	memset(static_cast<gs_transfer*>(this), 0, sizeof(gs_transfer));
	buff = reinterpret_cast<u8*>(memalign_alloc(32, GS_TRANSFER_BUFFER_SIZE));
}

GSState::GSTransferBuffer::~GSTransferBuffer()
{
	memalign_free(buff);
}
