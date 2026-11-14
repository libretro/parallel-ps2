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

#ifndef GS_TEXTURE_OGL_H
#define GS_TEXTURE_OGL_H

#include "common/Pcsx2Defs.h"
#include "glad.h"
#include "GS/Renderers/Common/GSTextureBase.h"

#ifdef __cplusplus
#include "GS/Renderers/Common/GSTexture.h"
#endif

/* A GL texture: the common part, the texture object, the area a map
 * holds until its unmap, and how its data is laid out for GL */
typedef struct gs_texture_gl
{
	gs_texture base;
	GLuint id;
	int r_x, r_y, r_w, r_h; /* the mapped area */
	int layer;
	u32 map_offset;         /* where in the upload buffer it is */
	GLenum int_format;
	GLenum int_type;
	u32 int_shift;          /* log2 of a texel's bytes in that data */
} gs_texture_gl;

/* A read back into a pixel pack buffer, persistently mapped where the
 * driver has buffer storage, else into CPU memory with glReadPixels */
typedef struct gs_download_texture_gl
{
	gs_download_texture base;
	GLuint buffer_id;
	u32 buffer_size;
	GLsync sync;
	u8* cpu_buffer;
} gs_download_texture_gl;

#ifdef __cplusplus
extern "C" {
#endif

/* A texture of type and format (gs_texture_type, gs_texture_format), at
 * least 1x1, with levels mipmap levels if it is a Texture; NULL if it
 * could not be allocated */
gs_texture* gs_texture_gl_create(unsigned type, int width, int height, int levels, unsigned format);

/* NULL if the buffer could not be made */
gs_download_texture* gs_download_texture_gl_create(u32 width, u32 height, unsigned format);

#ifdef __cplusplus
}

/* The device's view of a texture it made: the same struct, as a GSTexture */
class GSTextureOGL final : public GSTexture
{
public:
	GLuint m_texture_id;
	int m_r_x, m_r_y, m_r_w, m_r_h;
	int m_layer;
	u32 m_map_offset;
	GLenum m_int_format;
	GLenum m_int_type;
	u32 m_int_shift;

	GSTextureOGL() = delete;

	__fi GLenum GetIntFormat() const { return m_int_format; }
	__fi GLenum GetIntType() const { return m_int_type; }
	__fi u32 GetIntShift() const { return m_int_shift; }
	__fi u32 GetID() { return m_texture_id; }

	bool IsIntegerFormat() const { return (m_int_format == GL_RED_INTEGER || m_int_format == GL_RGBA_INTEGER); }
	bool IsUnsignedFormat() const
	{
		return (m_int_type == GL_UNSIGNED_BYTE || m_int_type == GL_UNSIGNED_SHORT || m_int_type == GL_UNSIGNED_INT);
	}
};

static_assert(sizeof(GSTextureOGL) == sizeof(gs_texture_gl), "GSTextureOGL is gs_texture_gl");
#endif

#endif
