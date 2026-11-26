/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-License-Identifier: GPL-3.0+ */

#include <string.h>

#include "arm64/aBlocks.h"

#define HASH(b, pc) (((pc) >> 2) & (b)->hash_mask)

void a64_blocks_clear(a64_blocks* b)
{
	memset(b->hash, 0, (b->hash_mask + 1) * sizeof(u32));
	memset(b->page_head, 0, b->npages * sizeof(u32));
	b->rec_free = b->rec_top = b->nrecs = 0;
	b->link_free = b->link_top = b->nlinks = 0;
	b->src_top = 0;
}

int a64_blocks_room(const a64_blocks* b, u32 words, u32 pages)
{
	return b->nrecs < b->max_recs && b->nlinks + pages <= b->max_links &&
	       b->src_top + words <= b->max_src;
}

a64_block* a64_blocks_find(const a64_blocks* b, u32 pc)
{
	u32 i = b->hash[HASH(b, pc)];
	while (i)
	{
		a64_block* r = &b->recs[i - 1];
		if (r->pc == pc)
			return r;
		i = r->hnext;
	}
	return NULL;
}

a64_block* a64_blocks_get(a64_blocks* b, u32 pc)
{
	a64_block* r = a64_blocks_find(b, pc);
	u32 i;
	if (r)
		return r;
	if (b->rec_free)
	{
		i = b->rec_free;
		b->rec_free = b->recs[i - 1].hnext;
	}
	else
		i = ++b->rec_top;
	r = &b->recs[i - 1];
	memset(r, 0, sizeof(*r));
	r->pc = pc;
	r->hnext = b->hash[HASH(b, pc)];
	b->hash[HASH(b, pc)] = i;
	b->nrecs++;
	return r;
}

void a64_blocks_erase(a64_blocks* b, u32 pc)
{
	u32* pp = &b->hash[HASH(b, pc)];
	while (*pp)
	{
		const u32 i = *pp;
		if (b->recs[i - 1].pc == pc)
		{
			*pp = b->recs[i - 1].hnext;
			b->recs[i - 1].hnext = b->rec_free;
			b->rec_free = i;
			b->nrecs--;
			return;
		}
		pp = &b->recs[i - 1].hnext;
	}
}

void a64_blocks_list(a64_blocks* b, u32 pg, u32 pc)
{
	u32 l = b->page_head[pg];
	while (l)
	{
		if (b->links[l - 1].pc == pc)
			return;
		l = b->links[l - 1].next;
	}
	if (b->link_free)
	{
		l = b->link_free;
		b->link_free = b->links[l - 1].next;
	}
	else
		l = ++b->link_top;
	b->links[l - 1].pc   = pc;
	b->links[l - 1].next = b->page_head[pg];
	b->page_head[pg] = l;
	b->nlinks++;
}

void a64_blocks_unlink(a64_blocks* b, u32* link)
{
	const u32 l = *link;
	*link = b->links[l - 1].next;
	b->links[l - 1].next = b->link_free;
	b->link_free = l;
	b->nlinks--;
}

u32 a64_blocks_src(a64_blocks* b, u32 n)
{
	const u32 at = b->src_top;
	b->src_top += n;
	return at;
}
