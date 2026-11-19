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

#ifndef GS_TEXTURE_CACHE_SW_H
#define GS_TEXTURE_CACHE_SW_H

/* The software renderer's textures: each a copy of a texture in local
 * memory, laid out for the scanline drawer and read block by block as
 * draws need them; and the cache of them, by the GS pages they lie in,
 * whose writes to those pages mark blocks to be read again. The hardware
 * renderer's software-drawn sprites keep textures of their own. */

#include "common/Pcsx2Defs.h"
#include "GS/GSRegs.h"
#include "GS/GSOffset.h"
#include "GS/GSLocalMemoryMaps.h"
#include "GS/Renderers/Common/GSFastList.h"

typedef struct gs_sw_texture
{
	gs_offset m_offset;
	gs_page_looper m_pages; /* every page it lies in */
	GIFRegTEX0 m_TEX0;
	GIFRegTEXA m_TEXA;
	void* m_buff;
	size_t m_buff_size; /* what m_buff was made for */
	u32 m_tw;           /* log2 of a row's texels in m_buff */
	u32 m_age;
	u8 m_complete;
	u8 m_repeating;     /* over more than a page: m_valid by tile */
	GSPage2TileMap* m_p2t;
	/* fast mode: each u32's bits are the 32 blocks of that page;
	 * repeating mode: one bit a tile (8x8) of the texture, which also
	 * comes to 512 at most ((1024*1024)/(8*8)/32) */
	u32 m_valid[GS_MAX_PAGES];
	u16 m_erase_it[GS_MAX_PAGES]; /* its place in each page's list */
	u32 m_index;                  /* in the cache's textures */
	const u32* m_sharedbits;
} gs_sw_texture;

typedef struct gs_sw_texture_cache
{
	/* every texture, dense (the cache owns them): one taken out has the
	 * last moved into its place, m_index following */
	gs_sw_texture** textures;
	u32 count;
	u32 capacity;
	gs_fast_list map[GS_MAX_PAGES]; /* gs_sw_texture*, most recent first */
	gs_fast_list_heap heap;         /* the lists' arrays, made at init */
} gs_sw_texture_cache;

#ifdef __cplusplus
extern "C" {
#endif

/* A texture of TEX0 (tw0: its rows' log2 width, 0 for the texture's
 * own, at least 32 bytes); NULL when out of memory */
gs_sw_texture* gs_sw_texture_new(u32 tw0, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA);
/* Another texture in the same one, its buffer kept for Update to make
 * again if the layout needs another size */
void gs_sw_texture_reset(gs_sw_texture* t, u32 tw0, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA);
void gs_sw_texture_free(gs_sw_texture* t);
/* The blocks of the rectangle not read since they were last written, read
 * into the buffer; zero when the buffer cannot be had */
int gs_sw_texture_update(gs_sw_texture* t, int left, int top, int right, int bottom);

void gs_sw_texture_cache_init(gs_sw_texture_cache* c);
void gs_sw_texture_cache_free(gs_sw_texture_cache* c);
/* The texture of TEX0 (and TEXA where its format takes alpha from it),
 * made the most recent, or a new one; NULL past 1024 texels a side or out
 * of memory */
gs_sw_texture* gs_sw_texture_cache_lookup(gs_sw_texture_cache* c, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA,
	u32 tw0);
/* Pages written in psm: the textures there that share its bits read
 * those blocks again */
void gs_sw_texture_cache_invalidate_pages(gs_sw_texture_cache* c, const gs_page_looper* pages, u32 psm);
/* Every texture on the page, whatever the format it was written in */
void gs_sw_texture_cache_invalidate_page(gs_sw_texture_cache* c, u32 page);
void gs_sw_texture_cache_remove_all(gs_sw_texture_cache* c);
/* A frame on: textures not used in ten are freed */
void gs_sw_texture_cache_inc_age(gs_sw_texture_cache* c);

/* What it reads local memory through (GSRenderer.cpp): a block of psm at
 * bp, as the drawer takes it, and TEX0's page-to-tile map */
void gs_sw_texture_read_block(u32 psm, u32 bp, u8* dst, int dstpitch, const GIFRegTEXA* TEXA);
GSPage2TileMap* gs_sw_texture_page2tile(const GIFRegTEX0* TEX0);

#ifdef __cplusplus
}

/* The C cache, with the software renderer's names */
class GSTextureCacheSW
{
	gs_sw_texture_cache m_c;

public:
	typedef gs_sw_texture Texture;

	GSTextureCacheSW() { gs_sw_texture_cache_init(&m_c); }
	~GSTextureCacheSW() { gs_sw_texture_cache_free(&m_c); }
	GSTextureCacheSW(const GSTextureCacheSW&) = delete;
	GSTextureCacheSW& operator=(const GSTextureCacheSW&) = delete;

	Texture* Lookup(const GIFRegTEX0& TEX0, const GIFRegTEXA& TEXA, u32 tw0 = 0)
	{
		return gs_sw_texture_cache_lookup(&m_c, &TEX0, &TEXA, tw0);
	}
	void InvalidatePages(const gs_page_looper& pages, u32 psm)
	{
		gs_sw_texture_cache_invalidate_pages(&m_c, &pages, psm);
	}
	void InvalidatePage(u32 page) { gs_sw_texture_cache_invalidate_page(&m_c, page); }
	void RemoveAll() { gs_sw_texture_cache_remove_all(&m_c); }
	void IncAge() { gs_sw_texture_cache_inc_age(&m_c); }
};
#endif

#endif
