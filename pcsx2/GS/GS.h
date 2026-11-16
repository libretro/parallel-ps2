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

/* GSConfig, m_disp_fb_sprite_blits, GSreopen and the renderer table:
 * GSBackend.h (GSEntry.c) */
#include "GSBackend.h"

/* GSSnapshotSave, GSSnapshotLoad, GSSnapshotKey: ../GS.h. */
#include "../GS.h"

/* The ID for the named function, otherwise -1. C linkage: the game
 * database (GameDatabase.c) looks them up. */
extern "C" {
s16 GSLookupGetSkipCountFunctionId(const char* name, size_t length);
s16 GSLookupBeforeDrawFunctionId(const char* name, size_t length);
s16 GSLookupMoveHandlerFunctionId(const char* name, size_t length);
}

