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

#include <cstring> /* memset/memcpy */
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include <limits>
#include <algorithm> /* clamp */
#include <cfloat> /* FLT_MAX */
#include <cmath>

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
  memcpy( std::addressof(t2), std::addressof(t1), sizeof(T1) );
  return t2;
}

static __fi bool IsAutoFlushEnabled()
{
	return GSIsHardwareRenderer() ? (GSConfig.UserHacks_AutoFlush != GSHWAutoFlushLevel::Disabled) : GSConfig.AutoFlushSW;
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
	size += (sizeof(GIFPath::tag) + sizeof(GIFPath::reg)) * 4 /* C89_ARRAY_SIZE(GSState::m_path) */; // std::size won't work without an instance.
	size += sizeof(m_q);

	return size;
}

GSState::GSState()
	: m_vt(this, IsFirstProvokingVertex())
{
	// m_nativeres seems to be a hack. Unfortunately it impacts draw call number which make debug painful in the replayer.
	// Let's keep it disabled to ease debug.
	m_nativeres = GSConfig.UpscaleMultiplier == 1.0f;
	m_mipmap = GSConfig.Mipmap;

	s_n = 0;
	s_transfer_n = 0;

	m_vr_merge_src = GSVector4::zero();
	m_vr_merge_dst = GSVector4::zero();
	m_vr_merge_tex = NULL;
	m_vr_eye_size  = GSVector2i(0, 0);
	m_vr_map_set   = false;
	m_vr_active    = false;

	memset(&m_v, 0, sizeof(m_v));
	memset(&m_vertex, 0, sizeof(m_vertex));
	memset(&m_index, 0, sizeof(m_index));
	memset(m_mem.m_vm8, 0, m_mem.m_vmsize);

	m_v.RGBAQ.Q = 1.0f;

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
		if (!hardware_reset && GSConfig.UseHardwareRenderer())
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

template<bool auto_flush, bool index_swap>
void GSState::SetPrimHandlers()
{
#define SetHandlerXYZ(P, auto_flush, index_swap) \
	m_fpGIFPackedRegHandlerXYZ[P][0] = &GSState::GIFPackedRegHandlerXYZF2<P, 0, auto_flush, index_swap>; \
	m_fpGIFPackedRegHandlerXYZ[P][1] = &GSState::GIFPackedRegHandlerXYZF2<P, 1, auto_flush, index_swap>; \
	m_fpGIFPackedRegHandlerXYZ[P][2] = &GSState::GIFPackedRegHandlerXYZ2<P, 0, auto_flush, index_swap>; \
	m_fpGIFPackedRegHandlerXYZ[P][3] = &GSState::GIFPackedRegHandlerXYZ2<P, 1, auto_flush, index_swap>; \
	m_fpGIFRegHandlerXYZ[P][0] = &GSState::GIFRegHandlerXYZF2<P, 0, auto_flush, index_swap>; \
	m_fpGIFRegHandlerXYZ[P][1] = &GSState::GIFRegHandlerXYZF2<P, 1, auto_flush, index_swap>; \
	m_fpGIFRegHandlerXYZ[P][2] = &GSState::GIFRegHandlerXYZ2<P, 0, auto_flush, index_swap>; \
	m_fpGIFRegHandlerXYZ[P][3] = &GSState::GIFRegHandlerXYZ2<P, 1, auto_flush, index_swap>; \
	m_fpGIFPackedRegHandlerSTQRGBAXYZF2[P] = &GSState::GIFPackedRegHandlerSTQRGBAXYZF2<P, auto_flush, index_swap>; \
	m_fpGIFPackedRegHandlerSTQRGBAXYZ2[P] = &GSState::GIFPackedRegHandlerSTQRGBAXYZ2<P, auto_flush, index_swap>;

	SetHandlerXYZ(GS_POINTLIST, true, false);
	SetHandlerXYZ(GS_LINELIST, auto_flush, index_swap);
	SetHandlerXYZ(GS_LINESTRIP, auto_flush, index_swap);
	SetHandlerXYZ(GS_TRIANGLELIST, auto_flush, index_swap);
	SetHandlerXYZ(GS_TRIANGLESTRIP, auto_flush, index_swap);
	SetHandlerXYZ(GS_TRIANGLEFAN, auto_flush, index_swap);
	SetHandlerXYZ(GS_SPRITE, auto_flush, false);
	SetHandlerXYZ(GS_INVALID, auto_flush, false);

#undef SetHandlerXYZ
}

void GSState::ResetHandlers()
{
	std::fill(std::begin(m_fpGIFPackedRegHandlers), std::end(m_fpGIFPackedRegHandlers), &GSState::GIFPackedRegHandlerNull);

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
	if (IsAutoFlushEnabled())
		IsFirstProvokingVertex() ? SetPrimHandlers<true, true>() : SetPrimHandlers<true, false>();
	else
		IsFirstProvokingVertex() ? SetPrimHandlers<false, true>() : SetPrimHandlers<false, false>();

	std::fill(std::begin(m_fpGIFRegHandlers), std::end(m_fpGIFRegHandlers), &GSState::GIFRegHandlerNull);

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

void GSState::ResetPCRTC()
{
	PCRTCDisplays.SetVideoMode(GetVideoMode());
	PCRTCDisplays.EnableDisplays(m_regs->PMODE, m_regs->SMODE2, isReallyInterlaced());
	PCRTCDisplays.SetRects(0, m_regs->DISP[0].DISPLAY, m_regs->DISP[0].DISPFB);
	PCRTCDisplays.SetRects(1, m_regs->DISP[1].DISPLAY, m_regs->DISP[1].DISPFB);
}

void GSState::UpdateSettingsBase(const Pcsx2Config::GSOptions& old_config)
{
	m_mipmap = GSConfig.Mipmap;

	if (
		GSConfig.AutoFlushSW != old_config.AutoFlushSW ||
		GSConfig.UserHacks_AutoFlush != old_config.UserHacks_AutoFlush ||
		GSConfig.UserHacks_ForceEvenSpritePosition != old_config.UserHacks_ForceEvenSpritePosition)
	{
		ResetHandlers();
	}
}

/* The privileged registers live in g_RealGSMem, which the EE writes with
 * whole 64-bit stores (gsWrite64_generic) while this thread reads single
 * bit-fields out of them during vsync.  TSan, over four load/unload
 * cycles: GSState::GetVideoMode and GSState::isReallyInterlaced against
 * gsWrite64_page_00, on SMODE1, SMODE2 and SYNCV.
 *
 * Load the whole register in one atomic read and take the fields off the
 * copy.  The value can still be one write out of date - it always could,
 * there is no ordering between a game changing video mode and the vsync
 * that observes it - but it is now a value that was actually written,
 * rather than halves of two different ones.  Pairs with the release
 * store in gsWrite64_generic. */
template <typename T>
static inline T gs_reg_read(const T& reg)
{
	T copy;
#if defined(RETRO_ATOMIC_HAS_64)
	copy.U64 = (u64)retro_atomic_load_acquire_64((const retro_atomic_64_t*)&reg.U64);
#else
	/* No 64-bit atomics on this target (old 32-bit __sync); the read is
	 * as it always was. */
	copy.U64 = reg.U64;
#endif
	return copy;
}

bool GSState::isinterlaced()
{
	return !!gs_reg_read(m_regs->SMODE2).INT;
}

bool GSState::isReallyInterlaced()
{
	// The FIELD register only flips if the CMOD field in SMODE1 is set to anything but 0 and Front Porch bottom bit in SYNCV is set.
	return (gs_reg_read(m_regs->SYNCV).VFP & 0x1) && gs_reg_read(m_regs->SMODE1).CMOD;
}

GSVideoMode GSState::GetVideoMode()
{
	// TODO: Get confirmation of videomode from SYSCALL ? not necessary but would be nice.
	// Other videomodes can't be detected on our side without the help of the data from core
	// You can only identify a limited number of video modes based on the info from CRTC registers.

	const GSRegSMODE1 smode1 = gs_reg_read(m_regs->SMODE1);
	const u8 Colorburst = smode1.CMOD; // Subcarrier frequency
	const u8 PLL_Divider = smode1.LC;  // Phased lock loop divider

	switch (Colorburst)
	{
		case 0:
			if (isinterlaced() && PLL_Divider == 22)
				return GSVideoMode::HDTV_1080I;
			else if (!isinterlaced() && PLL_Divider == 22)
				return GSVideoMode::HDTV_720P;
			else if (!isinterlaced() && PLL_Divider == 32)
				return GSVideoMode::SDTV_480P; // TODO: 576P will also be reported as 480P, find some way to differeniate.
			else
				return GSVideoMode::VESA;
		case 2:
			return GSVideoMode::NTSC;
		case 3:
			return GSVideoMode::PAL;
		default:
			break;
	}
	return GSVideoMode::Unknown;
}

float GSState::GetTvRefreshRate()
{
	const GSVideoMode videomode = GetVideoMode();

	//TODO: Check vertical frequencies for VESA video modes, old ones were untested.

	switch (videomode)
	{
		case GSVideoMode::NTSC:
		case GSVideoMode::SDTV_480P:
			return (60 / 1.001f);
		case GSVideoMode::PAL:
			return 50;
		case GSVideoMode::HDTV_720P:
		case GSVideoMode::HDTV_1080I:
			return 60;
		default:
			break;
	}

	return 0;
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
				InvalidateLocalMem(BITBLTBUF, r, true);
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

			InvalidateLocalMem(BITBLTBUF, r, true);
		}

		m_mem.m_clut.Write(m_env.CTXT[i].TEX0, m_env.TEXCLUT);
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

			if (m_draw_transfers.size() > 0 && m_env.BITBLTBUF.DBP == m_draw_transfers.back().blit.DBP)
				m_draw_transfers.back().rect = m_draw_transfers.back().rect.runion(r);
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
			Draw();

		m_index.tail = 0;
		m_vertex.head = 0;

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
GSVector4i GSState::GetTEX0Rect()
{
	GSVector4i ret = GSVector4i::zero();
	const GSDrawingContext& prev_ctx = m_prev_env.CTXT[m_prev_env.PRIM.CTXT];

	if (prev_ctx.CLAMP.WMS <= 1) // CLAMP/REPEAT
	{
		ret.x = 0;
		ret.z = 1 << prev_ctx.TEX0.TW;
	}
	else if (prev_ctx.CLAMP.WMS == 3) // REGION_REPEAT
	{
		ret.x = prev_ctx.CLAMP.MAXU;
		ret.z = prev_ctx.CLAMP.MAXU | prev_ctx.CLAMP.MINU;
	}
	else // 2 REGION_CLAMP
	{
		ret.x = prev_ctx.CLAMP.MINU;
		ret.z = prev_ctx.CLAMP.MAXU;
	}

	if (prev_ctx.CLAMP.WMT <= 1) // CLAMP/REPEAT
	{
		ret.y = 0;
		ret.w = 1 << prev_ctx.TEX0.TH;
	}
	else if (prev_ctx.CLAMP.WMT == 3) // REGION_REPEAT
	{
		ret.y = prev_ctx.CLAMP.MAXV;
		ret.w = prev_ctx.CLAMP.MAXV | prev_ctx.CLAMP.MINV;
	}
	else // 2 REGION_CLAMP
	{
		ret.y = prev_ctx.CLAMP.MINV;
		ret.w = prev_ctx.CLAMP.MAXV;
	}

	return ret;
}

void GSState::CheckWriteOverlap(bool req_write, bool req_read)
{
	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;
	const GIFRegBITBLTBUF& blit = m_env.BITBLTBUF;

	const GSDrawingContext& prev_ctx = m_prev_env.CTXT[m_prev_env.PRIM.CTXT];
	const GSVector4i write_rect = GSVector4i(m_env.TRXPOS.DSAX, m_env.TRXPOS.DSAY, m_env.TRXPOS.DSAX + w, m_env.TRXPOS.DSAY + h);
	const u32 write_start_bp = GSLocalMemory::GetStartBlockAddress(blit.DBP, blit.DBW, blit.DPSM, write_rect);
	const u32 write_end_bp = ((GSLocalMemory::GetEndBlockAddress(blit.DBP, blit.DBW, blit.DPSM, write_rect) + 1) + (GS_BLOCKS_PER_PAGE - 1)) & ~(GS_BLOCKS_PER_PAGE - 1);
	GSVector4i tex_rect = m_prev_env.PRIM.TME ? GetTEX0Rect() : GSVector4i::zero();

	if (m_index.tail > 0)
	{
		// Only flush on a NEW transfer if a pending one is using the same address or overlap.
		// Check Fast & Furious (Hardare mode) and Assault Suits Valken (either renderer) and Tomb Raider - Angel of Darkness menu (TBP != DBP but overlaps).
		// Cartoon Network overwrites its own Z buffer in the middle of a draw.
		// Alias wraps its transfers, so be careful
		const GSVector4i read_rect = GSVector4i(m_env.TRXPOS.SSAX, m_env.TRXPOS.SSAY, m_env.TRXPOS.SSAX + w, m_env.TRXPOS.SSAY + h);

		if (req_write && m_prev_env.PRIM.TME)
		{
			// Tex rect could be invalid showing 1024x1024 when it isn't. If the frame is only 1 page wide, it's either a big strip or a single page draw.
			// This large texture causes misdetection of overlapping writes, causing our heuristics in the hardware renderer for future draws to be missing.
			// Either way if we check the queued up coordinates, it should give us a fair idea. (Cabela's Trophy Bucks)
			if (prev_ctx.FRAME.FBW == 1 && (u32)tex_rect.width() > (prev_ctx.TEX0.TBW * 64))
			{
				GSVector4i tex_draw_rect = GSVector4i::zero();
				for (u32 i = 0; i < m_index.tail; i++)
				{
					const GSVertex* v = &m_vertex.buff[m_index.buff[i]];
					GSVector2i tex_coord;
					if (PRIM->FST)
					{
						tex_coord.x = v->U >> 4;
						tex_coord.y = v->V >> 4;
					}
					else
					{
						const float s = pcsx2_min_f((v->ST.S / v->RGBAQ.Q), 1.0f);
						const float t = pcsx2_min_f((v->ST.T / v->RGBAQ.Q), 1.0f);

						/* pcsx2_min_f is (a < b) ? a : b, so it returns 1.0f
						 * for a NaN and for an infinity and bounds s above.
						 * Nothing bounds it below: a zero Q with a negative
						 * S gives -inf, and the cast is undefined there. */
						tex_coord.x = f32_to_s32_sat(std::round((1 << m_context->TEX0.TW) * s));
						tex_coord.y = f32_to_s32_sat(std::round((1 << m_context->TEX0.TH) * t));
					}

					if (i == 0)
					{
						tex_draw_rect.x = tex_coord.x;
						tex_draw_rect.y = tex_coord.y;
						tex_draw_rect.z = tex_coord.x;
						tex_draw_rect.w = tex_coord.y;

						continue;
					}

					tex_draw_rect.x = pcsx2_min_f(tex_draw_rect.x, tex_coord.x);
					tex_draw_rect.z = pcsx2_max_f(tex_draw_rect.z, tex_coord.x);
					tex_draw_rect.y = pcsx2_min_f(tex_draw_rect.y, tex_coord.y);
					tex_draw_rect.w = pcsx2_max_f(tex_draw_rect.w, tex_coord.y);
				}

				tex_rect = tex_rect.rintersect(tex_draw_rect);
			}

			if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.TEX0.TBP0, prev_ctx.TEX0.TBW, prev_ctx.TEX0.PSM, tex_rect))
			{
				
				Flush(GSFlushReason::UPLOADDIRTYTEX);
			}
			if (prev_ctx.TEX1.MXL > 0 && prev_ctx.TEX1.MMIN >= 2 && prev_ctx.TEX1.MMIN <= 5)
			{
				switch (prev_ctx.TEX1.MXL)
				{
					case 6:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP2.TBP6, prev_ctx.MIPTBP2.TBW6, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 6, tex_rect.y >> 6, tex_rect.z >> 6, tex_rect.w >> 6)))
							Flush(GSFlushReason::UPLOADDIRTYTEX);
						// fallthrough
					case 5:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP2.TBP5, prev_ctx.MIPTBP2.TBW5, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 5, tex_rect.y >> 5, tex_rect.z >> 5, tex_rect.w >> 5)))
							Flush(GSFlushReason::UPLOADDIRTYTEX);
						// fallthrough
					case 4:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP2.TBP4, prev_ctx.MIPTBP2.TBW4, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 4, tex_rect.y >> 4, tex_rect.z >> 4, tex_rect.w >> 4)))
							Flush(GSFlushReason::UPLOADDIRTYTEX);
						// fallthrough
					case 3:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP1.TBP3, prev_ctx.MIPTBP1.TBW3, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 3, tex_rect.y >> 3, tex_rect.z >> 3, tex_rect.w >> 3)))
							Flush(GSFlushReason::UPLOADDIRTYTEX);
						// fallthrough
					case 2:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP1.TBP2, prev_ctx.MIPTBP1.TBW2, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 2, tex_rect.y >> 2, tex_rect.z >> 2, tex_rect.w >> 2)))
							Flush(GSFlushReason::UPLOADDIRTYTEX);
						// fallthrough
					case 1:
						if (GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.MIPTBP1.TBP1, prev_ctx.MIPTBP1.TBW1, prev_ctx.TEX0.PSM, GSVector4i(tex_rect.x >> 1, tex_rect.y >> 1, tex_rect.z >> 1, tex_rect.w >> 1)))
							Flush(GSFlushReason::UPLOADDIRTYTEX);
						break;
				}
			}
		}

		const u32 frame_mask = GSLocalMemory::m_psm[prev_ctx.FRAME.PSM].fmsk;
		const bool frame_required = (!(prev_ctx.TEST.ATE && prev_ctx.TEST.ATST == 0 && (prev_ctx.TEST.AFAIL == 2 || prev_ctx.TEST.AFAIL == 0)) && ((prev_ctx.FRAME.FBMSK & frame_mask) != frame_mask)) || prev_ctx.TEST.DATE;
		if (frame_required)
		{
			const GSFlushReason reason = req_write ? (req_read ? GSFlushReason::LOCALTOLOCALMOVE : GSFlushReason::UPLOADDIRTYFRAME) : GSFlushReason::DOWNLOADFIFO;

			if ((req_write && (blit.DBP == prev_ctx.FRAME.Block() || GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.FRAME.Block(), prev_ctx.FRAME.FBW, prev_ctx.FRAME.PSM, temp_draw_rect))) ||
				(req_read && (blit.DBP == prev_ctx.FRAME.Block() || GSLocalMemory::HasOverlap(blit.SBP, blit.SBW, blit.SPSM, read_rect, prev_ctx.FRAME.Block(), prev_ctx.FRAME.FBW, prev_ctx.FRAME.PSM, temp_draw_rect))))
				Flush(reason);
		}

		const bool zbuf_required = (!(prev_ctx.TEST.ATE && prev_ctx.TEST.ATST == 0 && prev_ctx.TEST.AFAIL != 2) && !prev_ctx.ZBUF.ZMSK) || (prev_ctx.TEST.ZTE && prev_ctx.TEST.ZTST > ZTST_ALWAYS);
		if (zbuf_required)
		{
			const GSFlushReason reason = req_write ? (req_read ? GSFlushReason::LOCALTOLOCALMOVE : GSFlushReason::UPLOADDIRTYZBUF) : GSFlushReason::DOWNLOADFIFO;

			if ((req_write && (blit.DBP == prev_ctx.ZBUF.Block() || GSLocalMemory::HasOverlap(blit.DBP, blit.DBW, blit.DPSM, write_rect, prev_ctx.ZBUF.Block(), prev_ctx.FRAME.FBW, prev_ctx.ZBUF.PSM, temp_draw_rect))) ||
				(req_read && (blit.DBP == prev_ctx.ZBUF.Block() || GSLocalMemory::HasOverlap(blit.SBP, blit.SBW, blit.SPSM, read_rect, prev_ctx.ZBUF.Block(), prev_ctx.FRAME.FBW, prev_ctx.ZBUF.PSM, temp_draw_rect))))
				Flush(reason);
		}
	}

	if (req_write)
	{
		// Invalid the CLUT if it crosses paths.
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
		if ((m_draw_transfers.size() > 0 && blit.DBP == m_draw_transfers.back().blit.DBP))
		{
			// Same BP, let's update the rect.
			GSUploadQueue transfer = m_draw_transfers.back();
			m_draw_transfers.pop_back();
			transfer.rect = transfer.rect.runion(r);
			transfer.draw = s_n;
			transfer.zero_clear = false;
			m_draw_transfers.push_back(transfer);
		}
		else
		{
			GSUploadQueue new_transfer = { blit, r, s_n, false };
			m_draw_transfers.push_back(new_transfer);
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

	// If it wraps memory, we need to break it up so we don't read out of bounds.
	if ((m_tr.end + len) > m_mem.m_vmsize)
	{
		const int first_transfer = m_mem.m_vmsize - m_tr.end;
		const int second_transfer = len - first_transfer;
		memcpy(mem, &m_tr.buff[m_tr.end], first_transfer);
		m_tr.end = 0;
		memcpy(&mem[first_transfer], &m_tr.buff, second_transfer);
		m_tr.end = second_transfer;
	}
	else
	{
		memcpy(mem, &m_tr.buff[m_tr.end], len);
		m_tr.end += len;
	}

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

	int sx = m_env.TRXPOS.SSAX;
	int sy = m_env.TRXPOS.SSAY;
	int dx = m_env.TRXPOS.DSAX;
	int dy = m_env.TRXPOS.DSAY;

	const int w = m_env.TRXREG.RRW;
	const int h = m_env.TRXREG.RRH;

	InvalidateLocalMem(m_env.BITBLTBUF, GSVector4i(sx, sy, sx + w, sy + h));
	InvalidateVideoMem(m_env.BITBLTBUF, GSVector4i(dx, dy, dx + w, dy + h));

	const bool overlaps = m_env.BITBLTBUF.SBP == m_env.BITBLTBUF.DBP;
	const bool intersect = overlaps && !(GSVector4i(sx, sy, sx + w, sy + h).rintersect(GSVector4i(dx, dy, dx + w, dy + h)).rempty());

	int xinc = 1;
	int yinc = 1;

	if (m_env.TRXPOS.DIRX)
	{
		// Only allow it to reverse if the destination is behind the source.
		if (!intersect || sx < dx)
		{
			sx += w - 1;
			dx += w - 1;
			xinc = -1;
		}
	}
	if (m_env.TRXPOS.DIRY)
	{
		// Only allow it to reverse if the destination is behind the source.
		if (!intersect || sy < dy)
		{
			sy += h - 1;
			dy += h - 1;
			yinc = -1;
		}
	}

	const GSLocalMemory::psm_t& spsm = GSLocalMemory::m_psm[m_env.BITBLTBUF.SPSM];
	const GSLocalMemory::psm_t& dpsm = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM];

	// TODO: unroll inner loops (width has special size requirement, must be multiples of 1 << n, depending on the format)

	const int sbp = m_env.BITBLTBUF.SBP;
	const int sbw = m_env.BITBLTBUF.SBW;
	const int dbp = m_env.BITBLTBUF.DBP;
	const int dbw = m_env.BITBLTBUF.DBW;
	const GSOffset spo = m_mem.GetOffset(sbp, sbw, m_env.BITBLTBUF.SPSM);
	const GSOffset dpo = m_mem.GetOffset(dbp, dbw, m_env.BITBLTBUF.DPSM);

	GSVector4i r;
	r.left = m_env.TRXPOS.DSAX;
	r.top = m_env.TRXPOS.DSAY;
	r.right = r.left + m_env.TRXREG.RRW;
	r.bottom = r.top + m_env.TRXREG.RRH;

	s_last_transfer_draw_n = s_n;
	// Store the transfer for preloading new RT's.
	if ((m_draw_transfers.size() > 0 && m_env.BITBLTBUF.DBP == m_draw_transfers.back().blit.DBP))
	{
		// Same BP, let's update the rect.
		GSUploadQueue transfer = m_draw_transfers.back();
		m_draw_transfers.pop_back();
		transfer.rect = transfer.rect.runion(r);
		transfer.draw = s_n;
		transfer.zero_clear = false;
		m_draw_transfers.push_back(transfer);
	}
	else
	{
		GSUploadQueue new_transfer = { m_env.BITBLTBUF, r, s_n, false };
		m_draw_transfers.push_back(new_transfer);
	}

	auto copy = [this, sbp, dbp, sx, sy, dx, dy, w, h, yinc, xinc](const GSOffset& dpo, const GSOffset& spo, auto&& pxCopyFn)
	{
		int _sy = sy, _dy = dy; // Faster with local copied variables, compiler optimizations are dumb
		if (xinc > 0)
		{
			const int page_width = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM].pgs.x;
			const int page_height = GSLocalMemory::m_psm[m_env.BITBLTBUF.DPSM].pgs.y;
			const int xpage = sx & ~(page_width - 1);
			const int ypage = _sy & ~(page_height - 1);
			// Copying from itself to itself (rotating textures) used in Gitaroo Man stage 8
			// What probably happens is because the copy is buffered, the source stays just ahead of the destination.
			// No need to do all this if the copy source/destination don't intersect, however.
			const bool intersect = !(GSVector4i(sx, sy, sx + w, sy + h).rintersect(GSVector4i(dx, dy, dx + w, dy + h)).rempty());

			if (intersect && sbp == dbp && (((_sy < _dy) && ((ypage + page_height) > _dy)) || ((sx < dx) && ((xpage + page_width) > dx))))
			{
				int starty = (yinc > 0) ? 0 : h-1;
				int endy = (yinc > 0) ? h : -1;
				int y_inc = yinc;

				if (((_sy < _dy) && ((ypage + page_height) > _dy)) && yinc > 0)
				{
					_sy += h-1;
					_dy += h-1;
					starty = h-1;
					endy = -1;
					y_inc = -y_inc;
				}

				for (int y = starty; y != endy; y+= y_inc, _sy += y_inc, _dy += y_inc)
				{
					GSOffset::PAHelper s = spo.paMulti(0, _sy);
					GSOffset::PAHelper d = dpo.paMulti(0, _dy);

					if (((sx < dx) && ((xpage + page_width) > dx)))
					{
						for (int x = w - 1; x >= 0; x--)
							pxCopyFn(d.value((dx + x) & 2047), s.value((sx + x) & 2047));
					}
					else
					{
						for (int x = 0; x < w; x++)
							pxCopyFn(d.value((dx + x) & 2047), s.value((sx + x) & 2047));
					}
				}
			}
			else
			{
				for (int y = 0; y < h; y++, _sy += yinc, _dy += yinc)
				{
					GSOffset::PAHelper s = spo.paMulti(0, _sy);
					GSOffset::PAHelper d = dpo.paMulti(0, _dy);

					for (int x = 0; x < w; x++)
						pxCopyFn(d.value((dx + x) & 2047), s.value((sx + x) & 2047));
				}
			}
		}
		else
		{
			for (int y = 0; y < h; y++, _sy += yinc, _dy += yinc)
			{
				GSOffset::PAHelper s = spo.paMulti(0, _sy);
				GSOffset::PAHelper d = dpo.paMulti(0, _dy);

				for (int x = 0; x < w; x++)
					pxCopyFn(d.value((dx - x) & 2047), s.value((sx - x) & 2047));
			}
		}
	};

	if (spsm.trbpp == dpsm.trbpp && spsm.trbpp >= 16)
	{
		if (spsm.trbpp == 32)
		{
			u32* vm = m_mem.vm32();
			copy(dpo.assertSizesMatch(GSLocalMemory::swizzle32), spo.assertSizesMatch(GSLocalMemory::swizzle32), [vm](u32 doff, u32 soff)
			{
				vm[doff] = vm[soff];
			});
		}
		else if (spsm.trbpp == 24)
		{
			u32* vm = m_mem.vm32();
			copy(dpo.assertSizesMatch(GSLocalMemory::swizzle32), spo.assertSizesMatch(GSLocalMemory::swizzle32), [vm](u32 doff, u32 soff)
			{
				vm[doff] = (vm[doff] & 0xff000000) | (vm[soff] & 0x00ffffff);
			});
		}
		else // if(spsm.trbpp == 16)
		{
			u16* vm = m_mem.vm16();
			copy(dpo.assertSizesMatch(GSLocalMemory::swizzle16), spo.assertSizesMatch(GSLocalMemory::swizzle16), [vm](u32 doff, u32 soff)
			{
				vm[doff] = vm[soff];
			});
		}
	}
	else if (m_env.BITBLTBUF.SPSM == PSMT8 && m_env.BITBLTBUF.DPSM == PSMT8)
	{
		u8* vm = m_mem.m_vm8;
		copy(GSOffset::fromKnownPSM(dbp, dbw, PSMT8), GSOffset::fromKnownPSM(sbp, sbw, PSMT8), [vm](u32 doff, u32 soff)
		{
			vm[doff] = vm[soff];
		});
	}
	else if (m_env.BITBLTBUF.SPSM == PSMT4 && m_env.BITBLTBUF.DPSM == PSMT4)
	{
		copy(GSOffset::fromKnownPSM(dbp, dbw, PSMT4), GSOffset::fromKnownPSM(sbp, sbw, PSMT4), [&](u32 doff, u32 soff)
		{
			m_mem.WritePixel4(doff, m_mem.ReadPixel4(soff));
		});
	}
	else
	{
		copy(dpo, spo, [&](u32 doff, u32 soff)
		{
			(m_mem.*dpsm.wpa)(doff, (m_mem.*spsm.rpa)(soff));
		});
	}

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

	GSTransferBuffer tb;

	if(m_tr.end >= m_tr.total || m_tr.write == true)
		tb.Init(TRXPOS.SSAX, TRXPOS.SSAY, BITBLTBUF, false);

	int len = qwc * 16;
	if (!tb.Update(w, h, bpp, len))
		return;

	if (m_tr.start == 0)
	{
		m_mem.ReadImageX(tb.x, tb.y, m_tr.buff, m_tr.total, BITBLTBUF, TRXPOS, TRXREG);
		m_tr.start += m_tr.total;
	}

	if ((m_tr.end + len) > m_mem.m_vmsize)
	{
		const int masked_end = m_tr.end & 0x3FFFFF; // 4mb.
		const int first_transfer = m_mem.m_vmsize - masked_end;
		const int second_transfer = len - first_transfer;
		memcpy(mem, &m_tr.buff[masked_end], first_transfer);
		memcpy(&mem[first_transfer], &m_tr.buff, second_transfer);
		m_tr.end += len;
	}
	else
	{
		memcpy(mem, &m_tr.buff[m_tr.end], len);
		m_tr.end += len;
	}
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

	if (GSConfig.UserHacks_ReadTCOnClose)
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

	Reset(true);

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
	ReadState(m_mem.m_vm8, data, m_mem.m_vmsize);
	m_mem_restores++;

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

	m_fpGIFPackedRegHandlers[GIF_REG_XYZF2] = m_fpGIFPackedRegHandlerXYZ[prim][0];
	m_fpGIFPackedRegHandlers[GIF_REG_XYZF3] = m_fpGIFPackedRegHandlerXYZ[prim][1];
	m_fpGIFPackedRegHandlers[GIF_REG_XYZ2] = m_fpGIFPackedRegHandlerXYZ[prim][2];
	m_fpGIFPackedRegHandlers[GIF_REG_XYZ3] = m_fpGIFPackedRegHandlerXYZ[prim][3];

	m_fpGIFRegHandlers[GIF_A_D_REG_XYZF2] = m_fpGIFRegHandlerXYZ[prim][0];
	m_fpGIFRegHandlers[GIF_A_D_REG_XYZF3] = m_fpGIFRegHandlerXYZ[prim][1];
	m_fpGIFRegHandlers[GIF_A_D_REG_XYZ2] = m_fpGIFRegHandlerXYZ[prim][2];
	m_fpGIFRegHandlers[GIF_A_D_REG_XYZ3] = m_fpGIFRegHandlerXYZ[prim][3];

	m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZF2] = m_fpGIFPackedRegHandlerSTQRGBAXYZF2[prim];
	m_fpGIFPackedRegHandlersC[GIF_REG_STQRGBAXYZ2] = m_fpGIFPackedRegHandlerSTQRGBAXYZ2[prim];
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

