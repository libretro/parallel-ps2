/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#pragma once

#include "VirtualMemory.h"
#include "vtlb.h"

#define PSM(mem)	(vtlb_GetPhyPtr((mem)&0x1fffffff)) /* pcsx2 is a competition.The one with most hacks wins :D */

#define psHu8(mem)	(*(u8 *)&eeHw[(mem) & 0xffff])
#define psHu16(mem)	(*(u16*)&eeHw[(mem) & 0xffff])
#define psHu32(mem)	(*(u32*)&eeHw[(mem) & 0xffff])
#define psHu64(mem)	(*(u64*)&eeHw[(mem) & 0xffff])
#define psHu128(mem)(*(u128*)&eeHw[(mem) & 0xffff])

#define psSu32(mem)	(*(u32 *)&eeMem->Scratch[(mem) & 0x3fff])
#define psSu64(mem)	(*(u64 *)&eeMem->Scratch[(mem) & 0x3fff])
#define psSu128(mem)	(*(u128*)&eeMem->Scratch[(mem) & 0x3fff])

#define memRead8 vtlb_memRead8
#define memRead16 vtlb_memRead16
#define memRead32 vtlb_memRead32
#define memRead64 vtlb_memRead64

#define memWrite8 vtlb_memWrite8
#define memWrite16 vtlb_memWrite16
#define memWrite32 vtlb_memWrite32
#define memWrite64 vtlb_memWrite64

/* This is a table of default virtual map addresses for ps2vm components.  These locations */
/* are provided and used to assist in debugging and possibly hacking; as it makes it possible */
/* for a programmer to know exactly where to look (consistently!) for the base address of */
/* the various virtual machine components.  These addresses can be keyed directly into the */
/* debugger's disasm window to get disassembly of recompiled code, and they can be used to help */
/* identify recompiled code addresses in the callstack. */

/* All of these areas should be reserved as soon as possible during program startup, and its */
/* important that none of the areas overlap.  In all but superVU's case, failure due to overlap */
/* or other conflict will result in the operating system picking a preferred address for the mapping. */

/* The host memory map: where each piece sits in the main and code maps. */
#define HOSTMEM_MAIN_SIZE           0x14000000
#define HOSTMEM_EEMEM_OFFSET        0x00000000 /* PS2 main memory, SPR and ROMs (~40.5MB, rounded up to 64MB) */
#define HOSTMEM_IOPMEM_OFFSET       0x04000000 /* IOP main memory and ROMs */
#define HOSTMEM_VUMEM_OFFSET        0x08000000 /* VU0 and VU1 memory */
#define HOSTMEM_BUMP_OFFSET         0x10000000 /* bump allocator: the rest of the main map */

#define HOSTMEM_CODE_SIZE           0x13100000 /* 305MB */
#define HOSTMEM_EEREC_OFFSET        0x00000000 /* EE recompiler, 64MB */
#define HOSTMEM_IOPREC_OFFSET       0x04000000 /* IOP recompiler, 32MB */
#define HOSTMEM_VIF0REC_OFFSET      0x06000000 /* newVif0 recompiler, 8MB */
#define HOSTMEM_VIF1REC_OFFSET      0x06800000 /* newVif1 recompiler, 8MB */
#define HOSTMEM_MVU0REC_OFFSET      0x07000000 /* microVU recompiler, 64MB */
#define HOSTMEM_MVU1REC_OFFSET      0x0B000000 /* microVU recompiler, 64MB */
#define HOSTMEM_VIFUNPACKREC_OFFSET 0x0F000000 /* SSE VIF unpack functions, 1MB */
#define HOSTMEM_SWREC_OFFSET        0x0F100000 /* software renderer JIT */
#define HOSTMEM_SWREC_SIZE          0x04000000

#ifdef __cplusplus
extern "C" {
#endif

/* The VM's memory: the main map (guest memory, shared-memory backed so
 * fastmem can alias it) and the code map the recompilers carve up. */
typedef struct SysMainMemory
{
	VirtualMemoryManager main;
	VirtualMemoryManager code;
	VirtualMemoryBumpAllocator bump;
} SysMainMemory;

/* Reserves both maps; the guest memory in them is carved out by
 * sys_memory_allocate. sys_memory_destroy undoes both. */
bool sys_memory_init(SysMainMemory* m);
void sys_memory_destroy(SysMainMemory* m);
bool sys_memory_allocate(SysMainMemory* m);
void sys_memory_reset(SysMainMemory* m);
void sys_memory_release(SysMainMemory* m);

extern SysMainMemory* GetVmMemory(void);

/* Binds the handlers that depend on configuration: the INTC_STAT
 * fast-forward and whether VU1 memory accesses go through the MTVU thread.
 * Called whenever either may have changed; resets keep the last binding. */
extern void memBindConditionalHandlers(bool intc_stat_hack, bool thread_vu1);

#ifdef __cplusplus
}
#endif

static __fi void memRead128(u32 mem, mem128_t* out)        { r128_store(out, vtlb_memRead128(mem)); }
#if PCSX2_MINGW_R128_BY_PTR
static __fi void memWrite128(u32 mem, const mem128_t* val) { vtlb_memWrite128(mem, (const r128*)(val)); }
#else
static __fi void memWrite128(u32 mem, const mem128_t* val) { vtlb_memWrite128(mem, r128_load(val)); }
#endif
