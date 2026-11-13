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

/* The CLUT is C (GSClut.c); this is what it needs of the renderer: the
 * palette a render target holds, drawn by the GPU, taken from it */

#include "GSClut.h"
#include "GSExtra.h"
#include "GSLocalMemory.h"
#include "Renderers/Common/GSDevice.h"
#include "Renderers/Common/GSRenderer.h"

GSClut::GSClut(GSLocalMemory* mem)
	: m_mem(mem)
{
	gs_clut_init(&m_c);
}

GSClut::~GSClut()
{
	gs_texture_free(m_gpu_clut4);
	gs_texture_free(m_gpu_clut8);
	gs_clut_deinit(&m_c);
}

void GSClut::Write(const GIFRegTEX0& TEX0, const GIFRegTEXCLUT& TEXCLUT)
{
	gs_clut_write(&m_c, m_mem->m_vm8, &TEX0, &TEXCLUT);
}

void GSClut::Read32(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA)
{
	if (!gs_clut_read32(&m_c, &TEX0, &TEXA))
		return;

	m_current_gpu_clut = nullptr;
	/* CSA counts sixteen-entry blocks into the 1KB CLUT buffer, so how far
	 * it can reach depends on the entry size: a 32-bit palette is 256
	 * entries and fills the buffer at CSA 15, while a 16-bit one is 512 and
	 * runs to CSA 31. */
	const u32 csa = TEX0.CSA & ((TEX0.CPSM == PSMCT16 || TEX0.CPSM == PSMCT16S) ? 31u : 15u);
	/* A palette loaded from memory that a render target holds is read from
	 * the target: the GPU drew it, and local memory has not seen it. The
	 * palette is the one the last load put in the CLUT buffer, so that
	 * load's CBP and CPSM name it, whatever CBP the draw's TEX0 carries (a
	 * change of CBP alone does not refresh the draw's registers).
	 * The GPU palette path carries CSM1 shaders only, and the lookup
	 * locates the palette from CBP alone, so a CSM2 palette's COU/COV cannot
	 * reach the shader. Its source window is sixteen blocks square, so a CSA
	 * past block 15 has nothing to read either. Leave m_current_gpu_clut null
	 * for both and let the CPU palette built above stand; that is the one the
	 * software renderer uses. */
	if (!TEX0.CSM && csa <= 15)
	{
		const bool is_4bit = (TEX0.PSM == PSMT4 || TEX0.PSM == PSMT4HL || TEX0.PSM == PSMT4HH);

		GSVector2i offset = {};
		GSVector2i size;
		float scale;
		float sample;
		size.x = is_4bit ? 8 : 16;
		size.y = is_4bit ? 2 : 16;

		GSTexture* src = g_gs_renderer->LookupPaletteSource(m_c.write.TEX0.CBP, m_c.write.TEX0.CPSM, 0, offset, &scale, &sample, size);
		if (src && g_gs_device)
		{
			GSTexture** slot = is_4bit ? &m_gpu_clut4 : &m_gpu_clut8;
			const u32 dst_size = is_4bit ? 16 : 256;
			const u32 dOffset = csa << 4;
			if (!*slot)
				*slot = g_gs_device->CreateRenderTarget(dst_size, 1, GSTexture::Format::Color, false);
			if (*slot)
			{
				g_gs_device->UpdateCLUTTexture(src, scale, sample, offset.x, offset.y, *slot, dOffset, dst_size);
				m_current_gpu_clut = *slot;
			}
		}
	}
}