bool GSState::TrianglesAreQuads(bool shuffle_check) const
{
	// If this is a quad, there should only be two distinct values for both X and Y, which
	// also happen to be the minimum/maximum bounds of the primitive.
	const GSVertex* const v = m_vertex.buff;
	for (u32 idx = 0; idx < m_index.tail; idx += 6)
	{
		const u16* const i = m_index.buff + idx;

		// Make sure the next set of triangles matches an edge of the previous triangle.
		if (idx > 0)
		{
			const u16* const prev_tri= m_index.buff + (idx - 3);
			GIFRegXYZ new_verts[3] = {v[i[0]].XYZ, v[i[1]].XYZ, v[i[2]].XYZ};

			if (shuffle_check)
			{
				new_verts[0].X -= 8 << 4;
				new_verts[1].X -= 8 << 4;
				new_verts[2].X -= 8 << 4;
			}
			u32 match_vert_count = 0;

			if (!(new_verts[0] != m_vertex.buff[prev_tri[0]].XYZ && new_verts[0] != m_vertex.buff[prev_tri[1]].XYZ && new_verts[0] != m_vertex.buff[prev_tri[2]].XYZ))
				match_vert_count++;
			if (!(new_verts[1] != m_vertex.buff[prev_tri[0]].XYZ && new_verts[1] != m_vertex.buff[prev_tri[1]].XYZ && new_verts[1] != m_vertex.buff[prev_tri[2]].XYZ))
				match_vert_count++;
			if (!(new_verts[2] != m_vertex.buff[prev_tri[0]].XYZ && new_verts[2] != m_vertex.buff[prev_tri[1]].XYZ && new_verts[2] != m_vertex.buff[prev_tri[2]].XYZ))
				match_vert_count++;

			if (match_vert_count != 2)
				return false;
		}
		// Degenerate triangles should've been culled already, so we can check indices.
		u32 extra_verts = 0;
		for (u32 j = 3; j < 6; j++)
		{
			const u16 tri2_idx = i[j];
			if (tri2_idx != i[0] && tri2_idx != i[1] && tri2_idx != i[2])
				extra_verts++;
		}
		if (extra_verts == 1)
			continue;

		// As a fallback, they might've used different vertices with a tri list, not strip.
		// Note that this won't work unless the quad is axis-aligned.
		u16 distinct_x_values[2] = {v[i[0]].XYZ.X};
		u16 distinct_y_values[2] = {v[i[0]].XYZ.Y};
		u32 num_distinct_x_values = 1, num_distinct_y_values = 1;
		for (u32 j = 1; j < 6; j++)
		{
			const GSVertex& jv = v[i[j]];
			if (jv.XYZ.X != distinct_x_values[0] && jv.XYZ.X != distinct_x_values[1])
			{
				if (num_distinct_x_values > 1)
					return false;

				distinct_x_values[num_distinct_x_values++] = jv.XYZ.X;
			}

			if (jv.XYZ.Y != distinct_y_values[0] && jv.XYZ.Y != distinct_y_values[1])
			{
				if (num_distinct_y_values > 1)
					return false;

				distinct_y_values[num_distinct_y_values++] = jv.XYZ.Y;
			}
		}
	}

	return true;
}

