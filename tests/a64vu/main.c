/* The arm64 microVU's C containers and register allocator, on any host:
 * the VI write-backup the allocator emits once per instruction, the flat
 * range list and the block manager's two lists. */
#include <stdio.h>
#include <string.h>

#include "arm64/aVU_IR.h"

PCSX2_ALIGN(32) microVU microVU0;
PCSX2_ALIGN(32) microVU microVU1;
Pcsx2Config EmuConfig;
retro_log_printf_t log_cb;
void memjit_write_begin(void) { }
void memjit_write_end(void) { }
int memsync(void *start, void *end) { (void)start; (void)end; return 0; }

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); fails++; } } while (0)

static u32 s_code[4096];

/* str wN, [x17] -- the store mVUra_writeVIBackup ends with */
static int count_backups(const u8* start, const u8* end)
{
	const u32* w;
	int n = 0;
	for (w = (const u32*)start; w < (const u32*)end; w++)
		if ((*w & 0xFFFFFFE0u) == 0xB9000220u)
			n++;
	return n;
}

/* One allocGPR(load, write, backup) on a fresh allocator, after caching
 * the VIs in pre[] (dirty), and the backups it emitted. */
static int backups_for(const int* pre, int npre, int load, int write)
{
	microRegAlloc ra;
	u8* start;
	u8* end;
	int i;
	mVUra_init(&ra, 0);
	armSetAsmPtr(s_code, sizeof(s_code));
	armStartBlock();
	for (i = 0; i < npre; i++)
		mVUra_clearNeeded(&ra, mVUra_allocGPR(&ra, pre[i], pre[i], false, false));
	start = a64_cursor(armAsm);
	mVUra_allocGPR(&ra, load, write, true, false);
	end = a64_cursor(armAsm);
	armEndBlock();
	return count_backups(start, end);
}

static void test_alloc(void)
{
	static const int both[2] = { 1, 2 };
	static const int dst[1]  = { 2 };
	printf("== allocator: one VI backup per write ==\n");
	CHECK(backups_for(both, 2, 1, 2) == 1, "load cached, write cached: one backup");
	CHECK(backups_for(dst, 1, 1, 2) == 1, "write cached only: one backup");
	CHECK(backups_for(NULL, 0, 1, 2) == 1, "nothing cached: one backup");
	CHECK(backups_for(dst, 1, 2, 2) == 1, "load == write: one backup");
}

static void test_ranges(void)
{
	microRangeList* l = mvu_rangelist_new();
	int i, ok = 1;
	printf("== range list ==\n");
	for (i = 0; i < 20; i++)
	{
		microRange r;
		r.start = i;
		r.end   = i + 100;
		mvu_rangelist_push_front(l, r);
	}
	CHECK(l->count == 20 && l->capacity >= 20, "grows past its first block");
	for (i = 0; i < 20; i++)
		ok &= l->data[i].start == 19 - i && l->data[i].end == 119 - i;
	CHECK(ok, "push_front keeps newest first");
	mvu_rangelist_erase(l, 5);
	CHECK(l->count == 19 && l->data[4].start == 15 && l->data[5].start == 13, "erase closes the gap");
	mvu_rangelist_erase(l, 18);
	CHECK(l->count == 18 && l->data[17].start == 1, "erase of the last one");
	mvu_rangelist_delete(l);
}

static void test_blocks(void)
{
	microBlockManager bm;
	microBlock b, e;
	microBlock *q1, *q2, *e1, *e2;
	u32 cap;
	printf("== block manager ==\n");
	mVUbm_init(&bm);
	memset(&b, 0, sizeof(b));
	memset(&e, 0, sizeof(e));
	b.pState.q = 1;
	b.pState.flagInfo = 0x10;
	q1 = mVUbm_add(&bm, &microVU0, &b);
	q2 = mVUbm_add(&bm, &microVU0, &b);
	CHECK(q1 && q1 == q2 && bm.qListI == 1 && bm.quickCount == 1, "a quick state is added once");
	CHECK(mVUbm_search(&bm, &microVU0, &b.pState) == q1, "quick search finds it");
	b.pState.q = 2;
	CHECK(mVUbm_search(&bm, &microVU0, &b.pState) == NULL, "a different quick state misses");

	e.pState.needExactMatch = 1;
	e.pState.xgkickcycles = 7;
	e1 = mVUbm_add(&bm, &microVU0, &e);
	e.pState.xgkickcycles = 8;
	e2 = mVUbm_add(&bm, &microVU0, &e);
	CHECK(e1 != e2 && bm.fListI == 2, "exact states differ past the quick word");
	e.pState.xgkickcycles = 7;
	CHECK(mVUbm_search(&bm, &microVU0, &e.pState) == e1, "exact search finds the first");
	e.pState.xgkickcycles = 8;
	CHECK(mVUbm_search(&bm, &microVU0, &e.pState) == e2, "and the second");
	CHECK(bm.fBlockList && &bm.fBlockList->block == e2 && bm.fBlockList->next && &bm.fBlockList->next->block == e1 && !bm.fBlockList->next->next, "an exact hit moves to the front");

	cap = bm.quickCapacity;
	mVUbm_reset(&bm);
	CHECK(bm.qListI == 0 && bm.fListI == 0 && bm.quickCount == 0 && !bm.qBlockList && !bm.fBlockList, "reset empties both lists");
	CHECK(bm.quickCapacity == cap && bm.quickLookup, "reset keeps the lookup for reuse");
	mVUbm_destroy(&bm);
}

int main(void)
{
	test_alloc();
	test_ranges();
	test_blocks();
	if (fails)
	{
		printf("FAIL: %d\n", fails);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
