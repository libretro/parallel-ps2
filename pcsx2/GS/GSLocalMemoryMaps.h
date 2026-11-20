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
 * the first time: each row and column's pixel offsets for a frame and Z
 * buffer's formats, from a pool made with local memory, and which tiles of
 * a texture live in each page, kept until local memory goes. */

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

/* 16-bit offsets into local memory of a frame (x) and Z buffer (y) pixel,
 * from their bases: each row's first, and each fourth column's from it.
 * A buffer's base is page aligned, and a pixel's address is the base plus
 * an offset that does not depend on it (local memory's size apart), so
 * the tables are the formats' and the width's alone, and the bases are
 * added to a row and a column where they are read. */
typedef struct GSPixelOffset4
{
#ifdef __cplusplus
	GSVector2i row[2048];
	GSVector2i col[512];
#else
	gs_int2 row[2048];
	gs_int2 col[512];
#endif
	u32 key; /* FRAME.PSM | ZBUF.PSM << 6 | FRAME.FBW << 12 */
} GSPixelOffset4;

/* The tables in use, made when local memory is: as many as a game draws
 * with between two clears, and more */
#define GS_PO4_POOL 32
typedef struct gs_po4_pool
{
	GSPixelOffset4* entries; /* GS_PO4_POOL of them */
	unsigned count;          /* in use */
	unsigned last;           /* the last found */
} gs_po4_pool;

#ifdef __cplusplus
extern "C" {
#endif

/* The pool's tables. Zero if they could not be made (every get is NULL). */
int gs_po4_pool_init(gs_po4_pool* pool);
void gs_po4_pool_free(gs_po4_pool* pool);
/* Forgets every table: only once nothing reads one (queued draws done) */
#define gs_po4_pool_clear(pool) ((pool)->count = 0, (pool)->last = 0)

/* The tables for FRAME and ZBUF's formats and FRAME's width, and in
 * base[0] and base[1] the frame and Z buffer's bases in the tables' units.
 * NULL if the pool has none and no room for them: the bases are set all
 * the same, and the caller clears the pool once nothing reads it. */
GSPixelOffset4* gs_pixel_offset4_get(gs_po4_pool* pool, const GIFRegFRAME* FRAME, const GIFRegZBUF* ZBUF, u32 base[2]);

/* FRAME and ZBUF's fields the tables and bases come from, as one key */
u64 gs_fzb_key(const GIFRegFRAME* FRAME, const GIFRegZBUF* ZBUF);
/* The tiles of TEX0's texture by page, from map (u64 -> GSPage2TileMap*) */
GSPage2TileMap* gs_page2tile_map_get(struct gs_flat_map* map, const GIFRegTEX0* TEX0);
/* Frees what the map holds, and the map */
void gs_local_memory_map_free(struct gs_flat_map* map);

#ifdef __cplusplus
}
#endif

#endif
