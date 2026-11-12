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

#pragma once

#include "../../common/WindowInfo.h"
#include "../SaveState.h"
#include "../Config.h"


#include <libretro.h>
#include "GSEntry.h"

enum class RenderAPI
{
	None,
	D3D11,
	D3D12,
	Vulkan,
	OpenGL
};

enum class GSVideoMode : u8
{
	Unknown,
	NTSC,
	PAL,
	VESA,
	SDTV_480P,
	HDTV_720P,
	HDTV_1080I
};

enum class GSDisplayAlignment
{
	Center,
	LeftOrTop,
	RightOrBottom
};

extern EmuGSOptions GSConfig;

/* GSSnapshotSave, GSSnapshotLoad, GSSnapshotKey: ../GS.h. */
#include "../GS.h"

/* The ID for the named function, otherwise -1. C linkage: the game
 * database (GameDatabase.c) looks them up. */
extern "C" {
s16 GSLookupGetSkipCountFunctionId(const char* name, size_t length);
s16 GSLookupBeforeDrawFunctionId(const char* name, size_t length);
s16 GSLookupMoveHandlerFunctionId(const char* name, size_t length);
}

void GSopen(const EmuGSOptions& config, GSRendererType renderer, enum retro_hw_context_type api, u8* basemem);
bool GSreopen(bool recreate_device, bool recreate_renderer, const EmuGSOptions& old_config);
/* What the rest of the core needs of a GS renderer, as a table of plain
 * functions. GS.cpp calls through whichever table is installed and knows
 * nothing else about the renderer behind it: the GSdx renderers fill one
 * in GS.cpp, paraLLEl-GS fills its own in GSRendererPGS.cpp. A NULL entry
 * is something that renderer has no use for and is skipped. `mode` is a
 * FreezeAction; it is an int here so the table stays plain C. */
struct gs_renderer_ops
{
	void (*reset)(bool hardware_reset);
	void (*gif_soft_reset)(u32 mask);
	void (*init_and_read_fifo)(u8* mem, u32 size);
	void (*read_local_memory_unsync)(u8* mem, u32 qwc, u64 BITBLITBUF, u64 TRXPOS, u64 TRXREG);
	void (*transfer)(const u8* mem, u32 size);
	void (*vsync)(u32 field, bool registers_written);
	int  (*freeze)(int mode, freezeData* data);
	void (*update_config)(void);
	u8*  (*regs_mem)(void);
};

void GSgifSoftReset(u32 mask);

void GSUpdateConfig(const EmuGSOptions& new_config, enum retro_hw_context_type api);


extern int m_disp_fb_sprite_blits;
