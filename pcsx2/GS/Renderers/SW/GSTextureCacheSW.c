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
#include <string.h>

#include <memalign.h>

#include "GSTextureCacheSW.h"
#include "GS/GSPsm.h"
#include "GS/GSUtil.h"

/* (the scanline drawer's loads) */
#define GS_SW_TEXTURE_ALIGN 32

/* Whether TEX0's texture spans more than one page */
static int gs_sw_tex0_repeating(const GIFRegTEX0* TEX0)
{
	if (TEX0->TBW < 2)
	{
		if (TEX0->PSM == PSMT8)
			return TEX0->TW > 7 || TEX0->TH > 6;
		if (TEX0->PSM == PSMT4)
			return TEX0->TW > 7 || TEX0->TH > 7;
	}
	return ((u32)TEX0->TBW << 6u) < (1u << TEX0->TW);
}

/* What a texture is, from TEX0: the rest as a new one has it */
static void gs_sw_texture_set(gs_sw_texture* t, u32 tw0, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA)
{
	const gs_psm_attr* psm = &gs_psm[TEX0->PSM];

	t->m_TEX0     = *TEX0;
	t->m_TEXA     = *TEXA;
	t->m_tw       = tw0;
	t->m_age      = 0;
	t->m_complete = 0;
	t->m_p2t      = NULL;
	/* (one row at least 32 bytes: the smallest block the buffer is made
	 * of) */
	if (t->m_tw == 0)
	{
		const u32 least = psm->pal == 0 ? 3 : 5;
		t->m_tw         = TEX0->TW > least ? TEX0->TW : least;
	}

	memset(t->m_valid, 0, sizeof(t->m_valid));
	t->m_sharedbits = gs_util_shared_bits_ptr(TEX0->PSM);
	t->m_offset     = gs_offset_make(&psm->info, TEX0->TBP0, TEX0->TBW, TEX0->PSM);
	gs_offset_page_looper(&t->m_pages, &t->m_offset, 0, 0, 1 << TEX0->TW, 1 << TEX0->TH);

	/* (repeating mode always works, only slightly slower) */
	t->m_repeating = (u8)gs_sw_tex0_repeating(TEX0);
	if (t->m_repeating)
		t->m_p2t = gs_sw_texture_page2tile(TEX0);
}

gs_sw_texture* gs_sw_texture_new(u32 tw0, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA)
{
	gs_sw_texture* t = (gs_sw_texture*)calloc(1, sizeof(*t));
	if (t)
		gs_sw_texture_set(t, tw0, TEX0, TEXA);
	return t;
}

void gs_sw_texture_reset(gs_sw_texture* t, u32 tw0, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA)
{
	gs_sw_texture_set(t, tw0, TEX0, TEXA);
}

void gs_sw_texture_free(gs_sw_texture* t)
{
	if (!t)
		return;
	if (t->m_buff)
		memalign_free(t->m_buff);
	free(t);
}

