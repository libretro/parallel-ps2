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

/* What the CLUT (GSClut.c) needs of the renderer: the palette a render
 * target holds, drawn by the GPU, taken from it */

#include <stddef.h>

#include "GSClut.h"
#include "GSBackend.h"
#include "Renderers/Common/GSTextureBase.h"

void gs_clut_gpu_create(gs_clut* c)
{
	if (!c->gpu_clut4)
		c->gpu_clut4 = gs_device_create_clut_target(16);
	if (!c->gpu_clut8)
		c->gpu_clut8 = gs_device_create_clut_target(256);
}

void gs_clut_gpu_release(gs_clut* c)
{
	if (c->gpu_clut4)
		c->gpu_clut4->m_ops->free(c->gpu_clut4);
	if (c->gpu_clut8)
		c->gpu_clut8->m_ops->free(c->gpu_clut8);
	c->gpu_clut4   = NULL;
	c->gpu_clut8   = NULL;
	c->gpu_current = NULL;
}

void gs_clut_read32_gpu(gs_clut* c, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA)
{
	u32 csa;
	int is_4bit;
	int offset_x = 0, offset_y = 0;
	float scale, sample;
	struct gs_texture* src;
	struct gs_texture* slot;

	if (!gs_clut_read32(c, TEX0, TEXA))
		return;

	c->gpu_current = NULL;
	/* CSA counts sixteen-entry blocks into the 1KB CLUT buffer: a 32-bit
	 * palette (256 entries) fills it at CSA 15, a 16-bit one (512) at 31 */
	csa = TEX0->CSA & ((TEX0->CPSM == PSMCT16 || TEX0->CPSM == PSMCT16S) ? 31u : 15u);
	/* A palette loaded from memory a render target holds is read from the
	 * target: the GPU drew it and local memory has not seen it. The last
	 * load's CBP and CPSM name it, whatever CBP the draw's TEX0 carries.
	 * The GPU path has CSM1 shaders only and locates the palette by CBP,
	 * so a CSM2 palette's COU/COV cannot reach it; its window is sixteen
	 * blocks square, so a CSA past 15 has nothing to read either. Both
	 * keep the CPU palette, the one the software renderer uses. */
	if (TEX0->CSM || csa > 15)
		return;

	is_4bit = TEX0->PSM == PSMT4 || TEX0->PSM == PSMT4HL || TEX0->PSM == PSMT4HH;
	src = gs_gsdx_lookup_palette_source(c->write.TEX0.CBP, c->write.TEX0.CPSM, 0, &offset_x, &offset_y,
		&scale, &sample, is_4bit ? 8 : 16, is_4bit ? 2 : 16);
	slot = is_4bit ? c->gpu_clut4 : c->gpu_clut8;
	if (src && slot
		&& gs_device_update_clut_texture(src, scale, sample, (u32)offset_x, (u32)offset_y, slot, csa << 4,
			is_4bit ? 16u : 256u))
		c->gpu_current = slot;
}
