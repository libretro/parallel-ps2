/* The software renderer's texture cache (GS/Renderers/SW/GSTextureCacheSW.c)
 * against the hash the C++ GSTextureCacheSW gave (the command line), with
 * local memory a block reader that hashes what it is asked for: scenario.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/GSUtil.h"
#include "GS/GSPsm.h"
#include "GS/GSLocalMemoryMaps.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include "GS/Renderers/SW/GSTextureCacheSW.h"
#include "fake.h"

static gs_p2t_cache sc_p2t;

void gs_sw_texture_read_block(u32 psm, u32 bp, u8* dst, int dstpitch, const GIFRegTEXA* TEXA)
{
	(void)psm;
	fk_read(bp, dst, dstpitch, TEXA->U64);
}

GSPage2TileMap* gs_sw_texture_page2tile(const GIFRegTEX0* TEX0)
{
	return gs_page2tile_map_get(&sc_p2t, TEX0);
}

typedef gs_sw_texture_cache* CACHE;
typedef gs_sw_texture TEX;
static gs_sw_texture_cache* c_new(void)
{
	gs_sw_texture_cache* c = (gs_sw_texture_cache*)malloc(sizeof(*c));
	gs_sw_texture_cache_init(c);
	return c;
}
static void c_free(gs_sw_texture_cache* c)
{
	gs_sw_texture_cache_free(c);
	free(c);
}
#define C_NEW() c_new()
#define C_FREE(c) c_free(c)
#define LOOKUP(c, t0, ta, tw) gs_sw_texture_cache_lookup(c, t0, ta, tw)
#define INVALIDATE_PAGES(c, p, psm) gs_sw_texture_cache_invalidate_pages(c, p, psm)
#define INVALIDATE_PAGE(c, p) gs_sw_texture_cache_invalidate_page(c, p)
#define REMOVE_ALL(c) gs_sw_texture_cache_remove_all(c)
#define INC_AGE(c) gs_sw_texture_cache_inc_age(c)
#define COUNT(c) (c)->count
#define T_UPDATE(t, l, tp, r, b) gs_sw_texture_update(t, l, tp, r, b)
#define T_NEW(tw, t0, ta) gs_sw_texture_new(tw, t0, ta)
#define T_RESET(t, tw, t0, ta) gs_sw_texture_reset(t, tw, t0, ta)
#define T_FREE(t) gs_sw_texture_free(t)

#include "scenario.h"

int main(int argc, char** argv)
{
	unsigned long long h;

	gs_util_init();
	if (!gs_p2t_cache_init(&sc_p2t))
		return 1;
	scenario();
	h = fk_hash;
	printf("gstexcachesw: %016llx\n", h);
	{
		/* the hardware renderer's texture, its pixels from the cache's
		 * block, outlives the cache: the block stays for it */
		GIFRegTEX0 t0;
		GIFRegTEXA ta;
		gs_sw_texture_cache* c = c_new();
		gs_sw_texture* t;
		t0.U64 = 0;
		t0.PSM = PSMCT32;
		t0.TBW = 4;
		t0.TW  = 8;
		t0.TH  = 8;
		ta.U64 = 0;
		t      = gs_sw_texture_new(0, &t0, &ta);
		gs_sw_texture_update(t, 0, 0, 256, 256);
		c_free(c);
		memset(t->m_buff, 0x5A, t->m_buff_size);
		gs_sw_texture_free(t);
		c = c_new();
		c_free(c);
	}
	{
		/* a repeating texture (wider than its buffer) holds its map, and
		 * gives it back when it is made something else and when it goes */
		GIFRegTEX0 t0;
		GIFRegTEXA ta;
		gs_sw_texture* t;
		GSPage2TileMap* m;
		t0.U64 = 0;
		t0.PSM = PSMCT32;
		t0.TBW = 1;
		t0.TW  = 8;
		t0.TH  = 6;
		ta.U64 = 0;
		t      = gs_sw_texture_new(0, &t0, &ta);
		m      = t->m_p2t;
		if (!t->m_repeating || !m || m->refs != 1)
		{
			printf("FAIL: a repeating texture holds no map\n");
			return 1;
		}
		t0.TBW = 8;
		gs_sw_texture_reset(t, 0, &t0, &ta);
		if (m->refs != 0 || t->m_p2t)
		{
			printf("FAIL: a reset texture kept its map\n");
			return 1;
		}
		t0.TBW = 1;
		gs_sw_texture_reset(t, 0, &t0, &ta);
		gs_sw_texture_free(t);
	}

	/* every texture gave its maps back */
	{
		size_t i;
		int held = 0;
		for (i = 0; i < GS_FLAT_MAP_SLOTS(&sc_p2t.map); i++)
			if (GS_FLAT_MAP_USED(&sc_p2t.map, i))
				held |= (*(GSPage2TileMap**)GS_FLAT_MAP_VALUE(&sc_p2t.map, i))->refs != 0;
		if (held)
		{
			printf("FAIL: a page-to-tile map still held\n");
			return 1;
		}
	}
	gs_p2t_cache_free(&sc_p2t);
	if (argc > 1 && strtoull(argv[1], NULL, 16) != h)
	{
		printf("FAIL: the C++ GSTextureCacheSW gave %s\n", argv[1]);
		return 1;
	}
	return 0;
}
