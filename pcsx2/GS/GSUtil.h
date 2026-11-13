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

#pragma once

#include "GSRegs.h"
#include <retro_inline.h>
#ifdef __cplusplus
#include "GS.h"
#endif

/* What the GS knows of the pixel formats' relationships, as bit tables
 * indexed by PSM: which share the bits of a 32-bit word, which hold the
 * same bit depth and which swizzle alike. Filled once by gs_util_init;
 * the queries are inline, since the texture cache asks them per draw. */
#ifdef __cplusplus
extern "C" {
#endif

extern u8  gs_util_prim_class[8];
extern u32 gs_util_compatible_bits[64][2];
extern u32 gs_util_shared_bits[64][2];
extern u32 gs_util_swizzle_bits[64][2];

void gs_util_init(void);
u32  gs_util_channel_mask(u32 spsm);
u32  gs_util_channel_mask_fbmsk(u32 spsm, u32 fbmsk);

#ifdef __cplusplus
}
#endif

static INLINE GS_PRIM_CLASS gs_util_get_prim_class(u32 prim)
{
	return (GS_PRIM_CLASS)gs_util_prim_class[prim];
}

static INLINE const u32* gs_util_shared_bits_ptr(u32 dpsm)
{
	return gs_util_shared_bits[dpsm];
}

static INLINE bool gs_util_has_shared_bits_ptr(u32 spsm, const u32* RESTRICT ptr)
{
	return (ptr[spsm >> 5] & (1u << (spsm & 0x1f))) == 0;
}

/* Pixels can NOT coexist in the same 32 bits of space: PSMT8H or
 * PSMT4HL/HH with CT24 fails this check. */
static INLINE bool gs_util_has_shared_bits(u32 spsm, u32 dpsm)
{
	return (gs_util_shared_bits[dpsm][spsm >> 5] & (1u << (spsm & 0x1f))) == 0;
}

/* The same, and SBP and DBP must match. */
static INLINE bool gs_util_has_shared_bits_bp(u32 sbp, u32 spsm, u32 dbp, u32 dpsm)
{
	return ((sbp ^ dbp) | (gs_util_shared_bits[dpsm][spsm >> 5] & (1u << (spsm & 0x1f)))) == 0;
}

/* Shares bit depths, only detects 16/24/32-bit formats: 24 and 32 bits
 * are cross compatible, 16 bits with 16 bits. */
static INLINE bool gs_util_has_compatible_bits(u32 spsm, u32 dpsm)
{
	return (gs_util_compatible_bits[spsm][dpsm >> 5] & (1u << (dpsm & 0x1f))) != 0;
}

static INLINE bool gs_util_has_same_swizzle_bits(u32 spsm, u32 dpsm)
{
	return (gs_util_swizzle_bits[spsm][dpsm >> 5] & (1u << (dpsm & 0x1f))) != 0;
}

#ifdef __cplusplus
/* The C++ renderers' spelling. */
struct GSUtil
{
	static void Init() { gs_util_init(); }
	static GS_PRIM_CLASS GetPrimClass(u32 prim) { return gs_util_get_prim_class(prim); }
	static const u32* HasSharedBitsPtr(u32 dpsm) { return gs_util_shared_bits_ptr(dpsm); }
	static bool HasSharedBits(u32 spsm, const u32* ptr) { return gs_util_has_shared_bits_ptr(spsm, ptr); }
	static bool HasSharedBits(u32 spsm, u32 dpsm) { return gs_util_has_shared_bits(spsm, dpsm); }
	static bool HasSharedBits(u32 sbp, u32 spsm, u32 dbp, u32 dpsm) { return gs_util_has_shared_bits_bp(sbp, spsm, dbp, dpsm); }
	static bool HasCompatibleBits(u32 spsm, u32 dpsm) { return gs_util_has_compatible_bits(spsm, dpsm); }
	static bool HasSameSwizzleBits(u32 spsm, u32 dpsm) { return gs_util_has_same_swizzle_bits(spsm, dpsm); }
	static u32 GetChannelMask(u32 spsm) { return gs_util_channel_mask(spsm); }
	static u32 GetChannelMask(u32 spsm, u32 fbmsk) { return gs_util_channel_mask_fbmsk(spsm, fbmsk); }
};
#endif
