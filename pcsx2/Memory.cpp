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

#include "IopHw.h"
#include "common/Pcsx2Defs.h"
#include <utility>
#include "GS.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "DEV9/DEV9.h"
#include "CDVD/CDVD.h"

#include "ps2/HwInternal.h"
#include "ps2/BiosTools.h"
#include "SPU2/spu2.h"
#include "NearPlace.h"

namespace HostMemoryMap
{
	extern "C" {
	uptr EEmem, IOPmem, VUmem, bumpAllocator;
	}
} // namespace HostMemoryMap

/* The recompilers reach this module's globals and, for constant-address loads
 * and stores, guest memory with RIP-relative or abs32 operands, and the C89
 * emitter aborts on one it cannot encode. Every byte of the code reservation
 * therefore has to be within rel32 of every byte of the loaded image and of
 * the main reservation. Both are placed so that image, main and code lie
 * inside one RIP_LIMIT window (see NearPlace.h): main only where the code
 * still fits beside it, then the code against the hull of image and main. */
static const uptr RIP_LIMIT     = 0x7C000000; /* 1.9375GB: 2GB less rip-bias/operand-tail headroom */
static const uptr RESERVE_ALIGN = 0x10000;

/* Where the main reservation went, for the code reservation to reach. */
static near_range_t s_main_range;

static inline uptr near_anchor(void)
{
	return (uptr)(void*)&near_anchor;
}

static VirtualMemoryManagerPtr TryAt(const char* name, uptr base, size_t size)
{
	VirtualMemoryManagerPtr mgr = std::make_shared<VirtualMemoryManager>(name, base, size, /*upper_bounds=*/0, /*strict=*/true);
	if (mgr->IsOk())
		return mgr;
	return nullptr;
}

/* Places the main reservation, or with code set the code reservation, within
 * reach as described above. */
static VirtualMemoryManagerPtr AllocateVirtualMemory(const char* name, size_t size, bool code)
{
	near_search_t s;
	near_range_t image;
	near_range_t target;
	near_range_t placed;
	VirtualMemoryManagerPtr mgr;
	uptr limit   = RIP_LIMIT;
	uptr ceiling = 0;
	unsigned i;

	near_image_extent((const void*)near_anchor(), &image);
	target = image;
	if (code)
	{
		if (s_main_range.hi)
			near_hull(&target, &s_main_range);
	}
	else
	{
		/* VTLB marks a hardware-register handler with the pointer sign bit
		 * (vtlb.cpp: "if ((sptr)p < 0)"), so guest RAM stays below it. */
		ceiling         = (uptr)1 << (sizeof(uptr) * 8 - 1);
		limit           = RIP_LIMIT - HostMemoryMap::CodeSize;
		s_main_range.lo = 0;
		s_main_range.hi = 0;
	}

	if (near_search_init(&s, &target, size, limit, RESERVE_ALIGN, ceiling))
	{
		near_search_scan(&s);
		for (i = 0; i < s.count; i++)
		{
			mgr = TryAt(name, s.base[i], size);
			if (!mgr)
				continue;
			if (code)
				return mgr;
			placed.lo = s.base[i];
			placed.hi = s.base[i] + size;
			near_hull(&placed, &image);
			if (near_place_room(&placed, HostMemoryMap::CodeSize, RIP_LIMIT, RESERVE_ALIGN))
			{
				s_main_range.lo = s.base[i];
				s_main_range.hi = s.base[i] + size;
				return mgr;
			}
		}
	}

	mgr = std::make_shared<VirtualMemoryManager>(name, 0, size);
	if (mgr->IsOk())
	{
		if (!code)
		{
			s_main_range.lo = (uptr)mgr->GetBase();
			s_main_range.hi = s_main_range.lo + size;
		}
		log_cb(RETRO_LOG_ERROR, "Could not place the %zu MB %s reservation within reach of the core "
		                        "(module at %p, reservation at %p). The recompilers cannot address "
		                        "it from there.\n",
		       size >> 20, code ? "code" : "main memory", (void*)near_anchor(), (void*)mgr->GetBase());
	}
	return mgr;
}

