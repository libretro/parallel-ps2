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

#include <retro_atomic.h>
#include <memalign.h>

#include "GSRingHeap.h"

#if defined(__GNUC__)
#define GS_RING_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define GS_RING_UNLIKELY(x) (x)
#endif

/* 64 bits of usage counters, four 16-bit ones, in size_t pieces */
#define GS_RING_USAGE_WORDS (8 / sizeof(size_t))
#define GS_RING_COUNTERS_PER_WORD (sizeof(size_t) / 2)

struct gs_ring_buffer
{
	/* the heap's 1 while it holds the buffer, plus the bytes of each
	 * allocation; the buffer is freed when it reaches 0 */
	retro_atomic_size_t amt_allocated;
	/* allocations living in each quarter */
	retro_atomic_size_t usage[GS_RING_USAGE_WORDS];
	size_t size;        /* the whole buffer, header included */
	size_t write_loc;   /* where the next allocation goes */
	int quadrant_shift; /* log2(size / 4) */
};

/* Allocations start past the header, on a cache line */
#define GS_RING_BEGIN (((sizeof(gs_ring_buffer) + 63) / 64) * 64)

static size_t gs_ring_align(size_t align_mask, size_t value)
{
	return (value + align_mask) & ~align_mask;
}

static unsigned gs_ring_quadrant(const gs_ring_buffer* b, size_t off)
{
	return (unsigned)(off >> b->quadrant_shift);
}

static void gs_ring_begin_use(gs_ring_buffer* b, uint64_t usage)
{
	size_t i;
	for (i = 0; i < GS_RING_USAGE_WORDS; i++)
		retro_atomic_fetch_add_size(&b->usage[i], (size_t)(usage >> (i * (64 / GS_RING_USAGE_WORDS))));
}

static void gs_ring_end_use(gs_ring_buffer* b, uint64_t usage)
{
	size_t i;
	for (i = 0; i < GS_RING_USAGE_WORDS; i++)
		retro_atomic_fetch_sub_size(&b->usage[i], (size_t)(usage >> (i * (64 / GS_RING_USAGE_WORDS))));
}

static int gs_ring_in_use(gs_ring_buffer* b, unsigned quadrant)
{
	const size_t word  = (quadrant / GS_RING_COUNTERS_PER_WORD) % GS_RING_USAGE_WORDS;
	const unsigned shift = (unsigned)(quadrant % GS_RING_COUNTERS_PER_WORD) * 16;
	return ((retro_atomic_load_acquire_size(&b->usage[word]) >> shift) & 0xFFFF) != 0;
}

/* The quarters size bytes at begin_off touch: at most two, so the first,
 * middle and last byte find them */
static uint64_t gs_ring_usage_mask(const gs_ring_buffer* b, size_t begin_off, size_t size)
{
	uint64_t mask = 0;
	mask |= (uint64_t)1 << (gs_ring_quadrant(b, begin_off) * 16);
	mask |= (uint64_t)1 << (gs_ring_quadrant(b, begin_off + size / 2) * 16);
	mask |= (uint64_t)1 << (gs_ring_quadrant(b, begin_off + size - 1) * 16);
	return mask;
}

static void gs_ring_decref(gs_ring_buffer* b, size_t amt)
{
	/* (the RMW is acq_rel: every free before this one is seen) */
	if (GS_RING_UNLIKELY(retro_atomic_fetch_sub_size(&b->amt_allocated, amt) == amt))
		memalign_free(b);
}

static void gs_ring_buffer_reset(gs_ring_buffer* b)
{
	size_t i;
	retro_atomic_store_release_size(&b->amt_allocated, 1);
	for (i = 0; i < GS_RING_USAGE_WORDS; i++)
		retro_atomic_store_release_size(&b->usage[i], 0);
	b->write_loc = GS_RING_BEGIN;
}

static gs_ring_buffer* gs_ring_buffer_make(gs_ring_heap* h, int quadrant_shift)
{
	const size_t size = (size_t)4 << quadrant_shift;
	/* on a cache line, so allocations can be too */
	gs_ring_buffer* b = (gs_ring_buffer*)memalign_alloc(GS_RING_MAX_ALIGN, size);
	if (!b)
		abort();
	b->size           = size;
	b->quadrant_shift = quadrant_shift;
	gs_ring_buffer_reset(b);
	h->made++;
	return b;
}

