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

/*

RAM
---
0x00100000-0x01ffffff this is the physical address for the ram.its cached there
0x20100000-0x21ffffff uncached
0x30100000-0x31ffffff uncached & accelerated
0xa0000000-0xa1ffffff MIRROR might...???
0x80000000-0x81ffffff MIRROR might... ????

scratch pad
----------
0x70000000-0x70003fff scratch pad

BIOS
----
0x1FC00000 - 0x1FFFFFFF un-cached
0x9FC00000 - 0x9FFFFFFF cached
0xBFC00000 - 0xBFFFFFFF un-cached
*/


/* C89. */
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "../common/Align.h"
#include "../common/SingleRegisterTypes.h"
#include "MemoryTypes.h"
#include "Memory.h"
#include "vtlb.h"
#include "Hw.h"
#include "IopHw.h"
#include "IopMem.h"
#include "R5900.h"
#include "VU.h"
#include "VUmicro.h"
#include "GSmmio.h"
#include "DEV9/DEV9mmio.h"
#include "CDVD/CDVDnvram.h"
#include "ps2/BiosTools.h"
#include "SPU2/spu2.h"
#include "NearPlace.h"

/* --------------------------------------------------------------------------
 *  Placement of the main and code maps
 * --------------------------------------------------------------------------
 * The recompilers reach this module's globals and, for constant-address
 * loads and stores, guest memory with RIP-relative or abs32 operands, and the
 * C89 emitter aborts on one it cannot encode. Every byte of the code map
 * therefore has to be within rel32 of every byte of the loaded image and of
 * the main map: all three are placed inside one RIP_LIMIT window (see
 * NearPlace.h), main only where the code still fits beside it. */
#define RIP_LIMIT     ((uptr)0x7C000000) /* 1.9375GB: 2GB less rip-bias/operand-tail headroom */
#define RESERVE_ALIGN ((uptr)0x10000)
/* VTLB marks a hardware-register handler with the pointer sign bit
 * (vtlb.cpp: "if ((sptr)p < 0)"), so guest RAM stays below it. */
#define MAIN_CEILING  ((uptr)1 << (sizeof(uptr) * 8 - 1))

/* Any object in the image will do as the point the image is found from. */
static const char s_image_anchor = 0;
static near_space_t s_space;

#if defined(__linux__) && defined(__x86_64__)
/* The last resort when no range within reach is free, as in a host that has
 * mapped a lot around the module: room for both maps in the module's own
 * .bss, which is loaded with the image and so always within reach. Until the
 * maps go over it, it is only reserved address space. */
static PCSX2_ALIGN(0x10000) u8 s_maps_in_image[HOSTMEM_MAIN_SIZE + HOSTMEM_CODE_SIZE];
#endif

static int place_map_main(void* ctx, uintptr_t base) { return vmm_map(&((SysMainMemory*)ctx)->main, base); }
static void place_unmap_main(void* ctx)              { vmm_unmap(&((SysMainMemory*)ctx)->main); }
static int place_map_code(void* ctx, uintptr_t base) { return vmm_map(&((SysMainMemory*)ctx)->code, base); }

static bool place_maps(SysMainMemory* m)
{
	near_pair_ops_t ops;
	near_range_t image;

	ops.map_main   = place_map_main;
	ops.unmap_main = place_unmap_main;
	ops.map_code   = place_map_code;
	ops.ctx        = m;
	near_image_extent(&s_image_anchor, &image);
	return near_place_pair(&s_space, &image, m->main.size, MAIN_CEILING, m->code.size,
			RIP_LIMIT, RESERVE_ALIGN, &ops) != 0;
}

