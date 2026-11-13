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

#ifndef GS_TEXTURE_SW_H
#define GS_TEXTURE_SW_H

#include "GS/Renderers/Common/GSTextureBase.h"

/* A texture in CPU memory: GSDeviceSW's (the software renderer's device
 * where there is no GPU context), and no other's. Rows are padded to
 * GS_TEXTURE_SW_ALIGN bytes so SIMD can address whole rows; Update is a
 * copy and Map hands the rows over. Only Color (RGBA8) is drawn with:
 * UNorm8 takes a byte a texel, every other format four, as storage the
 * device does not read. */
#define GS_TEXTURE_SW_ALIGN 32

typedef struct gs_texture_sw
{
	gs_texture base;
	u8* data;
	int pitch;
	int mapped;
} gs_texture_sw;

#ifdef __cplusplus
extern "C" {
#endif

/* A texture of type and format (gs_texture_type, gs_texture_format),
 * cleared to zero; NULL only if it could not be allocated at all (its
 * texels may be missing: it then refuses Update and Map). */
gs_texture* gs_texture_sw_create(unsigned type, int width, int height, unsigned format);

#ifdef __cplusplus
}

/* The device's view of a texture it made */
struct GSTextureSW : gs_texture_sw
{
	u8* GetPointer() const { return data; }
	int GetPitch() const { return pitch; }
	int GetWidth() const { return base.m_size.x; }
	int GetHeight() const { return base.m_size.y; }
};

static inline GSTextureSW* GSTextureSWOf(gs_texture* t)
{
	return reinterpret_cast<GSTextureSW*>(t);
}
#endif

#endif
