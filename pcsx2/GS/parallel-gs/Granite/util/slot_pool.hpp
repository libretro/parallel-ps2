/* Copyright (c) 2026 The RetroArch team
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "aligned_alloc.hpp"
#include <retro_atomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Between a pop's read of the top slot's next and its CAS: where another
 * thread's pop and push can land (the ABA case the tag is for). Empty;
 * tests/pgspool yields here to make that interleaving happen. */
#ifndef SLOT_POOL_BETWEEN_READ_AND_CAS
#define SLOT_POOL_BETWEEN_READ_AND_CAS()
#endif

namespace Util
{
/* Fixed-size slots any thread may take and give back, without a lock.
 *
 * Slots live in blocks: block b holds 64 << b of them, so slot index i
 * is found with a little arithmetic, and a block once made stays until
 * clear(). The free slots are a stack threaded through a per-slot next
 * index; its head is one 64-bit word, the top slot's index + 1 in the
 * low half and a tag the pops bump in the high half, so a CAS against a
 * head that was popped and pushed back in between (the ABA case) fails.
 * A stale next read by a pop that then loses its CAS is never used, and
 * the slot it was read from cannot have gone away, blocks being kept.
 *
 * An empty stack makes the thread that finds it so add a block: the one
 * whose CAS installs it pushes its slots as one chain, a loser frees its
 * own and takes from the winner's. Growth allocates; steady state takes
 * and gives back slots it already has. */
class SlotPool
{
public:
	SlotPool()
	{
		retro_atomic_64_init(&top, 0);
		for (unsigned b = 0; b < MAX_BLOCKS; b++)
			retro_atomic_ptr_init(&blocks[b], nullptr);
	}
	SlotPool(const SlotPool &) = delete;
	void operator=(const SlotPool &) = delete;

	~SlotPool()
	{
		clear();
	}

	/* Before any take: the slot size and its alignment (a power of
	 * two; at least 8). Blocks are 64-byte aligned. */
	void init(size_t size, size_t align)
	{
		align_ = align < 8 ? 8 : align;
		stride = (size + align_ - 1) & ~(align_ - 1);
	}

	void *take()
	{
		for (;;)
		{
			const uint64_t head = uint64_t(retro_atomic_load_acquire_64(&top));
			const uint32_t index1 = uint32_t(head);
			if (!index1)
			{
				if (!grow())
					return nullptr;
				continue;
			}
			const uint32_t next1 = uint32_t(retro_atomic_load_acquire_int(next_of(index1 - 1)));
			SLOT_POOL_BETWEEN_READ_AND_CAS();
			const uint64_t want = ((head >> 32) + 1) << 32 | next1;
			if (retro_atomic_cas_64(&top, int64_t(head), int64_t(want)))
				return slot(index1 - 1);
		}
	}

	void give(void *p)
	{
		const uint32_t index = index_of(p);
		push_chain(index, index);
	}

	/* No take or give may run alongside this. */
	void clear()
	{
		for (unsigned b = 0; b < MAX_BLOCKS; b++)
		{
			void *blk = retro_atomic_load_relaxed_ptr(&blocks[b]);
			if (!blk)
				break;
			memalign_free(blk);
			retro_atomic_store_relaxed_ptr(&blocks[b], nullptr);
		}
		retro_atomic_store_relaxed_64(&top, 0);
	}

private:
	enum { MAX_BLOCKS = 24, FIRST = 64 };

	retro_atomic_64_t top;
	retro_atomic_ptr_t blocks[MAX_BLOCKS];
	size_t stride = 0;
	size_t align_ = 8;

	static size_t block_slots(unsigned b) { return size_t(FIRST) << b; }
	static size_t block_base(unsigned b) { return size_t(FIRST) * ((size_t(1) << b) - 1); }

	/* A block: its slots, then a next index for each (the slots' total
	 * is a multiple of 64 bytes' worth of slots of at least 8, so the
	 * indices that follow are aligned). */
	size_t block_bytes(unsigned b) const
	{
		return block_slots(b) * stride + block_slots(b) * sizeof(retro_atomic_int_t);
	}

	static unsigned block_of(size_t i)
	{
		unsigned b = 0;
		while (i >= block_base(b + 1))
			b++;
		return b;
	}

	uint8_t *block(unsigned b) const
	{
		return static_cast<uint8_t *>(retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t *>(&blocks[b])));
	}

	void *slot(size_t i) const
	{
		const unsigned b = block_of(i);
		return block(b) + (i - block_base(b)) * stride;
	}

	retro_atomic_int_t *next_of(size_t i) const
	{
		const unsigned b = block_of(i);
		return reinterpret_cast<retro_atomic_int_t *>(block(b) + block_slots(b) * stride) + (i - block_base(b));
	}

	uint32_t index_of(const void *p) const
	{
		const uint8_t *q = static_cast<const uint8_t *>(p);
		for (unsigned b = 0; b < MAX_BLOCKS; b++)
		{
			const uint8_t *blk = block(b);
			if (!blk)
				break;
			if (q >= blk && q < blk + block_slots(b) * stride)
				return uint32_t(block_base(b) + size_t(q - blk) / stride);
		}
		abort(); /* not one of ours */
	}

	/* first .. last already linked to each other; link last to the top
	 * and make first the top. */
	void push_chain(uint32_t first, uint32_t last)
	{
		for (;;)
		{
			const uint64_t head = uint64_t(retro_atomic_load_acquire_64(&top));
			retro_atomic_store_release_int(next_of(last), int(uint32_t(head)));
			const uint64_t want = (head & ~uint64_t(0xffffffffu)) | (uint64_t(first) + 1);
			if (retro_atomic_cas_64(&top, int64_t(head), int64_t(want)))
				return;
		}
	}

	bool grow()
	{
		unsigned b = 0;
		while (b < MAX_BLOCKS && block(b))
			b++;
		if (b == MAX_BLOCKS)
			return false;
		uint8_t *blk = static_cast<uint8_t *>(memalign_alloc(align_ > 64 ? align_ : 64, block_bytes(b)));
		if (!blk)
			return false;
		retro_atomic_int_t *next = reinterpret_cast<retro_atomic_int_t *>(blk + block_slots(b) * stride);
		const size_t base = block_base(b);
		for (size_t k = 0; k < block_slots(b); k++)
			retro_atomic_int_init(&next[k], k + 1 < block_slots(b) ? int(base + k + 2) : 0);
		if (!retro_atomic_cas_ptr(&blocks[b], nullptr, blk))
		{
			/* Another thread added this block: take from it. */
			memalign_free(blk);
			return true;
		}
		push_chain(uint32_t(base), uint32_t(base + block_slots(b) - 1));
		return true;
	}
};
}
