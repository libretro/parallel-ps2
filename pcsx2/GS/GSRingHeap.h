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

#ifndef GS_RING_HEAP_H
#define GS_RING_HEAP_H

#include <stddef.h>
#include <stdint.h>

/* A ring buffer used as a heap: one thread allocates and writes, other
 * threads read what it shares and any thread frees, in roughly the order
 * of allocation. The buffer is cut in quarters, each counting what lives
 * in it; when the next quarter is still in use, or an allocation is over
 * half the buffer, the buffer is left to its allocations and a new one
 * taken. The last one left is kept: once all of its allocations are
 * freed and it is big enough, it is taken again rather than allocating,
 * so a heap that has grown to its working size allocates no more. */

typedef struct gs_ring_buffer gs_ring_buffer;

typedef struct gs_ring_heap
{
	gs_ring_buffer* cur;  /* allocations come from here */
	gs_ring_buffer* kept; /* the last one left, held for reuse */
	size_t made;          /* buffers allocated so far */
} gs_ring_heap;

/* Allocations are aligned to at least this */
#define GS_RING_MIN_ALIGN (sizeof(size_t) > sizeof(void*) ? sizeof(size_t) : sizeof(void*))
/* and to at most this; a buffer starts on it */
#define GS_RING_MAX_ALIGN 64

#ifdef __cplusplus
extern "C" {
#endif

void gs_ring_heap_init(gs_ring_heap* h);
/* Lets go of its buffers; each is freed with its last allocation */
void gs_ring_heap_deinit(gs_ring_heap* h);

/* size bytes aligned by align_mask + 1, with prefix_size bytes before
 * them; returns the prefix */
void* gs_ring_heap_alloc_prefixed(gs_ring_heap* h, size_t size, size_t align_mask, size_t prefix_size);
/* Frees what gs_ring_heap_alloc_prefixed returned, of prefix_size + size */
void gs_ring_heap_free_prefixed(void* ptr, size_t size);

/* size bytes aligned to align, which remember their size */
void* gs_ring_heap_alloc(gs_ring_heap* h, size_t size, size_t align);
void gs_ring_heap_free(void* ptr);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <new>

#include <retro_atomic.h>

#include "common/Pcsx2Defs.h"

class GSRingHeap
{
	gs_ring_heap m_heap;

	void* alloc_internal(size_t size, size_t align_mask, size_t prefix_size)
	{
		return gs_ring_heap_alloc_prefixed(&m_heap, size, align_mask, prefix_size);
	}
	static void free_internal(void* ptr, size_t size) noexcept
	{
		gs_ring_heap_free_prefixed(ptr, size);
	}

	/* constexpr context: the helpers are ordinary inline functions, so this
	 * one stays a conditional expression. */
	static constexpr size_t MIN_ALIGN =
		(alignof(size_t) > alignof(void*)) ? alignof(size_t) : alignof(void*);

	static size_t getAlignMask(size_t align)
	{
		return pcsx2_max_i(MIN_ALIGN, align) - 1;
	}

public:
	GSRingHeap(GSRingHeap&&) = delete;
	GSRingHeap() { gs_ring_heap_init(&m_heap); }
	~GSRingHeap() noexcept { gs_ring_heap_deinit(&m_heap); }

	/// Allocate a piece of memory with the given size and alignment
	void* alloc(size_t size, size_t align) { return gs_ring_heap_alloc(&m_heap, size, align); }

	/// Free a pointer allocated with `alloc`
	static void free(void* ptr) { gs_ring_heap_free(ptr); }

	/// A reference-counted pointer to an object on this allocator
	template <typename T>
	class SharedPtr
	{
		friend class GSRingHeap;

		struct alignas(MIN_ALIGN) AllocationHeader
		{
			uint32_t size;
			retro_atomic_int_t refcnt;
		};

		T* m_ptr;

		SharedPtr(T* ptr)
			: m_ptr(ptr)
		{
		}

		AllocationHeader* getHeader()
		{
			return const_cast<AllocationHeader*>(reinterpret_cast<const AllocationHeader*>(m_ptr)) - 1;
		}

	public:
		SharedPtr()
			: m_ptr(nullptr)
		{
		}

		SharedPtr(decltype(nullptr))
			: m_ptr(nullptr)
		{
		}

		SharedPtr(const SharedPtr& other)
			: m_ptr(other.m_ptr)
		{
			if (m_ptr)
				retro_atomic_inc_int(&getHeader()->refcnt);
		}

		SharedPtr(SharedPtr&& other)
			: m_ptr(other.m_ptr)
		{
			other.m_ptr = nullptr;
		}

		SharedPtr& operator=(const SharedPtr& other)
		{
			this->~SharedPtr();
			new (this) SharedPtr(other);
			return *this;
		}

		SharedPtr& operator=(SharedPtr&& other)
		{
			this->~SharedPtr();
			new (this) SharedPtr(other);
			return *this;
		}

		~SharedPtr()
		{
			if (!m_ptr)
				return;
			AllocationHeader* header = getHeader();
			// (See top) Expectation: Once shared, no one writes
			// Therefore we don't need acquire/release semantics here
			if (retro_atomic_fetch_sub_int(&header->refcnt, 1) == 1)
			{
				m_ptr->~T();
				free_internal(static_cast<void*>(header), header->size);
			}
		}

		T* operator->() const { return m_ptr; }
		T* get() const { return m_ptr; }

		/// static_cast the pointer to another type
		template <typename Other>
		SharedPtr<Other> cast() &&
		{
			SharedPtr<Other> other(static_cast<Other*>(m_ptr));
			m_ptr = nullptr;
			return other;
		}
	};

	/// Make a shared pointer to a default-constructed T, at the type's alignment
	template <typename T>
	SharedPtr<T> make_shared()
	{
		using Header = typename SharedPtr<T>::AllocationHeader;
		static constexpr size_t alloc_size = sizeof(T) + sizeof(Header);

		void* ptr = alloc_internal(sizeof(T), getAlignMask(alignof(T)), sizeof(Header));
		Header* header = static_cast<Header*>(ptr);
		header->size = static_cast<uint32_t>(alloc_size);
		retro_atomic_store_release_int(&header->refcnt, 1);

		T* tptr = reinterpret_cast<T*>(header + 1);
		new (tptr) T();
		return SharedPtr<T>(tptr);
	}
};

#endif

#endif
