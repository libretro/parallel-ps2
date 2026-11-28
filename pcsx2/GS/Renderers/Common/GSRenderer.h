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

#pragma once


#include "../../GSState.h"

#ifdef __cplusplus
/* GSRenderer was a class between GSState and the two renderers. What it
 * held is in GSState now; the name stays so that nothing that says
 * GSRenderer has to change. */
typedef GSState GSRenderer;
extern "C" {
#endif

/* The open GSdx renderer, or NULL (GSRenderer.c). A renderer is disposed
 * of through its table's free. */
extern struct gs_state* g_gs_renderer_c;

/* GSRendererHW, made and disposed of through its table, and the texture
 * cache's hold on the device let go of (GSRendererHW.cpp) */
struct gs_state* gs_renderer_hw_create(void);
void gs_renderer_hw_release_device(void);

#ifdef __cplusplus
}
#define g_gs_renderer (static_cast<GSRenderer*>(g_gs_renderer_c))
#endif
