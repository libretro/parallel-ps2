/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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
#include "Pcsx2Defs.h"

/* What the GS needs of the window it renders into. */
typedef struct WindowInfo
{
	u32 surface_width;  /* in pixels */
	u32 surface_height; /* in pixels */
} WindowInfo;

#ifdef __cplusplus
extern "C" {
#endif

/* The front end's answer when the GS creates a render device, which can
 * also be a fullscreen transition. */
WindowInfo host_acquire_render_window(void);

#ifdef __cplusplus
}
#endif
