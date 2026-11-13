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

#include "GSTextureFormat.h"

static const u8 s_block_bytes[GS_TEX_FORMATS] = {
	1,  /* Invalid */
	4,  /* Color: RGBA8 */
	8,  /* ColorClip: RGBA16 */
	4,  /* DepthStencil */
	1,  /* UNorm8: R8 */
	2,  /* UInt16: R16UI */
	4,  /* UInt32: R32UI */
	4,  /* PrimID: 32 bits */
	8,  /* BC1: 16 texels in 64 bits */
	16, /* BC2: 16 texels in 128 bits */
	16, /* BC3 */
	16  /* BC7 */
};

u32 gs_texture_block_bytes(unsigned format)
{
	return format < GS_TEX_FORMATS ? s_block_bytes[format] : 1;
}

u32 gs_texture_block_size(unsigned format)
{
	return gs_texture_is_compressed(format) ? 4 : 1;
}

u32 gs_texture_upload_pitch(unsigned format, u32 width)
{
	if (gs_texture_is_compressed(format))
		width = (width + 3) / 4;
	return width * gs_texture_block_bytes(format);
}

u32 gs_texture_row_length(unsigned format, u32 pitch)
{
	const u32 bytes = gs_texture_block_bytes(format);
	return ((pitch + (bytes - 1)) / bytes) * gs_texture_block_size(format);
}

u32 gs_texture_upload_size(unsigned format, u32 height, u32 pitch)
{
	const u32 block = gs_texture_block_size(format);
	return pitch * ((height + (block - 1)) / block);
}

u32 gs_texture_transfer_pitch(unsigned format, u32 width, u32 pitch_align)
{
	const u32 block = gs_texture_block_size(format);
	const u32 bw    = (width + (block - 1)) / block;
	return (bw * gs_texture_block_bytes(format) + (pitch_align - 1)) & ~(pitch_align - 1);
}

u32 gs_texture_buffer_size(unsigned format, u32 width, u32 height, u32 pitch_align)
{
	const u32 block = gs_texture_block_size(format);
	return gs_texture_transfer_pitch(format, width, pitch_align) * ((height + (block - 1)) / block);
}

gs_texture_transfer gs_texture_transfer_of(unsigned format, u32 pitch, u32 x, u32 y, u32 w, u32 h)
{
	const u32 block = gs_texture_block_size(format);
	const u32 bytes = gs_texture_block_bytes(format);
	gs_texture_transfer t;
	t.offset = ((y + (block - 1)) / block) * pitch + ((x + (block - 1)) / block) * bytes;
	t.size   = ((w + (block - 1)) / block) * bytes;
	t.rows   = (h + (block - 1)) / block;
	return t;
}
