/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2026 PCSX2 Dev Team
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

#include "GSBuddy.h"

#define GS_BUDDY_FREE 0x80

/* A free piece's first two words: the next and the previous free piece
 * of its order */
typedef struct gs_buddy_link
{
	void* next;
	void* prev;
} gs_buddy_link;

static size_t gs_buddy_index(const gs_buddy* b, const void* p)
{
	return (size_t)((const unsigned char*)p - b->base) >> b->min_shift;
}

static void gs_buddy_push(gs_buddy* b, void* p, unsigned order)
{
	gs_buddy_link* l = (gs_buddy_link*)p;
	l->next = b->free_head[order];
	l->prev = NULL;
	if (l->next)
		((gs_buddy_link*)l->next)->prev = p;
	b->free_head[order] = p;
	b->state[gs_buddy_index(b, p)] = (unsigned char)((order + 1) | GS_BUDDY_FREE);
}

static void gs_buddy_unlink(gs_buddy* b, void* p, unsigned order)
{
	gs_buddy_link* l = (gs_buddy_link*)p;
	if (l->prev)
		((gs_buddy_link*)l->prev)->next = l->next;
	else
		b->free_head[order] = l->next;
	if (l->next)
		((gs_buddy_link*)l->next)->prev = l->prev;
}

int gs_buddy_init(gs_buddy* b, unsigned size_shift, unsigned min_shift)
{
	memset(b, 0, sizeof(*b));
	if (min_shift < 6 || size_shift < min_shift || size_shift - min_shift >= 32)
		return 0;
	b->size      = (size_t)1 << size_shift;
	b->min_shift = min_shift;
	b->orders    = size_shift - min_shift + 1;
	b->base      = (unsigned char*)memalign_alloc((size_t)1 << min_shift, b->size);
	b->state     = (unsigned char*)calloc((size_t)1 << (size_shift - min_shift), 1);
	if (!b->base || !b->state)
	{
		gs_buddy_deinit(b);
		return 0;
	}
	gs_buddy_push(b, b->base, b->orders - 1);
	return 1;
}

void gs_buddy_deinit(gs_buddy* b)
{
	if (b->base)
		memalign_free(b->base);
	free(b->state);
	memset(b, 0, sizeof(*b));
}

void* gs_buddy_alloc(gs_buddy* b, size_t bytes)
{
	unsigned order = 0, k;
	unsigned char* p;

	if (!b->base || bytes > b->size)
		return NULL;
	while (((size_t)1 << (b->min_shift + order)) < bytes)
		order++;
	for (k = order; k < b->orders && !b->free_head[k]; k++)
		;
	if (k == b->orders)
		return NULL;
	p = (unsigned char*)b->free_head[k];
	gs_buddy_unlink(b, p, k);
	/* the upper halves of what is too big go back, one an order */
	while (k > order)
	{
		k--;
		gs_buddy_push(b, p + ((size_t)1 << (b->min_shift + k)), k);
	}
	b->state[gs_buddy_index(b, p)] = (unsigned char)(order + 1);
	b->used += (size_t)1 << (b->min_shift + order);
	return p;
}

int gs_buddy_free(gs_buddy* b, void* ptr)
{
	unsigned char* p = (unsigned char*)ptr;
	size_t idx;
	unsigned order;

	if (!b->base || p < b->base || p >= b->base + b->size)
		return 0;
	idx   = gs_buddy_index(b, p);
	order = (unsigned)(b->state[idx] & ~GS_BUDDY_FREE);
	/* (not a piece handed out: a second free, or inside one) */
	if (!order || (b->state[idx] & GS_BUDDY_FREE))
		return 1;
	order--;
	b->used -= (size_t)1 << (b->min_shift + order);
	b->state[idx] = 0;
	/* joined with its twin for as long as the twin is free and whole */
	while (order + 1 < b->orders)
	{
		const size_t twin = idx ^ ((size_t)1 << order);
		if (b->state[twin] != (unsigned char)((order + 1) | GS_BUDDY_FREE))
			break;
		gs_buddy_unlink(b, b->base + (twin << b->min_shift), order);
		b->state[twin] = 0;
		idx &= ~((size_t)1 << order);
		order++;
	}
	gs_buddy_push(b, b->base + (idx << b->min_shift), order);
	return 1;
}
