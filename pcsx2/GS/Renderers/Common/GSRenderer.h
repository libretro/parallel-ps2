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

#include <string>

#include "../../GSState.h"

/* GSRenderer was a class between GSState and the two renderers. What it
 * held is in GSState now; the name stays so that nothing that says
 * GSRenderer has to change. */
typedef GSState GSRenderer;

/* The open GSdx renderer, or NULL. A plain pointer: a renderer is disposed
 * of with Free(), through its table, since its destructor is not virtual. */
extern GSRenderer* g_gs_renderer;
