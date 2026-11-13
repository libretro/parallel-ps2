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

#ifndef GS_TEXTURE_FORMAT_H
#define GS_TEXTURE_FORMAT_H

#include <stddef.h>
#include <string.h>

#include <retro_inline.h>

#include "common/Pcsx2Defs.h"

/* GSTexture::Format's values, for C */
enum gs_texture_format
{
	GS_TEX_INVALID = 0,
	GS_TEX_COLOR,         /* RGBA8 */
	GS_TEX_COLOR_CLIP,    /* RGBA16 (colclip emulation) */
	GS_TEX_DEPTH_STENCIL,
	GS_TEX_UNORM8,        /* R8: palettes, the OSD font */
	GS_TEX_UINT16,        /* R16UI: 16-bit depth read back */
	GS_TEX_UINT32,        /* R32UI: 24 and 32-bit depth read back */
	GS_TEX_PRIMID,        /* DATE's primitive IDs */
	GS_TEX_BC1,           /* the compressed ones, for replacements */
	GS_TEX_BC2,
	GS_TEX_BC3,
	GS_TEX_BC7,
	GS_TEX_FORMATS
};

/* Where a rectangle of a download lands: from offset, rows of size bytes */
typedef struct gs_texture_transfer
{
	u32 offset;
	u32 size;
	u32 rows;
} gs_texture_transfer;

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes a block takes: a texel's, or a 4x4 block's when compressed */
u32 gs_texture_block_bytes(unsigned format);
/* Texels a block is wide and high: 4 when compressed, else 1 */
u32 gs_texture_block_size(unsigned format);

/* The pitch of width texels; the GL row length (in texels) of pitch
 * bytes; the bytes of height rows of pitch */
u32 gs_texture_upload_pitch(unsigned format, u32 width);
u32 gs_texture_row_length(unsigned format, u32 pitch);
u32 gs_texture_upload_size(unsigned format, u32 height, u32 pitch);

/* A download buffer's pitch for width texels, rows aligned to pitch_align
 * (a power of two), and its size for height rows */
u32 gs_texture_transfer_pitch(unsigned format, u32 width, u32 pitch_align);
u32 gs_texture_buffer_size(unsigned format, u32 width, u32 height, u32 pitch_align);
/* The rectangle x, y, w, h of a download whose rows are pitch apart */
gs_texture_transfer gs_texture_transfer_of(unsigned format, u32 pitch, u32 x, u32 y, u32 w, u32 h);

#ifdef __cplusplus
}
#endif

static INLINE int gs_texture_is_compressed(unsigned format)
{
	return format >= GS_TEX_BC1 && format <= GS_TEX_BC7;
}

/* count rows of copy_size bytes, src_stride and dst_stride apart: one copy
 * when the rows are packed alike */
static INLINE void gs_stride_memcpy(void* dst, size_t dst_stride, const void* src, size_t src_stride,
	size_t copy_size, size_t count)
{
	const unsigned char* s = (const unsigned char*)src;
	unsigned char* d       = (unsigned char*)dst;
	size_t i;
	if (src_stride == dst_stride && src_stride == copy_size)
	{
		memcpy(dst, src, src_stride * count);
		return;
	}
	for (i = 0; i < count; i++)
	{
		memcpy(d, s, copy_size);
		s += src_stride;
		d += dst_stride;
	}
}

#endif