static bool place_maps_in_image(SysMainMemory* m)
{
#if defined(__linux__) && defined(__x86_64__)
	if (m->main.size + m->code.size > sizeof(s_maps_in_image))
		return false;
	if (!vmm_map_in_image(&m->main, s_maps_in_image))
		return false;
	if (!vmm_map_in_image(&m->code, s_maps_in_image + m->main.size))
	{
		vmm_unmap(&m->main);
		return false;
	}
	log_cb(RETRO_LOG_WARN, "No free range within reach of the core for the %u MB main and %u MB code "
	                       "maps; they are in room kept in the core's own image (%p).\n",
	       (unsigned)(HOSTMEM_MAIN_SIZE >> 20), (unsigned)(HOSTMEM_CODE_SIZE >> 20),
	       (void*)s_maps_in_image);
	return true;
#else
	(void)m;
	return false;
#endif
}

/* --------------------------------------------------------------------------
 *  EE memory reserve
 * -------------------------------------------------------------------------- */
#define EE_MEMORY_RESERVE_SIZE pcsx2_page_align(sizeof(EEVM_MemoryAllocMess))

EEVM_MemoryAllocMess* eeMem = NULL;
PCSX2_ALIGN(__pagealignsize) u8 eeHw[PS2MEM_HARDWARE];

static void ee_memory_assign(const VirtualMemoryManager* main_map)
{
	eeMem = (EEVM_MemoryAllocMess*)vmm_alloc(main_map, HOSTMEM_EEMEM_OFFSET, EE_MEMORY_RESERVE_SIZE);
}

static void ee_memory_release(const VirtualMemoryManager* main_map)
{
	if (eeMem)
		vmm_free(main_map, eeMem, EE_MEMORY_RESERVE_SIZE);
	eeMem = NULL;
}

static void ee_memory_reset(void);

/* --------------------------------------------------------------------------
 *  SysMainMemory
 * -------------------------------------------------------------------------- */
bool sys_memory_init(SysMainMemory* m)
{
	if (!vmm_init(&m->main, "pcsx2", HOSTMEM_MAIN_SIZE) || !vmm_init(&m->code, NULL, HOSTMEM_CODE_SIZE))
	{
		log_cb(RETRO_LOG_ERROR, "Could not set up the host memory maps.\n");
		return false;
	}

	if (!place_maps(m) && !place_maps_in_image(m))
	{
		vmm_map(&m->main, 0);
		vmm_map(&m->code, 0);
		log_cb(RETRO_LOG_ERROR, "Could not place the %u MB main and %u MB code maps within reach "
		                        "of the core and each other (module near %p, main at %p, code at %p). "
		                        "The recompilers cannot address them from there.\n",
		       (unsigned)(HOSTMEM_MAIN_SIZE >> 20), (unsigned)(HOSTMEM_CODE_SIZE >> 20),
		       (const void*)&s_image_anchor, (void*)m->main.baseptr, (void*)m->code.baseptr);
	}

	vmm_bump_init(&m->bump, &m->main, HOSTMEM_BUMP_OFFSET, HOSTMEM_MAIN_SIZE - HOSTMEM_BUMP_OFFSET);
	return vmm_is_ok(&m->main) && vmm_is_ok(&m->code);
}

void sys_memory_destroy(SysMainMemory* m)
{
	sys_memory_release(m);
	vmm_destroy(&m->code);
	vmm_destroy(&m->main);
}

bool sys_memory_allocate(SysMainMemory* m)
{
	log_cb(RETRO_LOG_INFO, "Allocating host memory for virtual systems...\n");
	ee_memory_assign(&m->main);
	iop_memory_assign(&m->main);
	vu_memory_assign(&m->main);

	vtlb_Core_Alloc();

	return true;
}

void sys_memory_reset(SysMainMemory* m)
{
	(void)m;
	log_cb(RETRO_LOG_INFO, "Resetting host memory for virtual systems...\n");
	ee_memory_reset();
	iop_memory_reset();
	vu_memory_reset();

	/* newVif is reset as part of other VIF structures; software is reset
	 * on the GS thread. */
}

void sys_memory_release(SysMainMemory* m)
{
	log_cb(RETRO_LOG_INFO, "Releasing host memory for virtual systems...\n");

	/* Ahead of the reserves: calling order could otherwise miss it. */
	vtlb_Core_Free();

	ee_memory_release(&m->main);
	iop_memory_release(&m->main);
	vu_memory_release(&m->main);
}

