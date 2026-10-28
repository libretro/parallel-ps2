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

#include "MemoryTypes.h"
#include "VirtualMemory.h"

#include "../common/SingleRegisterTypes.h"

// Specialized function pointers for each read type
typedef  mem8_t vtlbMemR8FP(u32 addr);
typedef  mem16_t vtlbMemR16FP(u32 addr);
typedef  mem32_t vtlbMemR32FP(u32 addr);
typedef  mem64_t vtlbMemR64FP(u32 addr);
typedef  RETURNS_R128 vtlbMemR128FP(u32 addr);

// Specialized function pointers for each write type
typedef  void vtlbMemW8FP(u32 addr,mem8_t data);
typedef  void vtlbMemW16FP(u32 addr,mem16_t data);
typedef  void vtlbMemW32FP(u32 addr,mem32_t data);
typedef  void vtlbMemW64FP(u32 addr,mem64_t data);
#if PCSX2_MINGW_R128_BY_PTR
typedef  void vtlbMemW128FP(u32 addr, const r128* data);
#else
typedef  void TAKES_R128 vtlbMemW128FP(u32 addr,r128 data);
#endif

/* vtlbMemFP<Width, Write> mapped a width and direction onto a handler
 * typedef and an RWFT row index at compile time. The read/write paths name
 * the row and the function-pointer type directly now, so it has no users. */

typedef u32 vtlbHandler;

#ifdef __cplusplus
extern "C" {
#endif

extern bool vtlb_Core_Alloc(void);
extern void vtlb_Core_Free(void);
extern void vtlb_Alloc_Ppmap(void);
extern void vtlb_Init(void);
extern void vtlb_Shutdown(void);
extern void vtlb_Reset(void);
extern void vtlb_ResetFastmem(void);

/* Set when the fastmem area could not be made: fastmem stays off whatever
 * the options ask for, as nothing is mapped for it. */
extern bool vtlb_fastmem_unavailable;

extern vtlbHandler vtlb_NewHandler(void);

extern vtlbHandler vtlb_RegisterHandler(
	vtlbMemR8FP* r8,vtlbMemR16FP* r16,vtlbMemR32FP* r32,vtlbMemR64FP* r64,vtlbMemR128FP* r128,
	vtlbMemW8FP* w8,vtlbMemW16FP* w16,vtlbMemW32FP* w32,vtlbMemW64FP* w64,vtlbMemW128FP* w128
);

extern void vtlb_ReassignHandler( vtlbHandler rv,
	vtlbMemR8FP* r8,vtlbMemR16FP* r16,vtlbMemR32FP* r32,vtlbMemR64FP* r64,vtlbMemR128FP* r128,
	vtlbMemW8FP* w8,vtlbMemW16FP* w16,vtlbMemW32FP* w32,vtlbMemW64FP* w64,vtlbMemW128FP* w128
);


extern void vtlb_MapHandler(vtlbHandler handler,u32 start,u32 size);
extern void vtlb_MapBlock(void* base,u32 start,u32 size,u32 blocksize);
extern void* vtlb_GetPhyPtr(u32 paddr);
extern u32  vtlb_V2P(u32 vaddr);
extern void vtlb_DynV2P(void);

//virtual mappings
extern void vtlb_VMap(u32 vaddr,u32 paddr,u32 sz);
extern void vtlb_VMapBuffer(u32 vaddr,void* buffer,u32 sz);
extern void vtlb_VMapUnmap(u32 vaddr,u32 sz);

extern void vtlb_ClearLoadStoreInfo(void);
extern void vtlb_AddLoadStoreInfo(uptr code_address, u32 code_size, u32 guest_pc, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, bool is_signed, bool is_load, bool is_fpr);
extern void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr, u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register, u8 size_in_bits, int is_signed, int is_load, int is_fpr);
extern bool vtlb_IsFaultingPC(u32 guest_pc);

//Memory functions

extern mem8_t  vtlb_memRead8 (u32 mem);
extern mem16_t vtlb_memRead16(u32 mem);
extern mem32_t vtlb_memRead32(u32 mem);
extern mem64_t vtlb_memRead64(u32 mem);
extern RETURNS_R128 vtlb_memRead128(u32 mem);

extern void vtlb_memWrite8 (u32 mem, mem8_t  value);
extern void vtlb_memWrite16(u32 mem, mem16_t value);
extern void vtlb_memWrite32(u32 mem, mem32_t value);
extern void vtlb_memWrite64(u32 mem, mem64_t value);
#if PCSX2_MINGW_R128_BY_PTR
extern void vtlb_memWrite128(u32 mem, const r128* value);
#else
extern void TAKES_R128 vtlb_memWrite128(u32 mem, r128 value);
#endif

#ifdef __cplusplus
}
#endif

/* The recompiler entry points are C++: the dynarec declarations carry default
 * arguments and an alias template. */
#ifdef __cplusplus

// "Safe" variants of vtlb, designed for external tools.
using vtlb_ReadRegAllocCallback = int(*)(void);
extern int vtlb_DynGenReadNonQuad(u32 bits, int sign, int xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);
extern int vtlb_DynGenReadNonQuad_Const(u32 bits, int sign, int xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);
extern int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);
extern int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc = nullptr);

extern void vtlb_DynGenWrite(u32 sz, int xmm, int addr_reg, int value_reg);
extern void vtlb_DynGenWrite_Const(u32 bits, int xmm, u32 addr_const, int value_reg);

