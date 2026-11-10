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

#include "hash.hpp"
#include "aligned_alloc.hpp"
#include <retro_atomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <new>
#include <utility>

#ifndef RETRO_ATOMIC_HAS_PTR
#error "InsertOnlyHashMap needs pointer atomics (retro_atomic.h)"
#endif

namespace Util
{
/* An insert-only map from a hash to an object it owns, for the device's
 * caches (shaders, programs, layouts, render passes, samplers, and each
 * program's pipelines), none of which removes an entry before the map
 * itself goes.
 *
 * The objects stay where they are made: the map holds pointers to them,
 * in open-addressed slots. A slot is claimed with one pointer CAS; a
 * lookup is acquire loads and never waits on anything. Two threads
 * making the same key race on the CAS: one wins, the other destroys what
 * it made and returns the winner's.
 *
 * Slots live in segments. When one is half full it is sealed - every
 * slot still empty gets a sentinel, so nothing lands in it any more -
 * and a segment twice its size goes in front of it with one release
 * store. Nothing is copied or moved, and a segment is freed only with
 * the map. A lookup probes the newest segment first. An insert that
 * finds a sealed slot helps seal and moves on to the newer segment; the
 * sealing happens before the newer segment is published, so an entry
 * that made it into the old one is seen by whoever searches after.
 *
 * Nothing a lookup can reach is freed while the map exists - not an
 * object, not a segment - so there is no grace period to wait out and
 * nothing a reader has to announce: whatever thread looks up (the
 * thread recording commands, the device's own request_* callers, a
 * thread building pipelines ahead), however long it is held up, every
 * pointer it loaded stays valid until the device is destroyed, which
 * happens only once no thread uses it.
 *
 * It needs pointer atomics, which every target with Vulkan has. */
template <typename T>
class InsertOnlyHashMap
{
public:
	explicit InsertOnlyHashMap(size_t initial_capacity = 256)
	{
		size_t cap = 16;
		while (cap < initial_capacity)
			cap <<= 1;
		first_capacity = cap;
		retro_atomic_ptr_init(&head, nullptr);
	}

	InsertOnlyHashMap(const InsertOnlyHashMap &) = delete;
	void operator=(const InsertOnlyHashMap &) = delete;

	~InsertOnlyHashMap()
	{
		clear();
	}

	T *find(Hash hash) const
	{
		const Segment *seg = static_cast<const Segment *>(retro_atomic_load_acquire_ptr(
				const_cast<retro_atomic_ptr_t *>(&head)));
		for (; seg; seg = seg->older)
		{
			T *t = seg->find(hash);
			if (t)
				return t;
		}
		return nullptr;
	}

	/* The object for @hash: one made here from @p, or the one already
	 * there, in which case what was made is destroyed. */
	template <typename... P>
	T *emplace_yield(Hash hash, P &&... p)
	{
		void *mem = memalign_alloc(alignof(T) > 64 ? alignof(T) : 64, sizeof(T));
		if (!mem)
			return nullptr;
		T *t = new (mem) T(std::forward<P>(p)...);
		t->set_hash(hash);
		T *ret = insert_yield(t);
		if (ret != t)
			destroy(t);
		return ret;
	}

	/* Every object in the map; safe alongside lookups and inserts, though
	 * an insert racing the walk may or may not be visited. */
	template <typename F>
	void for_each(F &&func)
	{
		Segment *seg = static_cast<Segment *>(retro_atomic_load_acquire_ptr(&head));
		for (; seg; seg = seg->older)
		{
			for (size_t i = 0; i <= seg->mask; i++)
			{
				T *t = seg->load(i);
				if (t && !is_sealed(t))
					func(*t);
			}
		}
	}

	template <typename F>
	void for_each(F &&func) const
	{
		const_cast<InsertOnlyHashMap *>(this)->for_each([&](T &t) { func(static_cast<const T &>(t)); });
	}