static u16 ba0R16(u32 mem)
{
	if (mem == 0x1a000006)
	{
		static int ba6;
		ba6++;
		if (ba6 == 3) ba6 = 0;
		return ba6;
	}
	return 0;
}

/* --------------------------------------------------------------------------
 *  Regular memory
 * -------------------------------------------------------------------------- */
static vtlbHandler
	null_handler,

	tlb_fallback_0,
	tlb_fallback_2,
	tlb_fallback_3,
	tlb_fallback_4,
	tlb_fallback_5,
	tlb_fallback_6,
	tlb_fallback_7,
	tlb_fallback_8,

	vu0_micro_mem,
	vu1_micro_mem,
	vu1_data_mem,

	/* Unset until the first reset registers them; all 16, so the guard in
	 * memBindConditionalHandlers sees page 0xf unset before then. */
	hw_by_page[0x10] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
	                     0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF },

	gs_page_0,
	gs_page_1,

	iopHw_by_page_01,
	iopHw_by_page_03,
	iopHw_by_page_08;

/* The configuration the conditional handlers were last bound for, which a
 * reset rebinds. */
static bool s_intc_stat_hack;
static bool s_thread_vu1;

static void memMapVUmicro(void)
{
	/* VU0/VU1 micro mem (instructions). Like IOP memory, these are generally
	 * only used by the EE BIOS kernel during boot-up; applications are
	 * supposed to use the thread-safe VIF instead, or make sure all VIF/GIF
	 * transfers are finished and VU micro execution stopped first.
	 *
	 * The VU0 mapping repeats 4 times across the mapped range; its handlers
	 * mask and wrap the address, so it is not mirrored here. */
	vtlb_MapHandler(vu0_micro_mem,0x11000000,0x00004000);
	vtlb_MapHandler(vu1_micro_mem,0x11008000,0x00004000);

	/* VU0/VU1 memory (data). VU0 is 4k, mirrored 4 times across a 16k area.
	 * VU1 data always goes through its handler, which is bound for MTVU or
	 * not. */
	vtlb_MapBlock(vuRegs[0].Mem,0x11004000,0x00004000,0x1000);
	vtlb_MapHandler(vu1_data_mem,0x1100c000,0x00004000);
}

static void memMapPhy(void)
{
	uint i;

	/* Main memory, mirrored on the first 256MB? */
	vtlb_MapBlock(eeMem->Main,	0x00000000,PS2MEM_MAIN_RAM, 0);
	/* High memory, uninstalled on the configuration we emulate */
	vtlb_MapHandler(null_handler, PS2MEM_MAIN_RAM, 0x10000000 - PS2MEM_MAIN_RAM);

	/* Various ROMs (all read-only) */
	vtlb_MapBlock(eeMem->ROM,	0x1fc00000, PS2MEM_ROM, 0);
	vtlb_MapBlock(eeMem->ROM1,	0x1e000000, PS2MEM_ROM1, 0);
	vtlb_MapBlock(eeMem->ROM2,	0x1e400000, PS2MEM_ROM2, 0);

	/* IOP memory (used by the EE BIOS kernel during initial hardware
	 * initialization; applications are supposed to use the thread-safe SIF) */
	vtlb_MapBlock(iopMem->Main,0x1c000000,0x00800000, 0);

	/* Generic handlers; these fall back to the mem* functions */
	vtlb_MapHandler(tlb_fallback_7,0x14000000, _64kb);
	vtlb_MapHandler(tlb_fallback_4,0x18000000, _64kb);
	vtlb_MapHandler(tlb_fallback_5,0x1a000000, _64kb);
	vtlb_MapHandler(tlb_fallback_6,0x12000000, _64kb);
	vtlb_MapHandler(tlb_fallback_8,0x1f000000, _64kb);
	vtlb_MapHandler(tlb_fallback_3,0x1f400000, _64kb);
	vtlb_MapHandler(tlb_fallback_2,0x1f800000, _64kb);
	vtlb_MapHandler(tlb_fallback_8,0x1f900000, _64kb);

	/* Hardware register handlers: per-page handling of HW register accesses
	 * (hw_by_page is assigned in the reset before this runs) */
	for (i = 0; i < 16; ++i)
		vtlb_MapHandler(hw_by_page[i], 0x10000000 + (0x01000 * i), 0x01000);

	vtlb_MapHandler(gs_page_0, 0x12000000, 0x01000);
	vtlb_MapHandler(gs_page_1, 0x12001000, 0x01000);

	/* "Secret" IOP HW mappings, used by the EE BIOS kernel during boot and
	 * generally left untouched after that, as per EE/IOP thread safety rules. */
	vtlb_MapHandler(iopHw_by_page_01, 0x1f801000, 0x01000);
	vtlb_MapHandler(iopHw_by_page_03, 0x1f803000, 0x01000);
	vtlb_MapHandler(iopHw_by_page_08, 0x1f808000, 0x01000);
}

