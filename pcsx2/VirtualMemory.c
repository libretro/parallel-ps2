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

/* See VirtualMemory.h. C89. */
#include <stdlib.h>
#include <string.h>

#include <retro_atomic.h>
#include "common/Pcsx2Defs.h"
#include "VirtualMemory.h"
#include "HostMem.h"

#include "../common/Align.h"

static PageProtectionMode vmm_mode(bool write, bool exec)
{
	PageProtectionMode mode;
	mode.m_read  = true;
	mode.m_write = write;
	mode.m_exec  = exec;
	return mode;
}

bool vmm_init(VirtualMemoryManager* m, const char* file_mapping_name, size_t size)
{
	m->file_handle = NULL;
	m->baseptr     = NULL;
	m->size        = pcsx2_page_align(size);
	m->in_image    = false;
	m->pageuse     = (retro_atomic_int_t*)calloc(m->size / __pagesize, sizeof(*m->pageuse));
	if (!m->pageuse)
		return false;

	if (file_mapping_name && file_mapping_name[0])
	{
		char real_file_mapping_name[128];
		host_shm_name(real_file_mapping_name, sizeof(real_file_mapping_name), file_mapping_name);
		m->file_handle = memshm_create(real_file_mapping_name, m->size);
		if (!m->file_handle)
		{
			free(m->pageuse);
			m->pageuse = NULL;
			return false;
		}
	}
	return true;
}

/* Exactly at base, or NULL. Where the kernel can refuse an occupied range
 * outright that is one call; elsewhere the hint is checked and undone. */
