/* Copyright (c) 2024 The libretro team
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

/* Granite's threading, on rthreads and retro_atomic rather than <thread>
 * and <atomic>: the atomics it uses and a joinable thread. There is no
 * mutex and no condition variable here; Granite hands work between
 * threads with atomics, lock-free lists and eventcounts (owned.hpp says
 * which state has one owner instead). */

#ifndef PGS_THREAD_PRIMS_HPP
#define PGS_THREAD_PRIMS_HPP

#include <retro_atomic.h>
#include <rthreads/rthreads.h>

#include <stdint.h>
#include <utility>

namespace PGS
{
/* std::memory_order stand-in.  retro_atomic offers acquire loads, release
 * stores and full-barrier read-modify-writes, all at least as strong as
 * anything these call sites request, so the argument is accepted for
 * diff-with-std readability and otherwise ignored. */
enum memory_order
{
	memory_order_relaxed,
	memory_order_consume,
	memory_order_acquire,
	memory_order_release,
	memory_order_acq_rel,
	memory_order_seq_cst
};

/* The std::atomic surface Granite uses, over retro_atomic.  Only the
 * operations with call sites are offered; fetch_add and fetch_sub on the
 * 64-bit type are CAS loops because retro_atomic has no 64-bit fetch-add
 * primitive. */
class atomic_uint32_t
{
	retro_atomic_int_t v;

public:
	atomic_uint32_t() { retro_atomic_int_init(&v, 0); }
	explicit atomic_uint32_t(uint32_t init) { retro_atomic_int_init(&v, (int)init); }
	atomic_uint32_t(const atomic_uint32_t &) = delete;
	atomic_uint32_t &operator=(const atomic_uint32_t &) = delete;

	uint32_t load(memory_order = memory_order_seq_cst) const
	{
		return (uint32_t)retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t *>(&v));
	}
	void store(uint32_t value, memory_order = memory_order_seq_cst)
	{
		retro_atomic_store_release_int(&v, (int)value);
	}
	uint32_t fetch_add(uint32_t value, memory_order = memory_order_seq_cst)
	{
		return (uint32_t)retro_atomic_fetch_add_int(&v, (int)value);
	}
	uint32_t fetch_sub(uint32_t value, memory_order = memory_order_seq_cst)
	{
		return (uint32_t)retro_atomic_fetch_sub_int(&v, (int)value);
	}
	uint32_t fetch_and(uint32_t value, memory_order = memory_order_seq_cst)
	{
		return (uint32_t)retro_atomic_fetch_and_int(&v, (int)value);
	}
	bool compare_exchange_weak(uint32_t &expected, uint32_t desired,
	                           memory_order = memory_order_seq_cst,
	                           memory_order = memory_order_seq_cst)
	{
		if (retro_atomic_cas_int(&v, (int)expected, (int)desired))
			return true;
		expected = load();
		return false;
	}
	bool compare_exchange_strong(uint32_t &expected, uint32_t desired,
	                             memory_order = memory_order_seq_cst,
	                             memory_order = memory_order_seq_cst)
	{
		return compare_exchange_weak(expected, desired);
	}
};

class atomic_uint64_t
{
	retro_atomic_64_t v;

public:
	atomic_uint64_t() { retro_atomic_64_init(&v, 0); }
	explicit atomic_uint64_t(uint64_t init) { retro_atomic_64_init(&v, (int64_t)init); }
	atomic_uint64_t(const atomic_uint64_t &) = delete;
	atomic_uint64_t &operator=(const atomic_uint64_t &) = delete;

	uint64_t load(memory_order = memory_order_seq_cst) const
	{
		return (uint64_t)retro_atomic_load_acquire_64(const_cast<retro_atomic_64_t *>(&v));
	}
	void store(uint64_t value, memory_order = memory_order_seq_cst)
	{
		retro_atomic_store_release_64(&v, (int64_t)value);
	}
	uint64_t fetch_add(uint64_t value, memory_order = memory_order_seq_cst)
	{
		for (;;)
		{
			const uint64_t old = load();
			if (retro_atomic_cas_64(&v, (int64_t)old, (int64_t)(old + value)))
				return old;
		}
	}
	uint64_t operator=(uint64_t value)
	{
		store(value);
		return value;
	}
};

class atomic_bool
{
	retro_atomic_int_t v;

public:
	atomic_bool() { retro_atomic_int_init(&v, 0); }
	atomic_bool(bool init) { retro_atomic_int_init(&v, init ? 1 : 0); }
	atomic_bool(const atomic_bool &) = delete;
	atomic_bool &operator=(const atomic_bool &) = delete;

	bool load(memory_order = memory_order_seq_cst) const
	{
		return retro_atomic_load_acquire_int(const_cast<retro_atomic_int_t *>(&v)) != 0;
	}
	void store(bool value, memory_order = memory_order_seq_cst)
	{
		retro_atomic_store_release_int(&v, value ? 1 : 0);
	}
	bool operator=(bool value)
	{
		store(value);
		return value;
	}
	operator bool() const { return load(); }
};

/* A joinable thread over sthread_create.  rthreads takes a C entry point
 * and a void*, so the callable is heap-allocated and adopted by the new
 * thread's trampoline; join() reaps it.  Only the operations Granite
 * actually performs are offered: start on construction, joinable(),
 * join(). */
class thread
{
public:
	thread() : m_thread(nullptr) {}

	template <typename Fn>
	explicit thread(Fn &&fn)
	{
		auto *holder = new callable_holder<typename std::decay<Fn>::type>(std::forward<Fn>(fn));
		m_thread     = sthread_create(&trampoline<typename std::decay<Fn>::type>, holder);
		if (!m_thread)
			delete holder;
	}

	~thread() { join(); }

	thread(const thread &) = delete;
	thread &operator=(const thread &) = delete;

	thread(thread &&other) noexcept : m_thread(other.m_thread) { other.m_thread = nullptr; }

	thread &operator=(thread &&other) noexcept
	{
		if (this != &other)
		{
			join();
			m_thread       = other.m_thread;
			other.m_thread = nullptr;
		}
		return *this;
	}

	bool joinable() const { return m_thread != nullptr; }

	void join()
	{
		if (!m_thread)
			return;
		sthread_join(m_thread);
		m_thread = nullptr;
	}

private:
	template <typename Fn>
	struct callable_holder
	{
		explicit callable_holder(Fn &&f) : fn(std::move(f)) {}
		explicit callable_holder(const Fn &f) : fn(f) {}
		Fn fn;
	};

	template <typename Fn>
	static void trampoline(void *data)
	{
		auto *holder = static_cast<callable_holder<Fn> *>(data);
		holder->fn();
		delete holder;
	}

	sthread_t *m_thread;
};
} // namespace PGS

#endif