int gs_sw_texture_update(gs_sw_texture* t, int left, int top, int right, int bottom)
{
	const gs_psm_attr* psm;
	gs_bn_helper bn;
	size_t buff_size;
	u8* dst;
	int shift, tw, th, block_pitch, blk_right, blk_bottom;
	u32 pitch;

	if (t->m_complete)
		return 1;

	psm   = &gs_psm[t->m_TEX0.PSM];
	shift = psm->pal == 0 ? 2 : 0;
	tw    = 1 << t->m_TEX0.TW;
	th    = 1 << t->m_TEX0.TH;
	if (tw < psm->bs.x)
		tw = psm->bs.x;
	if (th < psm->bs.y)
		th = psm->bs.y;

	/* out to whole blocks */
	left   &= ~(psm->bs.x - 1);
	top    &= ~(psm->bs.y - 1);
	right   = (right + psm->bs.x - 1) & ~(psm->bs.x - 1);
	bottom  = (bottom + psm->bs.y - 1) & ~(psm->bs.y - 1);

	/* (lame, but better than nothing) */
	if (left == 0 && top == 0 && right == tw && bottom == th)
		t->m_complete = 1;

	/* The buffer's size follows the row pitch (m_tw and the format) and
	 * the height: a buffer kept through a reset for another layout is
	 * made again, its contents gone with it. */
	buff_size = ((size_t)(1 << t->m_tw) << shift) * (size_t)th * 4;
	if (t->m_buff && t->m_buff_size != buff_size)
	{
		memalign_free(t->m_buff);
		t->m_buff = NULL;
		memset(t->m_valid, 0, sizeof(t->m_valid));
	}
	if (!t->m_buff)
	{
		t->m_buff = memalign_alloc(GS_SW_TEXTURE_ALIGN, buff_size);
		if (!t->m_buff)
			return 0;
		t->m_buff_size = buff_size;
		/* (texture bounds somewhere are wrong, and a draw has sampled
		 * past what was read) */
		memset(t->m_buff, 0, buff_size);
	}

	pitch       = (u32)(1 << t->m_tw) << shift;
	dst         = (u8*)t->m_buff + pitch * top;
	block_pitch = (int)pitch * psm->bs.y;
	shift      += t->m_offset.swz.block_shift_x;
	blk_bottom  = bottom >> t->m_offset.swz.block_shift_y;
	blk_right   = right >> t->m_offset.swz.block_shift_x;

	for (gs_bn_init(&bn, &t->m_offset, left, top); bn.blk_y < blk_bottom; gs_bn_next_y(&bn), dst += block_pitch)
	{
		for (; bn.blk_x < blk_right; gs_bn_next_x(&bn))
		{
			const u32 block = gs_bn_value(&bn);
			/* repeating: the tile's bit; else the block's */
			const u32 i     = t->m_repeating ? (u32)((bn.blk_y << 7) + bn.blk_x) : block;
			const u32 row   = i >> 5;
			const u32 col   = 1u << (i & 31);

			if (!(t->m_valid[row] & col))
			{
				t->m_valid[row] |= col;
				gs_sw_texture_read_block(t->m_TEX0.PSM, block, &dst[bn.blk_x << shift], (int)pitch, &t->m_TEXA);
			}
		}
	}
	return 1;
}

/* ---- the cache ---- */

/* Each page the looper covers, once, in the C++ loopPages' order */
#define GS_SW_FOR_PAGES_BEGIN(looper, page)                              \
	{                                                                    \
		gs_page_rows rows_;                                              \
		u32 start_, end_, pos_;                                          \
		u32 touched_[GS_MAX_PAGES / 32];                                 \
		memset(touched_, 0, sizeof(touched_));                           \
		gs_page_rows_init(&rows_, looper);                               \
		while (gs_page_rows_next(&rows_, &start_, &end_))                \
			for (pos_ = start_; pos_ < end_; pos_++)                     \
			{                                                            \
				const u32 page = pos_ % GS_MAX_PAGES;                    \
				if ((looper)->slow)                                      \
				{                                                        \
					if (touched_[page / 32] & (1u << (page % 32)))       \
						continue;                                        \
					touched_[page / 32] |= 1u << (page % 32);            \
				}                                                        \
				{
#define GS_SW_FOR_PAGES_END \
				}           \
			}               \
	}

void gs_sw_texture_cache_init(gs_sw_texture_cache* c)
{
	u32 i;
	c->textures = NULL;
	c->count    = 0;
	c->capacity = 0;
	for (i = 0; i < GS_MAX_PAGES; i++)
		gs_fast_list_init(&c->map[i], sizeof(gs_sw_texture*));
}

void gs_sw_texture_cache_free(gs_sw_texture_cache* c)
{
	gs_sw_texture_cache_remove_all(c);
	free(c->textures);
	c->textures = NULL;
	c->capacity = 0;
}

/* Takes t out of the textures, the last into its place */
static void gs_sw_texture_cache_unlist(gs_sw_texture_cache* c, gs_sw_texture* t)
{
	const u32 i              = t->m_index;
	c->textures[i]           = c->textures[--c->count];
	c->textures[i]->m_index  = i;
}

static gs_sw_texture* gs_sw_list_texture(const gs_fast_list* l, u16 index)
{
	return *(gs_sw_texture* const*)gs_fast_list_data(l, index);
}