GSState::PRIM_OVERLAP GSState::PrimitiveOverlap()
{
	// Either 1 triangle or 1 line or 3 POINTs
	// It is bad for the POINTs but low probability that they overlap
	if (m_vertex.next < 4)
		return PRIM_OVERLAP_NO;

	if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
		return (m_index.tail == 6 && TrianglesAreQuads()) ? PRIM_OVERLAP_NO : PRIM_OVERLAP_UNKNOW;
	else if (m_vt.m_primclass != GS_SPRITE_CLASS)
		return PRIM_OVERLAP_UNKNOW; // maybe, maybe not

	// Check intersection of sprite primitive only
	const u32 count = m_vertex.next;
	PRIM_OVERLAP overlap = PRIM_OVERLAP_NO;
	const GSVertex* v = m_vertex.buff;

	m_drawlist.clear();
	u32 i = 0;
	while (i < count)
	{
		// In order to speed up comparison a bounding-box is accumulated. It removes a
		// loop so code is much faster (check game virtua fighter). Besides it allow to check
		// properly the Y order.

		// .x = min(v[i].XYZ.X, v[i+1].XYZ.X)
		// .y = min(v[i].XYZ.Y, v[i+1].XYZ.Y)
		// .z = max(v[i].XYZ.X, v[i+1].XYZ.X)
		// .w = max(v[i].XYZ.Y, v[i+1].XYZ.Y)
		GSVector4i all = GSVector4i(v[i].m[1]).upl16(GSVector4i(v[i + 1].m[1])).upl16().xzyw();
		all = all.xyxy().blend(all.zwzw(), all > all.zwxy());

		u32 j = i + 2;
		while (j < count)
		{
			GSVector4i sprite = GSVector4i(v[j].m[1]).upl16(GSVector4i(v[j + 1].m[1])).upl16().xzyw();
			sprite = sprite.xyxy().blend(sprite.zwzw(), sprite > sprite.zwxy());

			if (all.rintersect(sprite).rempty())
				all = all.runion(sprite);
			else
			{
				overlap = PRIM_OVERLAP_YES;
				break;
			}
			j += 2;
		}
		m_drawlist.push_back((j - i) >> 1); // Sprite count
		i = j;
	}

	return overlap;
}

bool GSState::SpriteDrawWithoutGaps()
{
	// Check that the height matches. Xenosaga 3 draws a letterbox around
	// the FMV with a sprite at the top and bottom of the framebuffer.
	const GSVertex* v = &m_vertex.buff[0];
	const int first_dpY = v[1].XYZ.Y - v[0].XYZ.Y;
	const int first_dpX = v[1].XYZ.X - v[0].XYZ.X;

	// Horizontal Match.
	if (((first_dpX + 8) >> 4) == m_r_no_scissor.z)
	{
		// Borrowed from MergeSprite() modified to calculate heights.
		for (u32 i = 2; i < m_vertex.next; i += 2)
		{
			const int last_pY = v[i - 1].XYZ.Y;
			const int dpY = v[i + 1].XYZ.Y - v[i].XYZ.Y;
			if (abs(dpY - first_dpY) >= 16 || abs(static_cast<int>(v[i].XYZ.Y) - last_pY) >= 16)
				return false;
		}

		return true;
	}

	// Vertical Match.
	if (((first_dpY + 8) >> 4) == m_r_no_scissor.w)
	{
		// Borrowed from MergeSprite().
		const int offset_X = m_context->XYOFFSET.OFX;
		for (u32 i = 2; i < m_vertex.next; i += 2)
		{
			const int last_pX = v[i - 1].XYZ.X;
			const int this_start_X = v[i].XYZ.X;
			const int last_start_X = v[i - 2].XYZ.X;

			const int dpX = v[i + 1].XYZ.X - v[i].XYZ.X;

			if (this_start_X < last_start_X)
			{
				const int prev_X = last_start_X - offset_X;
				if (abs(dpX - prev_X) >= 16 || abs(this_start_X - offset_X) >= 16)
					return false;
			}
			else
			{
				const int dpY = v[i + 1].XYZ.Y - v[i].XYZ.Y;
				if ((abs(dpY - first_dpY) >= 16 && (i + 2) < m_vertex.next) || abs(this_start_X - last_pX) >= 16)
					return false;
			}
		}

		return true;
	}

	return false;
}

