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

#include <limits.h>
#include <string.h>

#include <memalign.h>

/* The header's inline operations are static, and this file calls few. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "GSFastList.h"

/* Elements in a list's first array: no growth on the first insertions */
#define GS_FAST_LIST_FIRST 4

/* The heap's chain for a size: the smallest power of two from 64 that
 * holds it */
static unsigned gs_fl_heap_class(size_t bytes)
{
	unsigned c = 0;
	while (((size_t)64 << c) < bytes)
		c++;
	return c;
}

int gs_fast_list_heap_init(gs_fast_list_heap* h, size_t bytes)
{
	memset(h, 0, sizeof(*h));
	h->base = (unsigned char*)memalign_alloc(64, bytes);
	if (!h->base)
		return 0;
	h->size = bytes;
	return 1;
}

void gs_fast_list_heap_free(gs_fast_list_heap* h)
{
	memalign_free(h->base);
	memset(h, 0, sizeof(*h));
}

static unsigned char* gs_fl_alloc(gs_fast_list_heap* h, size_t bytes)
{
	unsigned c;
	size_t whole;
	unsigned char* p;
	if (!h)
		return (unsigned char*)memalign_alloc(64, bytes);
	c     = gs_fl_heap_class(bytes);
	whole = (size_t)64 << c;
	if (c < GS_FAST_LIST_HEAP_CLASSES && h->free_head[c])
	{
		p               = (unsigned char*)h->free_head[c];
		h->free_head[c] = *(void**)p;
		return p;
	}
	if (c < GS_FAST_LIST_HEAP_CLASSES && h->base && whole <= h->size - h->used)
	{
		p = h->base + h->used;
		h->used += whole;
		return p;
	}
	h->spilled++;
	return (unsigned char*)memalign_alloc(64, bytes);
}

static void gs_fl_free(gs_fast_list_heap* h, unsigned char* p, size_t bytes)
{
	unsigned c;
	if (!p)
		return;
	if (!h || p < h->base || p >= h->base + h->size)
	{
		memalign_free(p);
		return;
	}
	c               = gs_fl_heap_class(bytes);
	*(void**)p      = h->free_head[c];
	h->free_head[c] = p;
}

/* (an array of capacity elements and its free stack) */
static size_t gs_fl_bytes(const gs_fast_list* l, u16 capacity)
{
	return (size_t)capacity * l->stride + (size_t)(capacity - 1) * sizeof(u16);
}

void gs_fast_list_use_heap(gs_fast_list* l, gs_fast_list_heap* heap)
{
	gs_fast_list_clear(l);
	l->heap = heap;
}

void gs_fast_list_init(gs_fast_list* l, size_t elem_size)
{
	/* an element and its two indexes, at the element's alignment */
	const size_t align = elem_size >= 8 ? 8 : 4;
	l->buf       = NULL;
	l->heap      = NULL;
	l->capacity  = 0;
	l->count     = 0;
	l->elem_size = (u16)elem_size;
	l->stride    = (u16)((elem_size + 2 * sizeof(u16) + align - 1) & ~(align - 1));
	l->link      = (u16)(l->stride - 2 * sizeof(u16));
}

void gs_fast_list_clear(gs_fast_list* l)
{
	if (l->buf)
		gs_fl_free(l->heap, l->buf, gs_fl_bytes(l, l->capacity));
	l->buf      = NULL;
	l->capacity = 0;
	l->count    = 0;
}

int gs_fast_list_grow(gs_fast_list* l)
{
	const u16 capacity = !l->buf ? GS_FAST_LIST_FIRST
		: l->capacity <= USHRT_MAX / 2 ? (u16)(l->capacity * 2) : (u16)USHRT_MAX;
	unsigned char* buf;
	u16* stack;
	u16 i;

	/* (index 0 is the head: a full array of 65535 holds 65534) */
	if (capacity == l->capacity)
		return 0;
	buf = gs_fl_alloc(l->heap, gs_fl_bytes(l, capacity));
	if (!buf)
		return 0;
	stack = (u16*)(buf + (size_t)capacity * l->stride);

	if (l->buf)
	{
		/* in order: a kept index still names its element */
		memcpy(buf, l->buf, (size_t)l->capacity * l->stride);
		memcpy(stack, GS_FAST_LIST_FREE(l), (size_t)(l->capacity - 1) * sizeof(u16));
		gs_fl_free(l->heap, l->buf, gs_fl_bytes(l, l->capacity));
		i = (u16)(l->capacity - 1);
	}
	else
	{
		/* the head, chained to itself */
		memset(buf, 0, l->stride);
		i = 0;
	}
	for (; i < capacity - 1; i++)
		stack[i] = (u16)(i + 1);

	l->buf      = buf;
	l->capacity = capacity;
	return 1;
}
