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

/* The GSdx renderers, hardware and software, behind GS.c (GSBackend.h).
 * Compiled once. */

#include <stddef.h>

#include "GSRenderer.h"
#include "../../GSBackend.h"
#include "../../../FreezeTypes.h"
#include "../Common/GSDeviceBase.h"
#include "../SW/GSTextureCacheSW.h"
#include "../SW/GSRendererSW.h"
#include "common/CpuFeatures.h"

#include "../../../Config.h"

/* (GSLocalMemoryMultiISA.c's) */
extern gs_lm_psm gs_lm_psm_table[64];

struct gs_state* g_gs_renderer_c = NULL;

#define R g_gs_renderer_c
#define SELF ((gs_state_self*)(void*)R)

/* (whether the open one is GSRendererHW) */
static bool s_gsdx_hw;

/* --- what a renderer does, or the state's own where it does nothing ---- */

static void r_reset(bool hardware_reset)
{
	if (R->m_ops && R->m_ops->reset)
		R->m_ops->reset(SELF, hardware_reset);
	else
		gs_state_reset_base(R, hardware_reset);
}

static void r_update_render_fixes(void)
{
	if (R->m_ops && R->m_ops->update_render_fixes)
		R->m_ops->update_render_fixes(SELF);
}

static void r_purge_texture_cache(void)
{
	if (R->m_ops && R->m_ops->purge_texture_cache)
		R->m_ops->purge_texture_cache(SELF, true, true, true);
}

static void r_readback_texture_cache(void)
{
	if (R->m_ops && R->m_ops->readback_texture_cache)
		R->m_ops->readback_texture_cache(SELF);
}

/* --- the ops GS.c calls --------------------------------------------------- */

static void gsdx_op_reset(bool hardware_reset) { r_reset(hardware_reset); }

static void gsdx_op_init_and_read_fifo(u8* mem, u32 size)
{
	gs_state_init_read_fifo(R, mem, (int)size);
	gs_state_read_fifo(R, mem, (int)size);
}

static void gsdx_op_read_local_memory_unsync(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG)
{
	GIFRegBITBLTBUF b;
	GIFRegTRXPOS p;
	GIFRegTRXREG r;
	b.U64 = BITBLITBUF;
	p.U64 = TRXPOS;
	r.U64 = TRXREG;
	gs_state_read_local_memory_unsync(R, mem, (int)qwc, b, p, r);
}

static void gsdx_op_transfer(const u8* mem, u32 size) { gs_state_transfer(R, mem, size); }

static void gsdx_op_vsync(u32 field, bool registers_written)
{
	const bool idle = gs_present_idle_frame(&R->m_present);

	/* (the flush is not VSync's own: EE transfers are cleared in the HW
	 * VSync, and a buffered draw may still need them - FFX's FMVs) */
	gs_state_flush(R, GS_FLUSH_VSYNC);
	if (R->m_ops && R->m_ops->vsync)
		R->m_ops->vsync(SELF, field, registers_written, idle);
	else
		gs_present_vsync(&R->m_present, field, registers_written, idle);
}

static int gsdx_op_freeze(int mode, freezeData* data)
{
	if (mode == (int)FREEZE_SAVE)
		return gs_state_freeze(R, data, false);
	if (mode == (int)FREEZE_SIZE)
		return gs_state_freeze(R, data, true);
	if (mode == (int)FREEZE_LOAD)
	{
		/* Defrost does no hardware reset (it would clear local memory just
		 * before it is overwritten), so the current textures go here. */
		gs_device_clear_current(g_gs_device_c);
		return gs_state_defrost(R, data);
	}
	return 0;
}

static u8* gsdx_op_regs_mem(void) { return (u8*)R->m_regs; }

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

static struct gs_state* sw_create(int threads)
{
#ifdef MULTI_ISA_SHARED_COMPILATION
	if (CPU_HAS_AVX2)
		return gs_renderer_sw_create_isa_avx2(threads);
	if (CPU_HAS_AVX)
		return gs_renderer_sw_create_isa_avx(threads);
	return gs_renderer_sw_create_isa_sse4(threads);
#else
	return gs_renderer_sw_create_isa_native(threads);
#endif
}

