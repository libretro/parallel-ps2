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

#include <retro_atomic.h>
#include "common/Pcsx2Defs.h"
#include <utility>
#include "VirtualMemory.h"
#include "HostMem.h"

#include "../common/Align.h"

#include <cinttypes>

// --------------------------------------------------------------------------------------
//  VirtualMemoryManager  (implementations)
// --------------------------------------------------------------------------------------

VirtualMemoryManager::VirtualMemoryManager(const char* file_mapping_name, uptr base, size_t size, uptr upper_bounds, bool strict)
	: m_file_handle(nullptr)
	, m_baseptr(0)
	, m_pageuse(nullptr)
	, m_pages_reserved(0)
{
	if (!size)
		return;

	size_t reserved_bytes = pcsx2_page_align(size);
	m_pages_reserved = reserved_bytes / __pagesize;

	if (file_mapping_name && file_mapping_name[0])
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = false;
		char real_file_mapping_name[128];
		host_shm_name(real_file_mapping_name, sizeof(real_file_mapping_name), file_mapping_name);
		m_file_handle = memshm_create(real_file_mapping_name, reserved_bytes);
		if (!m_file_handle)
			return;

		m_baseptr = static_cast<u8*>(memshm_map(m_file_handle, 0, (void*)base, reserved_bytes, host_prot(mode)));
		if (!m_baseptr || (upper_bounds != 0 && (((uptr)m_baseptr + reserved_bytes) > upper_bounds)))
		{
			host_munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			// Let's try again at an OS-picked memory area, and then hope it meets needed
			// boundschecking criteria below.
			if (base)
				m_baseptr = static_cast<u8*>(memshm_map(m_file_handle, 0, nullptr, reserved_bytes, host_prot(mode)));
		}
	}
	else
	{
		PageProtectionMode mode;
		mode.m_read  = true;
		mode.m_write = true;
		mode.m_exec  = true;
		m_baseptr    = static_cast<u8*>(host_mmap((void*)base, reserved_bytes, mode));

		if (!m_baseptr || (upper_bounds != 0 && (((uptr)m_baseptr + reserved_bytes) > upper_bounds)))
		{
			host_munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			// Let's try again at an OS-picked memory area, and then hope it meets needed
			// boundschecking criteria below.
			if (base)
				m_baseptr = static_cast<u8*>(host_mmap(0, reserved_bytes, mode));
		}
	}

	bool fulfillsRequirements = true;
	if (strict && (uptr)m_baseptr != base)
		fulfillsRequirements = false;
	if ((upper_bounds != 0) && ((uptr)(m_baseptr + reserved_bytes) > upper_bounds))
		fulfillsRequirements = false;
	if (!fulfillsRequirements)
	{
		if (m_file_handle)
		{
			if (m_baseptr)
				memshm_unmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;

			memshm_destroy(m_file_handle);
			m_file_handle = nullptr;
		}
		else
		{
			host_munmap(m_baseptr, reserved_bytes);
			m_baseptr = 0;
		}
	}

	if (!m_baseptr)
		return;

	m_pageuse = new retro_atomic_int_t[m_pages_reserved]();
}

VirtualMemoryManager::~VirtualMemoryManager()
{
	if (m_pageuse)
		delete[] m_pageuse;
	if (m_baseptr)
	{
		const size_t bytes = m_pages_reserved * __pagesize;
		if (m_file_handle)
		{
			memshm_unmap((void*)m_baseptr, bytes);
#ifndef _WIN32
			/* On POSIX, UnmapSharedMemory does not unmap: it drops the
			 * shared mapping by overlaying an anonymous PROT_NONE
			 * MAP_FIXED region, which keeps the address range reserved.
			 * That is what SharedMemoryMappingArea::Unmap wants - it
			 * frees one page out of an area it still owns - but this
			 * destructor owns the whole region and is giving it back, so
			 * the placeholder has to go too.  Without this the range
			 * stays in the address space as ---p for the life of the
			 * process, and every VM teardown leaks its full size. */
			host_munmap(m_baseptr, bytes);
#endif
		}
		else
			host_munmap(m_baseptr, bytes);
	}
	if (m_file_handle)
		memshm_destroy(m_file_handle);
}

static bool VMMMarkPagesAsInUse(retro_atomic_int_t* begin, retro_atomic_int_t* end)
{
	for (auto current = begin; current < end; current++)
	{
		if (!retro_atomic_cas_int(current, 0, 1))
		{
			// This was already allocated!  Undo the things we've set until this point
			while (--current >= begin)
			{
				// In the time we were doing this, someone set one of the things we just set to true back to false
				// This should never happen, but if it does we'll just stop and hope nothing bad happens
				if (!retro_atomic_cas_int(current, 1, 0))
					return false;
			}
			return false;
		}
	}
	return true;
}

