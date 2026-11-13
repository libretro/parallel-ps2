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

#ifndef GS_GL_TEXTURE_FORMAT_H
#define GS_GL_TEXTURE_FORMAT_H

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GS/Renderers/Common/GSTextureFormat.h"

/* How a texture format is stored and read in GL: its internal format,
 * the pixel format and type data is uploaded and read back as, and the
 * log2 of the bytes a texel takes in that data */
typedef struct gs_gl_texture_format
{
	GLenum internal_format;
	GLenum format;
	GLenum type;
	u32 shift;
} gs_gl_texture_format;

#ifdef __cplusplus
extern "C" {
#endif

/* The GL storage of format (gs_texture_format). colclip_float: the colclip
 * target is RGBA32F (GLES without 16-bit norm). depth_only: depth without
 * stencil (where the shader fetches the framebuffer). */
gs_gl_texture_format gs_gl_texture_format_of(unsigned format, int colclip_float, int depth_only);

#ifdef __cplusplus
}
#endif

#endif