	/* No lookup or insert may run alongside this. */
	void clear()
	{
		Segment *seg = static_cast<Segment *>(retro_atomic_load_relaxed_ptr(&head));
		retro_atomic_store_relaxed_ptr(&head, nullptr);
		while (seg)
		{
			Segment *older = seg->older;
			for (size_t i = 0; i <= seg->mask; i++)
			{
				T *t = seg->load(i);
				if (t && !is_sealed(t))
					destroy(t);
			}
			free(seg);
			seg = older;
		}
	}

private:
	struct Segment
	{
		Segment *older; /* immutable once published */
		size_t mask;
		retro_atomic_int_t used;
		retro_atomic_ptr_t slots[1];

		T *load(size_t i) const
		{
			return static_cast<T *>(retro_atomic_load_acquire_ptr(const_cast<retro_atomic_ptr_t *>(&slots[i])));
		}

		/* Linear probing on an insert-only table: an entry sits before
		 * the first slot that was empty when it went in, and occupied
		 * slots stay occupied, so the first empty or sealed one ends
		 * the search. */
		T *find(Hash hash) const
		{
			size_t i = size_t(hash) & mask;
			for (size_t n = 0; n <= mask; n++, i = (i + 1) & mask)
			{
				T *t = load(i);
				if (!t || is_sealed(t))
					return nullptr;
				if (t->get_hash() == hash)
					return t;
			}
			return nullptr;
		}
	};

	retro_atomic_ptr_t head;
	size_t first_capacity;

	static T *sealed()
	{
		/* Odd, so never an object (they are 64-byte aligned). */
		return reinterpret_cast<T *>(uintptr_t(1));
	}

	static bool is_sealed(const T *t)
	{
		return t == sealed();
	}

	static void destroy(T *t)
	{
		t->~T();
		memalign_free(t);
	}

	static Segment *make_segment(size_t capacity, Segment *older)
	{
		Segment *seg = static_cast<Segment *>(calloc(1, sizeof(Segment) + (capacity - 1) * sizeof(seg->slots[0])));
		if (!seg)
			return nullptr;
		seg->older = older;
		seg->mask = capacity - 1;
		retro_atomic_int_init(&seg->used, 0);
		for (size_t i = 0; i < capacity; i++)
			retro_atomic_ptr_init(&seg->slots[i], nullptr);
		return seg;
	}

	/* Every empty slot of @seg sealed (any thread may help), then a
	 * segment twice its size in front of it, unless one already is. */
	bool grow(Segment *seg)
	{
		for (size_t i = 0; i <= seg->mask; i++)
			if (!retro_atomic_load_acquire_ptr(&seg->slots[i]))
				retro_atomic_cas_ptr(&seg->slots[i], nullptr, sealed());

		if (retro_atomic_load_acquire_ptr(&head) != seg)
			return true;
		Segment *next = make_segment((seg->mask + 1) * 2, seg);
		if (!next)
			return false;
		if (!retro_atomic_cas_ptr(&head, seg, next))
			free(next);
		return true;
	}

	T *insert_yield(T *t)
	{
		const Hash hash = t->get_hash();
		for (;;)
		{
			Segment *seg = static_cast<Segment *>(retro_atomic_load_acquire_ptr(&head));
			if (!seg)
			{
				Segment *first = make_segment(first_capacity, nullptr);
				if (!first)
					return nullptr;
				if (!retro_atomic_cas_ptr(&head, nullptr, first))
					free(first);
				continue;
			}

			/* Already there, in this segment or an older one. */
			for (Segment *s = seg->older; s; s = s->older)
			{
				T *found = s->find(hash);
				if (found)
					return found;
			}

			if (size_t(retro_atomic_load_acquire_int(&seg->used)) * 2 > seg->mask + 1)
			{
				if (!grow(seg))
					return nullptr;
				continue;
			}

			size_t i = size_t(hash) & seg->mask;
			for (size_t n = 0; n <= seg->mask; n++, i = (i + 1) & seg->mask)
			{
				T *cur = seg->load(i);
				if (!cur)
				{
					if (retro_atomic_cas_ptr(&seg->slots[i], nullptr, t))
					{
						retro_atomic_fetch_add_int(&seg->used, 1);
						return t;
					}
					/* Lost the slot: look at what took it. */
					cur = seg->load(i);
				}
				if (is_sealed(cur))
					break;
				if (cur->get_hash() == hash)
					return cur;
			}
			/* Sealed under us, or full: on to the newer segment. */
			if (!grow(seg))
				return nullptr;
		}
	}
};
}
