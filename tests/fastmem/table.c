/*  The fastmem backpatch table (pcsx2/vtlb.c), through its API.
 *
 *  The x86 EE recompiler registers every fastmem load and store it emits,
 *  so a fault on one can be patched to the slow path. The table used to
 *  realloc as sites came in -- an allocation in the middle of a game,
 *  every time the recompiler compiled past the last size -- and its
 *  faulting-PC list grew with fresh pages inside the fault handler. It is
 *  now reserved once with the code cache and never grows:
 *
 *    - registering sites up to the reserved size calls no allocator;
 *    - past it, vtlb_IsFaultingPC refuses the site, so the recompiler
 *      takes the slow path and every emitted site is in the table;
 *    - vtlb_LoadStoreInfoNearlyFull says when a block's worth is left,
 *      which is when the recompiler resets its cache;
 *    - a VM shutdown empties the table but keeps it, since the
 *      recompiler reserves it once per CPU thread, not per VM.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "vtlb.h"

/* Counted from the moment the table is reserved. */
static int s_allocs, s_counting;
void* __real_malloc(size_t n);
void* __real_realloc(void* p, size_t n);
void* __wrap_malloc(size_t n) { if (s_counting) s_allocs++; return __real_malloc(n); }
void* __wrap_realloc(void* p, size_t n) { if (s_counting) s_allocs++; return __real_realloc(p, n); }

static int s_fail;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fail = 1; } } while (0)

static void add(uptr code)
{
	vtlb_AddLoadStoreInfo(code, 5, 0x100000 + (u32)code, 0, 0, 1, 0, 32, false, true, false);
}

int main(void)
{
	const size_t cap = 64;
	size_t i;

	CHECK(vtlb_IsFaultingPC(0x1000), "with no table, every site takes the slow path");

	CHECK(vtlb_ReserveLoadStoreInfo(cap), "the table is reserved");
	s_counting = 1;

	CHECK(!vtlb_IsFaultingPC(0x1000), "an empty table takes sites");
	for (i = 0; i < cap - 8; i++)
		add(0x10000 + i * 16);
	CHECK(!vtlb_LoadStoreInfoNearlyFull(8), "8 left is not nearly full for 8");
	CHECK(vtlb_LoadStoreInfoNearlyFull(9), "8 left is nearly full for 9");
	for (; i < cap; i++)
		add(0x10000 + i * 16);
	CHECK(vtlb_IsFaultingPC(0x1000), "a full table refuses the next site");
	add(0x90000); /* would be the cap+1th: dropped, not grown */
	CHECK(s_allocs == 0, "registering sites calls no allocator");

	/* Re-registering a site replaces it rather than taking a slot. */
	vtlb_ClearLoadStoreInfo();
	add(0x10000);
	add(0x10000);
	CHECK(!vtlb_LoadStoreInfoNearlyFull(cap - 1), "a replaced site takes one slot");

	/* A VM shutdown empties the table and keeps it. */
	vtlb_Shutdown();
	CHECK(!vtlb_IsFaultingPC(0x1000), "after a VM shutdown the table still takes sites");
	for (i = 0; i < cap; i++)
		add(0x20000 + i * 16);
	CHECK(vtlb_IsFaultingPC(0x1000), "and fills to the same size");
	CHECK(s_allocs == 0, "still no allocator");

	s_counting = 0;
	vtlb_ReleaseLoadStoreInfo();
	CHECK(vtlb_IsFaultingPC(0x1000), "released, every site takes the slow path again");

	printf("fastmem table: %s\n", s_fail ? "FAIL" : "ok");
	return s_fail;
}
