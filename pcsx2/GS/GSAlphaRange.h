/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

#ifndef GS_ALPHA_RANGE_H
#define GS_ALPHA_RANGE_H

/* The least and greatest alpha of texels, as the texture cache keeps it
 * for a source: from RGBA8 texels already expanded, or from a texture's
 * block aligned rect in local memory without expanding it (min and max do
 * not care in which order the swizzle holds the texels). tests/gslm holds
 * the second against ReadTexture, tests/gsalpha the first against the
 * bytes. */

#include "common/Pcsx2Types.h"
#include "GS/GSLocalMemory.h"

typedef struct GSAlphaMinMax
{
	u8 min;
	u8 max;
} GSAlphaMinMax;

#ifdef __cplusplus
extern "C" {
#endif

/* Of width x height RGBA8 texels, rows stride bytes apart (any alignment) */
GSAlphaMinMax gs_rgba8_alpha_minmax(const void* data, u32 width, u32 height, u32 stride);

/* Of the block aligned rect r (left, top, right, bottom) of the texture at
 * off, as its format expands it with TEXA (paletted: the palette read
 * out); zero where that would take more than the expanded texels would (a
 * palette of more than one alpha, a format this does not read) */
int gs_lm_alpha_minmax(const gs_local_memory* mem, const gs_offset* off, const int r[4], u32 psm,
	const GIFRegTEXA* TEXA, u8* amin, u8* amax);

#ifdef __cplusplus
}

#include "GS/GSVector.h"

static inline GSAlphaMinMax GSGetRGBA8AlphaMinMax(const void* data, u32 width, u32 height, u32 stride)
{
	return gs_rgba8_alpha_minmax(data, width, height, stride);
}

static inline bool gs_local_alpha_minmax(const GSLocalMemory* mem, const GSOffset* off, const GSVector4i* r, u32 psm,
	const GIFRegTEXA* TEXA, u8* amin, u8* amax)
{
	const int rc[4] = {r->left, r->top, r->right, r->bottom};
	return gs_lm_alpha_minmax(mem, off, rc, psm, TEXA, amin, amax) != 0;
}
#endif

#endif
