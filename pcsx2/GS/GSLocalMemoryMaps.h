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
 * buffer's formats, and which tiles of a texture live in each page, both
 * in room made with local memory. */

#include "GSRegs.h"
#include "GSPsm.h"
#include "Renderers/Common/GSBuddy.h"
#include "Renderers/Common/GSFlatMap.h"

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
	u32 refs; /* the textures holding it */
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

/* The page-to-tile maps, made with local memory: room for GS_P2T_MAPS,
 * their tables in one block of 1 << GS_P2T_SHIFT bytes, and what a new
 * one is sorted in (a key a block of the largest texture, twice). Past
 * them a map is allocated. */
#define GS_P2T_MAPS 1024
#define GS_P2T_SHIFT 23
#define GS_P2T_KEYS (2 * (VM_SIZE / 64) + 1)
typedef struct gs_p2t_cache
{
	gs_flat_map_t map; /* u64 TEX0 bits -> GSPage2TileMap* */
	gs_buddy tables;
	unsigned* keys; /* GS_P2T_KEYS */
} gs_p2t_cache;

#ifdef __cplusplus
extern "C" {
#endif

/* Zero if the room could not be made */
int gs_p2t_cache_init(gs_p2t_cache* c);
/* Frees every map, held or not: nothing reads one after */
void gs_p2t_cache_free(gs_p2t_cache* c);

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
/* The tiles of TEX0's texture by page, a reference the caller gives back
 * with gs_page2tile_map_put. Made the first time and kept while there is
 * room, held or not; when there is none, those no texture holds go. */
GSPage2TileMap* gs_page2tile_map_get(gs_p2t_cache* c, const GIFRegTEX0* TEX0);
/* Gives back a reference (NULL: none) */
#define gs_page2tile_map_put(p2t) \
	do \
	{ \
		GSPage2TileMap* const put_ = (p2t); \
		if (put_ && put_->refs) \
			put_->refs--; \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif
