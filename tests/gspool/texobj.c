/* The texture object pools (gs_texture_objects_reserve and friends, in
 * GS/Renderers/HW/GSObjectPool.c): after the reserve, every renderer's
 * texture and download texture objects come out of them, zeroed, until
 * they are full; past that, or too big for a slot, from the allocator,
 * and each goes back where it came from. And a node pool's reserve: one
 * slab of the nodes asked for, taken without a refill. */
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/HW/GSObjectPool.h"

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

static void* held[GS_TEXTURE_OBJECTS + 1];

static int zeroed(const unsigned char* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		if (p[i])
			return 0;
	return 1;
}

/* count objects of size bytes (a multiple of 64, so the slots are back to
 * back) fill exactly one block of slots, and one more is elsewhere */
static void check_pool(int download, int count, size_t size)
{
	unsigned char *lo, *hi, *past;
	int i;
	for (i = 0; i <= count; i++)
	{
		held[i] = gs_texture_object_take(download, size);
		CHECK(held[i] != NULL);
	}
#if !GS_OBJECT_POOL_SANITIZED
	lo = hi = (unsigned char*)held[0];
	for (i = 1; i < count; i++)
	{
		if ((unsigned char*)held[i] < lo)
			lo = (unsigned char*)held[i];
		if ((unsigned char*)held[i] > hi)
			hi = (unsigned char*)held[i];
	}
	CHECK((size_t)(hi - lo) == (size_t)(count - 1) * size);
	past = (unsigned char*)held[count];
	CHECK(past < lo || past > hi);
#else
	(void)lo; (void)hi; (void)past;
#endif
	for (i = 0; i <= count; i++)
		gs_texture_object_give(download, held[i]);
}

static void* nodes[1024];

static void check_node_reserve(void)
{
	static gs_node_pool_t fresh = GS_NODE_POOL_INIT(88, 16);
	static gs_node_pool_t used  = GS_NODE_POOL_INIT(40, 16);
	unsigned char *lo, *hi;
	void* slabs;
	int i, j;

	/* a fresh pool: one slab, every node of it in address order */
	gs_node_pool_reserve(&fresh, 200);
	CHECK(fresh.capacity == 200);
	slabs = fresh.slabs;
	gs_node_pool_reserve(&fresh, 150); /* (holds that many already) */
	CHECK(fresh.capacity == 200 && fresh.slabs == slabs);
	lo = hi = NULL;
	for (i = 0; i < 200; i++)
	{
		nodes[i] = gs_node_pool_take(&fresh);
		if (!lo || (unsigned char*)nodes[i] < lo)
			lo = (unsigned char*)nodes[i];
		if (!hi || (unsigned char*)nodes[i] > hi)
			hi = (unsigned char*)nodes[i];
	}
	CHECK(fresh.slabs == slabs && fresh.capacity == 200);
	CHECK((size_t)(hi - lo) == 199 * fresh.stride);
	for (i = 0; i < 200; i++)
		gs_node_pool_give(&fresh, nodes[i]);

	/* one part used: the rest of what is asked goes on the free list,
	 * every node distinct, none past it refilled */
	for (i = 0; i < 3; i++)
		nodes[i] = gs_node_pool_take(&used);
	j = (int)used.capacity;
	gs_node_pool_reserve(&used, used.capacity + 100);
	CHECK((int)used.capacity == j + 100);
	slabs = used.slabs;
	j = (int)used.capacity;
	CHECK(j <= 1024);
	if (j > 1024)
		return;
	for (i = 3; i < j; i++)
		nodes[i] = gs_node_pool_take(&used);
	CHECK(used.slabs == slabs && (int)used.capacity == j);
	for (i = 1; i < j; i++)
		CHECK(nodes[i] != nodes[i - 1] && nodes[i] != nodes[0]);
	for (i = 0; i < j; i++)
		gs_node_pool_give(&used, nodes[i]);
	gs_node_pools_release();
	CHECK(fresh.slabs == NULL && used.slabs == NULL);
}

int main(void)
{
	unsigned char *a, *b, *big, *d;

	/* before the reserve: the allocator, given back to it */
	a = (unsigned char*)gs_texture_object_take(0, 100);
	CHECK(a && zeroed(a, 100));
	gs_texture_object_give(0, a);

	gs_texture_objects_reserve();
	gs_texture_objects_reserve(); /* (the second does nothing) */

	/* a slot, zeroed, and zeroed again when it is taken back dirty */
	a = (unsigned char*)gs_texture_object_take(0, GS_TEXTURE_OBJECT_BYTES);
	CHECK(a && zeroed(a, GS_TEXTURE_OBJECT_BYTES));
	memset(a, 0xA5, GS_TEXTURE_OBJECT_BYTES);
	gs_texture_object_give(0, a);
	b = (unsigned char*)gs_texture_object_take(0, GS_TEXTURE_OBJECT_BYTES);
	CHECK(b && zeroed(b, GS_TEXTURE_OBJECT_BYTES));
#if !GS_OBJECT_POOL_SANITIZED
	CHECK(b == a);
#endif
	gs_texture_object_give(0, b);

	/* too big for a slot: the allocator */
	big = (unsigned char*)gs_texture_object_take(0, GS_TEXTURE_OBJECT_BYTES + 1);
	CHECK(big && zeroed(big, GS_TEXTURE_OBJECT_BYTES + 1));
	gs_texture_object_give(0, big);

	/* downloads have their own */
	d = (unsigned char*)gs_texture_object_take(1, GS_DOWNLOAD_OBJECT_BYTES);
	CHECK(d && zeroed(d, GS_DOWNLOAD_OBJECT_BYTES));
	memset(d, 0x5A, GS_DOWNLOAD_OBJECT_BYTES);
	gs_texture_object_give(1, d);
	d = (unsigned char*)gs_texture_object_take(1, GS_DOWNLOAD_OBJECT_BYTES);
	CHECK(d && zeroed(d, GS_DOWNLOAD_OBJECT_BYTES));
	gs_texture_object_give(1, d);

	/* each whole pool, then one past it from the allocator */
	check_pool(0, GS_TEXTURE_OBJECTS, GS_TEXTURE_OBJECT_BYTES);
	check_pool(1, GS_DOWNLOAD_OBJECTS, GS_DOWNLOAD_OBJECT_BYTES);
	gs_texture_object_give(0, NULL);

	check_node_reserve();

	if (fails)
	{
		printf("gspool texture objects: %d failed\n", fails);
		return 1;
	}
	printf("gspool texture objects: ok\n");
	return 0;
}
