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

#include "slot_pool.hpp"
#include <retro_atomic.h>
#include <new>

namespace Util
{
/* Values any thread may push, taken all at once by one thread, without a
 * lock. A push links a node onto a stack with a CAS; a drain swaps the
 * whole stack out with one exchange and walks it. Nothing is ever popped
 * singly, so there is no ABA case: a pusher whose CAS succeeds against a
 * head that was drained and refilled in between has linked onto that
 * head as it now is. Nodes come from a SlotPool, so a steady state of
 * pushes and drains allocates nothing. Order is not kept. */
template <typename T>
class DeferredList
{
public:
	DeferredList()
	{
		pool.init(sizeof(Node), alignof(Node));
		retro_atomic_ptr_init(&head, nullptr);
	}

	DeferredList(const DeferredList &) = delete;
	void operator=(const DeferredList &) = delete;

	~DeferredList()
	{
		drain([](const T &) {});
	}

	/* False only when no node could be had. */
	bool push(const T &value)
	{
		void *mem = pool.take();
		if (!mem)
			return false;
		Node *n = new (mem) Node(value);
		for (;;)
		{
			Node *h = static_cast<Node *>(retro_atomic_load_acquire_ptr(&head));
			n->next = h;
			if (retro_atomic_cas_ptr(&head, h, n))
				return true;
		}
	}

	/* Everything pushed so far, to @func, on the one thread that drains. */
	template <typename F>
	void drain(F &&func)
	{
		Node *n = static_cast<Node *>(retro_atomic_exchange_ptr(&head, nullptr));
		while (n)
		{
			Node *next = n->next;
			func(n->value);
			n->~Node();
			pool.give(n);
			n = next;
		}
	}

private:
	struct Node
	{
		explicit Node(const T &v) : value(v) {}
		Node *next = nullptr;
		T value;
	};

	retro_atomic_ptr_t head;
	SlotPool pool;
};
}