gs_sw_texture* gs_sw_texture_cache_lookup(gs_sw_texture_cache* c, const GIFRegTEX0* TEX0, const GIFRegTEXA* TEXA,
	u32 tw0)
{
	const gs_psm_attr* psm = &gs_psm[TEX0->PSM];
	gs_fast_list* m;
	gs_sw_texture* t;
	u16 i;

	/* the cache lays a texture out at most 1024 texels a side */
	if (TEX0->TW > 10 || TEX0->TH > 10)
		return NULL;

	m = &c->map[TEX0->TBP0 >> 5];
	for (i = gs_fast_list_first(m); i; i = gs_fast_list_next(m, i))
	{
		t = gs_sw_list_texture(m, i);
		/* TBP0 TBW PSM TW TH */
		if (((TEX0->U32[0] ^ t->m_TEX0.U32[0]) | ((TEX0->U32[1] ^ t->m_TEX0.U32[1]) & 3)) != 0)
			continue;
		if ((psm->trbpp == 16 || psm->trbpp == 24) && TEX0->TCC && TEXA->U64 != t->m_TEXA.U64)
			continue;
		if (tw0 != 0 && t->m_tw != tw0)
			continue;
		gs_fast_list_move_front(m, i);
		t->m_age = 0;
		return t;
	}

	if (c->count == c->capacity)
	{
		const u32 capacity     = c->capacity ? c->capacity * 2 : 64;
		gs_sw_texture** grown = (gs_sw_texture**)realloc(c->textures, capacity * sizeof(*grown));
		if (!grown)
			return NULL;
		c->textures = grown;
		c->capacity = capacity;
	}
	if (!(t = gs_sw_texture_new(tw0, TEX0, TEXA)))
		return NULL;
	t->m_index                = c->count;
	c->textures[c->count++] = t;

	GS_SW_FOR_PAGES_BEGIN(&t->m_pages, page)
		t->m_erase_it[page] = gs_fast_list_insert_front(&c->map[page], &t);
	GS_SW_FOR_PAGES_END
	return t;
}

/* The blocks of the page in t are read again at its next update */
static void gs_sw_texture_invalidate_page(gs_sw_texture* t, u32 page)
{
	if (t->m_repeating)
	{
		const gs_int2* j  = t->m_p2t->entries + t->m_p2t->first[page];
		const gs_int2* je = t->m_p2t->entries + t->m_p2t->first[page + 1];
		for (; j != je; j++)
			t->m_valid[j->x] &= (u32)j->y;
	}
	else
		t->m_valid[page] = 0;
	t->m_complete = 0;
}

void gs_sw_texture_cache_invalidate_pages(gs_sw_texture_cache* c, const gs_page_looper* pages, u32 psm)
{
	GS_SW_FOR_PAGES_BEGIN(pages, page)
		const gs_fast_list* m = &c->map[page];
		u16 i;
		for (i = gs_fast_list_first(m); i; i = gs_fast_list_next(m, i))
		{
			gs_sw_texture* t = gs_sw_list_texture(m, i);
			if (gs_util_has_shared_bits_ptr(psm, t->m_sharedbits))
				gs_sw_texture_invalidate_page(t, page);
		}
	GS_SW_FOR_PAGES_END
}

void gs_sw_texture_cache_invalidate_page(gs_sw_texture_cache* c, u32 page)
{
	const gs_fast_list* m = &c->map[page];
	u16 i;
	for (i = gs_fast_list_first(m); i; i = gs_fast_list_next(m, i))
		gs_sw_texture_invalidate_page(gs_sw_list_texture(m, i), page);
}

void gs_sw_texture_cache_remove_all(gs_sw_texture_cache* c)
{
	u32 i;
	for (i = 0; i < c->count; i++)
		gs_sw_texture_free(c->textures[i]);
	c->count = 0;
	for (i = 0; i < GS_MAX_PAGES; i++)
		gs_fast_list_clear(&c->map[i]);
}

void gs_sw_texture_cache_inc_age(gs_sw_texture_cache* c)
{
	u32 i;
	/* (downwards: a texture taken out has the last moved into its place) */
	for (i = c->count; i-- > 0;)
	{
		gs_sw_texture* const t = c->textures[i];
		if (++t->m_age > 10)
		{
			gs_sw_texture_cache_unlist(c, t);
			GS_SW_FOR_PAGES_BEGIN(&t->m_pages, page)
				gs_fast_list_erase(&c->map[page], t->m_erase_it[page]);
			GS_SW_FOR_PAGES_END
			gs_sw_texture_free(t);
		}
	}
}
