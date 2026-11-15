/* The C list (GS/Renderers/Common/GSFastList.h) from C89: the element
 * layout for each size, growth keeping kept indexes, the ceiling, and
 * erase and move of indexes that are not. */
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

	printf(fails ? "fastlist c89: FAILED (%d)\n" : "fastlist c89: ok\n", fails);
	return fails != 0;
}
