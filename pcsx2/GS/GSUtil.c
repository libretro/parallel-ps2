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

#include "GSUtil.h"

#include <string.h>

u8  gs_util_prim_class[8];
u32 gs_util_compatible_bits[64][2];
u32 gs_util_shared_bits[64][2];
u32 gs_util_swizzle_bits[64][2];

#define GS_UTIL_SET(table, a, b) ((table)[a][(b) >> 5] |= 1u << ((b) & 0x1f))

void gs_util_init(void)
{
	int i;

	gs_util_prim_class[GS_POINTLIST]     = GS_POINT_CLASS;
	gs_util_prim_class[GS_LINELIST]      = GS_LINE_CLASS;
	gs_util_prim_class[GS_LINESTRIP]     = GS_LINE_CLASS;
	gs_util_prim_class[GS_TRIANGLELIST]  = GS_TRIANGLE_CLASS;
	gs_util_prim_class[GS_TRIANGLESTRIP] = GS_TRIANGLE_CLASS;
	gs_util_prim_class[GS_TRIANGLEFAN]   = GS_TRIANGLE_CLASS;
	gs_util_prim_class[GS_SPRITE]        = GS_SPRITE_CLASS;
	gs_util_prim_class[GS_INVALID]       = GS_INVALID_CLASS;

	memset(gs_util_compatible_bits, 0, sizeof(gs_util_compatible_bits));
	for (i = 0; i < 64; i++)
		GS_UTIL_SET(gs_util_compatible_bits, i, i);
	GS_UTIL_SET(gs_util_compatible_bits, PSMCT32, PSMCT24);
	GS_UTIL_SET(gs_util_compatible_bits, PSMCT24, PSMCT32);
	GS_UTIL_SET(gs_util_compatible_bits, PSMCT16, PSMCT16S);
	GS_UTIL_SET(gs_util_compatible_bits, PSMCT16S, PSMCT16);
	GS_UTIL_SET(gs_util_compatible_bits, PSMZ32, PSMZ24);
	GS_UTIL_SET(gs_util_compatible_bits, PSMZ24, PSMZ32);
	GS_UTIL_SET(gs_util_compatible_bits, PSMZ16, PSMZ16S);
	GS_UTIL_SET(gs_util_compatible_bits, PSMZ16S, PSMZ16);

	memset(gs_util_swizzle_bits, 0, sizeof(gs_util_swizzle_bits));
	for (i = 0; i < 64; i++)
		GS_UTIL_SET(gs_util_swizzle_bits, i, i);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMCT32, PSMCT24);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMCT24, PSMCT32);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMT8H, PSMCT32);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMCT32, PSMT8H);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMT4HL, PSMCT32);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMCT32, PSMT4HL);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMT4HH, PSMCT32);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMCT32, PSMT4HH);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMZ32, PSMZ24);
	GS_UTIL_SET(gs_util_swizzle_bits, PSMZ24, PSMZ32);

	memset(gs_util_shared_bits, 0, sizeof(gs_util_shared_bits));
	GS_UTIL_SET(gs_util_shared_bits, PSMCT24, PSMT8H);
	GS_UTIL_SET(gs_util_shared_bits, PSMCT24, PSMT4HL);
	GS_UTIL_SET(gs_util_shared_bits, PSMCT24, PSMT4HH);
	GS_UTIL_SET(gs_util_shared_bits, PSMZ24, PSMT8H);
	GS_UTIL_SET(gs_util_shared_bits, PSMZ24, PSMT4HL);
	GS_UTIL_SET(gs_util_shared_bits, PSMZ24, PSMT4HH);
	GS_UTIL_SET(gs_util_shared_bits, PSMT8H, PSMCT24);
	GS_UTIL_SET(gs_util_shared_bits, PSMT8H, PSMZ24);
	GS_UTIL_SET(gs_util_shared_bits, PSMT4HL, PSMCT24);
	GS_UTIL_SET(gs_util_shared_bits, PSMT4HL, PSMZ24);
	GS_UTIL_SET(gs_util_shared_bits, PSMT4HL, PSMT4HH);
	GS_UTIL_SET(gs_util_shared_bits, PSMT4HH, PSMCT24);
	GS_UTIL_SET(gs_util_shared_bits, PSMT4HH, PSMZ24);
	GS_UTIL_SET(gs_util_shared_bits, PSMT4HH, PSMT4HL);
}

u32 gs_util_channel_mask(u32 spsm)
{
	switch (spsm)
	{
		case PSMCT24:
		case PSMZ24:
			return 0x7;
		case PSMT8H:
		case PSMT4HH: /* No way to express half channels yet. */
		case PSMT4HL:
			return 0x8;
		default:
			break;
	}
	return 0xf;
}

u32 gs_util_channel_mask_fbmsk(u32 spsm, u32 fbmsk)
{
	u32 mask = gs_util_channel_mask(spsm);
	mask &= ((fbmsk & 0xFF) == 0xFF) ? (~0x1u & 0xf) : 0xf;
	mask &= ((fbmsk & 0xFF00) == 0xFF00) ? (~0x2u & 0xf) : 0xf;
	mask &= ((fbmsk & 0xFF0000) == 0xFF0000) ? (~0x4u & 0xf) : 0xf;
	mask &= ((fbmsk & 0xFF000000) == 0xFF000000) ? (~0x8u & 0xf) : 0xf;
	return mask;
}