static u8* vmm_map_at(const VirtualMemoryManager* m, uptr base)
{
	void* p;
#if defined(__linux__) && defined(MAP_FIXED_NOREPLACE)
	/* The backing's handle is its descriptor (memmap.h). */
	if (m->file_handle)
		p = mmap((void*)base, m->size, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_FIXED_NOREPLACE, (int)(intptr_t)m->file_handle, 0);
	else
		p = mmap((void*)base, m->size, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	/* Kernels before 4.17 take the flag as a plain hint. */
	if ((uptr)p != base)
	{
		munmap(p, m->size);
		return NULL;
	}
	return (u8*)p;
#else
	if (!m->file_handle)
		return (u8*)host_mmap((void*)base, m->size, vmm_mode(true, true));
	p = memshm_map(m->file_handle, 0, (void*)base, m->size, host_prot(vmm_mode(true, false)));
	if (p && (uptr)p != base)
	{
		memshm_unmap(p, m->size);
		p = NULL;
	}
	return (u8*)p;
#endif
}

bool vmm_map(VirtualMemoryManager* m, uptr base)
{
	if (m->baseptr || !m->pageuse)
		return false;
	if (base)
		m->baseptr = vmm_map_at(m, base);
	else if (m->file_handle)
		m->baseptr = (u8*)memshm_map(m->file_handle, 0, NULL, m->size, host_prot(vmm_mode(true, false)));
	else
		m->baseptr = (u8*)host_mmap(NULL, m->size, vmm_mode(true, true));
	return m->baseptr != NULL;
}

#ifdef __linux__
/* Gives the space back to the image as the zeroed memory it was. */
static void vmm_restore_image(u8* space, size_t size)
{
	mmap(space, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
}

bool vmm_map_in_image(VirtualMemoryManager* m, u8* space)
{
	void* p;
	if (m->baseptr || !m->pageuse)
		return false;
	if (m->file_handle)
		p = mmap(space, m->size, PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_FIXED, (int)(intptr_t)m->file_handle, 0);
	else
		p = mmap(space, m->size, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED)
	{
		/* A failed fixed mapping can leave the range unmapped */
		vmm_restore_image(space, m->size);
		return false;
	}
	m->baseptr  = (u8*)p;
	m->in_image = true;
	return true;
}
#endif

void vmm_unmap(VirtualMemoryManager* m)
{
	if (!m->baseptr)
		return;
#ifdef __linux__
	if (m->in_image)
		vmm_restore_image(m->baseptr, m->size);
	else
#endif
	if (m->file_handle)
		memshm_unmap(m->baseptr, m->size);
	else
		host_munmap(m->baseptr, m->size);
	m->baseptr  = NULL;
	m->in_image = false;
	memset(m->pageuse, 0, (m->size / __pagesize) * sizeof(*m->pageuse));
}

void vmm_destroy(VirtualMemoryManager* m)
{
	vmm_unmap(m);
	if (m->file_handle)
		memshm_destroy(m->file_handle);
	m->file_handle = NULL;
	free(m->pageuse);
	m->pageuse = NULL;
}

/* Claims [begin, end); on a page already claimed, gives back what this call
 * claimed and fails. */
static bool vmm_claim_pages(retro_atomic_int_t* begin, retro_atomic_int_t* end)
{
	retro_atomic_int_t* current;
	for (current = begin; current < end; current++)
	{
		if (!retro_atomic_cas_int(current, 0, 1))
		{
			while (--current >= begin)
				retro_atomic_cas_int(current, 1, 0);
			return false;
		}
	}
	return true;
}

u8* vmm_alloc(const VirtualMemoryManager* m, uptr offset, size_t size)
{
	size = pcsx2_page_align(size);
	if (offset % __pagesize != 0 || size + offset > m->size || !m->baseptr)
		return NULL;
	if (!vmm_claim_pages(&m->pageuse[offset / __pagesize], &m->pageuse[(offset + size) / __pagesize]))
		return NULL;
	return m->baseptr + offset;
}

void vmm_free(const VirtualMemoryManager* m, void* address, size_t size)
{
	retro_atomic_int_t* page;
	retro_atomic_int_t* end;
	uptr offset = (uptr)address - (uptr)m->baseptr;
	if (offset % __pagesize != 0)
	{
		uptr aligned = pcsx2_page_align(offset);
		size -= aligned - offset;
		offset = aligned;
	}
	size -= size % __pagesize;
	if (size + offset > m->size)
		return;
	end = &m->pageuse[(offset + size) / __pagesize];
	for (page = &m->pageuse[offset / __pagesize]; page < end; page++)
		retro_atomic_cas_int(page, 1, 0);
}

void vmm_bump_init(VirtualMemoryBumpAllocator* b, const VirtualMemoryManager* m, uptr offset, size_t size)
{
	retro_atomic_store_release_size(&b->cursor, (size_t)vmm_alloc(m, offset, size));
}

u8* vmm_bump_alloc(VirtualMemoryBumpAllocator* b, size_t size)
{
	if (retro_atomic_load_acquire_size(&b->cursor) == 0)
		return NULL;
	return (u8*)retro_atomic_fetch_add_size(&b->cursor, pcsx2_page_align(size));
}

void code_reserve_init(struct CodeReserve* r)
{
	r->baseptr   = NULL;
	r->size      = 0;
	r->allocator = NULL;
}

void code_reserve_assign(struct CodeReserve* r, const VirtualMemoryManager* allocator, size_t offset, size_t size)
{
	/* Anything passed to the memory allocator must be page aligned. */
	size = pcsx2_page_align(size);

	/* The code map is already reserved, so this fails only on overlap. */
	r->baseptr = vmm_alloc(allocator, offset, size);
	if (!r->baseptr)
		log_cb(RETRO_LOG_ERROR, "(CodeReserve) Failed to allocate %u bytes at offset %u\n",
				(unsigned)size, (unsigned)offset);
	r->size      = size;
	r->allocator = allocator;
}

void code_reserve_release(struct CodeReserve* r)
{
	if (!r->baseptr)
		return;

	vmm_free(r->allocator, r->baseptr, r->size);
	r->baseptr   = NULL;
	r->size      = 0;
	r->allocator = NULL;
}

void code_reserve_allow_modification(struct CodeReserve* r)
{
	mprotect(r->baseptr, r->size, host_prot(vmm_mode(true, true)));
}

void code_reserve_forbid_modification(struct CodeReserve* r)
{
	mprotect(r->baseptr, r->size, host_prot(vmm_mode(false, true)));
}
