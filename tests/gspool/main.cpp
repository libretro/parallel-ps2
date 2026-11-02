/* The texture cache's object pool (GS/Renderers/HW/GSObjectPool.c), the
 * real one: slots handed out once each, aligned, not overlapping,
 * returned and reused, and NULL past the pool's size so the caller can
 * fall back. */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include "GS/Renderers/HW/GSObjectPool.h"

#define POOL 384
static gs_object_pool_t pool_obj;
static void* take(size_t size)
{
	void* p;
	if (!pool_obj.slots)
		gs_object_pool_init(&pool_obj, size, POOL);
	p = gs_object_pool_take(&pool_obj, size);
	return p ? p : memalign_alloc(32, size);
}
static void give(void* p)
{
	if (p && !gs_object_pool_give(&pool_obj, p))
		memalign_free(p);
}

static int fails;
#define CHECK(c,m) do{ if(!(c)){ printf("  FAIL: %s\n", m); fails++; } }while(0)
int main()
{
	const size_t sz = 3472;
	std::set<void*> live;
	void* p[POOL+16];
	int i, pass;
	for (pass = 0; pass < 3; pass++)
	{
		live.clear();
		for (i = 0; i < POOL; i++)
		{
			p[i] = take(sz);
			CHECK(p[i] != NULL, "pool slot handed out");
			CHECK(((uintptr_t)p[i] & 31) == 0, "slot is 32-byte aligned");
			CHECK(live.insert(p[i]).second, "slot not handed out twice");
			memset(p[i], 0xA5, sz);           /* the whole slot is ours */
		}
		/* Past the pool: still works, from the allocator. */
		for (i = 0; i < 16; i++)
		{
			p[POOL + i] = take(sz);
			CHECK(p[POOL + i] != NULL, "overflow allocation");
			CHECK(live.insert(p[POOL + i]).second, "overflow is distinct");
			memset(p[POOL + i], 0x5A, sz);
		}
		for (i = POOL + 15; i >= 0; i--)
			give(p[i]);
	}
	/* Slots do not overlap: write a marker in each, then check them all. */
	for (i = 0; i < POOL; i++) { p[i] = take(sz); memset(p[i], i & 0xff, sz); }
	for (i = 0; i < POOL; i++)
	{
		unsigned char* q = (unsigned char*)p[i];
		CHECK(q[0] == (i & 0xff) && q[sz - 1] == (i & 0xff), "slot contents intact");
	}
	for (i = 0; i < POOL; i++) give(p[i]);
	/* A second free of the same object must not put the slot on the free
	 * list twice: that hands one slot to two callers, and a live object
	 * gets overwritten by an unrelated one - which shows up as the same
	 * address being destroyed over and over, and then a lockup.
	 *
	 * paraLLEl-GS cannot have this bug, because it owns images through a
	 * refcounted handle and a second free is not something a call site
	 * can express. Sources and targets here are raw pointers.
	 *
	 * Not with an address sanitizer: the pool hands out nothing then,
	 * so that the sanitizer sees every object, and this checked takes
	 * that could not succeed. */
#if !GS_OBJECT_POOL_SANITIZED
	{
		gs_object_pool_t dbl;
		void* x;
		void* y;
		void* z;
		memset(&dbl, 0, sizeof(dbl));
		gs_object_pool_init(&dbl, 64, 4);
		x = gs_object_pool_take(&dbl, 64);
		CHECK(x != NULL, "took one");
		CHECK(gs_object_pool_give(&dbl, x) == 1, "gave it back");
		CHECK(gs_object_pool_give(&dbl, x) == 1, "gave it back twice, which is a caller bug");
		CHECK(dbl.double_frees == 1, "and the pool counted it rather than corrupting itself");
		y = gs_object_pool_take(&dbl, 64);
		z = gs_object_pool_take(&dbl, 64);
		CHECK(y != z, "two takes never hand out one slot");
		gs_object_pool_free(&dbl);
	}
#endif

	/* Node pools (GSNodeAllocator's): nodes come out aligned, distinct
	 * and not overlapping, across more than one slab; given back they
	 * are taken again; and release frees the slabs only once every node
	 * is back, so a node still in a map is never freed under it. */
	{
		static gs_node_pool_t np = GS_NODE_POOL_INIT(88, 8);
		static gs_node_pool_t wide = GS_NODE_POOL_INIT(40, 64);
		enum { NODES = 1000 };
		static unsigned char* n[NODES];
		int j;
		CHECK(np.stride == 88, "an 88-byte node keeps its size");
		CHECK(wide.stride == 64, "a 64-aligned node rounds up to 64");
		for (i = 0; i < NODES; i++)
		{
			n[i] = (unsigned char*)gs_node_pool_take(&np);
			CHECK(n[i] && ((size_t)n[i] & 7) == 0, "node aligned");
			memset(n[i], i & 0xff, 88);
		}
		CHECK(np.capacity >= NODES && np.slabs && *(void**)np.slabs, "more than one slab");
		for (i = 0; i < NODES; i++)
		{
			CHECK(n[i][0] == (i & 0xff) && n[i][87] == (i & 0xff), "nodes do not overlap");
			for (j = 0; j < i && j < 8; j++)
				CHECK(n[i] != n[i - 1 - j], "nodes are distinct");
		}
		for (i = 0; i < 64; i++)
		{
			unsigned char* w = (unsigned char*)gs_node_pool_take(&wide);
			CHECK(((size_t)w & 63) == 0, "64-aligned node aligned");
			gs_node_pool_give(&wide, w);
		}
		gs_node_pool_give(&np, n[5]);
		CHECK(gs_node_pool_take(&np) == n[5], "a node given back is the next taken");
		for (i = 1; i < NODES; i++)
			gs_node_pool_give(&np, n[i]);
		gs_node_pools_release();
		CHECK(np.slabs != NULL, "release keeps the slabs while a node is out");
		CHECK(n[0][0] == 0 && n[0][87] == 0, "and that node is untouched");
		gs_node_pool_give(&np, n[0]);
		gs_node_pools_release();
		CHECK(np.slabs == NULL && np.capacity == 0 && np.free_head == NULL, "release frees them once all are back");
		CHECK(wide.slabs == NULL, "every pool with all its nodes back is released");
		n[0] = (unsigned char*)gs_node_pool_take(&np);
		CHECK(n[0] != NULL && np.slabs != NULL, "a released pool refills");
		gs_node_pool_give(&np, n[0]);
		gs_node_pools_release();
	}

	printf(fails ? "pool: FAILED (%d)\n" : "pool: ok\n", fails);
	return fails != 0;
}
