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

#include "GSTextureBase.h"

void gs_texture_base_init(gs_texture* t, unsigned type, unsigned format, int width, int height)
{
	t->m_ops                     = NULL;
	t->m_size.x                  = width;
	t->m_size.y                  = height;
	t->m_mipmap_levels           = 0;
	t->m_type                    = (u8)type;
	t->m_format                  = (u8)format;
	t->m_state                   = GS_TEXSTATE_DIRTY;
	t->m_needs_mipmaps_generated = 1;
	t->m_last_frame_used         = 0;
	t->m_clear_value.color       = 0;
}

void gs_download_texture_base_init(gs_download_texture* t, u32 width, u32 height, unsigned format)
{
	t->m_ops           = NULL;
	t->m_width         = width;
	t->m_height        = height;
	t->m_map_pointer   = NULL;
	t->m_current_pitch = 0;
	t->m_format        = (u8)format;
	t->m_needs_flush   = 0;
}