// --------------------------------------------------------------------------------------
//  SysReserveVM  (implementations)
// --------------------------------------------------------------------------------------
SysMainMemory::SysMainMemory()
	: m_mainMemory(AllocateVirtualMemory("pcsx2", HostMemoryMap::MainSize, false))
	, m_codeMemory(AllocateVirtualMemory(nullptr, HostMemoryMap::CodeSize, true))
	, m_bumpAllocator(m_mainMemory, HostMemoryMap::bumpAllocatorOffset, HostMemoryMap::MainSize - HostMemoryMap::bumpAllocatorOffset)
{
	uptr main_base = (uptr)MainMemory()->GetBase();
	HostMemoryMap::EEmem = main_base + HostMemoryMap::EEmemOffset;
	HostMemoryMap::IOPmem = main_base + HostMemoryMap::IOPmemOffset;
	HostMemoryMap::VUmem = main_base + HostMemoryMap::VUmemOffset;
	HostMemoryMap::bumpAllocator = main_base + HostMemoryMap::bumpAllocatorOffset;
}

SysMainMemory::~SysMainMemory()
{
	Release();
}

bool SysMainMemory::Allocate()
{
	log_cb(RETRO_LOG_INFO, "Allocating host memory for virtual systems...\n");
	m_ee.Assign(MainMemory());
	m_iop.Assign(MainMemory());
	m_vu.Assign(MainMemory());

	vtlb_Core_Alloc();

	return true;
}

void SysMainMemory::Reset()
{
	log_cb(RETRO_LOG_INFO, "Resetting host memory for virtual systems...\n");
	m_ee.Reset();
	m_iop.Reset();
	m_vu.Reset();

	// Note: newVif is reset as part of other VIF structures.
	// Software is reset on the GS thread.
}

void SysMainMemory::Release()
{
	log_cb(RETRO_LOG_INFO, "Releasing host memory for virtual systems...\n");

	vtlb_Core_Free(); // Just to be sure... (calling order could result in it getting missed during Decommit).

	m_ee.Release();
	m_iop.Release();
	m_vu.Release();
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

/////////////////////////////
// REGULAR MEM START
/////////////////////////////
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

	hw_by_page[0x10] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},

	gs_page_0,
	gs_page_1,

	iopHw_by_page_01,
	iopHw_by_page_03,
	iopHw_by_page_08;


static void memMapVUmicro(void)
{
	// VU0/VU1 micro mem (instructions)
	// (Like IOP memory, these are generally only used by the EE Bios kernel during
	//  boot-up.  Applications/games are "supposed" to use the thread-safe VIF instead;
	//  or must ensure all VIF/GIF transfers are finished and all VUmicro execution stopped
	//  prior to accessing VU memory directly).

	// The VU0 mapping actually repeats 4 times across the mapped range, but we don't bother
	// to manually mirror it here because the indirect memory handler for it (see vuMicroRead*
	// functions below) automatically mask and wrap the address for us.

	vtlb_MapHandler(vu0_micro_mem,0x11000000,0x00004000);
	vtlb_MapHandler(vu1_micro_mem,0x11008000,0x00004000);

	// VU0/VU1 memory (data)
	// VU0 is 4k, mirrored 4 times across a 16k area.
	vtlb_MapBlock(vuRegs[0].Mem,0x11004000,0x00004000,0x1000);
	// Note: In order for the below conditional to work correctly
	// support needs to be coded to reset the memMappings when MTVU is
	// turned off/on. For now we just always use the vu data handlers...
	if (1||THREAD_VU1) vtlb_MapHandler(vu1_data_mem,0x1100c000,0x00004000);
	else               vtlb_MapBlock  (vuRegs[1].Mem,     0x1100c000,0x00004000, 0);
}

