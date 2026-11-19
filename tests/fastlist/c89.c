/* The C list (GS/Renderers/Common/GSFastList.h) from C89: the element
 * layout for each size, growth keeping kept indexes, the ceiling, and
 * erase and move of indexes that are not; and lists on a heap made at
 * start, which take their arrays from it and give them back. */
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSFastList.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

int main(void)
{
	gs_fast_list l;
	u16 idx[100];
	u32 v, i, n;
	u16 at;

	gs_fast_list_init(&l, 1);
	CHECK(l.stride == 8 && l.link == 4, "1 byte: 8 a stride");
	gs_fast_list_init(&l, 8);
	CHECK(l.stride == 16 && l.link == 12, "8 bytes: 16 a stride");
	gs_fast_list_init(&l, 24);
	CHECK(l.stride == 32 && l.link == 28, "24 bytes: 32 a stride");

	gs_fast_list_init(&l, sizeof(u32));
	CHECK(!l.buf && gs_fast_list_first(&l) == 0 && gs_fast_list_last(&l) == 0, "fresh: nothing, no array");
	for (v = 0; v < 3; v++)
		idx[v] = gs_fast_list_insert_front(&l, &v);
	CHECK(l.capacity == 4, "3 in: the first array, 4");
	/* (an index one past the array: ignored, not read through) */
	gs_fast_list_erase(&l, l.capacity);
	gs_fast_list_move_front(&l, l.capacity);
	CHECK(l.count == 3 && *(u32*)gs_fast_list_data(&l, gs_fast_list_first(&l)) == 2, "index == capacity: ignored");
	for (v = 3; v < 100; v++)
		idx[v] = gs_fast_list_insert_front(&l, &v);
	CHECK(l.count == 100 && l.capacity == 128, "100 in: grown to 128");
	for (v = 0; v < 100; v++)
		CHECK(*(u32*)gs_fast_list_data(&l, idx[v]) == v, "a kept index names its element after growth");
	gs_fast_list_move_front(&l, idx[0]);
	CHECK(gs_fast_list_first(&l) == idx[0], "moved to the front");
	CHECK(*(u32*)gs_fast_list_data(&l, gs_fast_list_last(&l)) == 1, "the oldest is now 1");
	n = 0;
	for (at = gs_fast_list_first(&l); at; at = gs_fast_list_next(&l, at))
		n++;
	CHECK(n == 100, "walks 100");
	for (v = 0; v < 100; v += 2)
		gs_fast_list_erase(&l, idx[v]);
	CHECK(l.count == 50, "every other erased");
	gs_fast_list_clear(&l);
	for (v = 0; v < 100; v++)
	{
		gs_fast_list_erase(&l, idx[v]);
		gs_fast_list_move_front(&l, idx[v]);
	}
	CHECK(l.count == 0 && !l.buf, "stale indexes on a cleared list: nothing");

	for (i = 0; i < 65534; i++)
		if (!gs_fast_list_insert_front(&l, &i))
			break;
	CHECK(i == 65534 && l.capacity == 65535, "65534 held in 65535");
	CHECK(gs_fast_list_insert_front(&l, &i) == 0 && l.count == 65534, "and no more");
	gs_fast_list_clear(&l);

	/* Lists on a heap: their arrays from it, a list's given back for
	 * another to grow into, the heap's end then the allocator, and what
	 * the allocator gave going back to it, not into the heap. */
	{
		gs_fast_list_heap h;
		gs_fast_list a, b2, c;
		unsigned char* first;
		unsigned char* grown;
		CHECK(gs_fast_list_heap_init(&h, 4096), "a heap of 4096 bytes");
		gs_fast_list_init(&a, sizeof(u32));
		gs_fast_list_init(&b2, sizeof(u32));
		gs_fast_list_init(&c, sizeof(u32));
		gs_fast_list_use_heap(&a, &h);
		gs_fast_list_use_heap(&b2, &h);
		gs_fast_list_use_heap(&c, &h);
		v = 7;
		gs_fast_list_insert_front(&a, &v);
		first = a.buf;
		CHECK(first == h.base && h.used == 64, "a's first array: the heap's first 64 bytes");
		for (v = 0; v < 20; v++)
			idx[v] = gs_fast_list_insert_front(&a, &v);
		CHECK(a.capacity == 32 && a.buf >= h.base && a.buf < h.base + h.size, "a grown to 32, in the heap");
		for (v = 0; v < 20; v++)
			CHECK(*(u32*)gs_fast_list_data(&a, idx[v]) == v, "a grown in the heap keeps its elements");
		/* (b's first array: the 64 bytes a gave back when it grew) */
		gs_fast_list_insert_front(&b2, &v);
		CHECK(b2.buf == first, "an array given back is the next of its size");
		grown = a.buf;
		gs_fast_list_clear(&a);
		for (v = 0; v < 21; v++)
			gs_fast_list_insert_front(&a, &v);
		CHECK(a.buf == grown, "a cleared and grown again: the same array, no more of the heap");
		n = (u32)h.used;
		for (v = 0; v < 2000; v++)
			if (!gs_fast_list_insert_front(&c, &v))
				break;
		CHECK(v == 2000 && h.spilled > 0, "past the heap's end: the allocator");
		CHECK(c.buf < h.base || c.buf >= h.base + h.size, "a list that outgrew the heap: its array from the allocator");
		CHECK(h.used <= h.size && h.used >= n, "the heap's end not passed");
		gs_fast_list_clear(&c);
		gs_fast_list_clear(&a);
		gs_fast_list_clear(&b2);
		gs_fast_list_heap_free(&h);
	}

	printf(fails ? "fastlist c89: FAILED (%d)\n" : "fastlist c89: ok\n", fails);
	return fails != 0;
}