void GSState::CalculatePrimitiveCoversWithoutGaps()
{
	m_primitive_covers_without_gaps = FullCover;

	// Draw shouldn't be offset.
	if (((m_r.eq32(GSVector4i::zero())).mask() & 0xff) != 0xff)
		m_primitive_covers_without_gaps = GapsFound;

	if (m_vt.m_primclass == GS_POINT_CLASS)
	{
		m_primitive_covers_without_gaps = (m_vertex.next < 2) ? m_primitive_covers_without_gaps : GapsFound;
		return;
	}
	else if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
	{
		m_primitive_covers_without_gaps = (m_index.tail == 6 && TrianglesAreQuads()) ? m_primitive_covers_without_gaps : GapsFound;
		return;
	}
	else if (m_vt.m_primclass != GS_SPRITE_CLASS)
	{
		m_primitive_covers_without_gaps = GapsFound;
		return;
	}

	// Simple case: one sprite.
	if (m_primitive_covers_without_gaps != GapsFound && m_index.tail == 2)
		return;

	m_primitive_covers_without_gaps = SpriteDrawWithoutGaps() ? (m_primitive_covers_without_gaps == GapsFound ? SpriteNoGaps : m_primitive_covers_without_gaps) : GapsFound;
}

__forceinline bool GSState::IsAutoFlushDraw(u32 prim)
{
	if (!PRIM->TME || (GSConfig.UserHacks_AutoFlush == GSHWAutoFlushLevel::SpritesOnly && prim != GS_SPRITE))
		return false;

	// Not using the same channels.
	if (!(GSUtil::GetChannelMask(m_context->TEX0.PSM) & GSUtil::GetChannelMask(m_context->FRAME.PSM, m_context->FRAME.FBMSK | ~(GSLocalMemory::m_psm[m_context->FRAME.PSM].fmsk))))
		return false;

	const u32 frame_mask = GSLocalMemory::m_psm[m_context->FRAME.PSM].fmsk;
	const bool frame_hit = m_context->FRAME.Block() == m_context->TEX0.TBP0 && !(m_context->TEST.ATE && m_context->TEST.ATST == 0 && m_context->TEST.AFAIL == 2) && ((m_context->FRAME.FBMSK & frame_mask) != frame_mask);
	// There's a strange behaviour we need to test on a PS2 here, if the FRAME is a Z format, like Powerdrome something swaps over, and it seems Alpha Fail of "FB Only" writes to the Z.. it's odd.
	const bool z_needed = !(m_context->TEST.ATE && m_context->TEST.ATST == 0 && m_context->TEST.AFAIL != 2) && !m_context->ZBUF.ZMSK;
	const bool zbuf_hit = (m_context->ZBUF.Block() == m_context->TEX0.TBP0) && z_needed;
	const u32 frame_z_psm = frame_hit ? m_context->FRAME.PSM : m_context->ZBUF.PSM;
	const u32 frame_z_bp = frame_hit ? m_context->FRAME.Block() : m_context->ZBUF.Block();

	if ((frame_hit || zbuf_hit) && GSUtil::HasSharedBits(frame_z_bp, frame_z_psm, m_context->TEX0.TBP0, m_context->TEX0.PSM))
		return true;

	return false;
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
			return (std::numeric_limits<u16>::max() / 4) - 4;

		// Four indices per two vertices.
		case GS_SPRITE:
			return (std::numeric_limits<u16>::max() / 2) - 2;

		case GS_TRIANGLELIST:
		case GS_TRIANGLESTRIP:
		case GS_TRIANGLEFAN:
		default:
			return (std::numeric_limits<u16>::max() - 3);
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
			const u32 startbp = fpsm.info.bn(temp_draw_rect.x, temp_draw_rect.y, ctx.FRAME.Block(), ctx.FRAME.FBW);

			// If it's a point, then we only have one coord, so the address for start and end will be the same, which is bad for the following check.
			u32 endbp = startbp;
			// otherwise calculate the end.
			if (prim != GS_POINTLIST || (m_index.tail > 1))
				endbp = fpsm.info.bn(temp_draw_rect.z - 1, temp_draw_rect.w - 1, ctx.FRAME.Block(), ctx.FRAME.FBW);

			m_mem.m_clut.InvalidateRange(startbp, endbp, true);
		}
	}
}

