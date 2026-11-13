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

#ifndef GS_LOCAL_MEMORY_MAPS_H
#define GS_LOCAL_MEMORY_MAPS_H

/* What the renderers look up in local memory by a register's value, made
 * the first time and kept until local memory goes: each row and column's
 * pixel offsets for a frame and Z buffer, and which tiles of a texture
 * live in each page. */

#include "GSRegs.h"
#include "GSPsm.h"

struct gs_flat_map; /* Renderers/Common/GSFlatMap.h */

/* Which tiles of a texture live in each GS page, for the repeating-texture
 * invalidation in both texture caches: the entries pages back to back,
 * page p's from entries[first[p]] up to entries[first[p + 1]], one
 * allocation with the header. C++ reads the entries as GSVector2i, the
 * same two ints. */
typedef struct GSPage2TileMap
{
#ifdef __cplusplus
	const GSVector2i* entries;
#else
	const gs_int2* entries;
#endif
	u32 first[GS_MAX_PAGES + 1];
#ifdef __cplusplus
	__fi const GSVector2i* begin(u32 page) const { return entries + first[page]; }
	__fi const GSVector2i* end(u32 page) const { return entries + first[page + 1]; }
#endif
} GSPage2TileMap;

/* 16-bit offsets into local memory of a frame (x) and Z buffer (y) pixel:
 * each row's first, and each fourth column's from it */
typedef struct GSPixelOffset4
{
#ifdef __cplusplus
	GSVector2i row[2048];
	GSVector2i col[512];
#else
	gs_int2 row[2048];
	gs_int2 col[512];
#endif
	u64 hash; /* FBP, ZBP, FBW and both PSMs whole */
	u32 fbp, zbp, fpsm, zpsm, bw;
} GSPixelOffset4;

#ifdef __cplusplus
extern "C" {
#endif

/* The offsets for FRAME and ZBUF, from map (u64 -> GSPixelOffset4*) */
GSPixelOffset4* gs_pixel_offset4_get(struct gs_flat_map* map, const GIFRegFRAME* FRAME, const GIFRegZBUF* ZBUF);
/* The tiles of TEX0's texture by page, from map (u64 -> GSPage2TileMap*) */
GSPage2TileMap* gs_page2tile_map_get(struct gs_flat_map* map, const GIFRegTEX0* TEX0);
/* Frees what either map holds, and the map */
void gs_local_memory_map_free(struct gs_flat_map* map);

#ifdef __cplusplus
}
#endif

#endif