u8* VirtualMemoryManager::Alloc(uptr offsetLocation, size_t size) const
{
	size = pcsx2_page_align(size);
	if (!(offsetLocation % __pagesize == 0))
		return nullptr;
	if (!(size + offsetLocation <= m_pages_reserved * __pagesize))
		return nullptr;
	if (m_baseptr == 0)
		return nullptr;
	auto puStart = &m_pageuse[offsetLocation / __pagesize];
	auto puEnd = &m_pageuse[(offsetLocation + size) / __pagesize];
	if (!(VMMMarkPagesAsInUse(puStart, puEnd)))
		return nullptr;
	return m_baseptr + offsetLocation;
}

void VirtualMemoryManager::Free(void* address, size_t size) const
{
	uptr offsetLocation = (uptr)address - (uptr)m_baseptr;
	if (!(offsetLocation % __pagesize == 0))
	{
		uptr newLoc = pcsx2_page_align(offsetLocation);
		size -= (offsetLocation - newLoc);
		offsetLocation = newLoc;
	}
	if (!(size % __pagesize == 0))
		size -= size % __pagesize;
	if (!(size + offsetLocation <= m_pages_reserved * __pagesize))
		return;
	auto puStart = &m_pageuse[offsetLocation / __pagesize];
	auto puEnd = &m_pageuse[(offsetLocation + size) / __pagesize];
	for (; puStart < puEnd; puStart++)
	{
		if (!retro_atomic_cas_int(puStart, 1, 0)) { }
	}
}

// --------------------------------------------------------------------------------------
//  VirtualMemoryBumpAllocator  (implementations)
// --------------------------------------------------------------------------------------
VirtualMemoryBumpAllocator::VirtualMemoryBumpAllocator(VirtualMemoryManagerPtr allocator, uptr offsetLocation, size_t size)
	: m_allocator(std::move(allocator))
{
	u8* base = m_allocator->Alloc(offsetLocation, size);
	retro_atomic_store_release_size(&m_basecursor, (size_t)base);
	m_endptr = base + size;
}

u8* VirtualMemoryBumpAllocator::Alloc(size_t size)
{
	if (retro_atomic_load_acquire_size(&m_basecursor) == 0) // True if constructed from bad VirtualMemoryManager (assertion was on initialization)
		return nullptr;

	size_t reservedSize = pcsx2_page_align(size);

	u8* out = (u8*)retro_atomic_fetch_add_size(&m_basecursor, reservedSize);

	return out;
}

// --------------------------------------------------------------------------------------
//  VirtualMemoryReserve  (implementations)
// --------------------------------------------------------------------------------------
VirtualMemoryReserve::VirtualMemoryReserve() { }
VirtualMemoryReserve::~VirtualMemoryReserve() { }

// Notes:
//  * This method should be called if the object is already in an released (unreserved) state.
//    Subsequent calls will be ignored, and the existing reserve will be returned.
//
// Parameters:
//   baseptr - the new base pointer that's about to be assigned
//   size - size of the region pointed to by baseptr
//
void VirtualMemoryReserve::Assign(VirtualMemoryManagerPtr allocator, u8* baseptr, size_t size)
{
	m_allocator = std::move(allocator);
	m_baseptr = baseptr;
	m_size = size;
}

u8* VirtualMemoryReserve::BumpAllocate(VirtualMemoryBumpAllocator& allocator, size_t size)
{
	u8* base = allocator.Alloc(size);
	if (base)
		Assign(allocator.GetAllocator(), base, size);

	return base;
}

void VirtualMemoryReserve::Release()
{
	if (!m_baseptr)
		return;

	m_allocator->Free(m_baseptr, m_size);
	m_allocator.reset();
	m_baseptr = nullptr;
	m_size = 0;
}

// --------------------------------------------------------------------------------------
//  RecompiledCodeReserve  (implementations)
// --------------------------------------------------------------------------------------

// Constructor!
// Parameters:
//   name - a nice long name that accurately describes the contents of this reserve.
RecompiledCodeReserve::RecompiledCodeReserve() : VirtualMemoryReserve() { }
RecompiledCodeReserve::~RecompiledCodeReserve() { Release(); }

// Anchor in this module's data segment. Recompiled code reaches objects like
// cpuRegs, psxRegs and tlb[] -- all of which live in the module image, not in
// the VM reservation -- and the module is a few megabytes wide, so the
// distance from any one of them stands in for all of them.
static u8 s_module_data_anchor;