template<u32 prim, bool index_swap>
__noinline void GSState::HandleAutoFlush()
{
	// Kind of a cheat, making the assumption that 2 consecutive fan/strip triangles won't overlap each other (*should* be safe)
	if ((m_index.tail & 1) && (prim == GS_TRIANGLESTRIP || prim == GS_TRIANGLEFAN) && !m_texflush_flag)
		return;

	// To briefly explain what's going on here, what we are checking for is draws over a texture when the source and destination are themselves.
	// Because one page of the texture gets buffered in the Texture Cache (the PS2's one) if any of those pixels are overwritten, you still read the old data.
	// So we need to calculate if a page boundary is being crossed for the format it is in and if the same part of the texture being written and read inside the draw.
	if (IsAutoFlushDraw(prim))
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

				tex_coord.x = f32_to_s32_sat(std::round((1 << m_context->TEX0.TW) * s));
				tex_coord.y = f32_to_s32_sat(std::round((1 << m_context->TEX0.TH) * t));
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

			tex_coord.x = f32_to_s32_sat(std::round((1 << m_context->TEX0.TW) * s));
			tex_coord.y = f32_to_s32_sat(std::round((1 << m_context->TEX0.TH) * t));
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
		if (tex_psm.depth == frame_psm.depth && tex_rect.rintersect(temp_draw_rect).rempty())
			return;
		if (m_texflush_flag)
		{
			Flush(GSFlushReason::AUTOFLUSH);
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

				while (current_draw_end >= n)
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
						Flush(GSFlushReason::AUTOFLUSH);
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
					GSVector4i area_out = temp_draw_rect;
					area_out += GSVector4i(0, 0, frame_page_mask.z, frame_page_mask.w); // round up to the next page as we will be comparing by page.
					area_out &= frame_page_mask;
					area_out = GSVector4i(area_out.x / frame_psm.pgs.x, area_out.y / frame_psm.pgs.y, area_out.z / frame_psm.pgs.x, area_out.w / frame_psm.pgs.y);

					if (!area_out.rintersect(tex_rect).rempty())
						Flush(GSFlushReason::AUTOFLUSH);
				}
				else // Formats are too different so just flush it.
					Flush(GSFlushReason::AUTOFLUSH);
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

	if (auto_flush && skip == 0 && m_index.tail > 0 && ((m_vertex.tail + 1) - m_vertex.head) >= n)
	{
		HandleAutoFlush<prim, index_swap>();
	}

	u32 head = m_vertex.head;
	u32 tail = m_vertex.tail;
	u32 next = m_vertex.next;
	u32 xy_tail = m_vertex.xy_tail;

	if (GSConfig.UserHacks_Limit24BitDepth != GSLimit24BitDepth::Disabled &&
		GSIsHardwareRenderer() && GSLocalMemory::m_psm[m_context->ZBUF.PSM].bpp == 32)
	{
		if (GSConfig.UserHacks_Limit24BitDepth == GSLimit24BitDepth::PrioritizeUpper)
			m_v.XYZ.Z = ((m_v.XYZ.Z >> 8) & ~0xFF) | (m_v.XYZ.Z & 0xFF);
		else if (GSConfig.UserHacks_Limit24BitDepth == GSLimit24BitDepth::PrioritizeLower)
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

	constexpr u32 max_vertices = MaxVerticesForPrim(prim);
	if (max_vertices != 0 && m_vertex.tail >= max_vertices)
		Flush(VERTEXCOUNT);
}

/// Checks if region repeat is used (applying it does something to at least one of the values in min...max)
/// Also calculates the real min and max values seen after applying the region repeat to all values in min...max
static bool UsesRegionRepeat(int fix, int msk, int min, int max, int* min_out, int* max_out)
{
	if ((min < 0) != (max < 0))
	{
		// Algorithm doesn't work properly if bits overflow when incrementing (happens on the -1 → 0 crossing)
		// Conveniently, crossing zero guarantees you use the full range
		*min_out = fix;
		*max_out = (fix | msk) + 1;
		return true;
	}

	const int cleared_bits = ~msk & ~fix; // Bits that are always cleared by applying msk and fix
	const int set_bits = fix; // Bits that are always set by applying msk and fix
	unsigned long msb;
	int variable_bits = min ^ max;
	if (_BitScanReverse(&msb, variable_bits))
		variable_bits |= (1 << msb) - 1; // Fill in all lower bits

	const int always_set = min & ~variable_bits;   // Bits that are set in every value in min...max
	const int sometimes_set = min | variable_bits; // Bits that are set in at least one value in min...max

	const bool sets_bits = (set_bits | always_set) != always_set; // At least one bit in min...max is set by applying msk and fix
	const bool clears_bits = (cleared_bits & sometimes_set) != 0; // At least one bit in min...max is cleared by applying msk and fix

	const int overwritten_variable_bits = (cleared_bits | set_bits) & variable_bits;
	// A variable bit that's `0` in `min` will at some point switch to a `1` (because it's variable)
	// When it does, all bits below it will switch to a `0` (that's how incrementing works)
	// If the 0 to 1 switch is reflected in the final output (not masked and not replaced by a fixed value),
	// the final value would be larger than the previous.  Otherwise, the final value will be less.
	// The true minimum value is `min` with all bits below the most significant replaced variable `0` bit cleared
	const int min_overwritten_variable_zeros = ~min & overwritten_variable_bits;
	if (_BitScanReverse(&msb, min_overwritten_variable_zeros))
		min &= (~0u << msb);
	// Similar thing for max, but the first masked `1` bit
	const int max_overwritten_variable_ones = max & overwritten_variable_bits;
	if (_BitScanReverse(&msb, max_overwritten_variable_ones))
		max |= (1 << msb) - 1;

	*min_out = (msk & min) | fix;
	*max_out = ((msk & max) | fix) + 1;

	return sets_bits || clears_bits;
}

GSState::TextureMinMaxResult GSState::GetTextureMinMax(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear, bool clamp_to_tsize)
{
	// TODO: some of the +1s can be removed if linear == false

	const int tw = TEX0.TW;
	const int th = TEX0.TH;

	const int w = 1 << tw;
	const int h = 1 << th;
	const int tw_mask = (1 << tw) - 1;
	const int th_mask = (1 << th) - 1;

	GSVector4i tr(0, 0, w, h);

	const int wms = CLAMP.WMS;
	const int wmt = CLAMP.WMT;

	const int minu = (int)CLAMP.MINU;
	const int minv = (int)CLAMP.MINV;
	const int maxu = (int)CLAMP.MAXU;
	const int maxv = (int)CLAMP.MAXV;

	GSVector4i vr = tr;

	switch (wms)
	{
		case CLAMP_REPEAT:
			break;
		case CLAMP_CLAMP:
			break;
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
		case CLAMP_REPEAT:
			break;
		case CLAMP_CLAMP:
			break;
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

	// Software renderer fixes TEX0 so that TW/TH contain MAXU/MAXV.
	// Hardware renderer doesn't, and handles it in the texture cache, so don't clamp here.
	if (clamp_to_tsize)
		vr = vr.rintersect(tr);
	else
		tr = tr.runion(vr);

	u8 uses_border = 0;

	if (m_vt.m_max.t.x >= FLT_MAX || m_vt.m_min.t.x <= -FLT_MAX ||
		m_vt.m_max.t.y >= FLT_MAX || m_vt.m_min.t.y <= -FLT_MAX)
	{
		// If any of the min/max values are +-FLT_MAX we can't rely on them
		// so just assume full texture.
		uses_border = 0xF;
	}
	else
	{
		// Optimisation aims to reduce the amount of texture loaded to only the bit which will be read
		GSVector4 st = m_vt.m_min.t.xyxy(m_vt.m_max.t);
		if (linear)
		{
			st += GSVector4(-0.5f, 0.5f).xxyy();
			
			// If it's the start of the texture and our little adjustment is all that pushed it over, clamp it to 0.
			// This stops the border check failing when using repeat but needed less than the full texture
			// since this was making it take the full texture even though it wasn't needed.
			if (!clamp_to_tsize && ((m_vt.m_min.t.floor() == GSVector4::zero()).mask() & 0x3) == 0x3)
				st = st._max(GSVector4::zero());
		}

		// draw will get scissored, adjust UVs to suit
		const GSVector2 pos_range(pcsx2_max_f(m_vt.m_max.p.x - m_vt.m_min.p.x, 1.0f), pcsx2_max_f(m_vt.m_max.p.y - m_vt.m_min.p.y, 1.0f));
		const GSVector2 uv_range(m_vt.m_max.t.x - m_vt.m_min.t.x, m_vt.m_max.t.y - m_vt.m_min.t.y);
		const GSVector2 grad(uv_range / pos_range);
		// Adjust texture range when sprites get scissor clipped. Since we linearly interpolate, this
		// optimization doesn't work when perspective correction is enabled.
		if (m_vt.m_primclass == GS_SPRITE_CLASS && PRIM->FST == 1 && m_primitive_covers_without_gaps != NoGapsType::GapsFound)
		{
			// When coordinates are fractional, GS appears to draw to the right/bottom (effectively
			// taking the ceiling), not to the top/left (taking the floor).
			const GSVector4i int_rc(m_vt.m_min.p.ceil().xyxy(m_vt.m_max.p.floor()));
			const GSVector4i scissored_rc(int_rc.rintersect(m_context->scissor.in));
			if (!int_rc.eq(scissored_rc))
			{

				const GSVertex* vert_first = &m_vertex.buff[m_index.buff[0]];
				const GSVertex* vert_second = &m_vertex.buff[m_index.buff[1]];
				const GSVertex* vert_third = &m_vertex.buff[m_index.buff[2]];

				GSVector4 new_st = st;
				// Check if the UV coords are going in a different direction to the verts, if they match direction, no need to swap.
				// Use the FST integer UV directly, otherwise derive it from the STQ coords (divided by Q).
				//
				// The first-vs-second compare divides both sides by the FIRST
				// vertex's Q while the first-vs-third one below gives each
				// vertex its own. That asymmetry is upstream's -- it is in
				// PCSX2 master and it survived 4e1975ec8, the commit that
				// fixed the other copy/paste slip in this function -- so it is
				// left as is. Sharing the divisor cancels it, making the pair
				// compare raw S, which is what a sprite wants anyway: Q is
				// flat across one, and only the second vertex's is even valid.
				// For a triangle it means one leg of the OR is not
				// perspective-corrected and the other is.
				bool u_forward_check = PRIM->FST ? (vert_first->U < vert_second->U) : ((vert_first->ST.S / vert_first->RGBAQ.Q) < (vert_second->ST.S / vert_first->RGBAQ.Q));
				bool x_forward_check = vert_first->XYZ.X < vert_second->XYZ.X;

				if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
				{
					u_forward_check |= PRIM->FST ? (vert_first->U < vert_third->U) : ((vert_first->ST.S / vert_first->RGBAQ.Q) < (vert_third->ST.S / vert_third->RGBAQ.Q));
					x_forward_check |= vert_first->XYZ.X < vert_third->XYZ.X;
				}

				const bool swap_x = u_forward_check != x_forward_check;

				if (int_rc.left < scissored_rc.left)
				{
					if (!swap_x)
						new_st.x += floor(static_cast<float>(scissored_rc.left - int_rc.left) * grad.x);
					else
						new_st.z -= floor(static_cast<float>(scissored_rc.left - int_rc.left) * grad.x);
				}
				if (int_rc.right > scissored_rc.right)
				{
					if (!swap_x)
						new_st.z -= floor(static_cast<float>(int_rc.right - scissored_rc.right) * grad.x);
					else
						new_st.x += floor(static_cast<float>(int_rc.right - scissored_rc.right) * grad.x);
				}
				// we need to check that it's not going to repeat over the non-clipped part
				if (wms != CLAMP_REGION_REPEAT && (wms != CLAMP_REPEAT || (static_cast<int>(new_st.x) & ~tw_mask) == (static_cast<int>(new_st.z - 1) & ~tw_mask)))
				{
					st.x = new_st.x;
					st.z = new_st.z;
				}

				bool v_forward_check = PRIM->FST ? (vert_first->V < vert_second->V) : ((vert_first->ST.T / vert_first->RGBAQ.Q) < (vert_second->ST.T / vert_first->RGBAQ.Q));
				bool y_forward_check = vert_first->XYZ.Y < vert_second->XYZ.Y;

				if (m_vt.m_primclass == GS_TRIANGLE_CLASS)
				{
					v_forward_check |= PRIM->FST ? (vert_first->V < vert_third->V) : ((vert_first->ST.T / vert_first->RGBAQ.Q) < (vert_third->ST.T / vert_third->RGBAQ.Q));
					y_forward_check |= vert_first->XYZ.Y < vert_third->XYZ.Y;
				}

				const bool swap_y = v_forward_check != y_forward_check;

				if (int_rc.top < scissored_rc.top)
				{
					if (!swap_y)
						new_st.y += floor(static_cast<float>(scissored_rc.top - int_rc.top) * grad.y);
					else
						new_st.w -= floor(static_cast<float>(scissored_rc.top - int_rc.top) * grad.y);
				}
				if (int_rc.bottom > scissored_rc.bottom)
				{
					if (!swap_y)
						new_st.w -= floor(static_cast<float>(int_rc.bottom - scissored_rc.bottom) * grad.y);
					else
						new_st.y += floor(static_cast<float>(int_rc.bottom - scissored_rc.bottom) * grad.y);
				}
				if (wmt != CLAMP_REGION_REPEAT && (wmt != CLAMP_REPEAT || (static_cast<int>(new_st.y) & ~th_mask) == (static_cast<int>(new_st.w - 1) & ~th_mask)))
				{
					st.y = new_st.y;
					st.w = new_st.w;
				}
			}
		}

		const GSVector4i uv = GSVector4i(st.floor());
		uses_border = GSVector4::cast((uv < vr).blend32<0xc>(uv >= vr)).mask();
		// Need to make sure we don't oversample, this can cause trouble in grabbing textures.
		// This may be inaccurate depending on the draw, but adding 1 all the time is wrong too.
		// FIXME: It breaks sw renderer so let's still use 1 for SW mode for now.
		const int inclusive_x_req = GSIsHardwareRenderer() ? (((m_vt.m_primclass < GS_TRIANGLE_CLASS) || (grad.x < 1.0f || (grad.x == 1.0f && m_vt.m_max.p.x != floor(m_vt.m_max.p.x)))) ? 1 : 0) : 1;
		const int inclusive_y_req = GSIsHardwareRenderer() ? (((m_vt.m_primclass < GS_TRIANGLE_CLASS) || (grad.y < 1.0f || (grad.y == 1.0f && m_vt.m_max.p.y != floor(m_vt.m_max.p.y)))) ? 1 : 0) : 1;

		// Roughly cut out the min/max of the read (Clamp)

		switch (wms)
		{
			case CLAMP_REPEAT:
				if ((uv.x & ~tw_mask) == (uv.z & ~tw_mask))
				{
					vr.x = pcsx2_max_f(vr.x, uv.x & tw_mask);
					vr.z = pcsx2_min_f(vr.z, (uv.z & tw_mask) + inclusive_x_req);
				}
				break;
			case CLAMP_CLAMP:
			case CLAMP_REGION_CLAMP:
				if (vr.x < uv.x)
					vr.x = pcsx2_min_f(uv.x, vr.z - 1);
				if (vr.z > (uv.z + 1))
					vr.z = pcsx2_max_f(uv.z, vr.x) + inclusive_x_req;
				break;
			case CLAMP_REGION_REPEAT:
				if (UsesRegionRepeat(maxu, minu, uv.x, uv.z, &vr.x, &vr.z) || maxu >= tw)
					uses_border |= TextureMinMaxResult::USES_BOUNDARY_U;
				break;
		}

		switch (wmt)
		{
			case CLAMP_REPEAT:
				if ((uv.y & ~th_mask) == (uv.w & ~th_mask))
				{
					vr.y = pcsx2_max_f(vr.y, uv.y & th_mask);
					vr.w = pcsx2_min_f(vr.w, (uv.w & th_mask) + inclusive_y_req);
				}
				break;
			case CLAMP_CLAMP:
			case CLAMP_REGION_CLAMP:
				if (vr.y < uv.y)
					vr.y = pcsx2_min_f(uv.y, vr.w - 1);
				if (vr.w > (uv.w + 1))
					vr.w = pcsx2_max_f(uv.w, vr.y) + inclusive_y_req;
				break;
			case CLAMP_REGION_REPEAT:
				if (UsesRegionRepeat(maxv, minv, uv.y, uv.w, &vr.y, &vr.w) || maxv >= th)
					uses_border |= TextureMinMaxResult::USES_BOUNDARY_V;
				break;
		}
	}

	vr = vr.rintersect(tr);

	// This really shouldn't happen now except with the clamping region set entirely outside the texture,
	// special handling should be written for that case.
	if (vr.rempty())
	{
		// NOTE: this can happen when texcoords are all outside the texture or clamping area is zero, but we can't
		// let the texture cache update nothing, the sampler will still need a single texel from the border somewhere
		// examples:
		// - THPS (no visible problems)
		// - NFSMW (strange rectangles on screen, might be unrelated)
		// - Lupin 3rd (huge problems, textures sizes seem to be randomly specified)

		const bool inc_x = vr.x < tr.z;
		const bool inc_y = vr.y < tr.w;
		vr = (vr + GSVector4i(inc_x ? 0 : -1, inc_y ? 0 : -1, inc_x ? 1 : 0, inc_y ? 1 : 0)).rintersect(tr);
	}
	else if (vr.xxzz().rempty())
	{
		const bool inc_x = vr.x < tr.z;
		vr = (vr + GSVector4i(inc_x ? 0 : -1, 0, inc_x ? 1 : 0, 0)).rintersect(tr);
	}
	else if (vr.yyww().rempty())
	{
		const bool inc_y = vr.y < tr.w;
		vr = (vr + GSVector4i(0, inc_y ? 0 : -1, 0, inc_y ? 1 : 0)).rintersect(tr);
	}

	return { vr, uses_border };
}

void GSState::CalcAlphaMinMax(const int tex_alpha_min, const int tex_alpha_max)
{
	if (m_vt.m_alpha.valid && tex_alpha_min == 0 && tex_alpha_max == 255)
		return;

	// We wanted to force an update as we now know the alpha of the non-indexed texture.
	// Limit max to 255 as we send 500 when we don't know, makes calculating 24/16bit easier.
	int min = tex_alpha_min, max = pcsx2_min_i(tex_alpha_max, 255);

	if (IsCoverageAlpha())
	{
		// HW renderer doesn't currently support AA, so its min is 128.
		// If we add AA support to the HW renderer, this will need to be changed.
		// (Will probably only be supported with ROV/FBFetch so we would want to check for that.)
		min = GSIsHardwareRenderer() ? 128 : 0;
		max = 128;
	}
	else
	{
		const GSDrawingContext* context = m_context;
		GSVector4i a = m_vt.m_min.c.uph32(m_vt.m_max.c).zzww();
		if (PRIM->TME && context->TEX0.TCC)
		{
			const GSDrawingEnvironment& env = *m_draw_env;

			switch (GSLocalMemory::m_psm[context->TEX0.PSM].fmt)
			{
				case 0:
					a.y = min;
					a.w = max;
					break;
				case 1:
					// If we're using the alpha from the texture, not the whole range, we can just use tex_alpha_min/max.
					// AEM and TA0 re precomputed with GSBlock::ReadAndExpandBlock24, so already worked out for tex_alpha.
					a.y = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? min : (env.TEXA.AEM ? 0 : env.TEXA.TA0);
					a.w = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? max : env.TEXA.TA0;
					break;
				case 2:
					// If we're using the alpha from the texture, not the whole range, we can just use tex_alpha_min/max.
					// AEM, TA0 and TA1 are precomputed with GSBlock::ReadAndExpandBlock16, so already worked out for tex_alpha.
					a.y = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? min : (env.TEXA.AEM ? 0 : pcsx2_min_i(env.TEXA.TA0, env.TEXA.TA1));
					a.w = (tex_alpha_max < INVALID_ALPHA_MINMAX) ? max : pcsx2_max_i(env.TEXA.TA0, env.TEXA.TA1);
					break;
				case 3:
					if (tex_alpha_max < INVALID_ALPHA_MINMAX)
					{
						a.y = min;
						a.w = max;
					}
					else
						m_mem.m_clut.GetAlphaMinMax32(a.y, a.w);
					break;
				default:
					break;
			}

			switch (context->TEX0.TFX)
			{
				case TFX_MODULATE:
					a.x = (a.x * a.y) >> 7;
					a.z = (a.z * a.w) >> 7;
					if (a.x > 0xff)
						a.x = 0xff;
					if (a.z > 0xff)
						a.z = 0xff;
					break;
				case TFX_DECAL:
					a.x = a.y;
					a.z = a.w;
					break;
				case TFX_HIGHLIGHT:
					a.x = a.x + a.y;
					a.z = a.z + a.w;
					if (a.x > 0xff)
						a.x = 0xff;
					if (a.z > 0xff)
						a.z = 0xff;
					break;
				case TFX_HIGHLIGHT2:
					a.x = a.y;
					a.z = a.w;
					break;
				default:
					break;
			}
		}
		min = a.x;
		max = a.z;
	}

	m_vt.m_alpha.min = min;
	m_vt.m_alpha.max = max;
	m_vt.m_alpha.valid = true;
}

void GSState::CorrectATEAlphaMinMax(const u32 atst, const int aref)
{
	const GSVertexTrace::VertexAlpha& aminmax = GetAlphaMinMax();
	int amin = aminmax.min;
	int amax = aminmax.max;
	switch (atst)
	{
		case ATST_LESS:
			amin = pcsx2_min_i(amin, pcsx2_max_i(aref - 1, amin));
			amax = pcsx2_min_i(amax, pcsx2_max_i(aref - 1, amin));
			break;
		case ATST_LEQUAL:
			amin = pcsx2_min_i(amin, pcsx2_max_i(aref, amin));
			amax = pcsx2_min_i(amax, pcsx2_max_i(aref, amin));
			break;
		case ATST_EQUAL:
			amax = aref;
			amin = aref;
			break;
		case ATST_GEQUAL:
			amax = pcsx2_max_i(amax, pcsx2_min_i(aref, amax));
			amin = pcsx2_max_i(amin, pcsx2_min_i(aref, amax));
			break;
		case ATST_GREATER:
			amax = pcsx2_max_i(amax, pcsx2_min_i(aref + 1, amax));
			amin = pcsx2_max_i(amin, pcsx2_min_i(aref + 1, amax));
			break;
		default:
			break;
	}

	m_vt.m_alpha.min = amin;
	m_vt.m_alpha.max = amax;
}

bool GSState::TryAlphaTest(u32& fm, u32& zm)
{
	// Shortcut for the easy case
	if (m_context->TEST.ATST == ATST_ALWAYS)
		return true;

	const u32 framemask = GSLocalMemory::m_psm[m_context->FRAME.PSM].fmsk;
	const u32 framemaskalpha = GSLocalMemory::m_psm[m_context->FRAME.PSM].fmsk & 0xFF000000;
	const u32 fail_type = m_context->TEST.GetAFAIL(m_context->FRAME.PSM);
	// Alpha test can only control the write of some channels. If channels are already masked
	// the alpha test is therefore a nop.
	switch (fail_type)
	{
		case AFAIL_KEEP:
			break;
		case AFAIL_FB_ONLY:
			if (zm == 0xFFFFFFFF)
				return true;
			break;
		case AFAIL_ZB_ONLY:
			if ((fm & framemask) == framemask)
				return true;
			break;
		case AFAIL_RGB_ONLY:
			if (zm == 0xFFFFFFFF && (fm & framemaskalpha) == framemaskalpha)
				return true;
			break;
		default:
			break;
	}

	bool pass = true;

	if (m_context->TEST.ATST == ATST_NEVER)
	{
		pass = false; // Shortcut to avoid GetAlphaMinMax below
	}
	else
	{
		const GSVertexTrace::VertexAlpha& aminmax = GetAlphaMinMax();
		const int amin = aminmax.min;
		const int amax = aminmax.max;

		const int aref = m_context->TEST.AREF;

		switch (m_context->TEST.ATST)
		{
			case ATST_NEVER:
				pass = false;
				break;
			case ATST_ALWAYS:
				pass = true;
				break;
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
				fm = zm = 0xffffffff;
				break;
			case AFAIL_FB_ONLY:
				zm = 0xffffffff;
				break;
			case AFAIL_ZB_ONLY:
				fm = 0xffffffff;
				break;
			case AFAIL_RGB_ONLY:
				fm |= 0xff000000;
				zm = 0xffffffff;
				break;
			default:
				break;
		}
	}

	return true;
}

bool GSState::IsOpaque()
{
	if (PRIM->AA1)
		return false;

	if (!PRIM->ABE)
		return true;

	const GSDrawingContext* context = m_context;

	int amin = 0;
	int amax = 0xff;

	if (context->ALPHA.A != context->ALPHA.B)
	{
		if (context->ALPHA.C == 0)
		{
			amin = GetAlphaMinMax().min;
			amax = GetAlphaMinMax().max;
		}
		else if (context->ALPHA.C == 1)
		{
			if (context->FRAME.PSM == PSMCT24 || context->FRAME.PSM == PSMZ24)
				amin = amax = 0x80;
		}
		else if (context->ALPHA.C == 2)
		{
			amin = amax = context->ALPHA.FIX;
		}
	}

	return context->ALPHA.IsOpaque(amin, amax);
}

bool GSState::IsMipMapDraw()
{
	return m_context->TEX1.MXL > 0 && m_context->TEX1.MMIN >= 2 && m_context->TEX1.MMIN <= 5 && m_vt.m_lod.y > 0;
}

bool GSState::IsMipMapActive()
{
	return m_mipmap && IsMipMapDraw();
}

bool GSState::IsCoverageAlpha()
{
	return !PRIM->ABE && PRIM->AA1 && (m_vt.m_primclass == GS_LINE_CLASS || m_vt.m_primclass == GS_TRIANGLE_CLASS);
}

GIFRegTEX0 GSState::GetTex0Layer(u32 lod)
{
	// Shortcut
	if (lod == 0)
		return m_context->TEX0;

	GIFRegTEX0 TEX0 = m_context->TEX0;

	switch (lod)
	{
		case 1:
			TEX0.TBP0 = m_context->MIPTBP1.TBP1;
			TEX0.TBW = m_context->MIPTBP1.TBW1;
			break;
		case 2:
			TEX0.TBP0 = m_context->MIPTBP1.TBP2;
			TEX0.TBW = m_context->MIPTBP1.TBW2;
			break;
		case 3:
			TEX0.TBP0 = m_context->MIPTBP1.TBP3;
			TEX0.TBW = m_context->MIPTBP1.TBW3;
			break;
		case 4:
			TEX0.TBP0 = m_context->MIPTBP2.TBP4;
			TEX0.TBW = m_context->MIPTBP2.TBW4;
			break;
		case 5:
			TEX0.TBP0 = m_context->MIPTBP2.TBP5;
			TEX0.TBW = m_context->MIPTBP2.TBW5;
			break;
		case 6:
			TEX0.TBP0 = m_context->MIPTBP2.TBP6;
			TEX0.TBW = m_context->MIPTBP2.TBW6;
			break;
		default:
			log_cb(RETRO_LOG_ERROR, "GS: Invalid guest lod setting. Please report: https://github.com/PCSX2/pcsx2/issues\n");
	}

	// Correct the texture size
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

// GSTransferBuffer

GSState::GSTransferBuffer::GSTransferBuffer()
{
	constexpr size_t alloc_size = 1024 * 1024 * 4;
	buff = reinterpret_cast<u8*>(memalign_alloc(32, alloc_size));
}

GSState::GSTransferBuffer::~GSTransferBuffer()
{
	memalign_free(buff);
}

void GSState::GSTransferBuffer::Init(int tx, int ty, const GIFRegBITBLTBUF& blit, bool is_write)
{
	x = tx;
	y = ty;
	total = 0;
	start = 0;
	end = 0;
	m_blit = blit;
	write = is_write;
}

bool GSState::GSTransferBuffer::Update(int tw, int th, int bpp, int& len)
{
	int tex_size = (((tw * th * bpp) + 7) >> 3); // Round to nearest byte

	if (total == 0)
		total = pcsx2_min_i(tex_size, 1024 * 1024 * 4);

	const int remaining = total - end;

	if (len > remaining)
		len = remaining;
	return len > 0;
}

// The horizontal offset values (under z) for PAL and NTSC have been tweaked
// they should be apparently 632 and 652 respectively, but that causes a thick black line on the left
// these values leave a small black line on the right in a bunch of games, but it's not so bad.
// The only conclusion I can come to is there is horizontal overscan expected so there would normally
// be black borders either side anyway, or both sides slightly covered.
static inline constexpr GSVector4i VideoModeOffsets[6] = {
	GSVector4i::cxpr(640, 224, 642, 25),
	GSVector4i::cxpr(640, 256, 676, 36),
	GSVector4i::cxpr(640, 480, 276, 34),
	GSVector4i::cxpr(720, 480, 232, 35),
	GSVector4i::cxpr(1280, 720, 302, 24),
	GSVector4i::cxpr(1920, 540, 238, 40)
};

static inline constexpr GSVector4i VideoModeOffsetsOverscan[6] = {
	GSVector4i::cxpr(711, 240, 498, 17),
	GSVector4i::cxpr(711, 288, 532, 21),
	GSVector4i::cxpr(640, 480, 276, 34),
	GSVector4i::cxpr(720, 480, 232, 35),
	GSVector4i::cxpr(1280, 720, 302, 24),
	GSVector4i::cxpr(1920, 540, 238, 40)
};

static inline constexpr GSVector4i VideoModeDividers[6] = {
	GSVector4i::cxpr(3, 0, 2559, 239),
	GSVector4i::cxpr(3, 0, 2559, 287),
	GSVector4i::cxpr(1, 0, 1279, 479),
	GSVector4i::cxpr(1, 0, 1439, 479),
	GSVector4i::cxpr(0, 0, 1279, 719),
	GSVector4i::cxpr(0, 0, 1919, 1079)
};

bool GSState::GSPCRTCRegs::IsAnalogue()
{
	const GSVideoMode video = static_cast<GSVideoMode>(videomode + 1);
	return video == GSVideoMode::NTSC || video == GSVideoMode::PAL || video == GSVideoMode::HDTV_1080I;
}

// Calculates which display is closest to matching zero offsets in either direction.
GSVector2i GSState::GSPCRTCRegs::NearestToZeroOffset()
{
	GSVector2i returnValue = { 1, 1 };

	if (!PCRTCDisplays[0].enabled && !PCRTCDisplays[1].enabled)
		return returnValue;

	for (int i = 0; i < 2; i++)
	{
		if (!PCRTCDisplays[i].enabled)
		{
			returnValue.x = 1 - i;
			returnValue.y = 1 - i;
			return returnValue;
		}
	}

	if (abs(PCRTCDisplays[0].displayOffset.x - VideoModeOffsets[VideoModeRow()].z) <
		abs(PCRTCDisplays[1].displayOffset.x - VideoModeOffsets[VideoModeRow()].z))
		returnValue.x = 0;

	// When interlaced, the vertical base offset is doubled
	const int verticalOffset = VideoModeOffsets[VideoModeRow()].w * (1 << interlaced);

	if (abs(PCRTCDisplays[0].displayOffset.y - verticalOffset) <
		abs(PCRTCDisplays[1].displayOffset.y - verticalOffset))
		returnValue.y = 0;

	return returnValue;
}

void GSState::GSPCRTCRegs::SetVideoMode(GSVideoMode videoModeIn)
{
	videomode = static_cast<int>(videoModeIn) - 1;
}

// Enable each of the displays.
void GSState::GSPCRTCRegs::EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle)
{
	PCRTCDisplays[0].enabled = pmode.EN1;
	PCRTCDisplays[1].enabled = pmode.EN2;

	interlaced = smode2.INT && IsAnalogue();
	FFMD = smode2.FFMD;
	toggling_field = smodetoggle && IsAnalogue();
}

void GSState::GSPCRTCRegs::CheckSameSource()
{
	if (PCRTCDisplays[0].enabled != PCRTCDisplays[1].enabled || (PCRTCDisplays[0].enabled | PCRTCDisplays[1].enabled) == false)
	{
		PCRTCSameSrc = false;
		return;
	}

	PCRTCSameSrc = PCRTCDisplays[0].FBP == PCRTCDisplays[1].FBP &&
	PCRTCDisplays[0].FBW == PCRTCDisplays[1].FBW &&
	GSUtil::HasCompatibleBits(PCRTCDisplays[0].PSM, PCRTCDisplays[1].PSM);
}
		
bool GSState::GSPCRTCRegs::FrameWrap()
{
	const GSVector4i combined_rect = GSVector4i(PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect));
	return combined_rect.w >= 2048 || combined_rect.z >= 2048;
}

// If the start point of both frames match, we can do a single read
bool GSState::GSPCRTCRegs::FrameRectMatch()
{
	return PCRTCSameSrc;
}

GSVector2i GSState::GSPCRTCRegs::GetResolution()
{
	GSVector2i resolution;

	const GSVector4i offsets = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()] : VideoModeOffsetsOverscan[VideoModeRow()];
	const bool is_full_height = interlaced || (toggling_field && GSConfig.InterlaceMode != GSInterlaceMode::Off);

	if (!GSConfig.PCRTCOffsets)
	{
		if (PCRTCDisplays[0].enabled && PCRTCDisplays[1].enabled)
		{
			const GSVector4i combined_size = PCRTCDisplays[0].displayRect.runion(PCRTCDisplays[1].displayRect);
			resolution = { combined_size.width(), combined_size.height() };
		}
		else if (PCRTCDisplays[0].enabled)
		{
			resolution = { PCRTCDisplays[0].displayRect.width(), PCRTCDisplays[0].displayRect.height() };
		}
		else
		{
			resolution = { PCRTCDisplays[1].displayRect.width(), PCRTCDisplays[1].displayRect.height() };
		}
	}
	else
	{
		const int shift = is_full_height ? 1 : 0;
		resolution = { offsets.x, offsets.y << shift };
	}

	resolution.x = pcsx2_min_f(resolution.x, offsets.x);
	resolution.y = pcsx2_min_f(resolution.y, is_full_height ? offsets.y << 1 : offsets.y);

	return resolution;
}


