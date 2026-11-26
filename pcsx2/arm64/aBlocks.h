/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0+ */

/* The arm64 recompilers' block index, in storage the caller sizes once:
 * records chained per pc hash, and per-page lists of the pcs compiled from
 * each guest page. Nothing here allocates, and records never move, so code
 * can hold a record's address. A list may name a pc whose record is gone;
 * walkers skip those. Source snapshots, for recompilers that keep them, are
 * bump-allocated and given back when the index is cleared. Indices are
 * stored plus one, so 0 ends a chain. */

#ifndef ARM64_ABLOCKS_H
#define ARM64_ABLOCKS_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*a64_block_fn)(void);

typedef struct a64_block
{
	a64_block_fn fn;
	u32 pc;
	u32 end;   /* Norm(one past the native-covered range) */
	u32 src;   /* offset of its source snapshot in a64_blocks.src */
	u32 nsrc;  /* words in it */
	u32 hnext; /* pc hash chain, or the free list */
	u8 live;
	u8 manual;
} a64_block;

typedef struct a64_page_link
{
	u32 pc;
	u32 next;
} a64_page_link;

typedef struct a64_blocks
{
	a64_block*     recs;
	a64_page_link* links;
	u32*           hash;      /* hash_mask + 1 heads */
	u32*           page_head; /* one per guest page */
	u32*           src;
	u32 max_recs, max_links, hash_mask, npages, max_src;
	u32 rec_free, rec_top, nrecs;
	u32 link_free, link_top, nlinks;
	u32 src_top;
} a64_blocks;

/* Forget every record, list and snapshot. */
void a64_blocks_clear(a64_blocks* b);
/* Room for one more block of up to `words` snapshot words on `pages` pages. */
int a64_blocks_room(const a64_blocks* b, u32 words, u32 pages);
a64_block* a64_blocks_find(const a64_blocks* b, u32 pc);
/* The record for pc, zeroed if it is new. Needs room. */
a64_block* a64_blocks_get(a64_blocks* b, u32 pc);
void a64_blocks_erase(a64_blocks* b, u32 pc);
/* Lists pc on page pg, once. Needs a free link. */
void a64_blocks_list(a64_blocks* b, u32 pg, u32 pc);
/* Drops the link *link from its list (walkers hold the pointer to it). */
void a64_blocks_unlink(a64_blocks* b, u32* link);
/* Room for n more snapshot words: their offset. */
u32 a64_blocks_src(a64_blocks* b, u32 n);

#ifdef __cplusplus
}
#endif

#endif
