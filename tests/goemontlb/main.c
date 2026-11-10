/*  Goemon TLB gamefix (pcsx2/GoemonTlb.c), driven against a fake EE RAM.
 *
 *  The fix looks for the game's TLB cache table at three addresses, one
 *  per release. The scan once ran sizeof(table) times, 12, over the three
 *  addresses, reading the stack past them and indexing EE RAM with what
 *  it found there; and when no table matched, both hooks dereferenced the
 *  NULL it returned. Any JAL or JR to the hooked addresses in a game that
 *  is not one of the three, with the gamefix on, could crash.
 *
 *  Checked: no table means no mapping and no crash; a table at each of
 *  the three addresses is found; preload maps an entry's range at both
 *  its address and the uncached mirror, only on unmapped pages; unload
 *  unmaps by key and marks the entry free the way the game does.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "vtlb.h"

EEVM_MemoryAllocMess* eeMem;
PCSX2_ALIGN(64) vtlb_map_t vtlbdata;

static int s_maps, s_unmaps;
static u32 s_last_vaddr, s_last_paddr, s_last_size;

void vtlb_VMap(u32 vaddr, u32 paddr, u32 size)
{
	s_maps++;
	s_last_vaddr = vaddr;
	s_last_paddr = paddr;
	s_last_size  = size;
}

void vtlb_VMapUnmap(u32 vaddr, u32 size)
{
	s_unmaps++;
	s_last_vaddr = vaddr;
	s_last_size  = size;
}

static int s_fail;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fail = 1; } } while (0)

static const u32 kTables[3] = { 0x3d5580, 0x3db400, 0x3dcd80 };

static void reset(void)
{
	/* Not a table: the scan takes valid >= 2 as "not here". */
	memset(eeMem->Main, 0xFE, sizeof(eeMem->Main));
	s_maps = s_unmaps = 0;
}

static GoemonTlb* table_at(int which)
{
	GoemonTlb* t = (GoemonTlb*)&eeMem->Main[kTables[which]];
	memset(t, 0, sizeof(GoemonTlb) * 150);
	return t;
}

int main(void)
{
	const u32 vaddr = 0x00500000, paddr = 0x00100000, size = 0x8000;
	int which;

	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	vtlbdata.vmap = (vtlb_virt_t*)calloc(VTLB_VMAP_ITEMS, sizeof(vtlb_virt_t));
	if (!eeMem || !vtlbdata.vmap)
		return 2;

	/* Every page unmapped: handler 0 at its own address. */
	{
		u32 i;
		for (i = 0; i < VTLB_VMAP_ITEMS; i++)
			vtlbdata.vmap[i] = vtlb_virt_make(vtlb_phys_from_handler(0),
				i << VTLB_PAGE_BITS, i << VTLB_PAGE_BITS);
	}

	reset();
	GoemonPreloadTlb();
	GoemonUnloadTlb(7);
	CHECK(s_maps == 0 && s_unmaps == 0, "no table: nothing mapped or unmapped");

	for (which = 0; which < 3; which++)
	{
		GoemonTlb* t;
		reset();
		t = table_at(which);
		t[5].valid        = 1;
		t[5].key          = 7;
		t[5].low_add      = vaddr;
		t[5].high_add     = vaddr + size;
		t[5].physical_add = paddr;

		GoemonPreloadTlb();
		CHECK(s_maps == 2, "preload maps the entry twice");
		CHECK(s_last_vaddr == (0x20000000 | vaddr) && s_last_paddr == paddr && s_last_size == size,
			"the second mapping is the uncached mirror");

		/* A page already mapped is left alone. */
		s_maps = 0;
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = vtlb_virt_from_ptr((uptr)eeMem->Main, vaddr);
		GoemonPreloadTlb();
		CHECK(s_maps == 0, "preload skips a mapped page");
		vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS] = vtlb_virt_make(vtlb_phys_from_handler(0), vaddr, vaddr);

		GoemonUnloadTlb(8);
		CHECK(s_unmaps == 0, "unload ignores another key");
		GoemonUnloadTlb(7);
		CHECK(s_unmaps == 2, "unload unmaps both ranges");
		CHECK(t[5].valid == 0 && t[5].key == 0xFEFEFEFE && t[5].low_add == 0xFEFEFEFE
			&& t[5].high_add == 0xFEFEFEFE, "unload frees the entry as the game does");
	}

	free(vtlbdata.vmap);
	free(eeMem);
	printf("goemontlb: %s\n", s_fail ? "FAIL" : "ok");
	return s_fail;
}
