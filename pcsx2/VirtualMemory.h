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

#include <stddef.h>
#include <retro_atomic.h>
#include "../common/General.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 *  VirtualMemoryManager
 * --------------------------------------------------------------------------
 * One block of address space the VM's memory or recompiled code lives in.
 * vmm_init sets up the bookkeeping and, given a name, the shared-memory
 * backing (the main map is backed so fastmem can alias it); vmm_map then puts
 * the block at an address, and can be retried at another without recreating
 * the backing. Pieces of a mapped block are handed out with vmm_alloc. */
typedef struct VirtualMemoryManager
{
	void* file_handle;           /* shared-memory backing, or NULL: anonymous RWX */
	u8* baseptr;                 /* NULL while unmapped */
	retro_atomic_int_t* pageuse; /* one flag per page, set while handed out */
	size_t size;                 /* page-aligned */
	bool in_image;               /* mapped over room in the module's own image */
} VirtualMemoryManager;

bool vmm_init(VirtualMemoryManager* m, const char* file_mapping_name, size_t size);
/* Maps the block at exactly base, or anywhere when base is 0. */
bool vmm_map(VirtualMemoryManager* m, uptr base);
#ifdef __linux__
/* Maps the block over space, page-aligned room the module owns such as a
 * static array; it is handed back as plain memory when unmapped. */
bool vmm_map_in_image(VirtualMemoryManager* m, u8* space);
#endif
void vmm_unmap(VirtualMemoryManager* m);
void vmm_destroy(VirtualMemoryManager* m);

/* The size bytes at offset (both page-aligned) of a mapped block, or NULL
 * when any of those pages is already handed out. */
u8* vmm_alloc(const VirtualMemoryManager* m, uptr offset, size_t size);
void vmm_free(const VirtualMemoryManager* m, void* address, size_t size);

#define vmm_is_ok(m) ((m)->baseptr != NULL)

/* --------------------------------------------------------------------------
 *  Bump allocator: the tail of a block, for allocations with no fixed spot
 * -------------------------------------------------------------------------- */
typedef struct VirtualMemoryBumpAllocator
{
	retro_atomic_size_t cursor; /* next free address; 0 when the block was not mapped */
} VirtualMemoryBumpAllocator;

void vmm_bump_init(VirtualMemoryBumpAllocator* b, const VirtualMemoryManager* m, uptr offset, size_t size);
u8* vmm_bump_alloc(VirtualMemoryBumpAllocator* b, size_t size);

/* --------------------------------------------------------------------------
 *  Code reserve
 * --------------------------------------------------------------------------
 * A recompiled-code reserve is a sequential-growth block of executable memory
 * carved out of the code map. Every code reserve is owned by a recompiler
 * that is torn down before the VM's memory map, so the reserve never
 * outlives its manager. */
struct CodeReserve
{
	u8*    baseptr;
	size_t size;
	const VirtualMemoryManager* allocator;
};

void code_reserve_init(struct CodeReserve* r);
void code_reserve_assign(struct CodeReserve* r, const VirtualMemoryManager* allocator, size_t offset, size_t size);
void code_reserve_release(struct CodeReserve* r);
void code_reserve_allow_modification(struct CodeReserve* r);
void code_reserve_forbid_modification(struct CodeReserve* r);

#ifdef __cplusplus
}
#endif