const struct gs_renderer_ops* gs_gsdx_open(GSRendererType renderer, u8* basemem)
{
	s_gsdx_hw = renderer != GS_RENDERER_SW;
	R = s_gsdx_hw ? gs_renderer_hw_create() : sw_create(GSConfig.SWExtraThreads);

	R->m_regs = (GSPrivRegSet*)basemem;
	R->m_present.regs = R->m_regs;
	gs_pcrtc_reset(&R->PCRTCDisplays, R->m_regs);
	r_update_render_fixes();
	return &gsdx_renderer_ops;
}

void gs_gsdx_close(void)
{
	if (!R)
		return;
	if (R->m_ops && R->m_ops->destroy)
		R->m_ops->destroy(SELF);
	if (R->m_ops && R->m_ops->free)
		R->m_ops->free(SELF);
	R = NULL;
}

bool gs_gsdx_is_open(void) { return R != NULL; }

void gs_gsdx_before_reopen(bool recreate_device, bool recreate_renderer)
{
	if (!R)
		return;
	gs_state_flush(R, GS_FLUSH_GSREOPEN);
	if (recreate_device && !recreate_renderer)
	{
		/* Keeping the renderer: nothing of the old device left in it. The
		 * texture cache's download textures and snapshots outlived a purge,
		 * and the GPU palette's targets every reopen, so the new device
		 * was handed the old one's buffers and images. */
		if (s_gsdx_hw)
			gs_renderer_hw_release_device();
		else
			r_purge_texture_cache();
		gs_clut_gpu_release(&R->m_mem.m_clut.m_c);
		gs_present_purge_pool();
	}
	else if (GSConfig.UserHacks_ReadTCOnClose)
		r_readback_texture_cache();
}

void gs_gsdx_after_device_reopen(void)
{
	if (R)
		gs_clut_gpu_create(&R->m_mem.m_clut.m_c);
}

void gs_gsdx_update_render_fixes(void) { r_update_render_fixes(); }

void gs_gsdx_update_settings(const EmuGSOptions* old_config)
{
	if (R->m_ops && R->m_ops->update_settings)
		R->m_ops->update_settings(SELF, old_config);
	else
		gs_state_update_settings_base(R, old_config);
}

void gs_gsdx_readback_texture_cache(void) { r_readback_texture_cache(); }

void gs_gsdx_purge_texture_cache(void)
{
	r_purge_texture_cache();
	gs_present_purge_pool();
}

gs_texture* gs_gsdx_lookup_palette_source(u32 cbp, u32 cpsm, u32 cbw, int* offset_x, int* offset_y,
	float* scale, float* sample, int w, int h)
{
	gs_point offset, size;
	struct GSTexture* src;

	if (!R || !R->m_ops || !R->m_ops->lookup_palette_source)
		return NULL;
	offset.x = *offset_x;
	offset.y = *offset_y;
	size.x = w;
	size.y = h;
	src = R->m_ops->lookup_palette_source(SELF, cbp, cpsm, cbw, (struct GSVector2i*)(void*)&offset, scale, sample,
		(const struct GSVector2i*)(const void*)&size);
	*offset_x = offset.x;
	*offset_y = offset.y;
	return (gs_texture*)src;
}

/* What the software textures (SW/GSTextureCacheSW.c) read local memory
 * through */
void gs_sw_texture_read_block(u32 psm, u32 bp, u8* dst, int dstpitch, const GIFRegTEXA* TEXA)
{
	gs_lm_psm_table[psm].rtxbP(&R->m_mem.lm, bp, dst, dstpitch, TEXA);
}

GSPage2TileMap* gs_sw_texture_page2tile(const GIFRegTEX0* TEX0)
{
	return gs_page2tile_map_get(&R->m_mem.lm.m_p2tmap, TEX0);
}
