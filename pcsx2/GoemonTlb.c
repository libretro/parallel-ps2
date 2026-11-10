/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* The Goemon TLB gamefix. Goemon keeps its own cache of the TLB entries
 * it would otherwise take misses on; the interpreter's JAL and JR hooks
 * (Interpreter.c) map and unmap from that table directly. */

#include "Common.h"
#include "vtlb.h"

#define GOEMON_TLB_ENTRIES 150

/* The table's address in the final JPN release and the June 22 and
 * August 26 prototypes; NULL when this is none of them. */
static GoemonTlb* FindGoemonTlbCacheAddress(void)
{
	static const u32 tlbAddrs[] = { 0x3d5580, 0x3db400, 0x3dcd80 };
	size_t i;
	for (i = 0; i < sizeof(tlbAddrs) / sizeof(tlbAddrs[0]); i++)
	{
		GoemonTlb* testTlb = (GoemonTlb*)&eeMem->Main[tlbAddrs[i]];
		if (testTlb[0].valid < 2)
			return testTlb;
	}
	return NULL;
}

void GoemonPreloadTlb(void)
{
	GoemonTlb* tlb = FindGoemonTlbCacheAddress();
	u32 i;

	if (!tlb)
		return;

	for (i = 0; i < GOEMON_TLB_ENTRIES; i++)
	{
		if (tlb[i].valid == 0x1 && tlb[i].low_add != tlb[i].high_add)
		{
			const u32 size  = tlb[i].high_add - tlb[i].low_add;
			const u32 vaddr = tlb[i].low_add;
			const u32 paddr = tlb[i].physical_add;
			/* Only pages still on handler 0, the unmapped one. */
			const vtlb_virt_t vmv = vtlbdata.vmap[vaddr >> VTLB_PAGE_BITS];
			if (vtlb_virt_is_handler(vmv, vaddr) && vtlb_virt_id(vmv) == 0)
			{
				vtlb_VMap(vaddr, paddr, size);
				vtlb_VMap(0x20000000 | vaddr, paddr, size);
			}
		}
	}
}

void GoemonUnloadTlb(u32 key)
{
	GoemonTlb* tlb = FindGoemonTlbCacheAddress();
	u32 i;

	if (!tlb)
		return;

	for (i = 0; i < GOEMON_TLB_ENTRIES; i++)
	{
		if (tlb[i].key == key && tlb[i].valid == 0x1)
		{
			const u32 size  = tlb[i].high_add - tlb[i].low_add;
			const u32 vaddr = tlb[i].low_add;

			vtlb_VMapUnmap(vaddr, size);
			vtlb_VMapUnmap(0x20000000 | vaddr, size);

			/* Unmap the entry in the game's table, which fills unused
			 * fields with FEFEFEFE. */
			tlb[i].valid    = 0;
			tlb[i].key      = 0xFEFEFEFE;
			tlb[i].low_add  = 0xFEFEFEFE;
			tlb[i].high_add = 0xFEFEFEFE;
		}
	}
}
