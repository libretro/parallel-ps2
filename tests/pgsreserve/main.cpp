/* Granite's containers made ahead (util/insert_only_hash_map.hpp,
 * intrusive_hash_map.hpp, object_pool.hpp): after reserve(n), n entries
 * go in without allocating - the insert-only map's from four threads at
 * once - every one is found, entries past n still go in (allocating),
 * and everything is given back (the sanitizers see a leak). The
 * allocator is this test's: it counts. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <new>

#include <rthreads/rthreads.h>
#include <retro_atomic.h>

/* (libretro-common's aligned allocator, counted: Granite's goes through it) */
static retro_atomic_int_t s_allocs;
extern "C" void *memalign_alloc(size_t boundary, size_t size)
{
	void *p = nullptr;
	retro_atomic_fetch_add_int(&s_allocs, 1);
#ifdef _WIN32
	p = _aligned_malloc(size, boundary);
#else
	if (posix_memalign(&p, boundary < sizeof(void *) ? sizeof(void *) : boundary, size))
		p = nullptr;
#endif
	return p;
}
extern "C" void memalign_free(void *p)
{
#ifdef _WIN32
	_aligned_free(p);
#else
	free(p);
#endif
}
void *operator new(size_t n)
{
	void *p;
	retro_atomic_fetch_add_int(&s_allocs, 1);
	p = malloc(n ? n : 1);
	if (!p)
		abort();
	return p;
}
void operator delete(void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }

#include "insert_only_hash_map.hpp"
#include "intrusive_hash_map.hpp"
#include "object_pool.hpp"

static int fails;
#define CHECK(c) \
	do \
	{ \
		if (!(c)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			fails++; \
		} \
	} while (0)

static int allocs()
{
	return retro_atomic_load_acquire_int(&s_allocs);
}

struct Node : Util::IntrusiveHashMapEnabled<Node>
{
	explicit Node(uint64_t v_ = 0) : v(v_) {}
	uint64_t v;
	char pad[40];
};

/* (a hash that is not the key: entries collide and probe) */
static uint64_t mix(uint64_t k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdull;
	k ^= k >> 33;
	return k;
}

#define THREADS 4
#define PER 2000
static Util::InsertOnlyHashMap<Node> *s_map;

static void inserter(void *arg)
{
	const uint64_t t = (uint64_t)(uintptr_t)arg;
	uint64_t i;
	for (i = 0; i < PER; i++)
	{
		const uint64_t key = t * PER + i;
		Node *n = s_map->emplace_yield(mix(key), key);
		if (!n || n->v != key)
			fails++;
	}
}

int main()
{
	int before;
	uint64_t k;

	retro_atomic_int_init(&s_allocs, 0);

	/* the insert-only map: four threads, THREADS * PER entries reserved */
	{
		Util::InsertOnlyHashMap<Node> m;
		sthread_t *th[THREADS];
		unsigned i;
		m.reserve(THREADS * PER);
		s_map  = &m;
		before = allocs();
		for (i = 0; i < THREADS; i++)
			th[i] = sthread_create(inserter, (void *)(uintptr_t)i);
		/* (the threads themselves allocate: count from after they run) */
		for (i = 0; i < THREADS; i++)
			sthread_join(th[i]);
		for (k = 0; k < THREADS * PER; k++)
		{
			Node *n = m.find(mix(k));
			CHECK(n && n->v == k);
		}
		/* again, single threaded, to count what inserting alone costs */
	}
	{
		Util::InsertOnlyHashMap<Node> m;
		m.reserve(3000);
		before = allocs();
		for (k = 0; k < 3000; k++)
			CHECK(m.emplace_yield(mix(k), k) != nullptr);
		CHECK(allocs() == before);
		/* (the same key again: the one there, nothing more) */
		CHECK(m.emplace_yield(mix(7), 7)->v == 7);
		/* past the room: still in, allocating */
		for (k = 3000; k < 5000; k++)
			CHECK(m.emplace_yield(mix(k), k) != nullptr);
		CHECK(allocs() > before);
		for (k = 0; k < 5000; k++)
			CHECK(m.find(mix(k)) && m.find(mix(k))->v == k);
		m.reserve(10); /* (once only: a second does nothing) */
	}

	/* the intrusive map: n in, out and in again */
	{
		Util::IntrusiveHashMap<Node> m;
		m.reserve(1000);
		before = allocs();
		for (k = 0; k < 1000; k++)
			m.emplace_yield(mix(k), k);
		for (k = 0; k < 1000; k += 2)
			m.erase(mix(k));
		for (k = 0; k < 1000; k += 2)
			m.emplace_yield(mix(k), k);
		CHECK(allocs() == before);
		for (k = 0; k < 1000; k++)
			CHECK(m.find(mix(k)) && m.find(mix(k))->v == k);
		m.clear();
		for (k = 0; k < 1000; k++)
			m.emplace_yield(mix(k + 5000), k);
		CHECK(allocs() == before);
	}

	/* the object pool */
	{
		Util::ObjectPool<Node> pool;
		static Node *held[2000];
		int i;
		pool.reserve(2000);
		before = allocs();
		for (i = 0; i < 2000; i++)
			held[i] = pool.allocate((uint64_t)i);
		for (i = 0; i < 2000; i++)
			pool.free(held[i]);
		for (i = 0; i < 2000; i++)
			held[i] = pool.allocate((uint64_t)i);
		CHECK(allocs() == before);
		for (i = 0; i < 2000; i++)
			pool.free(held[i]);
	}

	if (fails)
	{
		printf("pgsreserve: %d failed\n", fails);
		return 1;
	}
	printf("pgsreserve: ok\n");
	return 0;
}