static void memMapPhy(void)
{
	// Main memory
	vtlb_MapBlock(eeMem->Main,	0x00000000,Ps2MemSize::MainRam, 0);//mirrored on first 256 mb ?
	// High memory, uninstalled on the configuration we emulate
	vtlb_MapHandler(null_handler, Ps2MemSize::MainRam, 0x10000000 - Ps2MemSize::MainRam);

	// Various ROMs (all read-only)
	vtlb_MapBlock(eeMem->ROM,	0x1fc00000, Ps2MemSize::Rom, 0);
	vtlb_MapBlock(eeMem->ROM1,	0x1e000000, Ps2MemSize::Rom1, 0);
	vtlb_MapBlock(eeMem->ROM2,	0x1e400000, Ps2MemSize::Rom2, 0);

	// IOP memory
	// (used by the EE Bios Kernel during initial hardware initialization, Apps/Games
	//  are "supposed" to use the thread-safe SIF instead.)
	vtlb_MapBlock(iopMem->Main,0x1c000000,0x00800000, 0);

	// Generic Handlers; These fallback to mem* stuff...
	vtlb_MapHandler(tlb_fallback_7,0x14000000, _64kb);
	vtlb_MapHandler(tlb_fallback_4,0x18000000, _64kb);
	vtlb_MapHandler(tlb_fallback_5,0x1a000000, _64kb);
	vtlb_MapHandler(tlb_fallback_6,0x12000000, _64kb);
	vtlb_MapHandler(tlb_fallback_8,0x1f000000, _64kb);
	vtlb_MapHandler(tlb_fallback_3,0x1f400000, _64kb);
	vtlb_MapHandler(tlb_fallback_2,0x1f800000, _64kb);
	vtlb_MapHandler(tlb_fallback_8,0x1f900000, _64kb);

	// Hardware Register Handlers : specialized/optimized per-page handling of HW register accesses
	// (note that hw_by_page handles are assigned in memReset prior to calling this function)

	for( uint i=0; i<16; ++i)
		vtlb_MapHandler(hw_by_page[i], 0x10000000 + (0x01000 * i), 0x01000);

	vtlb_MapHandler(gs_page_0, 0x12000000, 0x01000);
	vtlb_MapHandler(gs_page_1, 0x12001000, 0x01000);

	// "Secret" IOP HW mappings - Used by EE Bios Kernel during boot and generally
	// left untouched after that, as per EE/IOP thread safety rules.

	vtlb_MapHandler(iopHw_by_page_01, 0x1f801000, 0x01000);
	vtlb_MapHandler(iopHw_by_page_03, 0x1f803000, 0x01000);
	vtlb_MapHandler(iopHw_by_page_08, 0x1f808000, 0x01000);

}

//Why is this required ?
static void memMapKernelMem(void)
{
	//lower 512 mb: direct map
	//vtlb_VMap(0x00000000,0x00000000,0x20000000);
	//0x8* mirror
	vtlb_VMap(0x80000000, 0x00000000, _1mb*512);
	//0xa* mirror
	vtlb_VMap(0xA0000000, 0x00000000, _1mb*512);
}

static mem8_t  nullRead8(u32 mem)  { return 0; }
static mem16_t nullRead16(u32 mem) { return 0; }
static mem32_t nullRead32(u32 mem) { return 0; }
static mem64_t nullRead64(u32 mem) { return 0; }
static RETURNS_R128 nullRead128(u32 mem) { return r128_zero(); }
static void nullWrite8(u32 mem, mem8_t value)   { }
static void nullWrite16(u32 mem, mem16_t value) { }
static void nullWrite32(u32 mem, mem32_t value) { }
static void nullWrite64(u32 mem, mem64_t value) { }
#if PCSX2_MINGW_R128_BY_PTR
static void nullWrite128(u32 mem, const r128* value) { (void)mem; (void)value; }
#else
static void TAKES_R128 nullWrite128(u32 mem, r128 value) { }
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
static void TAKES_R128 ext_miss_write128(u32 mem, r128 value) { cpuTlbMiss(mem, cpuRegs.branch, EXC_CODE_TLBS); }
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

typedef void ClearFunc_t( u32 addr, u32 qwc );

/* VU micro/data memory handlers. These were template<int vunum> families:
 * every body opened with `vunum ? &vuRegs[1] : &vuRegs[0]`, an address mask
 * of 0x3fff or 0xfff, and a `vunum && THREAD_VU1` branch -- all three fold
 * per instantiation, so the macro below emits the same straight-line code
 * with the unit's register block, mask and MTVU behaviour as literals.
 * Only the instantiations that are registered exist: micro for VU0 and VU1,
 * data for VU1. */
static __fi void ClearVuFunc0(u32 addr, u32 size) { CpuVU0->Clear(addr, size); }
static __fi void ClearVuFunc1(u32 addr, u32 size) { CpuVU1->Clear(addr, size); }

