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

/* What GS.c opens and closes, and drives through a table: the devices
 * and renderers, which are C++ (each defined beside its class). */
#ifndef PCSX2_GS_BACKEND_H
#define PCSX2_GS_BACKEND_H

#include <boolean.h>
#include <libretro.h>

#include "../../common/Pcsx2Defs.h"
#include "../FreezeTypes.h"
#include "../EmuOptions.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the rest of the core needs of a GS renderer, as a table of plain
 * functions. GS.c calls through whichever table is installed and knows
 * nothing else about the renderer behind it. A NULL entry is something
 * that renderer has no use for and is skipped. `mode` is a FreezeAction. */
struct gs_renderer_ops
{
	void (*reset)(bool hardware_reset);
	void (*init_and_read_fifo)(u8* mem, u32 size);
	void (*read_local_memory_unsync)(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG);
	void (*transfer)(const u8* mem, u32 size);
	void (*vsync)(u32 field, bool registers_written);
	int  (*freeze)(int mode, freezeData* data);
	void (*update_config)(void);
	u8*  (*regs_mem)(void);
};

/* GS.c */
extern EmuGSOptions GSConfig;
extern int m_disp_fb_sprite_blits;
bool GSreopen(bool recreate_device, bool recreate_renderer, const EmuGSOptions* old_config);

/* The device on the frontend's context (Renderers/Common/GSDevice.cpp):
 * none for paraLLEl-GS, which brings its own. False if none came up. */
bool gs_device_open(enum retro_hw_context_type context, GSRendererType renderer);
void gs_device_close(void);
void gs_device_clear_sampler_cache(void);

/* The GSdx renderers, hardware and software (Renderers/Common/GSRenderer.cpp) */
const struct gs_renderer_ops* gs_gsdx_open(GSRendererType renderer, u8* basemem);
void gs_gsdx_close(void);
bool gs_gsdx_is_open(void);
/* Before GSreopen takes the renderer or device away: drawn out, and what
 * the texture cache holds read back or dropped */
void gs_gsdx_before_reopen(bool recreate_device, bool recreate_renderer);
/* After it, when the renderer was kept: what it holds on the device made
 * again on the new one */
void gs_gsdx_after_device_reopen(void);
void gs_gsdx_update_render_fixes(void);
void gs_gsdx_update_settings(const EmuGSOptions* old_config);
void gs_gsdx_readback_texture_cache(void);
void gs_gsdx_purge_texture_cache(void);

/* paraLLEl-GS (Renderers/parallel-gs/GSRendererPGS.cpp, under
 * HAVE_PARALLEL_GS): NULL if it did not come up */
const struct gs_renderer_ops* gs_pgs_open(u8* basemem);
void gs_pgs_close(void);

/* The software vertex's per-ISA tables (Renderers/SW/GSRendererSW.cpp) */
void gs_vertex_sw_init_static(void);

#ifdef __cplusplus
}
#endif

#endif