// The emitters address module globals with RIP-relative operands, and both
// paths that build them degrade silently rather than failing:
//
//   ptr32[&global]  emits mov eax, [rip+disp32] when the target is within
//                   +-2GB of the emit cursor, and mov eax, [disp32] when it
//                   is not -- truncating the address to 32 bits.
//   xLEA/xMOV       same shape; xMOV(reg64, imm) had the identical problem
//                   until 4c941ed.
//
// So the entire recompiler rests on the code reservation landing within 2GB
// of the loaded module. Those are two independently placed regions -- one a
// runtime reservation, one wherever the loader put the image -- and nothing
// has ever checked that they end up close enough. If they do not, the failure
// is thousands of memory operands quietly reading and writing near address
// zero, which presents as arbitrary corruption rather than as an error.
//
// One comparison at reservation time turns that into a message. The anchor is
// one symbol, but the globals span the whole data section, and the operand is
// emitted from anywhere in the reservation -- so the check pads the anchor by
// MODULE_SPAN and tests the reservation's far corners, matching what actually
// has to encode rather than a single point-to-base distance.
#define MODULE_SPAN ((sptr)0x10000000) /* 256MB: over-estimate of the image */
static void CheckRipRelativeReach(const char* what, const u8* base, size_t size)
{
	const sptr anchor = (sptr)&s_module_data_anchor;
	const sptr lo = (sptr)base;
	const sptr hi = (sptr)base + (sptr)size;
	/* Worst pair: reservation far end to the low global, and the high
	 * global to the reservation base. */
	const sptr d_lo = lo - (anchor + MODULE_SPAN);
	const sptr d_hi = hi - (anchor - MODULE_SPAN);

	if (d_lo == (sptr)(s32)d_lo && d_hi == (sptr)(s32)d_hi)
		return;

	log_cb(RETRO_LOG_ERROR, "(%s) Code reservation at %p-%p is more than 2GB from this "
	              "module's data at %p. RIP-relative operands to module "
	              "globals cannot be encoded and will be silently truncated; "
	              "expect memory corruption.\n",
	              what, (void*)lo, (void*)hi, (void*)anchor);
}

void code_reserve_init(struct CodeReserve* r)
{
	r->baseptr   = NULL;
	r->size      = 0;
	r->allocator = NULL;
}

void code_reserve_assign(struct CodeReserve* r, const VirtualMemoryManager* allocator, size_t offset, size_t size)
{
	u8* base;

	/* Anything passed to the memory allocator must be page aligned. */
	size = pcsx2_page_align(size);

	/* Since the memory has already been allocated as part of the main memory
	 * map, this should never fail. */
	base = allocator->Alloc(offset, size);
	if (!base)
		log_cb(RETRO_LOG_ERROR, "(CodeReserve) Failed to allocate %zu bytes at offset %zu\n", size, offset);
	else
		CheckRipRelativeReach("CodeReserve", base, size);

	r->baseptr   = base;
	r->size      = size;
	r->allocator = allocator;
}

void code_reserve_release(struct CodeReserve* r)
{
	if (!r->baseptr)
		return;

	r->allocator->Free(r->baseptr, r->size);
	r->baseptr   = NULL;
	r->size      = 0;
	r->allocator = NULL;
}

void code_reserve_allow_modification(struct CodeReserve* r)
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = true;
	mprotect(r->baseptr, r->size, host_prot(pg));
}

void code_reserve_forbid_modification(struct CodeReserve* r)
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = false;
	mprotect(r->baseptr, r->size, host_prot(pg));
}

void RecompiledCodeReserve::Assign(VirtualMemoryManagerPtr allocator, size_t offset, size_t size)
{
	// Anything passed to the memory allocator must be page aligned.
	size = pcsx2_page_align(size);

	// Since the memory has already been allocated as part of the main memory map, this should never fail.
	u8* base = allocator->Alloc(offset, size);
	if (!base)
		log_cb(RETRO_LOG_ERROR, "(RecompiledCodeReserve) Failed to allocate %zu bytes at offset %zu\n", size, offset);
	else
		CheckRipRelativeReach("RecompiledCodeReserve", base, size);

	VirtualMemoryReserve::Assign(std::move(allocator), base, size);
}

void RecompiledCodeReserve::Reset()
{
}

void RecompiledCodeReserve::AllowModification()
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = true;
	mprotect(m_baseptr, m_size, host_prot(pg));
}

void RecompiledCodeReserve::ForbidModification()
{
	PageProtectionMode pg;
	pg.m_read  = true;
	pg.m_exec  = true;
	pg.m_write = false;
	mprotect(m_baseptr, m_size, host_prot(pg));
}
