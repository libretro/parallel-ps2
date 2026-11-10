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

/* State with one owner at a time, and no lock.
 *
 * The device's queues, submissions, command pools, buffer blocks, memory
 * heaps, descriptor arenas and framebuffer cache are touched only by the
 * thread that drives the renderer. MTGS hands that role from thread to
 * thread one at a time (the GS thread, or the frontend's thread or its
 * video thread inside a context callback), with a happens-before at each
 * handoff. Every other thread reaches the device only through what is
 * lock-free already: the caches, the object pools, the deletion inbox,
 * and timeline waits.
 *
 * OWNED() marks a scope that relies on that. It is nothing in a normal
 * build. Built with GRANITE_OWNERSHIP_CHECK, it records the thread inside
 * and aborts when a second thread enters while the first is still there;
 * the owner's own nested entries are counted. tests/pgsowner builds the
 * core that way and runs the renderer suites. */

#ifndef GRANITE_OWNERSHIP_CHECK

#define OWNED() ((void)0)

#else

#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <stdio.h>
#include <stdlib.h>

namespace Util
{
struct OwnedScope
{
	static retro_atomic_ptr_t &owner()
	{
		static retro_atomic_ptr_t o;
		return o;
	}

	static unsigned &depth()
	{
		static thread_local unsigned d;
		return d;
	}

	const char *where;

	explicit OwnedScope(const char *where_) : where(where_)
	{
		void *self = (void *)sthread_get_current_thread_id();
		if (depth()++ == 0)
		{
			void *prev = retro_atomic_exchange_ptr(&owner(), self);
			if (prev && prev != self)
			{
				fprintf(stderr, "GRANITE_OWNERSHIP_CHECK: two threads in the device at once (%s)\n", where);
				abort();
			}
		}
	}

	~OwnedScope()
	{
		if (--depth() == 0)
			retro_atomic_store_release_ptr(&owner(), nullptr);
	}
};
}

#define OWNED_CAT2(a, b) a##b
#define OWNED_CAT(a, b) OWNED_CAT2(a, b)
#define OWNED() ::Util::OwnedScope OWNED_CAT(owned_scope_, __LINE__)(__func__)

#endif
