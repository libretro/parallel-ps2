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

#include <stdlib.h>

#include <memalign.h>

#include "GSLocalMemoryMaps.h"
#include "GSOffset.h"
#include "GSPage2Tile.h"
#include "Renderers/Common/GSFlatMap.h"
#include "Renderers/Common/GSKeyHash.h"

GSPixelOffset4* gs_pixel_offset4_get(gs_flat_map_t* map, const GIFRegFRAME* FRAME, const GIFRegZBUF* ZBUF)
{
	const u32 fbp = FRAME->FBP << 5, zbp = ZBUF->ZBP << 5;
	const u32 fpsm = FRAME->PSM, zpsm = ZBUF->PSM, bw = FRAME->FBW;
	/* every field whole: FRAME and ZBUF take any of the six-bit formats,
	 * not only the ones a render target is meant to have */
	const u64 hash = (u64)FRAME->FBP | ((u64)ZBUF->ZBP << 9) | ((u64)bw << 18)
		| ((u64)fpsm << 24) | ((u64)zpsm << 30);
	const size_t key_hash = gs_key_hash(&hash, sizeof(hash));
	GSPixelOffset4** found = (GSPixelOffset4**)gs_flat_map_find(map, &hash, sizeof(hash), key_hash);
	GSPixelOffset4* off;
	GSPixelOffset4** slot;
	gs_offset fo, zo, fo0, zo0;
	gs_pa_helper fr, zr;
	int fs, zs, i, inserted;

	if (found)
		return *found;

	off = (GSPixelOffset4*)memalign_alloc(32, sizeof(GSPixelOffset4));
	if (!off)
		abort();
	off->hash = hash;
	off->fbp  = fbp;
	off->zbp  = zbp;
	off->fpsm = fpsm;
	off->zpsm = zpsm;
	off->bw   = bw;

	fs = gs_psm[fpsm].bpp >> 5;
	zs = gs_psm[zpsm].bpp >> 5;

	/* x = 0 on each row */
	fo = gs_offset_make(&gs_psm[fpsm].info, fbp, bw, fpsm);
	zo = gs_offset_make(&gs_psm[zpsm].info, zbp, bw, zpsm);
	for (i = 0; i < 2048; i++)
	{
		off->row[i].x = (int)gs_offset_pa(&fo, 0, i) << fs;
		off->row[i].y = (int)gs_offset_pa(&zo, 0, i) << zs;
	}

	/* every fourth x along row 0 of a 2048-wide buffer at 0, from x = 0 */
	fo0 = gs_offset_make(&gs_psm[fpsm].info, 0, 32, fpsm);
	zo0 = gs_offset_make(&gs_psm[zpsm].info, 0, 32, zpsm);
	gs_pa_init(&fr, &fo0, 0, 0);
	gs_pa_init(&zr, &zo0, 0, 0);
	for (i = 0; i < 512; i++)
	{
		off->col[i].x = (int)(gs_pa_value(&fr, i * 4) - gs_pa_value(&fr, 0)) << fs;
		off->col[i].y = (int)(gs_pa_value(&zr, i * 4) - gs_pa_value(&zr, 0)) << zs;
	}

	slot = (GSPixelOffset4**)gs_flat_map_insert(map, &hash, key_hash, &inserted);
	if (!slot)
		abort();
	*slot = off;
	return off;
}

GSPage2TileMap* gs_page2tile_map_get(gs_flat_map_t* map, const GIFRegTEX0* TEX0)
{
	const u64 hash = TEX0->U64 & 0x3ffffffffull; /* TBP0 TBW PSM TW TH */
	const size_t key_hash = gs_key_hash(&hash, sizeof(hash));
	GSPage2TileMap** found = (GSPage2TileMap**)gs_flat_map_find(map, &hash, sizeof(hash), key_hash);
	const gs_int2 bs = gs_psm[TEX0->PSM].bs;
	GSPage2TileMap* p2t;
	GSPage2TileMap** slot;
	gs_offset off;
	gs_bn_helper bn;
	unsigned* keys;
	u8* block;
	size_t blocks, n = 0, total;
	int tw, th, bw_blk, bh_blk, inserted;

	if (found)
		return *found;

	tw = 1 << TEX0->TW;
	th = 1 << TEX0->TH;
	if (tw < bs.x)
		tw = bs.x;
	if (th < bs.y)
		th = bs.y;
	/* no more than local memory holds */
	if ((u32)(tw * th) > VM_SIZE)
		tw = th = 2048;

	off = gs_offset_make(&gs_psm[TEX0->PSM].info, TEX0->TBP0, TEX0->TBW, TEX0->PSM);
	bw_blk = tw >> off.swz.block_shift_x;
	bh_blk = th >> off.swz.block_shift_y;

	/* A key a block: its page, and y:x, 7 bits each (at most 128x128
	 * tiles, 1024x1024 32bpp in 8x8 blocks), then sorted and merged into
	 * each page's rows (GSPage2Tile.c) */
	blocks = (size_t)bh_blk * (size_t)bw_blk;
	keys = (unsigned*)malloc(2 * blocks * sizeof(unsigned) + 1);
	if (!keys)
		abort();
	for (gs_bn_init(&bn, &off, 0, 0); bn.blk_y < bh_blk; gs_bn_next_y(&bn))
		for (; bn.blk_x < bw_blk; gs_bn_next_x(&bn))
		{
			const u32 page = (gs_bn_value(&bn) >> 5) % GS_MAX_PAGES;
			keys[n++] = GS_PAGE2TILE_KEY(page, (bn.blk_y << 7) + bn.blk_x);
		}
	total = gs_page2tile_sort(keys, keys + blocks, n);

	/* the header and its entries in one allocation */
	block = (u8*)memalign_alloc(16, sizeof(GSPage2TileMap) + total * sizeof(gs_int2));
	if (!block)
		abort();
	p2t = (GSPage2TileMap*)block;
	p2t->entries = (const gs_int2*)(block + sizeof(GSPage2TileMap));
	gs_page2tile_fill(keys, n, GS_MAX_PAGES, (int*)(block + sizeof(GSPage2TileMap)), p2t->first);
	free(keys);

	slot = (GSPage2TileMap**)gs_flat_map_insert(map, &hash, key_hash, &inserted);
	if (!slot)
		abort();
	*slot = p2t;
	return p2t;
}

void gs_local_memory_map_free(gs_flat_map_t* map)
{
	size_t i;
	for (i = 0; i < GS_FLAT_MAP_SLOTS(map); i++)
		if (GS_FLAT_MAP_USED(map, i))
			memalign_free(*(void**)GS_FLAT_MAP_VALUE(map, i));
	gs_flat_map_free(map);
}
