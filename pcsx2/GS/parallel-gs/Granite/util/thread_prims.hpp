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

/* Granite's threading, on rthreads rather than <thread>/<mutex>/
 * <condition_variable>.
 *
 * pcsx2 proper already runs on rthreads through common/Threading.h, but
 * Granite is built as its own set of static libraries (granite-util,
 * granite-vulkan, parallel-gs) that do not see pcsx2's headers, so the
 * same RAII lives here.  The shapes deliberately mirror the std types
 * they replace -- lock() / unlock() / try_lock(), a scoped lock_guard, a
 * unique_lock the condition variable can release and re-take, wait() with
 * and without a predicate -- so that call sites change in name only and
 * the diff stays reviewable.
 *
 * Semantics match the std pair: wait() releases the mutex while sleeping
 * and re-acquires before returning, and spurious wakeups are possible, so
 * always wait on a predicate.
 */

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

class mutex
{
public:
	mutex() : m_lock(slock_new()) {}
	~mutex() { if (m_lock) slock_free(m_lock); }
	mutex(const mutex &) = delete;
	mutex &operator=(const mutex &) = delete;

	void lock() { slock_lock(m_lock); }
	void unlock() { slock_unlock(m_lock); }
	bool try_lock() { return slock_try_lock(m_lock); }

	slock_t *native() { return m_lock; }

private:
	slock_t *m_lock;
};

class lock_guard
{
public:
	explicit lock_guard(mutex &m) : m_mutex(m) { m_mutex.lock(); }
	~lock_guard() { m_mutex.unlock(); }
	lock_guard(const lock_guard &) = delete;
	lock_guard &operator=(const lock_guard &) = delete;

private:
	mutex &m_mutex;
};

/* Holds the mutex, but can hand it back and re-take it, which is what a
 * condition variable needs and what the std::unique_lock call sites in
 * Granite use it for. */
/* unique_lock(m, defer_lock): constructed without taking m. */
struct defer_lock_t {};
static constexpr defer_lock_t defer_lock{};

class unique_lock
{
public:
	explicit unique_lock(mutex &m) : m_mutex(&m), m_held(true) { m_mutex->lock(); }
	unique_lock(mutex &m, defer_lock_t) : m_mutex(&m), m_held(false) {}
	~unique_lock() { if (m_held) m_mutex->unlock(); }
	unique_lock(const unique_lock &) = delete;
	unique_lock &operator=(const unique_lock &) = delete;

	unique_lock(unique_lock &&other) noexcept
		: m_mutex(other.m_mutex), m_held(other.m_held)
	{
		other.m_mutex = nullptr;
		other.m_held  = false;
	}

	void lock() { m_mutex->lock(); m_held = true; }
	void unlock() { m_mutex->unlock(); m_held = false; }
	bool owns_lock() const { return m_held; }

	mutex *mutex_ptr() const { return m_mutex; }

private:
	mutex *m_mutex;
	bool m_held;
};

class condition_variable
{
public:
	condition_variable() : m_cond(scond_new()) {}
	~condition_variable() { if (m_cond) scond_free(m_cond); }
	condition_variable(const condition_variable &) = delete;
	condition_variable &operator=(const condition_variable &) = delete;

	void wait(unique_lock &holder)
	{
		scond_wait(m_cond, holder.mutex_ptr()->native());
	}

	template <typename Predicate>
	void wait(unique_lock &holder, Predicate pred)
	{
		while (!pred())
			scond_wait(m_cond, holder.mutex_ptr()->native());
	}

	void notify_one() { scond_signal(m_cond); }
	void notify_all() { scond_broadcast(m_cond); }

private:
	scond_t *m_cond;
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