extern void vtlb_DynGenDispatchers(void);

#endif /* __cplusplus */

/* vtlb constants and lookup machinery. C89-shaped: the wrapper classes
 * that used to live in namespace vtlb_private are plain integer typedefs
 * with inline accessor functions; the packed representation is unchanged.
 *
 * A physical entry is a host pointer when non-negative, or a handler ID
 * in the low byte with the pointer sign bit set. A virtual entry is the
 * physical entry biased by -vaddr, so entry + vaddr recovers either the
 * host pointer or the sign-set handler cookie, whose low byte is the
 * handler ID as long as paddr and vaddr share page alignment. */

#define VTLB_PAGE_BITS     12
#define VTLB_PAGE_MASK     4095
#define VTLB_PAGE_SIZE     4096

#define VTLB_PMAP_SZ       (_1mb * 512)
#define VTLB_PMAP_ITEMS    (VTLB_PMAP_SZ / VTLB_PAGE_SIZE)
#define VTLB_VMAP_ITEMS    (_4gb / VTLB_PAGE_SIZE)

#define VTLB_HANDLER_ITEMS 128

#define POINTER_SIGN_BIT   (1ULL << (sizeof(uptr) * 8 - 1))

#if defined(_MSC_VER)
#define VTLB_INLINE __forceinline
#elif defined(__GNUC__)
#define VTLB_INLINE __inline__ __attribute__((always_inline))
#else
#define VTLB_INLINE
#endif

typedef sptr vtlb_phys_t; /* host pointer, or (handler | POINTER_SIGN_BIT) */
typedef uptr vtlb_virt_t; /* physical entry biased by -vaddr */

typedef struct
{
	/* first indexer  -- 8/16/32/64/128 bit tables [values 0-4]
	 * second indexer -- read/write [0 or 1]
	 * third indexer  -- 128 possible handlers */
	void* RWFT[5][2][VTLB_HANDLER_ITEMS];

	vtlb_phys_t pmap[VTLB_PMAP_ITEMS]; /* 512KB, PS2 physical to x86 physical */

	vtlb_virt_t* vmap; /* 4MB (allocated by vtlb_init), PS2 virtual to x86 physical */

	u32* ppmap; /* 4MB (allocated by vtlb_init), PS2 virtual to PS2 physical */

	uptr fastmem_base;
} vtlb_map_t;

PCSX2_ALIGN(64) extern vtlb_map_t vtlbdata;

static VTLB_INLINE vtlb_phys_t vtlb_phys_from_ptr(sptr ptr) { return ptr; }
static VTLB_INLINE vtlb_phys_t vtlb_phys_from_handler(vtlbHandler handler) { return (vtlb_phys_t)(handler | POINTER_SIGN_BIT); }
static VTLB_INLINE int  vtlb_phys_is_handler(vtlb_phys_t v) { return v < 0; }
static VTLB_INLINE uptr vtlb_phys_ptr(vtlb_phys_t v) { return (uptr)v; }

static VTLB_INLINE vtlb_virt_t vtlb_virt_make(vtlb_phys_t phys, u32 paddr, u32 vaddr)
{
	if (vtlb_phys_is_handler(phys))
		return (vtlb_virt_t)phys + paddr - vaddr;
	return (vtlb_virt_t)phys - vaddr;
}
static VTLB_INLINE vtlb_virt_t vtlb_virt_from_ptr(uptr ptr, u32 vaddr) { return ptr - vaddr; }
static VTLB_INLINE int  vtlb_virt_is_handler(vtlb_virt_t v, u32 vaddr) { return (sptr)(v + vaddr) < 0; }
static VTLB_INLINE uptr vtlb_virt_ptr(vtlb_virt_t v, u32 vaddr) { return v + vaddr; }
static VTLB_INLINE u32  vtlb_virt_id(vtlb_virt_t v) { return (u8)v; }
static VTLB_INLINE u32  vtlb_virt_paddr(vtlb_virt_t v, u32 vaddr) { return (u32)((v + vaddr - (u8)v) & ~POINTER_SIGN_BIT); }
static VTLB_INLINE void* vtlb_virt_handler_raw(vtlb_virt_t v, int index, int write)
{
	return vtlbdata.RWFT[index][write][(u8)v];
}


typedef enum vtlb_ProtectionMode
{
	ProtMode_None = 0, 	// page is 'unaccounted' -- neither protected nor unprotected
	ProtMode_Write, 	// page is under write protection (exception handler)
	ProtMode_Manual, 	// page is under manual protection (self-checked at execution)
	ProtMode_NotRequired 	// page doesn't require any protection
} vtlb_ProtectionMode;

extern vtlb_ProtectionMode mmap_GetRamPageInfo(u32 paddr);
extern void mmap_MarkCountedRamPage(u32 paddr);
extern void mmap_ResetBlockTracking();
extern void mmap_UnprotectRamRange(u32 paddr, u32 size);

// --------------------------------------------------------------------------------------
//  Goemon game fix
// --------------------------------------------------------------------------------------
struct GoemonTlb {
	u32 valid;
	u32 unk1; // could be physical address also
	u32 unk2;
	u32 low_add;
	u32 physical_add;
	u32 unk3; // likely the size
	u32 high_add;
	u32 key; // uniq number attached to an allocation
	u32 unk5;
};