#if PCSX2_MINGW_R128_BY_PTR
#define VU_WRITE128_HEAD(name) static void name(u32 addr, const r128* data_ptr) { const r128 data = r128_load(data_ptr);
#else
#define VU_WRITE128_HEAD(name) static void TAKES_R128 name(u32 addr, r128 data) {
#endif

/* vunum: 0 or 1.  mask: address mask.  mtvu: 1 when writes may be deferred to
 * the MTVU thread (VU1 only).  clear: the unit's ClearVuFunc. */
#define VU_DEFINE_MICRO(vunum, mask, mtvu, clear)                              \
static mem8_t vuMicroRead8_##vunum(u32 addr)                                   \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return vu->Micro[addr];                                                    \
}                                                                              \
static mem16_t vuMicroRead16_##vunum(u32 addr)                                 \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return *(u16*)&vu->Micro[addr];                                            \
}                                                                              \
static mem32_t vuMicroRead32_##vunum(u32 addr)                                 \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return *(u32*)&vu->Micro[addr];                                            \
}                                                                              \
static mem64_t vuMicroRead64_##vunum(u32 addr)                                 \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return *(u64*)&vu->Micro[addr];                                            \
}                                                                              \
static RETURNS_R128 vuMicroRead128_##vunum(u32 addr)                           \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return r128_load(&vu->Micro[addr]);                                        \
}                                                                              \
static void vuMicroWrite8_##vunum(u32 addr, mem8_t data)                       \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u8));                      \
		return;                                                                \
	}                                                                          \
	if (vu->Micro[addr] != data) /* clear before writing new data */           \
	{                                                                          \
		clear(addr, 8); /* 8 bytes: one instruction */                         \
		vu->Micro[addr] = data;                                                \
	}                                                                          \
}                                                                              \
static void vuMicroWrite16_##vunum(u32 addr, mem16_t data)                     \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u16));                     \
		return;                                                                \
	}                                                                          \
	if (*(u16*)&vu->Micro[addr] != data)                                       \
	{                                                                          \
		clear(addr, 8);                                                        \
		*(u16*)&vu->Micro[addr] = data;                                        \
	}                                                                          \
}                                                                              \
static void vuMicroWrite32_##vunum(u32 addr, mem32_t data)                     \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u32));                     \
		return;                                                                \
	}                                                                          \
	if (*(u32*)&vu->Micro[addr] != data)                                       \
	{                                                                          \
		clear(addr, 8);                                                        \
		*(u32*)&vu->Micro[addr] = data;                                        \
	}                                                                          \
}                                                                              \
static void vuMicroWrite64_##vunum(u32 addr, mem64_t data)                     \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteMicroMem(addr, &data, sizeof(u64));                     \
		return;                                                                \
	}                                                                          \
	if (*(u64*)&vu->Micro[addr] != data)                                       \
	{                                                                          \
		clear(addr, 8);                                                        \
		*(u64*)&vu->Micro[addr] = data;                                        \
	}                                                                          \
}                                                                              \
VU_WRITE128_HEAD(vuMicroWrite128_##vunum)                                      \
	VURegs* vu = &vuRegs[vunum];                                               \
	u128 udata;                                                                \
	u128 comp;                                                                 \
	addr &= mask;                                                              \
	udata = r128_to_u128(data);                                                \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteMicroMem(addr, &udata, sizeof(u128));                   \
		return;                                                                \
	}                                                                          \
	comp = (u128&)vu->Micro[addr];                                             \
	if ((comp.lo != udata.lo) || (comp.hi != udata.hi))                        \
	{                                                                          \
		clear(addr, 16);                                                       \
		r128_store_unaligned(&vu->Micro[addr], data);                          \
	}                                                                          \
}

VU_DEFINE_MICRO(0, 0xfff,  0, ClearVuFunc0)
VU_DEFINE_MICRO(1, 0x3fff, 1, ClearVuFunc1)

