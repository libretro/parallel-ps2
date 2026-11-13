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

#include <string.h>

#include "GLState.h"

gs_gl_state g_gl_state;

void gs_gl_state_clear(void)
{
	gs_gl_state* s = &g_gl_state;
	memset(s, 0, sizeof(*s));
	s->viewport[0] = 1;
	s->viewport[1] = 1;
	s->scissor[2]  = 1;
	s->scissor[3]  = 1;

	s->line_width   = 1.0f;
	s->eq_RGB       = GL_FUNC_ADD;
	s->f_sRGB       = GL_ONE;
	s->f_dRGB       = GL_ZERO;
	s->f_sA         = GL_ONE;
	s->f_dA         = GL_ZERO;
	s->wrgba        = 0xF;
	s->depth_func   = GL_LESS;
	s->stencil_func = GL_ALWAYS;
	s->stencil_pass = GL_KEEP;
}
