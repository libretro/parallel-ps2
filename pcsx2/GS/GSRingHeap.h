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

#pragma once

#include <retro_atomic.h>
#include <new>
#include <stdint.h>
#include <stdlib.h>

/// A ring buffer pretending to be a heap (screams if you don't actually use it like a ring buffer)
/// Meant for one producer thread creating data and sharing it with multiple consumer threads
/// Expectations:
/// - One thread allocates and writes to allocations
/// - Other threads read from allocations (once shared, no one writes)
/// - Any thread can free
/// - Frees are done in approximately the same order as allocations (but not exactly the same order)
class GSRingHeap
{
	struct Buffer;
	Buffer* m_current_buffer;

	void orphanBuffer() noexcept;
	/// Allocate a value of `size` bytes with `prefix_size` bytes before it (for allocation tracking) and alignment specified by `align_mask`
	void* alloc_internal(size_t size, size_t align_mask, size_t prefix_size);
	/// Free a value of size `size` (equal to prefix_size + size when allocated)
	static void free_internal(void* ptr, size_t size) noexcept;

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
	GSRingHeap();
	~GSRingHeap() noexcept;

	/// Allocate a piece of memory with the given size and alignment
	void* alloc(size_t size, size_t align)
	{
		size_t alloc_size = size + sizeof(size_t);
		void* ptr = alloc_internal(size, getAlignMask(align), sizeof(size_t));
		size_t* header = static_cast<size_t*>(ptr);
		*header = alloc_size;
		return static_cast<void*>(header + 1);
	};

	/// Free a pointer allocated with `alloc`
	static void free(void* ptr)
	{
		size_t* header = static_cast<size_t*>(ptr) - 1;
		free_internal(static_cast<void*>(header), *header);
	}

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
		SharedPtr<Other> cast() const&
		{
			retro_atomic_inc_int(&getHeader().refcount);
			return SharedPtr<Other>(static_cast<Other*>(m_ptr));
		}

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