// --------------------------------------------------------------------------------------
//  Vertex / quad utility functions
// --------------------------------------------------------------------------------------
// Ported from upstream refactor 26bd916e3 ("GS: Add utility functions for
// vertices/quads"), used by the texture-shuffle detection refactor.
//
// Fork adaptations:
//  - The GS_TRIANGLE_CLASS path of GetQuadCornersImpl (which detects when two
//    triangles form a quad via AreTrianglesQuad/TriangleOrdering) is stubbed to
//    return false. The fork's shuffle detection is sprite-class only, matching
//    its current behaviour; triangle-quad detection and AreTrianglesQuad are a
//    separate prerequisite that can be ported later if triangle shuffles are
//    wanted. The sprite path is faithful.
//  - The GEN_TMPL_SELECT_* macros' unreachable else branch used pxFail(), which
//    the fork's GS does not provide; it is dropped (the branch is unreachable
//    because m_primclass is sprite or triangle for these calls).

__fi GSVector4 GSState::GetXYWindow(const GSVertex& v)
{
	return GSVector4(GetVertexXY(v) - m_context->scissor.xyof.xyxy()) / 16.0f;
}

template<bool fst>
__fi GSVector4 GSState::GetTexCoordsImpl(const GSVertex& v, float q)
{
	if constexpr (fst)
	{
		return GSVector4(GetVertexUV(v)) / 16.0f;
	}
	else
	{
		const float tw = static_cast<float>(1 << m_context->TEX0.TW);
		const float th = static_cast<float>(1 << m_context->TEX0.TH);
		const GSVector4 tex_size(tw, th, tw, th);
		return GSVector4(GetVertexST(v) / q * tex_size);
	}
}

