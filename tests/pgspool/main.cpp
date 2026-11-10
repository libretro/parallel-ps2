/* paraLLEl-GS's lock-free pools (Granite util/slot_pool.hpp), under
 * threads that take and give back at once.
 *
 *   - ThreadSafeObjectPool: every thread allocates, swaps its object into
 *     a shared slot and frees whatever it swapped out, so objects are
 *     freed on threads other than the one that made them. Each object
 *     carries an in-use flag set on allocation: a slot handed to two
 *     threads at once trips it, and so does a torn free list;
 *   - ThreadSafeSlabAllocator, the same way, through a stamp in the slab;
 *   - growth: one thread holds enough objects live to need many blocks,
 *     every one distinct. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>

/* Widen the window a pop leaves between reading the top's next and its
 * CAS, every few pops: on a host with few cores the ABA interleaving
 * would otherwise almost never be hit. */
static thread_local unsigned s_pops;
#define SLOT_POOL_BETWEEN_READ_AND_CAS() \
	do { if ((++s_pops & 7) == 0) sthread_yield(); } while (0)

#include "object_pool.hpp"
#include "slab_allocator.hpp"

namespace
{
struct Obj
{
	explicit Obj(uint32_t owner_) : owner(owner_) {}
	uint32_t owner;
	uint32_t payload[7];
};

enum { THREADS = 4, SHARED = 16, LIVE = 20000 };

Util::ThreadSafeObjectPool<Obj> s_pool;
Util::ThreadSafeSlabAllocator s_slab;
retro_atomic_ptr_t s_shared[SHARED];
retro_atomic_ptr_t s_shared_slab[SHARED];
retro_atomic_int_t s_fails;
int s_iters;

bool claim(Obj *o)
{
	/* payload[0] is the flag: 0 free, 1 in use. A second claim before
	 * the release means two holders. */
	auto *flag = reinterpret_cast<retro_atomic_int_t *>(&o->payload[0]);
	return retro_atomic_cas_int(flag, 0, 1);
}

void release(Obj *o)
{
	auto *flag = reinterpret_cast<retro_atomic_int_t *>(&o->payload[0]);
	retro_atomic_store_release_int(flag, 0);
}

void worker(void *arg)
{
	const uint32_t id = uint32_t(uintptr_t(arg));
	uint32_t rng = id * 2654435761u + 1;
	for (int i = 0; i < s_iters; i++)
	{
		rng = rng * 1664525u + 1013904223u;
		const unsigned k = (rng >> 16) % SHARED;

		Obj *o = s_pool.allocate(id);
		if (!o || !claim(o))
		{
			retro_atomic_inc_int(&s_fails);
			return;
		}
		o->payload[1] = id;
		Obj *old = static_cast<Obj *>(retro_atomic_exchange_ptr(&s_shared[k], o));
		if (old)
		{
			release(old);
			s_pool.free(old);
		}

		uint8_t *b = s_slab.allocate();
		if (!b)
		{
			retro_atomic_inc_int(&s_fails);
			return;
		}
		uint32_t stamp = id ^ uint32_t(i);
		memcpy(b, &stamp, sizeof(stamp));
		uint8_t *oldb = static_cast<uint8_t *>(retro_atomic_exchange_ptr(&s_shared_slab[k], b));
		if (oldb)
			s_slab.free(oldb);
	}
}
}

/* Obj's constructor leaves payload alone: payload[0] is the in-use flag,
 * which must outlive a free and an allocate, or a slot handed to two
 * threads would just reset it. */
int main(int argc, char **argv)
{
	sthread_t *t[THREADS];
	int i;
	s_iters = argc > 1 ? atoi(argv[1]) : 200000;
	retro_atomic_int_init(&s_fails, 0);
	for (i = 0; i < SHARED; i++)
	{
		retro_atomic_ptr_init(&s_shared[i], nullptr);
		retro_atomic_ptr_init(&s_shared_slab[i], nullptr);
	}
	s_slab.init(24);

	/* Growth: LIVE objects at once, all distinct. */
	{
		static Obj *live[LIVE];
		int dup = 0;
		for (i = 0; i < LIVE; i++)
		{
			live[i] = s_pool.allocate(uint32_t(i));
			if (!live[i])
			{
				dup++;
				continue;
			}
			/* Fresh block memory: the flag starts here. The workers
			 * below never need more slots than these, so every slot
			 * they see has had its flag set clear. */
			retro_atomic_int_init(reinterpret_cast<retro_atomic_int_t *>(&live[i]->payload[0]), 0);
			if (!claim(live[i]))
				dup++;
		}
		for (i = 0; i < LIVE; i++)
			if (live[i] && live[i]->owner != uint32_t(i))
				dup++;
		for (i = 0; i < LIVE; i++)
			if (live[i])
			{
				release(live[i]);
				s_pool.free(live[i]);
			}
		if (dup)
		{
			printf("  FAIL: %d of %d live objects were not distinct\n", dup, LIVE);
			return 1;
		}
	}

	for (i = 0; i < THREADS; i++)
		t[i] = sthread_create(worker, (void *)uintptr_t(i + 1));
	for (i = 0; i < THREADS; i++)
		sthread_join(t[i]);

	for (i = 0; i < SHARED; i++)
	{
		Obj *o = static_cast<Obj *>(retro_atomic_load_acquire_ptr(&s_shared[i]));
		uint8_t *b = static_cast<uint8_t *>(retro_atomic_load_acquire_ptr(&s_shared_slab[i]));
		if (o)
		{
			release(o);
			s_pool.free(o);
		}
		if (b)
			s_slab.free(b);
	}

	if (retro_atomic_load_acquire_int(&s_fails))
	{
		printf("  FAIL: %d objects handed to two holders at once\n", retro_atomic_load_acquire_int(&s_fails));
		return 1;
	}
	printf("  ok: %d live objects distinct; %d threads x %d take/give, none handed out twice\n",
	       LIVE, THREADS, s_iters);
	return 0;
}
