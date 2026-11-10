/* Copyright (c) 2017-2026 Hans-Kristian Arntzen
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

#include "slot_pool.hpp"
#include <memory>
#include <vector>
#include <algorithm>
#include <stdlib.h>
#include "aligned_alloc.hpp"

//#define OBJECT_POOL_DEBUG

namespace Util
{
template<typename T>
class ObjectPool
{
public:
	template<typename... P>
	T *allocate(P &&... p)
	{
#ifndef OBJECT_POOL_DEBUG
		if (vacants.empty())
		{
			unsigned num_objects = 64u << memory.size();
			T *ptr = static_cast<T *>(memalign_alloc(std::max<size_t>(64, alignof(T)),
			                                         num_objects * sizeof(T)));
			if (!ptr)
				return nullptr;

			for (unsigned i = 0; i < num_objects; i++)
				vacants.push_back(&ptr[i]);

			memory.emplace_back(ptr);
		}

		T *ptr = vacants.back();
		vacants.pop_back();
		new(ptr) T(std::forward<P>(p)...);
		return ptr;
#else
		return new T(std::forward<P>(p)...);
#endif
	}

	void free(T *ptr)
	{
#ifndef OBJECT_POOL_DEBUG
		ptr->~T();
		vacants.push_back(ptr);
#else
		delete ptr;
#endif
	}

	void clear()
	{
#ifndef OBJECT_POOL_DEBUG
		vacants.clear();
		memory.clear();
#endif
	}

protected:
#ifndef OBJECT_POOL_DEBUG
	std::vector<T *> vacants;

	struct MallocDeleter
	{
		void operator()(T *ptr)
		{
			memalign_free(ptr);
		}
	};

	std::vector<std::unique_ptr<T, MallocDeleter>> memory;
#endif
};

/* ObjectPool for objects made and freed on any thread, without a lock:
 * the slots are a SlotPool's. */
template<typename T>
class ThreadSafeObjectPool
{
public:
	ThreadSafeObjectPool()
	{
		pool.init(sizeof(T), alignof(T));
	}

	template<typename... P>
	T *allocate(P &&... p)
	{
#ifndef OBJECT_POOL_DEBUG
		void *mem = pool.take();
		if (!mem)
			return nullptr;
		return new(mem) T(std::forward<P>(p)...);
#else
		return new T(std::forward<P>(p)...);
#endif
	}

	void free(T *ptr)
	{
#ifndef OBJECT_POOL_DEBUG
		ptr->~T();
		pool.give(ptr);
#else
		delete ptr;
#endif
	}

	/* No allocate or free may run alongside this. */
	void clear()
	{
		pool.clear();
	}

private:
	SlotPool pool;
};
}