/* VU data memory. Only the VU1 instantiation is registered. */
#define VU_DEFINE_DATA(vunum, mask, mtvu)                                      \
static mem8_t vuDataRead8_##vunum(u32 addr)                                    \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return vu->Mem[addr];                                                      \
}                                                                              \
static mem16_t vuDataRead16_##vunum(u32 addr)                                  \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return *(u16*)&vu->Mem[addr];                                              \
}                                                                              \
static mem32_t vuDataRead32_##vunum(u32 addr)                                  \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return *(u32*)&vu->Mem[addr];                                              \
}                                                                              \
static mem64_t vuDataRead64_##vunum(u32 addr)                                  \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return *(u64*)&vu->Mem[addr];                                              \
}                                                                              \
static RETURNS_R128 vuDataRead128_##vunum(u32 addr)                            \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1) vu1Thread.WaitVU();                                \
	return r128_load(&vu->Mem[addr]);                                          \
}                                                                              \
static void vuDataWrite8_##vunum(u32 addr, mem8_t data)                        \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteDataMem(addr, &data, sizeof(u8));                       \
		return;                                                                \
	}                                                                          \
	vu->Mem[addr] = data;                                                      \
}                                                                              \
static void vuDataWrite16_##vunum(u32 addr, mem16_t data)                      \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteDataMem(addr, &data, sizeof(u16));                      \
		return;                                                                \
	}                                                                          \
	*(u16*)&vu->Mem[addr] = data;                                              \
}                                                                              \
static void vuDataWrite32_##vunum(u32 addr, mem32_t data)                      \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteDataMem(addr, &data, sizeof(u32));                      \
		return;                                                                \
	}                                                                          \
	*(u32*)&vu->Mem[addr] = data;                                              \
}                                                                              \
static void vuDataWrite64_##vunum(u32 addr, mem64_t data)                      \
{                                                                              \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		vu1Thread.WriteDataMem(addr, &data, sizeof(u64));                      \
		return;                                                                \
	}                                                                          \
	*(u64*)&vu->Mem[addr] = data;                                              \
}                                                                              \
VU_WRITE128_HEAD(vuDataWrite128_##vunum)                                       \
	VURegs* vu = &vuRegs[vunum];                                               \
	addr &= mask;                                                              \
	if (mtvu && THREAD_VU1)                                                    \
	{                                                                          \
		alignas(16) const u128 udata = r128_to_u128(data);                     \
		vu1Thread.WriteDataMem(addr, &udata, sizeof(u128));                    \
		return;                                                                \
	}                                                                          \
	r128_store_unaligned(&vu->Mem[addr], data);                                \
}

VU_DEFINE_DATA(1, 0x3fff, 1)

///////////////////////////////////////////////////////////////////////////
// PS2 Memory Init / Reset / Shutdown

EEVM_MemoryAllocMess* eeMem = NULL;
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];

void memBindConditionalHandlers(void)
{
	if( hw_by_page[0xf] == 0xFFFFFFFF ) return;

	if (EmuConfig.Speedhacks.IntcStat)
	{
		vtlbMemR16FP* page0F16(hwRead16_page_0F_INTC_HACK);
		vtlbMemR32FP* page0F32(hwRead32_page_0F_INTC_HACK);

		vtlb_ReassignHandler( hw_by_page[0xf],
			hwRead8<0x0f>,	page0F16,		page0F32,		hwRead64<0x0f>,		hwRead128<0x0f>,
			hwWrite8<0x0f>,	hwWrite16<0x0f>,	hwWrite32<0x0f>,	hwWrite64<0x0f>,	hwWrite128<0x0f>
		);
	}
	else
	{
		vtlbMemR16FP* page0F16(hwRead16<0x0f>);
		vtlbMemR32FP* page0F32(hwRead32<0x0f>);

		vtlb_ReassignHandler( hw_by_page[0xf],
			hwRead8<0x0f>,	page0F16,		page0F32,		hwRead64<0x0f>,		hwRead128<0x0f>,
			hwWrite8<0x0f>,	hwWrite16<0x0f>,	hwWrite32<0x0f>,	hwWrite64<0x0f>,	hwWrite128<0x0f>
		);
	}
}


// --------------------------------------------------------------------------------------
//  eeMemoryReserve  (implementations)
// --------------------------------------------------------------------------------------
/* EE Main Memory */
eeMemoryReserve::eeMemoryReserve() : _parent() { }
eeMemoryReserve::~eeMemoryReserve() { Release(); }

void eeMemoryReserve::Assign(VirtualMemoryManagerPtr allocator)
{
	_parent::Assign(std::move(allocator), HostMemoryMap::EEmemOffset, sizeof(*eeMem));
	eeMem = reinterpret_cast<EEVM_MemoryAllocMess*>(GetPtr());
}


