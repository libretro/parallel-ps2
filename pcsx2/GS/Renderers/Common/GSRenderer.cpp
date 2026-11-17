/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#include "GSRenderer.h"
#include "../HW/GSRendererHW.h"
#include "../../GSBackend.h"
#include "../../MultiISA.h"
#include "../SW/GSTextureCacheSW.h"

#include "../../../Config.h"

GSRenderer* g_gs_renderer = NULL;

/* --- the GSdx renderers behind GS.c (GSBackend.h) ----------------------- */

static void gsdx_op_reset(bool hardware_reset)  { g_gs_renderer->Reset(hardware_reset); }

static void gsdx_op_init_and_read_fifo(u8* mem, u32 size)
{
	g_gs_renderer->InitReadFIFO(mem, size);
	g_gs_renderer->ReadFIFO(mem, size);
}

static void gsdx_op_read_local_memory_unsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG)
{
	g_gs_renderer->ReadLocalMemoryUnsync(mem, qwc, GIFRegBITBLTBUF{BITBLITBUF}, GIFRegTRXPOS{TRXPOS}, GIFRegTRXREG{TRXREG});
}

static void gsdx_op_transfer(const u8* mem, u32 size) { g_gs_renderer->Transfer(mem, size); }

static void gsdx_op_vsync(u32 field, bool registers_written)
{
	/* (the flush is not VSync's own: EE transfers are cleared in the HW
	 * VSync, and a buffered draw may still need them - FFX's FMVs) */
	g_gs_renderer->Flush(GSState::VSYNC);
	g_gs_renderer->VSync(field, registers_written, g_gs_renderer->IsIdleFrame());
}

static int gsdx_op_freeze(int mode, freezeData* data)
{
	if (mode == static_cast<int>(FREEZE_SAVE))
		return g_gs_renderer->Freeze(data, false);
	if (mode == static_cast<int>(FREEZE_SIZE))
		return g_gs_renderer->Freeze(data, true);
	if (mode == static_cast<int>(FREEZE_LOAD))
	{
		/* Defrost does no hardware reset (it would clear local memory just
		 * before it is overwritten), so the current textures go here. */
		g_gs_device->ClearCurrent();
		return g_gs_renderer->Defrost(data);
	}
	return 0;
}

static u8* gsdx_op_regs_mem(void) { return g_gs_renderer->GetRegsMem(); }

static const struct gs_renderer_ops gsdx_renderer_ops = {
	gsdx_op_reset,
	gsdx_op_init_and_read_fifo,
	gsdx_op_read_local_memory_unsync,
	gsdx_op_transfer,
	gsdx_op_vsync,
	gsdx_op_freeze,
	NULL, /* update_config: GSUpdateConfig's own, through the gs_gsdx_* below */
	gsdx_op_regs_mem
};

/* (whether the open one is GSRendererHW) */
static bool s_gsdx_hw;

const struct gs_renderer_ops* gs_gsdx_open(GSRendererType renderer, u8* basemem)
{
	s_gsdx_hw = renderer != GS_RENDERER_SW;
	if (s_gsdx_hw)
		g_gs_renderer = new GSRendererHW();
	else
		g_gs_renderer = MULTI_ISA_SELECT(makeGSRendererSW)(GSConfig.SWExtraThreads);

	g_gs_renderer->SetRegsMem(basemem);
	g_gs_renderer->ResetPCRTC();
	g_gs_renderer->UpdateRenderFixes();
	return &gsdx_renderer_ops;
}

void gs_gsdx_close(void)
{
	if (!g_gs_renderer)
		return;
	g_gs_renderer->Destroy();
	g_gs_renderer->Free();
	g_gs_renderer = NULL;
}

bool gs_gsdx_is_open(void) { return g_gs_renderer != NULL; }

void gs_gsdx_before_reopen(bool recreate_device, bool recreate_renderer)
{
	if (!g_gs_renderer)
		return;
	g_gs_renderer->Flush(GSState::GSFlushReason::GSREOPEN);
	if (recreate_device && !recreate_renderer)
	{
		/* Keeping the renderer: nothing of the old device left in it. The
		 * texture cache's download textures and snapshots outlived a purge,
		 * and the GPU palette's targets every reopen, so the new device
		 * was handed the old one's buffers and images. */
		if (s_gsdx_hw)
			g_texture_cache->ReleaseDevice();
		else
			g_gs_renderer->PurgeTextureCache(true, true, true);
		g_gs_renderer->m_mem.m_clut.ReleaseGPUTextures();
		g_gs_renderer->PurgePool();
	}
	else if (GSConfig.UserHacks_ReadTCOnClose)
		g_gs_renderer->ReadbackTextureCache();
}

void gs_gsdx_after_device_reopen(void)
{
	if (g_gs_renderer)
		g_gs_renderer->m_mem.m_clut.CreateGPUTextures();
}

void gs_gsdx_update_render_fixes(void) { g_gs_renderer->UpdateRenderFixes(); }
void gs_gsdx_update_settings(const EmuGSOptions* old_config) { g_gs_renderer->UpdateSettings(*old_config); }
void gs_gsdx_readback_texture_cache(void) { g_gs_renderer->ReadbackTextureCache(); }

void gs_gsdx_purge_texture_cache(void)
{
	g_gs_renderer->PurgeTextureCache(true, true, true);
	g_gs_renderer->PurgePool();
}

gs_texture* gs_gsdx_lookup_palette_source(u32 cbp, u32 cpsm, u32 cbw, int* offset_x, int* offset_y,
	float* scale, float* sample, int w, int h)
{
	GSVector2i offset(*offset_x, *offset_y);
	const GSVector2i size(w, h);
	GSTexture* src;
	if (!g_gs_renderer)
		return NULL;
	src = g_gs_renderer->LookupPaletteSource(cbp, cpsm, cbw, offset, scale, sample, size);
	*offset_x = offset.x;
	*offset_y = offset.y;
	return src;
}

/* What the software textures (SW/GSTextureCacheSW.c) read local memory
 * through */
extern "C" void gs_sw_texture_read_block(u32 psm, u32 bp, u8* dst, int dstpitch, const GIFRegTEXA* TEXA)
{
	GSLocalMemory::m_psm[psm].rtxbP(g_gs_renderer->m_mem, bp, dst, dstpitch, *TEXA);
}

extern "C" GSPage2TileMap* gs_sw_texture_page2tile(const GIFRegTEX0* TEX0)
{
	return g_gs_renderer->m_mem.GetPage2TileMap(*TEX0);
}