template<bool fst>
__fi GSVector4 GSState::GetTexCoordsImpl(const GSVertex& v)
{
	return GetTexCoordsImpl<fst>(v, v.RGBAQ.Q);
}

__fi GSVector4 GSState::GetTexCoords(const GSVertex& v, float q)
{
	if (PRIM->FST)
	{
		return GetTexCoordsImpl<true>(v, q);
	}
	else
	{
		return GetTexCoordsImpl<false>(v, q);
	}
}

__fi GSVector4 GSState::GetTexCoords(const GSVertex& v)
{
	return GetTexCoords(v, v.RGBAQ.Q);
}

template<u32 primclass, bool tme, bool fst>
bool GSState::GetQuadCornersImpl(const GSVertex* v, const u16* i, GSVertex& vout0, GSVertex& vout1)
{
	static_assert(primclass == GS_SPRITE_CLASS || primclass == GS_TRIANGLE_CLASS);

	if constexpr (primclass == GS_TRIANGLE_CLASS)
	{
		// Triangle-quad detection (AreTrianglesQuad/TriangleOrdering) is not
		// ported yet; the fork's shuffle detection is sprite-class only.
		return false;
	}
	else
	{
		// primclass == GS_SPRITE_CLASS
		vout0 = v[i[0]];
		vout1 = v[i[1]];
	}

	return true;
}

template<u32 primclass>
void GSState::GetQuadBBoxWindowImpl(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout)
{
	const GSVector4 xy0 = GetXYWindow(v0);
	const GSVector4 xy1 = GetXYWindow(v1);

	xyout = xy0._min(xy1).xyzw(xy0._max(xy1));
}

template<u32 primclass, bool tme, bool fst>
void GSState::GetQuadBBoxWindowImpl(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order)
{
	if constexpr (!tme)
	{
		GetQuadBBoxWindowImpl<primclass>(v0, v1, xyout);
		return;
	}

	GSVector4 xy0 = GetXYWindow(v0);
	GSVector4 xy1 = GetXYWindow(v1);
	GSVector4 tex0 = GetTexCoordsImpl<fst>(v0, primclass == GS_SPRITE_CLASS ? v1.RGBAQ.Q : v0.RGBAQ.Q);
	GSVector4 tex1 = GetTexCoordsImpl<fst>(v1, v1.RGBAQ.Q);

	if (!keep_tex_order)
	{
		xyout = xy0._min(xy1).xyzw(xy0._max(xy1));
		texout = tex0._min(tex1).xyzw(tex0._max(tex1));
	}
	else
	{
		xyout = xy0.xyzw(xy1);
		texout = tex0.xyzw(tex1);

		const int swap = (xy0 > xy1).mask();

		if (swap & 1)
		{
			xyout = xyout.zyxw();
			texout = texout.zyxw();
		}

		if (swap & 2)
		{
			xyout = xyout.xwzy();
			texout = texout.xwzy();
		}
	}
}

#define GEN_TMPL_SELECT_1(func, ...) \
if (m_vt.m_primclass == GS_TRIANGLE_CLASS) \
{ \
	func<GS_TRIANGLE_CLASS>(__VA_ARGS__); \
} \
else if (m_vt.m_primclass == GS_SPRITE_CLASS) \
{ \
	func<GS_SPRITE_CLASS>(__VA_ARGS__); \
}


#define GEN_TMPL_SELECT_2(func, ...) \
if (m_vt.m_primclass == GS_TRIANGLE_CLASS) \
{ \
	if (PRIM->TME) \
	{ \
		if (PRIM->FST) \
		{ \
			return func<GS_TRIANGLE_CLASS, true, true>(__VA_ARGS__); \
		} \
		else \
		{ \
			return func<GS_TRIANGLE_CLASS, true, false>(__VA_ARGS__); \
		} \
	} \
	else \
	{ \
		return func<GS_TRIANGLE_CLASS>(__VA_ARGS__); \
	} \
} \
else if (m_vt.m_primclass == GS_SPRITE_CLASS) \
{ \
	if (PRIM->TME) \
	{ \
		if (PRIM->FST) \
		{ \
			return func<GS_SPRITE_CLASS, true, true>(__VA_ARGS__); \
		} \
		else \
		{ \
			return func<GS_SPRITE_CLASS, true, false>(__VA_ARGS__); \
		} \
	} \
	else \
	{ \
		return func<GS_SPRITE_CLASS>(__VA_ARGS__); \
	} \
}

bool GSState::GetQuadCorners(const GSVertex* v, const u16* i, GSVertex& vout0, GSVertex& vout1)
{
	GEN_TMPL_SELECT_2(GetQuadCornersImpl, v, i, vout0, vout1);
	return false;
}

void GSState::GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout)
{
	GEN_TMPL_SELECT_1(GetQuadBBoxWindowImpl, v0, v1, xyout);
}

void GSState::GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order)
{
	GEN_TMPL_SELECT_2(GetQuadBBoxWindowImpl, v0, v1, xyout, texout, keep_tex_order);
}

#undef GEN_TMPL_SELECT_2
#undef GEN_TMPL_SELECT_1

void GSState::GetQuadRasterizedPoints(GSVector4& xy, GSVector4& tex, bool keep_order)
{
	// Swap so that coordinates are top-left and bottom-right.
	const int swap = (xy.xyxy() > xy.zwzw()).mask();

	if (swap & 1)
	{
		xy = xy.zyxw();
		tex = tex.zyxw();
	}

	if (swap & 2)
	{
		xy = xy.xwzy();
		tex = tex.xwzy();
	}

	const GSVector4 grad = (tex.zwzw() - tex.xyxy()) / (xy.zwzw() - xy.xyxy());

	// Round XY to contained pixels. Omit bottom-right pixels on the edge.
	GSVector4 xy_round = xy.ceil().xyzw(xy.floor());
	const GSVector4 bottom_right = GSVector4::zero().xyzw(xy == xy_round);
	xy_round = xy_round.blend32(xy_round - GSVector4(1.0f), bottom_right);

	// Interpolate texture coords.
	tex += grad * (xy_round - xy);

	xy = xy_round;

	// Swap back to original order if needed.
	if (keep_order)
	{
		if (swap & 1)
		{
			xy = xy.zyxw();
			tex = tex.zyxw();
		}

		if (swap & 2)
		{
			xy = xy.xwzy();
			tex = tex.xwzy();
		}
	}
}

void GSState::GetQuadRasterizedPoints(GSVector4& xy, bool keep_order)
{
	GSVector4 tex_ignore;
	GetQuadRasterizedPoints(xy, tex_ignore, keep_order);
}

GSVector4i GSState::GSPCRTCRegs::GetFramebufferRect(int display)
{
	if (display == -1)
		return GSVector4i(PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect));
	return PCRTCDisplays[display].framebufferRect;
}

int GSState::GSPCRTCRegs::GetFramebufferBitDepth()
{
	if (PCRTCDisplays[0].enabled)
		return GSLocalMemory::m_psm[PCRTCDisplays[0].PSM].bpp;
	else if (PCRTCDisplays[1].enabled)
		return GSLocalMemory::m_psm[PCRTCDisplays[1].PSM].bpp;

	return 32;
}

GSVector2i GSState::GSPCRTCRegs::GetFramebufferSize(int display)
{
	int max_height = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()].y : VideoModeOffsetsOverscan[VideoModeRow()].y;

	if (!(FFMD && interlaced))
		max_height *= 2;

	if (display == -1)
	{
		GSVector4i combined_rect = PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect);

		if (combined_rect.z >= 2048)
		{
			const int high_x = (PCRTCDisplays[0].framebufferRect.x > PCRTCDisplays[1].framebufferRect.x) ? PCRTCDisplays[0].framebufferRect.x : PCRTCDisplays[1].framebufferRect.x;
			combined_rect.z -= GSConfig.UseHardwareRenderer() ? 2048 : high_x;
			combined_rect.x = 0;
		}

		if (combined_rect.w >= 2048)
		{
			const int high_y = (PCRTCDisplays[0].framebufferRect.y > PCRTCDisplays[1].framebufferRect.y) ? PCRTCDisplays[0].framebufferRect.y : PCRTCDisplays[1].framebufferRect.y;
			combined_rect.w -= GSConfig.UseHardwareRenderer() ? 2048 : high_y;
			combined_rect.y = 0;
		}

		// Cap the framebuffer read to the maximum display height, otherwise the hardware renderer gets messy.
		const int min_mag = pcsx2_max_f(1, pcsx2_min_f(PCRTCDisplays[0].magnification.y, PCRTCDisplays[1].magnification.y));
		int offset = PCRTCDisplays[0].displayRect.runion(PCRTCDisplays[1].displayRect).y;

		if (FFMD && interlaced)
		{
			offset = (offset - 1) / 2;
		}

		// Hardware mode needs a wider framebuffer as it can't offset the read.
		if (GSConfig.UseHardwareRenderer())
		{
			combined_rect.z += pcsx2_max_f(PCRTCDisplays[0].framebufferOffsets.x, PCRTCDisplays[1].framebufferOffsets.x);
			combined_rect.w += pcsx2_max_f(PCRTCDisplays[0].framebufferOffsets.y, PCRTCDisplays[1].framebufferOffsets.y);
		}

		max_height += combined_rect.y;

		offset = (max_height / min_mag) - offset;
		combined_rect.w = pcsx2_min_f(combined_rect.w, offset);

		// For Off mode with non-interlaced content, ensure the framebuffer covers the full display height
		// (avoids truncation when the video mode offset cap is smaller than the actual frame height).
		if (GSConfig.InterlaceMode == GSInterlaceMode::Off && !interlaced)
			combined_rect.w = pcsx2_max_f(combined_rect.w, pcsx2_max_f(PCRTCDisplays[0].displayRect.w, PCRTCDisplays[1].displayRect.w));

		return GSVector2i(combined_rect.z, combined_rect.w);
	}
	else
	{
		GSVector4i out_rect = PCRTCDisplays[display].framebufferRect;

		if (out_rect.z >= 2048)
			out_rect.z -= out_rect.x;

		if (out_rect.w >= 2048)
			out_rect.w -= out_rect.y;

		// Cap the framebuffer read to the maximum display height, otherwise the hardware renderer gets messy.
		const int min_mag = pcsx2_max_f(1, PCRTCDisplays[display].magnification.y);
		int offset = PCRTCDisplays[display].displayRect.y;

		if (FFMD && interlaced)
			offset = (offset - 1) / 2;

		max_height += out_rect.y;

		offset = (max_height / min_mag) - offset;
		out_rect.w = pcsx2_min_f(out_rect.w, offset);

		// For Off mode, ensure the framebuffer covers the full display height.
		if (GSConfig.InterlaceMode == GSInterlaceMode::Off)
			out_rect.w = pcsx2_max_f(out_rect.w, PCRTCDisplays[display].displayRect.w);

		return GSVector2i(out_rect.z, out_rect.w);
	}
}

// Sets up the rectangles for both the framebuffer read and the displays for the merge circuit.
void GSState::GSPCRTCRegs::SetRects(int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg)
{
	// Save framebuffer information first, while we're here.
	PCRTCDisplays[display].FBP = framebufferReg.FBP;
	PCRTCDisplays[display].FBW = framebufferReg.FBW;
	PCRTCDisplays[display].PSM = framebufferReg.PSM;
	PCRTCDisplays[display].prevFramebufferReg = framebufferReg;
	// Probably not really enabled but will cause a mess.
	// Q-Ball Billiards enables both circuits but doesn't set one of them up.
	if (PCRTCDisplays[display].FBW == 0 && displayReg.DW == 0 && displayReg.DH == 0 && displayReg.MAGH == 0)
	{
		PCRTCDisplays[display].enabled = false;
		return;
	}
	PCRTCDisplays[display].magnification = GSVector2i(displayReg.MAGH + 1, displayReg.MAGV + 1);
	const u32 DW = displayReg.DW + 1;
	const u32 DH = displayReg.DH + 1;

	const int renderWidth = DW / PCRTCDisplays[display].magnification.x;
	const int renderHeight = DH / PCRTCDisplays[display].magnification.y;

	u32 finalDisplayWidth = renderWidth;
	u32 finalDisplayHeight = renderHeight;
	// When using screen offsets the screen gets squashed/resized in to the actual screen size.
	if (GSConfig.PCRTCOffsets)
	{
		finalDisplayWidth = DW / (VideoModeDividers[VideoModeRow()].x + 1);
		finalDisplayHeight = DH / (VideoModeDividers[VideoModeRow()].y + 1);
	}
	else
	{
		finalDisplayWidth = pcsx2_min_f(finalDisplayWidth, DW / (VideoModeDividers[VideoModeRow()].x + 1));
		finalDisplayHeight = pcsx2_min_f(finalDisplayHeight, DH / (VideoModeDividers[VideoModeRow()].y + 1));
	}

	// Framebuffer size and offsets.
	PCRTCDisplays[display].prevFramebufferOffsets = PCRTCDisplays[display].framebufferOffsets;
	PCRTCDisplays[display].framebufferRect.x = 0;
	PCRTCDisplays[display].framebufferRect.y = 0;
	PCRTCDisplays[display].framebufferRect.z = renderWidth;

	if(FFMD && interlaced) // Round up the height as if it's an odd value, this will cause havok with the merge circuit.
		PCRTCDisplays[display].framebufferRect.w = (renderHeight + 1) >> (FFMD * interlaced); // Half height read if FFMD + INT enabled.
	else
		PCRTCDisplays[display].framebufferRect.w = renderHeight;
	PCRTCDisplays[display].framebufferOffsets.x = framebufferReg.DBX;
	PCRTCDisplays[display].framebufferOffsets.y = framebufferReg.DBY;

	const bool is_interlaced_resolution = interlaced || (toggling_field && GSConfig.InterlaceMode != GSInterlaceMode::Off);

	// If the interlace flag isn't set, but it's still interlacing, the height is likely reported wrong.
	// Q-Ball Billiards.
	if (is_interlaced_resolution && !interlaced)
		finalDisplayHeight *= 2;

	// Display size and offsets.
	PCRTCDisplays[display].displayRect.x = 0;
	PCRTCDisplays[display].displayRect.y = 0;
	PCRTCDisplays[display].displayRect.z = finalDisplayWidth;
	PCRTCDisplays[display].displayRect.w = finalDisplayHeight;
	PCRTCDisplays[display].prevDisplayOffset = PCRTCDisplays[display].displayOffset;
	PCRTCDisplays[display].displayOffset.x = displayReg.DX;
	PCRTCDisplays[display].displayOffset.y = displayReg.DY;
}