static void memMapKernelMem(void)
{
	/* 0x8* and 0xa* mirrors of the lower 512MB */
	vtlb_VMap(0x80000000, 0x00000000, _1mb*512);
	vtlb_VMap(0xA0000000, 0x00000000, _1mb*512);
}

static mem8_t  nullRead8(u32 mem)  { (void)mem; return 0; }
static mem16_t nullRead16(u32 mem) { (void)mem; return 0; }
static mem32_t nullRead32(u32 mem) { (void)mem; return 0; }
static mem64_t nullRead64(u32 mem) { (void)mem; return 0; }
static RETURNS_R128 nullRead128(u32 mem) { (void)mem; return r128_zero(); }
static void nullWrite8(u32 mem, mem8_t value)   { (void)mem; (void)value; }
static void nullWrite16(u32 mem, mem16_t value) { (void)mem; (void)value; }
static void nullWrite32(u32 mem, mem32_t value) { (void)mem; (void)value; }
static void nullWrite64(u32 mem, mem64_t value) { (void)mem; (void)value; }
#if PCSX2_MINGW_R128_BY_PTR
static void nullWrite128(u32 mem, const r128* value) { (void)mem; (void)value; }
#else
static void TAKES_R128 nullWrite128(u32 mem, r128 value) { (void)mem; (void)value; }
#endif

/* Unmapped-page fallbacks, shared by every page kind for the widths that
 * kind does not handle. */
