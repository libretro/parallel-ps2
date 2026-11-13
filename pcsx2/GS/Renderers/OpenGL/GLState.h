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

#ifndef GS_GL_STATE_H
#define GS_GL_STATE_H

#include "common/Pcsx2Defs.h"
#include "glad.h"

#ifdef __cplusplus
#include "GS/GSVector.h"
class GSTextureOGL;
#endif

/* What the GL device last bound or set, so it binds and sets only what
 * changes. The viewport and scissor are the same ints either way; C++
 * takes them as GSVector2i and GSVector4i. */
typedef struct gs_gl_state
{
	GLuint vao; /* vertex array object */
	GLuint fbo; /* frame buffer object */
#ifdef __cplusplus
	GSVector2i viewport;
	GSVector4i scissor;
#else
	int viewport[2];
	PCSX2_ALIGN(16) int scissor[4];
#endif

	u8 point_size;
	float line_width;

	u8 blend;
	u16 eq_RGB;
	u16 f_sRGB;
	u16 f_dRGB;
	u16 f_sA;
	u16 f_dA;
	u8 bf;
	u8 wrgba;

	u8 depth;
	GLenum depth_func;
	u8 depth_mask;

	u8 stencil;
	GLenum stencil_func;
	GLenum stencil_pass;

	GLuint ps_ss; /* sampler */

#ifdef __cplusplus
	GSTextureOGL* rt; /* render target */
	GSTextureOGL* ds; /* depth stencil */
#else
	void* rt;
	void* ds;
#endif
	GLuint tex_unit[8];     /* shader input textures */
	GLuint64 tex_handle[8];
} gs_gl_state;

#ifdef __cplusplus
extern "C" {
#endif

extern gs_gl_state g_gl_state;

/* Back to what a new context has */
void gs_gl_state_clear(void);

#ifdef __cplusplus
}
#endif

#endif