// Resets memory mappings, unmaps TLBs, reloads bios roms, etc.
void eeMemoryReserve::Reset()
{
	_parent::Reset();

	// Note!!  Ideally the vtlb should only be initialized once, and then subsequent
	// resets of the system hardware would only clear vtlb mappings, but since the
	// rest of the emu is not really set up to support a "soft" reset of that sort
	// we opt for the hard/safe version.
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

	// Dynarec versions of VUs
	vu0_micro_mem = vtlb_RegisterHandler(
		vuMicroRead8_0, vuMicroRead16_0, vuMicroRead32_0, vuMicroRead64_0, vuMicroRead128_0,
		vuMicroWrite8_0, vuMicroWrite16_0, vuMicroWrite32_0, vuMicroWrite64_0, vuMicroWrite128_0);
	vu1_micro_mem = vtlb_RegisterHandler(
		vuMicroRead8_1, vuMicroRead16_1, vuMicroRead32_1, vuMicroRead64_1, vuMicroRead128_1,
		vuMicroWrite8_1, vuMicroWrite16_1, vuMicroWrite32_1, vuMicroWrite64_1, vuMicroWrite128_1);
	vu1_data_mem  = (1||THREAD_VU1) ? vtlb_RegisterHandler(
		vuDataRead8_1, vuDataRead16_1, vuDataRead32_1, vuDataRead64_1, vuDataRead128_1,
		vuDataWrite8_1, vuDataWrite16_1, vuDataWrite32_1, vuDataWrite64_1, vuDataWrite128_1) : 0;

	//////////////////////////////////////////////////////////////////////////////////////////
	// IOP's "secret" Hardware Register mapping, accessible from the EE (and meant for use
	// by debugging or BIOS only).  The IOP's hw regs are divided into three main pages in
	// the 0x1f80 segment, and then another oddball page for CDVD in the 0x1f40 segment.
	//

	using namespace IopMemory;

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


	// psHw Optimized Mappings
	// The HW Registers have been split into pages to improve optimization.

#define hwHandlerTmpl(page) \
	hwRead8<page>,	hwRead16<page>,	hwRead32<page>,	hwRead64<page>,	hwRead128<page>, \
	hwWrite8<page>,	hwWrite16<page>,hwWrite32<page>,hwWrite64<page>,hwWrite128<page>

	hw_by_page[0x0] = vtlb_RegisterHandler( hwHandlerTmpl(0x00) );
	hw_by_page[0x1] = vtlb_RegisterHandler( hwHandlerTmpl(0x01) );
	hw_by_page[0x2] = vtlb_RegisterHandler( hwHandlerTmpl(0x02) );
	hw_by_page[0x3] = vtlb_RegisterHandler( hwHandlerTmpl(0x03) );
	hw_by_page[0x4] = vtlb_RegisterHandler( hwHandlerTmpl(0x04) );
	hw_by_page[0x5] = vtlb_RegisterHandler( hwHandlerTmpl(0x05) );
	hw_by_page[0x6] = vtlb_RegisterHandler( hwHandlerTmpl(0x06) );
	hw_by_page[0x7] = vtlb_RegisterHandler( hwHandlerTmpl(0x07) );
	hw_by_page[0x8] = vtlb_RegisterHandler( hwHandlerTmpl(0x08) );
	hw_by_page[0x9] = vtlb_RegisterHandler( hwHandlerTmpl(0x09) );
	hw_by_page[0xa] = vtlb_RegisterHandler( hwHandlerTmpl(0x0a) );
	hw_by_page[0xb] = vtlb_RegisterHandler( hwHandlerTmpl(0x0b) );
	hw_by_page[0xc] = vtlb_RegisterHandler( hwHandlerTmpl(0x0c) );
	hw_by_page[0xd] = vtlb_RegisterHandler( hwHandlerTmpl(0x0d) );
	hw_by_page[0xe] = vtlb_RegisterHandler( hwHandlerTmpl(0x0e) );
	hw_by_page[0xf] = vtlb_NewHandler();		// redefined later based on speedhacking prefs
	memBindConditionalHandlers();

	//////////////////////////////////////////////////////////////////////
	// GS Optimized Mappings

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

	// Must happen after BIOS load, depends on BIOS version.
	cdvdLoadNVRAM();
}

void eeMemoryReserve::Release()
{
	eeMem = nullptr;
	_parent::Release();
}