static mem8_t  ext_miss_read8  (u32 mem) { cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL); return 0; }
static mem16_t ext_miss_read16 (u32 mem) { cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL); return 0; }
static mem32_t ext_miss_read32 (u32 mem) { cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL); return 0; }
static u64     ext_miss_read64 (u32 mem) { cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL); return 0; }
static RETURNS_R128 ext_miss_read128(u32 mem) { cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBL); return r128_zero(); }
static void ext_miss_write8 (u32 mem, mem8_t  value) { (void)value; cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
static void ext_miss_write16(u32 mem, mem16_t value) { (void)value; cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
static void ext_miss_write32(u32 mem, mem32_t value) { (void)value; cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
static void ext_miss_write64(u32 mem, mem64_t value) { (void)value; cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
#if PCSX2_MINGW_R128_BY_PTR
static void ext_miss_write128(u32 mem, const r128* value) { (void)value; cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
#else
static void TAKES_R128 ext_miss_write128(u32 mem, r128 value) { (void)value; cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
#endif

/* page kind 3 (psh4) */
static mem8_t _ext_memRead8_3(u32 mem) { return psxHw4Read8(mem); }
static void _ext_memWrite8_3(u32 mem, mem8_t value) { psxHw4Write8(mem, value); }

/* page kind 4 (b80): 16-bit reads answer zero, everything else misses. */
static mem16_t _ext_memRead16_4(u32 mem) { (void)mem; return 0; }

/* page kind 5 (ba0): 16-bit reads go through ba0R16 -- the BIOS polls
 * 0x1a000006 during boot -- and 16-bit writes are swallowed. */
static mem16_t _ext_memRead16_5(u32 mem) { return ba0R16(mem); }
static void _ext_memWrite16_5(u32 mem, mem16_t value) { (void)mem; (void)value; }

/* page kind 8 (spu2): 16-bit reads and writes go to SPU2, others miss. */
static mem16_t _ext_memRead16_8(u32 mem) { return SPU2read(mem); }
static void _ext_memWrite16_8(u32 mem, mem16_t value) { SPU2write(mem, value); }

/* page kind 7 (dev9): 8/16/32 MMIO with the address mask, others miss. */
static mem8_t  _ext_memRead8_7 (u32 mem) { return DEV9read8 (mem & ~0xa4000000); }
static mem16_t _ext_memRead16_7(u32 mem) { return DEV9read16(mem & ~0xa4000000); }
static mem32_t _ext_memRead32_7(u32 mem) { return DEV9read32(mem & ~0xa4000000); }
static void _ext_memWrite8_7 (u32 mem, mem8_t  value) { DEV9write8 (mem & ~0xa4000000, value); }
static void _ext_memWrite16_7(u32 mem, mem16_t value) { DEV9write16(mem & ~0xa4000000, value); }
static void _ext_memWrite32_7(u32 mem, mem32_t value) { DEV9write32(mem & ~0xa4000000, value); }

/* page kind 6 (gsm): only the 128-bit read arm is still referenced here --
 * the GS tables below name gsRead8/16/32 and gsWrite* directly. */
static RETURNS_R128 _ext_memRead128_6(u32 mem) { return r128_load(PS2GS_BASE(mem)); }

/* VU micro/data memory handlers, generated per unit with its register
 * block, address mask and Clear as literals. VU1 has a second set for when
 * MTVU owns its memory: reads wait for the VU1 thread, writes go through its
 * ring, each store landing straight in the ring slot. memBindConditional-
 * Handlers picks the set, so neither tests the configuration per access. */
#define ClearVuFunc0(addr, size) CpuVU0->Clear((addr), (size))
#define ClearVuFunc1(addr, size) CpuVU1->Clear((addr), (size))

#if PCSX2_MINGW_R128_BY_PTR
#define VU_WRITE128_HEAD(name) static void name(u32 addr, const r128* data_ptr) { const r128 data = r128_load(data_ptr);
#else
#define VU_WRITE128_HEAD(name) static void TAKES_R128 name(u32 addr, r128 data) {
#endif

/* Reads of one width from mem (Micro or Mem) of unit vunum; wait is a
 * statement run first. */
#define VU_DEFINE_READS(name, vunum, mem, mask, wait)                          \
static mem8_t name##Read8(u32 addr)                                            \
{                                                                              \
	wait;                                                                      \
	return vuRegs[vunum].mem[addr & (mask)];                                   \
}                                                                              \
static mem16_t name##Read16(u32 addr)                                          \
{                                                                              \
	u16 v;                                                                     \
	wait;                                                                      \
	memcpy(&v, &vuRegs[vunum].mem[addr & (mask)], sizeof(v));                  \
	return v;                                                                  \
}                                                                              \
static mem32_t name##Read32(u32 addr)                                          \
{                                                                              \
	u32 v;                                                                     \
	wait;                                                                      \
	memcpy(&v, &vuRegs[vunum].mem[addr & (mask)], sizeof(v));                  \
	return v;                                                                  \
}                                                                              \
static mem64_t name##Read64(u32 addr)                                          \
{                                                                              \
	u64 v;                                                                     \
	wait;                                                                      \
	memcpy(&v, &vuRegs[vunum].mem[addr & (mask)], sizeof(v));                  \
	return v;                                                                  \
}                                                                              \
static RETURNS_R128 name##Read128(u32 addr)                                    \
{                                                                              \
	wait;                                                                      \
	return r128_load(&vuRegs[vunum].mem[addr & (mask)]);                       \
}

/* Micro memory: a write that changes an instruction clears the recompiled
 * code for it first (8 bytes: one instruction). */
#define VU_DEFINE_MICRO_WRITE(name, vunum, mask, clear, T)                     \
static void name(u32 addr, T data)                                             \
{                                                                              \
	u8* p = &vuRegs[vunum].Micro[addr &= (mask)];                              \
	if (memcmp(p, &data, sizeof(data)) != 0)                                   \
	{                                                                          \
		clear(addr, 8);                                                        \
		memcpy(p, &data, sizeof(data));                                        \
	}                                                                          \
}

#define VU_DEFINE_MICRO(vunum, mask, clear)                                    \
VU_DEFINE_READS(vuMicro##vunum##_, vunum, Micro, mask, (void)0)                \
VU_DEFINE_MICRO_WRITE(vuMicro##vunum##_Write8,  vunum, mask, clear, mem8_t)    \
VU_DEFINE_MICRO_WRITE(vuMicro##vunum##_Write16, vunum, mask, clear, mem16_t)   \
VU_DEFINE_MICRO_WRITE(vuMicro##vunum##_Write32, vunum, mask, clear, mem32_t)   \
VU_DEFINE_MICRO_WRITE(vuMicro##vunum##_Write64, vunum, mask, clear, mem64_t)   \
VU_WRITE128_HEAD(vuMicro##vunum##_Write128)                                    \
	u8* p = &vuRegs[vunum].Micro[addr &= (mask)];                              \
	if (!r128_equals_unaligned(p, data))                                       \
	{                                                                          \
		clear(addr, 16);                                                       \
		r128_store_unaligned(p, data);                                         \
	}                                                                          \
}

/* Data memory: plain stores. */
#define VU_DEFINE_DATA_WRITE(name, vunum, mask, T)                             \
static void name(u32 addr, T data)                                             \
{                                                                              \
	memcpy(&vuRegs[vunum].Mem[addr & (mask)], &data, sizeof(data));            \
}

#define VU_DEFINE_DATA(vunum, mask)                                            \
VU_DEFINE_READS(vuData##vunum##_, vunum, Mem, mask, (void)0)                   \
VU_DEFINE_DATA_WRITE(vuData##vunum##_Write8,  vunum, mask, mem8_t)             \
VU_DEFINE_DATA_WRITE(vuData##vunum##_Write16, vunum, mask, mem16_t)            \
VU_DEFINE_DATA_WRITE(vuData##vunum##_Write32, vunum, mask, mem32_t)            \
VU_DEFINE_DATA_WRITE(vuData##vunum##_Write64, vunum, mask, mem64_t)            \
VU_WRITE128_HEAD(vuData##vunum##_Write128)                                     \
	r128_store_unaligned(&vuRegs[vunum].Mem[addr & (mask)], data);             \
}

/* MTVU: the VU1 thread applies the write (and the micro Clear) itself. */
#define VU_DEFINE_MTVU_WRITE(name, data_mem, mask, T)                          \
static void name(u32 addr, T data)                                             \
{                                                                              \
	memcpy(mtvu_write_mem_begin(data_mem, addr & (mask), sizeof(data)), &data, sizeof(data)); \
	mtvu_write_mem_commit();                                                   \
}

#define VU_DEFINE_MTVU(name, mem, data_mem, mask)                              \
VU_DEFINE_READS(name##_, 1, mem, mask, mtvu_wait_vu())                         \
VU_DEFINE_MTVU_WRITE(name##_Write8,  data_mem, mask, mem8_t)                   \
VU_DEFINE_MTVU_WRITE(name##_Write16, data_mem, mask, mem16_t)                  \
VU_DEFINE_MTVU_WRITE(name##_Write32, data_mem, mask, mem32_t)                  \
VU_DEFINE_MTVU_WRITE(name##_Write64, data_mem, mask, mem64_t)                  \
VU_WRITE128_HEAD(name##_Write128)                                              \
	r128_store_unaligned(mtvu_write_mem_begin(data_mem, addr & (mask), 16), data); \
	mtvu_write_mem_commit();                                                   \
}

VU_DEFINE_MICRO(0, 0xfff,  ClearVuFunc0)
VU_DEFINE_MICRO(1, 0x3fff, ClearVuFunc1)
VU_DEFINE_DATA(1, 0x3fff)
VU_DEFINE_MTVU(vuMicro1Mtvu, Micro, 0, 0x3fff)
VU_DEFINE_MTVU(vuData1Mtvu,  Mem,   1, 0x3fff)

#define VU_HANDLERS(name)                                                      \
	name##_Read8, name##_Read16, name##_Read32, name##_Read64, name##_Read128,     \
	name##_Write8, name##_Write16, name##_Write32, name##_Write64, name##_Write128

/* --------------------------------------------------------------------------
 *  PS2 memory init / reset / shutdown
 * -------------------------------------------------------------------------- */
void memBindConditionalHandlers(bool intc_stat_hack, bool thread_vu1)
{
	s_intc_stat_hack = intc_stat_hack;
	s_thread_vu1     = thread_vu1;

	if (hw_by_page[0xf] == 0xFFFFFFFF)
		return;

	hwAssignPage0FHandlers(hw_by_page[0xf], intc_stat_hack);

	if (thread_vu1)
	{
		vtlb_ReassignHandler(vu1_micro_mem, VU_HANDLERS(vuMicro1Mtvu));
		vtlb_ReassignHandler(vu1_data_mem,  VU_HANDLERS(vuData1Mtvu));
	}
	else
	{
		vtlb_ReassignHandler(vu1_micro_mem, VU_HANDLERS(vuMicro1));
		vtlb_ReassignHandler(vu1_data_mem,  VU_HANDLERS(vuData1));
	}
}

/* Resets memory mappings, unmaps TLBs, reloads the BIOS ROMs, etc. */
static void ee_memory_reset(void)
{
	u32 page;

	memset(eeMem, 0, EE_MEMORY_RESERVE_SIZE);

	/* Ideally the vtlb would be initialized once and later resets would
	 * only clear its mappings, but the rest of the emulator is not set up
	 * for a "soft" reset of that sort, so it is the hard one. */
	vtlb_Init();

	null_handler = vtlb_RegisterHandler(nullRead8, nullRead16, nullRead32, nullRead64, nullRead128,
		nullWrite8, nullWrite16, nullWrite32, nullWrite64, nullWrite128);

	/* page kind 0: no MMIO arms in any width -- all ten are the miss path. */
	tlb_fallback_0 = vtlb_RegisterHandler(
		ext_miss_read8, ext_miss_read16, ext_miss_read32, ext_miss_read64, ext_miss_read128,
		ext_miss_write8, ext_miss_write16, ext_miss_write32, ext_miss_write64, ext_miss_write128);
	/* page kind 3 (psh4): 8-bit MMIO, every other width misses. */
	tlb_fallback_3 = vtlb_RegisterHandler(
		_ext_memRead8_3, ext_miss_read16, ext_miss_read32, ext_miss_read64, ext_miss_read128,
		_ext_memWrite8_3, ext_miss_write16, ext_miss_write32, ext_miss_write64, ext_miss_write128);
	tlb_fallback_4 = vtlb_RegisterHandler(
		ext_miss_read8, _ext_memRead16_4, ext_miss_read32, ext_miss_read64, ext_miss_read128,
		ext_miss_write8, ext_miss_write16, ext_miss_write32, ext_miss_write64, ext_miss_write128);
	tlb_fallback_5 = vtlb_RegisterHandler(
		ext_miss_read8, _ext_memRead16_5, ext_miss_read32, ext_miss_read64, ext_miss_read128,
		ext_miss_write8, _ext_memWrite16_5, ext_miss_write32, ext_miss_write64, ext_miss_write128);
	tlb_fallback_7 = vtlb_RegisterHandler(
		_ext_memRead8_7, _ext_memRead16_7, _ext_memRead32_7, ext_miss_read64, ext_miss_read128,
		_ext_memWrite8_7, _ext_memWrite16_7, _ext_memWrite32_7, ext_miss_write64, ext_miss_write128);
	tlb_fallback_8 = vtlb_RegisterHandler(
		ext_miss_read8, _ext_memRead16_8, ext_miss_read32, ext_miss_read64, ext_miss_read128,
		ext_miss_write8, _ext_memWrite16_8, ext_miss_write32, ext_miss_write64, ext_miss_write128);

	/* VU memory; VU1's two are bound by memBindConditionalHandlers below. */
	vu0_micro_mem = vtlb_RegisterHandler(VU_HANDLERS(vuMicro0));
	vu1_micro_mem = vtlb_NewHandler();
	vu1_data_mem  = vtlb_NewHandler();

	/* The IOP's "secret" hardware register mapping, accessible from the EE
	 * (meant for debugging or the BIOS only). The IOP's registers are three
	 * pages in the 0x1f80 segment, and another page for CDVD at 0x1f40. */
	tlb_fallback_2 = vtlb_RegisterHandler(
		iopHwRead8_generic, iopHwRead16_generic, iopHwRead32_generic, ext_miss_read64, ext_miss_read128,
		iopHwWrite8_generic, iopHwWrite16_generic, iopHwWrite32_generic, ext_miss_write64, ext_miss_write128
	);

	iopHw_by_page_01 = vtlb_RegisterHandler(
		iopHwRead8_Page1, iopHwRead16_Page1, iopHwRead32_Page1, ext_miss_read64, ext_miss_read128,
		iopHwWrite8_Page1, iopHwWrite16_Page1, iopHwWrite32_Page1, ext_miss_write64, ext_miss_write128
	);

	iopHw_by_page_03 = vtlb_RegisterHandler(
		iopHwRead8_Page3, iopHwRead16_Page3, iopHwRead32_Page3, ext_miss_read64, ext_miss_read128,
		iopHwWrite8_Page3, iopHwWrite16_Page3, iopHwWrite32_Page3, ext_miss_write64, ext_miss_write128
	);

	iopHw_by_page_08 = vtlb_RegisterHandler(
		iopHwRead8_Page8, iopHwRead16_Page8, iopHwRead32_Page8, ext_miss_read64, ext_miss_read128,
		iopHwWrite8_Page8, iopHwWrite16_Page8, iopHwWrite32_Page8, ext_miss_write64, ext_miss_write128
	);

	/* The EE hardware registers, one handler set per page. */
	for (page = 0; page < 0x10; page++)
		hw_by_page[page] = hwRegisterPageHandlers(page);
	memBindConditionalHandlers(s_intc_stat_hack, s_thread_vu1);

	/* GS mappings */
	tlb_fallback_6 = vtlb_RegisterHandler(
		gsRead8, gsRead16, gsRead32, gsRead64, _ext_memRead128_6,
		gsWrite8, gsWrite16, gsWrite32, gsWrite64_generic, gsWrite128_generic
	);

	gs_page_0 = vtlb_RegisterHandler(
		gsRead8, gsRead16, gsRead32, gsRead64, _ext_memRead128_6,
		gsWrite8, gsWrite16, gsWrite32, gsWrite64_page_00, gsWrite128_generic
	);

	gs_page_1 = vtlb_RegisterHandler(
		gsRead8, gsRead16, gsRead32, gsRead64, _ext_memRead128_6,
		gsWrite8, gsWrite16, gsWrite32, gsWrite64_page_01, gsWrite128_page_01
	);

	memMapPhy();
	memMapVUmicro();
	memMapKernelMem();

	vtlb_VMap(0x00000000,0x00000000,0x20000000);
	vtlb_VMapUnmap(0x20000000,0x60000000);

	if (!LoadBIOS())
		log_cb(RETRO_LOG_ERROR, "Failed to load BIOS\n");

	/* After the BIOS load: it depends on the BIOS version. */
	cdvdLoadNVRAM();
}