/* NULL when it would run into a quarter still in use */
static void* gs_ring_buffer_alloc(gs_ring_buffer* b, size_t size, size_t align_mask, size_t prefix_size)
{
	unsigned prev_quadrant = gs_ring_quadrant(b, b->write_loc - 1);
	size_t base_off = gs_ring_align(align_mask, b->write_loc + prefix_size);
	unsigned first_quadrant = gs_ring_quadrant(b, base_off - prefix_size);
	unsigned new_quadrant = gs_ring_quadrant(b, base_off + size - 1);
	/* (an allocation starting at the very end goes round to the start) */
	uint64_t usage_mask = first_quadrant < 4 ? (uint64_t)1 << (first_quadrant * 16) : 0;

	if (prev_quadrant != new_quadrant)
	{
		unsigned cur_quadrant = prev_quadrant + 1;
		if (new_quadrant >= 4)
		{
			/* round to the start */
			cur_quadrant = 0;
			usage_mask = 0;
			base_off = gs_ring_align(align_mask, GS_RING_BEGIN + prefix_size);
			new_quadrant = gs_ring_quadrant(b, base_off + size - 1);
		}
		do
		{
			usage_mask |= (uint64_t)1 << (cur_quadrant * 16);
			if (GS_RING_UNLIKELY(gs_ring_in_use(b, cur_quadrant)))
				return NULL;
		} while (++cur_quadrant <= new_quadrant);
	}

	b->write_loc = base_off + size;
	gs_ring_begin_use(b, usage_mask);
	retro_atomic_fetch_add_size(&b->amt_allocated, size + prefix_size);
	return (char*)b + base_off - prefix_size;
}

void gs_ring_heap_init(gs_ring_heap* h)
{
	h->made = 0;
	h->kept = NULL;
	h->cur  = gs_ring_buffer_make(h, 14); /* 64 KiB */
}

void gs_ring_heap_deinit(gs_ring_heap* h)
{
	if (h->kept)
		gs_ring_decref(h->kept, 1);
	if (h->cur)
		gs_ring_decref(h->cur, 1);
	h->kept = h->cur = NULL;
}

void* gs_ring_heap_alloc_prefixed(gs_ring_heap* h, size_t size, size_t align_mask, size_t prefix_size)
{
	size_t total;
	int shift;
	gs_ring_buffer* next;
	void* ptr;

	prefix_size += sizeof(gs_ring_buffer*); /* each allocation names its buffer */
	total = size + prefix_size;

	if (total <= h->cur->size / 2)
	{
		ptr = gs_ring_buffer_alloc(h->cur, size, align_mask, prefix_size);
		if (ptr)
		{
			*(gs_ring_buffer**)ptr = h->cur;
			return (gs_ring_buffer**)ptr + 1;
		}
	}

	/* Two buffers of a size take turns: the one left last time is taken
	 * back once every allocation from it is freed, and the first time a
	 * buffer is left a second like it is made. Only when the other one is
	 * still in use too, or the allocation is too big, does the heap grow,
	 * past what is needed as it always has: to at most 64 MiB, unless the
	 * allocation needs more. */
	if (h->kept && h->kept->quadrant_shift >= h->cur->quadrant_shift
		&& total <= h->kept->size / 2
		&& retro_atomic_load_acquire_size(&h->kept->amt_allocated) == 1)
	{
		next = h->kept;
		gs_ring_buffer_reset(next);
	}
	else if (!h->kept && total <= h->cur->size / 2)
		next = gs_ring_buffer_make(h, h->cur->quadrant_shift);
	else
	{
		shift = h->cur->quadrant_shift;
		do
		{
			shift++;
		} while (total > ((size_t)2 << shift));
		if (shift > 24 && total <= ((size_t)2 << (shift - 1)))
			shift--;
		if (h->kept)
			gs_ring_decref(h->kept, 1);
		next = gs_ring_buffer_make(h, shift);
	}

	/* the current one is kept for next time, with the heap's 1 */
	h->kept = h->cur;
	h->cur  = next;

	ptr = gs_ring_buffer_alloc(next, size, align_mask, prefix_size);
	if (!ptr)
		abort();
	*(gs_ring_buffer**)ptr = next;
	return (gs_ring_buffer**)ptr + 1;
}

void gs_ring_heap_free_prefixed(void* ptr, size_t size)
{
	gs_ring_buffer** bptr = (gs_ring_buffer**)ptr - 1;
	gs_ring_buffer* b = *bptr;
	const size_t begin_off = (size_t)((char*)bptr - (char*)b);
	size += sizeof(gs_ring_buffer*);
	gs_ring_end_use(b, gs_ring_usage_mask(b, begin_off, size));
	gs_ring_decref(b, size);
}

void* gs_ring_heap_alloc(gs_ring_heap* h, size_t size, size_t align)
{
	const size_t align_mask = (align > GS_RING_MIN_ALIGN ? align : GS_RING_MIN_ALIGN) - 1;
	size_t* header = (size_t*)gs_ring_heap_alloc_prefixed(h, size, align_mask, sizeof(size_t));
	*header = size + sizeof(size_t);
	return header + 1;
}

void gs_ring_heap_free(void* ptr)
{
	size_t* header = (size_t*)ptr - 1;
	gs_ring_heap_free_prefixed(header, *header);
}