// Calculate framebuffer read offsets, should be considered if only one circuit is enabled, or difference is more than 1 line.
// Only considered if "Anti-blur" is enabled.
void GSState::GSPCRTCRegs::CalculateFramebufferOffset(bool scanmask)
{
	if (GSConfig.PCRTCAntiBlur && PCRTCSameSrc && !scanmask)
	{
		GSVector2i fb0 = GSVector2i(PCRTCDisplays[0].framebufferOffsets.x, PCRTCDisplays[0].framebufferOffsets.y);
		GSVector2i fb1 = GSVector2i(PCRTCDisplays[1].framebufferOffsets.x, PCRTCDisplays[1].framebufferOffsets.y);

		if (fb0.x + PCRTCDisplays[0].displayRect.z > 2048)
		{
			fb0.x -= 2048;
			fb0.x = abs(fb0.x);
		}
		if (fb0.y + PCRTCDisplays[0].displayRect.w > 2048)
		{
			fb0.y -= 2048;
			fb0.y = abs(fb0.y);
		}
		if (fb1.x + PCRTCDisplays[1].displayRect.z > 2048)
		{
			fb1.x -= 2048;
			fb1.x = abs(fb1.x);
		}
		if (fb1.y + PCRTCDisplays[1].displayRect.w > 2048)
		{
			fb1.y -= 2048;
			fb1.y = abs(fb1.y);
		}

		if (abs(fb1.y - fb0.y) == 1
			&& PCRTCDisplays[0].displayRect.y == PCRTCDisplays[1].displayRect.y)
		{
			if (fb1.y < fb0.y)
				PCRTCDisplays[0].framebufferOffsets.y = fb1.y;
			else
				PCRTCDisplays[1].framebufferOffsets.y = fb0.y;
		}
		if (abs(fb1.x - fb0.x) == 1
			&& PCRTCDisplays[0].displayRect.x == PCRTCDisplays[1].displayRect.x)
		{
			if (fb1.x < fb0.x)
				PCRTCDisplays[0].framebufferOffsets.x = fb1.x;
			else
				PCRTCDisplays[1].framebufferOffsets.x = fb0.x;
		}
	}
	PCRTCDisplays[0].framebufferRect.x += PCRTCDisplays[0].framebufferOffsets.x;
	PCRTCDisplays[0].framebufferRect.z += PCRTCDisplays[0].framebufferOffsets.x;
	PCRTCDisplays[0].framebufferRect.y += PCRTCDisplays[0].framebufferOffsets.y;
	PCRTCDisplays[0].framebufferRect.w += PCRTCDisplays[0].framebufferOffsets.y;

	PCRTCDisplays[1].framebufferRect.x += PCRTCDisplays[1].framebufferOffsets.x;
	PCRTCDisplays[1].framebufferRect.z += PCRTCDisplays[1].framebufferOffsets.x;
	PCRTCDisplays[1].framebufferRect.y += PCRTCDisplays[1].framebufferOffsets.y;
	PCRTCDisplays[1].framebufferRect.w += PCRTCDisplays[1].framebufferOffsets.y;
}

// Used in software mode to align the buffer when reading. Offset is accounted for (block aligned) by GetOutput.
void GSState::GSPCRTCRegs::RemoveFramebufferOffset(int display)
{
	if (display >= 0)
	{
		// Hardware needs nothing but handling for wrapped framebuffers.
		if (GSConfig.UseHardwareRenderer())
		{
			if (PCRTCDisplays[display].framebufferRect.z >= 2048)
			{
				PCRTCDisplays[display].displayRect.x += 2048 - PCRTCDisplays[display].framebufferRect.x;
				PCRTCDisplays[display].displayRect.z += 2048 - PCRTCDisplays[display].framebufferRect.x;
				PCRTCDisplays[display].framebufferRect.x = 0;
				PCRTCDisplays[display].framebufferRect.z -= 2048;
			}
			if (PCRTCDisplays[display].framebufferRect.w >= 2048)
			{
				PCRTCDisplays[display].displayRect.y += 2048 - PCRTCDisplays[display].framebufferRect.y;
				PCRTCDisplays[display].displayRect.w += 2048 - PCRTCDisplays[display].framebufferRect.y;
				PCRTCDisplays[display].framebufferRect.y = 0;
				PCRTCDisplays[display].framebufferRect.w -= 2048;
			}
		}
		else
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[PCRTCDisplays[display].PSM];

			// Software mode - See note below.
			GSVector4i r = PCRTCDisplays[display].framebufferRect;
			r = r.ralign<Align_Outside>(psm.bs);

			PCRTCDisplays[display].framebufferRect.z -= r.x;
			PCRTCDisplays[display].framebufferRect.w -= r.y;
			PCRTCDisplays[display].framebufferRect.x -= r.x;
			PCRTCDisplays[display].framebufferRect.y -= r.y;
		}
	}
	else
	{
		// Software Mode Note:
		// This code is to read the framebuffer nicely block aligned in software, then leave the remaining offset in to the block.
		// In hardware mode this doesn't happen, it reads the whole framebuffer, so we need to keep the offset.
		if (!GSConfig.UseHardwareRenderer())
		{
			const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[PCRTCDisplays[1].PSM];

			GSVector4i r = PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect);
			r = r.ralign<Align_Outside>(psm.bs);

			PCRTCDisplays[0].framebufferRect.x -= r.x;
			PCRTCDisplays[0].framebufferRect.y -= r.y;
			PCRTCDisplays[0].framebufferRect.z -= r.x;
			PCRTCDisplays[0].framebufferRect.w -= r.y;
			PCRTCDisplays[1].framebufferRect.x -= r.x;
			PCRTCDisplays[1].framebufferRect.y -= r.y;
			PCRTCDisplays[1].framebufferRect.z -= r.x;
			PCRTCDisplays[1].framebufferRect.w -= r.y;
		}
	}
}

// If the two displays are offset from each other, move them to the correct offsets.
// If using screen offsets, calculate the positions here.
void GSState::GSPCRTCRegs::CalculateDisplayOffset(bool scanmask)
{
	const bool both_enabled = PCRTCDisplays[0].enabled && PCRTCDisplays[1].enabled;
	// Offsets are generally ignored, the "hacky" way of doing the displays, but direct to framebuffers.
	if (!GSConfig.PCRTCOffsets)
	{
		const GSVector4i offsets = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()] : VideoModeOffsetsOverscan[VideoModeRow()];
		int int_off[2]           = { 0, 0 };
		GSVector2i zeroDisplay   = NearestToZeroOffset();
		GSVector2i baseOffset    = PCRTCDisplays[zeroDisplay.y].displayOffset;

		if (both_enabled)
		{
			int blurOffset = abs(PCRTCDisplays[1].displayOffset.y - PCRTCDisplays[0].displayOffset.y);
			if (GSConfig.PCRTCAntiBlur && !scanmask && blurOffset < 4)
			{
				if (PCRTCDisplays[1].displayOffset.y > PCRTCDisplays[0].displayOffset.y)
					PCRTCDisplays[1].displayOffset.y -= blurOffset;
				else
					PCRTCDisplays[0].displayOffset.y -= blurOffset;
			}
		}

		// If there's a single pixel offset, account for it else it can throw interlacing out.
		for (int i = 0; i < 2; i++)
		{
			if (!PCRTCDisplays[i].enabled)
				continue;

			// Should this be MAGV/H in the DISPLAY register rather than the "default" magnification?
			const int offset = (PCRTCDisplays[i].displayOffset.y - (offsets.w * (interlaced + 1))) / (VideoModeDividers[VideoModeRow()].y + 1);

			if (offset > 4)
				continue;

			int_off[i] = offset & 1;
			if (offset < 0)
				int_off[i] = -int_off[i];

			PCRTCDisplays[i].displayRect.y += int_off[i];
			PCRTCDisplays[i].displayRect.w += int_off[i];
		}

		// Handle difference in offset between the two displays, used in games like DmC and Time Crisis 2 (for split screen).
		// Offset is not screen based, but relative to each other.
		if (both_enabled)
		{
			GSVector2i offset = {
				(PCRTCDisplays[1 - zeroDisplay.x].displayOffset.x - PCRTCDisplays[zeroDisplay.x].displayOffset.x) / (VideoModeDividers[VideoModeRow()].x + 1),
				(PCRTCDisplays[1 - zeroDisplay.y].displayOffset.y - PCRTCDisplays[zeroDisplay.y].displayOffset.y) / (VideoModeDividers[VideoModeRow()].y + 1)
			};

			if (offset.x >= 4 || !GSConfig.PCRTCAntiBlur || scanmask)
			{
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.x += offset.x;
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.z += offset.x;
			}
			if (offset.y >= 4 || !GSConfig.PCRTCAntiBlur || scanmask)
			{
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.y += offset.y - int_off[1 - zeroDisplay.y];
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.w += offset.y - int_off[1 - zeroDisplay.y];
			}

			baseOffset = PCRTCDisplays[zeroDisplay.y].displayOffset;
		}

		// Handle any large vertical offset from the zero position on the screen.
		// Example: Hokuto no Ken, does a rougly -14 offset to bring the screen up.
		// Ignore the lowest bit, we've already accounted for this
		int vOffset = ((static_cast<int>(baseOffset.y) - (offsets.w * (interlaced + 1))) / (VideoModeDividers[VideoModeRow()].y + 1));

		if(vOffset <= 4 && vOffset != 0)
		{
			PCRTCDisplays[0].displayRect.y += vOffset - int_off[0];
			PCRTCDisplays[0].displayRect.w += vOffset - int_off[0];
			PCRTCDisplays[1].displayRect.y += vOffset - int_off[1];
			PCRTCDisplays[1].displayRect.w += vOffset - int_off[1];
		}
	}
	else // We're using screen offsets, so just calculate the entire offset.
	{
		const GSVector4i offsets = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()] : VideoModeOffsetsOverscan[VideoModeRow()];
		GSVector2i zeroDisplay = NearestToZeroOffset();

		if (both_enabled)
		{
			int blurOffset = abs(PCRTCDisplays[1].displayOffset.y - PCRTCDisplays[0].displayOffset.y);
			if (GSConfig.PCRTCAntiBlur && !scanmask && blurOffset < 4)
			{
				if (PCRTCDisplays[1].displayOffset.y > PCRTCDisplays[0].displayOffset.y)
					PCRTCDisplays[1].displayOffset.y -= blurOffset;
				else
					PCRTCDisplays[0].displayOffset.y -= blurOffset;
			}
		}

		for (int i = 0; i < 2; i++)
		{
			// Should this be MAGV/H in the DISPLAY register rather than the "default" magnification?
			const GSVector2i offset = {
				(static_cast<int>(PCRTCDisplays[i].displayOffset.x) - offsets.z) / (VideoModeDividers[VideoModeRow()].x + 1),
				(static_cast<int>(PCRTCDisplays[i].displayOffset.y) - (offsets.w * (interlaced + 1))) / (VideoModeDividers[VideoModeRow()].y + 1)
			};

			PCRTCDisplays[i].displayRect.x += offset.x;
			PCRTCDisplays[i].displayRect.z += offset.x;
			PCRTCDisplays[i].displayRect.y += offset.y;
			PCRTCDisplays[i].displayRect.w += offset.y;
		}

		if (both_enabled)
		{
			const GSVector2i offset = {
				(PCRTCDisplays[1 - zeroDisplay.x].displayRect.x - PCRTCDisplays[zeroDisplay.x].displayRect.x),
				(PCRTCDisplays[1 - zeroDisplay.y].displayRect.y - PCRTCDisplays[zeroDisplay.y].displayRect.y)
			};

			if (offset.x > 0 && offset.x < 4 && GSConfig.PCRTCAntiBlur)
			{
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.x -= offset.x;
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.z -= offset.x;
			}
			if (offset.y > 0 && offset.y < 4 && GSConfig.PCRTCAntiBlur)
			{
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.y -= offset.y;
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.w -= offset.y;
			}
		}
	}
}
