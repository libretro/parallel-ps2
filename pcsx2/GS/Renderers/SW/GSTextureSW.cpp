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
#include "common/Align.h"
#include <memalign.h>

#include "GS/GSExtra.h"
#include "GS/Renderers/SW/GSTextureSW.h"

/* The table GSTexture calls this one through (gs_texture_ops):
 * every entry is this class's function of that name. */
GS_TEXTURE_OPS_DEFINE(GSTextureSW, sw);

GSTextureSW::GSTextureSW(Type type, int width, int height, Format format)
{
	m_ops = &s_sw_texture_ops;
	m_type   = type;
	m_format = format;
	m_size.x = width;
	m_size.y = height;
	m_mipmap_levels = 1;

	/* Bytes-per-pixel. Match GetMemUsage(): 1 for UNorm8, 4 otherwise.
	 * SW renderer only meaningfully uses Color (RGBA8), but other
	 * formats need to allocate *something* sensible so the device
	 * doesn't crash if the SW renderer never reaches a code path that
	 * uses them. */
	const int bpp = (format == Format::UNorm8) ? 1 : 4;

	/* Pitch is row-byte-count rounded up to VECTOR_ALIGNMENT so that
	 * SIMD reads/writes can address full rows without spilling. */
	m_pitch = static_cast<int>(pcsx2_align_up_pow2_u32(static_cast<u32>(width * bpp), VECTOR_ALIGNMENT));

	const size_t bytes = static_cast<size_t>(m_pitch) * static_cast<size_t>(height);
	m_data = (u8*)memalign_alloc(VECTOR_ALIGNMENT, bytes ? bytes : VECTOR_ALIGNMENT);
	if (m_data && bytes)
		memset(m_data, 0, bytes);
}

GSTextureSW::~GSTextureSW()
{
	memalign_free(m_data);
	m_data = NULL;
}

bool GSTextureSW::Update(const GSVector4i& r, const void* data, int pitch, int /*layer*/)
{
	if (!m_data || !data)
		return false;

	const int bpp = (m_format == Format::UNorm8) ? 1 : 4;
	const int rx  = r.x;
	const int ry  = r.y;
	const int rw  = r.width();
	const int rh  = r.height();

	if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0)
		return false;
	if (rx + rw > m_size.x || ry + rh > m_size.y)
		return false;

	const u8* src = static_cast<const u8*>(data);
	u8* dst = m_data + (size_t)ry * m_pitch + (size_t)rx * bpp;
	const size_t row_bytes = (size_t)rw * bpp;

	for (int y = 0; y < rh; y++)
	{
		memcpy(dst, src, row_bytes);
		dst += m_pitch;
		src += pitch;
	}
	return true;
}

bool GSTextureSW::Map(GSMap& m, const GSVector4i* r, int /*layer*/)
{
	if (!m_data || m_mapped)
		return false;

	const int bpp = (m_format == Format::UNorm8) ? 1 : 4;
	const int rx = r ? r->x : 0;
	const int ry = r ? r->y : 0;

	m.bits  = m_data + (size_t)ry * m_pitch + (size_t)rx * bpp;
	m.pitch = m_pitch;
	m_mapped = true;
	return true;
}

void GSTextureSW::Unmap()
{
	m_mapped = false;
}
