/*  The fastmem reverse map (pcsx2/vtlb.c), through vtlb_VMap.
 *
 *  Fastmem keeps, per main-memory page, the guest virtual pages mapped
 *  onto it, so that write-protecting a RAM page for self-modifying-code
 *  detection protects its fastmem views too. Remapping a virtual page
 *  from one RAM page to another dropped the view under the NEW page
 *  instead of the old one: the old page kept a view that now showed
 *  different RAM. Protecting the old page then write-protected that
 *  view, and the first fastmem store through it faulted with nothing to
 *  explain it; every remap also leaked a node, which is why the node
 *  pool kept growing.
 *
 *  Checked, by recording the protections vtlb asks for: after a remap
 *  from A to B, protecting A leaves the view alone, protecting B
 *  protects it, and remapping back and forth keeps exactly one view.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "vtlb.h"
#include "HotConfig.h"

#define FAKE_FASTMEM_BASE ((uptr)0x700000000000ull)

/* Every protection vtlb asks for in the fastmem window, by page. */
static int s_window_protects[64];
int __real_mprotect(void* addr, size_t len, int prot);
int __wrap_mprotect(void* addr, size_t len, int prot)
{
	const uptr a = (uptr)addr;
	(void)len; (void)prot;
	if (a >= FAKE_FASTMEM_BASE && a < FAKE_FASTMEM_BASE + 64 * VTLB_PAGE_SIZE)
		s_window_protects[(a - FAKE_FASTMEM_BASE) / VTLB_PAGE_SIZE]++;
	return 0; /* nothing here is really protected */
}

static int s_fail;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fail = 1; } } while (0)

int main(void)
{
	/* A guest virtual page, and two RAM pages it is mapped onto in turn. */
	const u32 vpage = 5;
	const u32 vaddr = vpage * VTLB_PAGE_SIZE;
	const u32 ram_a = 0x00200000, ram_b = 0x00300000;
	int i;

	eeMem  = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	iopMem = (IopVM_MemoryAllocMess*)calloc(1, sizeof(*iopMem));
	if (!eeMem || !iopMem)
		return 2;
	emu_hot = EMU_HOT_FASTMEM | EMU_HOT_EEREC;

	if (!vtlb_Core_Alloc() || vtlbdata.fastmem_base != FAKE_FASTMEM_BASE)
	{
		printf("  FAIL: the fastmem window was not set up\n");
		return 1;
	}
	vtlb_MapBlock(eeMem->Main, 0, PS2MEM_MAIN_RAM, 0);

	vtlb_VMap(vaddr, ram_a, VTLB_PAGE_SIZE);
	vtlb_VMap(vaddr, ram_b, VTLB_PAGE_SIZE);

	memset(s_window_protects, 0, sizeof(s_window_protects));
	mmap_MarkCountedRamPage(ram_a);
	CHECK(s_window_protects[vpage] == 0, "protecting the page left behind leaves the view alone");

	memset(s_window_protects, 0, sizeof(s_window_protects));
	mmap_MarkCountedRamPage(ram_b);
	CHECK(s_window_protects[vpage] == 1, "protecting the page the view shows protects it");

	/* Back and forth: one view at any time, never an accumulation. */
	mmap_ResetBlockTracking();
	for (i = 0; i < 1000; i++)
		vtlb_VMap(vaddr, (i & 1) ? ram_b : ram_a, VTLB_PAGE_SIZE);
	memset(s_window_protects, 0, sizeof(s_window_protects));
	mmap_MarkCountedRamPage(ram_a);
	mmap_MarkCountedRamPage(ram_b);
	CHECK(s_window_protects[vpage] == 1, "after many remaps the view is protected once");

	printf("fastmem rmap: %s\n", s_fail ? "FAIL" : "ok");
	return s_fail;
}
