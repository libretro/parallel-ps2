/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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
 * GS local memory's backing: size bytes mapped repeat times in a row, each
 * view the same memory, so an access that runs off the end of one lands
 * at the start of the next, as the console's address wrap does. All or
 * nothing: a mapping that cannot be made whole gives NULL, the views made
 * undone. One at a time (the GS makes its local memory once).
 */

#include <stddef.h>

#include "common/Pcsx2Defs.h"
#include "GSWrappedMemory.h"

#ifdef _WIN32

#include <windows.h>

/* (older SDKs: the Windows 10 placeholder flags) */
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

/* Windows 10's placeholder APIs, resolved at run time so the core still
 * loads on 8 and 8.1 (RedtapeWindows.h has why); the extended parameters
 * are never passed, so a pointer stands for their type */
typedef PVOID(WINAPI* gs_virtual_alloc2_fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef PVOID(WINAPI* gs_map_view3_fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
typedef BOOL(WINAPI* gs_unmap_view2_fn)(HANDLE, PVOID, ULONG);

static HANDLE s_fh;
/* the mapping was made with the placeholder APIs (else the Windows 8
 * fallback), for the free to undo it the same way */
static int s_fh_placeholder;

static int gs_wrapped_placeholders(gs_virtual_alloc2_fn* valloc2, gs_map_view3_fn* map3, gs_unmap_view2_fn* unmap2)
{
	HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
	*valloc2   = NULL;
	*map3      = NULL;
	*unmap2    = NULL;
	if (kb)
	{
		*valloc2 = (gs_virtual_alloc2_fn)(void*)GetProcAddress(kb, "VirtualAlloc2");
		*map3    = (gs_map_view3_fn)(void*)GetProcAddress(kb, "MapViewOfFile3");
		*unmap2  = (gs_unmap_view2_fn)(void*)GetProcAddress(kb, "UnmapViewOfFile2");
	}
	return *valloc2 && *map3 && *unmap2;
}

static void* gs_wrapped_placeholder_map(size_t size, size_t repeat, gs_virtual_alloc2_fn valloc2,
	gs_map_view3_fn map3, gs_unmap_view2_fn unmap2)
{
	/* the whole area reserved, split into one placeholder a view, and the
	 * one file mapped over each */
	u8* base = (u8*)valloc2(GetCurrentProcess(), NULL, repeat * size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
		PAGE_NOACCESS, NULL, 0);
	size_t i, j;

	if (!base)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to reserve VA space of size %u. WIN API ERROR:%u\n", (unsigned)(repeat * size),
			(unsigned)GetLastError());
		return NULL;
	}
	for (i = 0; i < repeat; i++)
	{
		u8* addr = base + i * size;
		if ((i != repeat - 1
				&& !VirtualFreeEx(GetCurrentProcess(), addr, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
			|| !map3(s_fh, GetCurrentProcess(), addr, 0, size, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to map repeat %u of size %u.\n", (unsigned)i, (unsigned)size);
			/* the views made so far, each at its own address */
			for (j = 0; j < i; j++)
				unmap2(GetCurrentProcess(), base + j * size, MEM_PRESERVE_PLACEHOLDER);
			VirtualFreeEx(GetCurrentProcess(), base, 0, MEM_RELEASE);
			return NULL;
		}
	}
	return base;
}

/* Windows 8 / 8.1: a hole found by reserving and releasing it, then the
 * view mapped into each slot (a window between the release and the maps
 * that nothing else allocates in: the GS sets up on one thread) */
static void* gs_wrapped_view_map(size_t size, size_t repeat)
{
	u8* base = (u8*)VirtualAlloc(NULL, repeat * size, MEM_RESERVE, PAGE_NOACCESS);
	size_t i, j;

	if (!base)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to reserve VA space of size %u. WIN API ERROR:%u\n", (unsigned)(repeat * size),
			(unsigned)GetLastError());
		return NULL;
	}
	VirtualFree(base, 0, MEM_RELEASE);
	for (i = 0; i < repeat; i++)
		if (!MapViewOfFileEx(s_fh, FILE_MAP_ALL_ACCESS, 0, 0, size, base + i * size))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to map repeat %u of size %u.\n", (unsigned)i, (unsigned)size);
			for (j = 0; j < i; j++)
				UnmapViewOfFile(base + j * size);
			return NULL;
		}
	return base;
}

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	gs_virtual_alloc2_fn valloc2;
	gs_map_view3_fn map3;
	gs_unmap_view2_fn unmap2;
	void* p;

	if (repeat == 0 || size > ((size_t)-1) / repeat)
		return NULL;
	s_fh = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((u64)size >> 32), (DWORD)size, NULL);
	if (!s_fh)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to create file mapping of size %u. WIN API ERROR:%u\n", (unsigned)size,
			(unsigned)GetLastError());
		return NULL;
	}

	s_fh_placeholder = gs_wrapped_placeholders(&valloc2, &map3, &unmap2);
	p = s_fh_placeholder ? gs_wrapped_placeholder_map(size, repeat, valloc2, map3, unmap2)
	                     : gs_wrapped_view_map(size, repeat);
	if (!p)
	{
		CloseHandle(s_fh);
		s_fh = NULL;
	}
	return p;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	size_t i;

	if (!ptr)
		return;
	if (s_fh_placeholder)
	{
		gs_virtual_alloc2_fn valloc2;
		gs_map_view3_fn map3;
		gs_unmap_view2_fn unmap2;
		gs_wrapped_placeholders(&valloc2, &map3, &unmap2);
		for (i = 0; i < repeat; i++)
			unmap2(GetCurrentProcess(), (u8*)ptr + i * size, MEM_PRESERVE_PLACEHOLDER);
		VirtualFreeEx(GetCurrentProcess(), ptr, 0, MEM_RELEASE);
	}
	else
	{
		/* (each view its own) */
		for (i = 0; i < repeat; i++)
			UnmapViewOfFile((u8*)ptr + i * size);
	}
	if (s_fh)
		CloseHandle(s_fh);
	s_fh = NULL;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
#endif

static int s_shm_fd = -1;

/* An unnamed shared memory file: a memfd where the kernel has one (the
 * libc wrapper is declared from Android API 30, the syscall is in every
 * kernel since Android 8; 1 is MFD_CLOEXEC), else a POSIX one unlinked
 * the moment it is open. Android has only the memfd: Bionic declares no
 * shm_open below API 26 and there is no /dev/shm. */
static int gs_wrapped_file(void)
{
	int fd;
#if (defined(__linux__) || defined(__ANDROID__)) && defined(__NR_memfd_create)
	fd = (int)syscall(__NR_memfd_create, "GS.mem", 1U);
	if (fd != -1 || errno != ENOSYS)
		return fd;
#elif defined(__ANDROID__)
	fd    = -1;
	errno = ENOSYS;
#endif
#ifndef __ANDROID__
	{
		/* (a name of this process's own: another instance holds its own) */
		char name[32];
		unsigned long pid = (unsigned long)getpid();
		int i = 0;
		memcpy(name, "/GS.mem.", 8);
		do
			name[8 + i++] = (char)('0' + pid % 10);
		while ((pid /= 10) && i < 20);
		name[8 + i] = '\0';
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd != -1)
			shm_unlink(name);
	}
#endif
	return fd;
}

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	u8* base;
	size_t i;

	if (repeat == 0 || size > ((size_t)-1) / repeat)
		return NULL;
	s_shm_fd = gs_wrapped_file();
	if (s_shm_fd == -1)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to make GS memory: %s\n", strerror(errno));
		return NULL;
	}
	if ((off_t)size < 0 || (size_t)(off_t)size != size || ftruncate(s_shm_fd, (off_t)size) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to size GS memory to %lu: %s\n", (unsigned long)size, strerror(errno));
		close(s_shm_fd);
		s_shm_fd = -1;
		return NULL;
	}

	/* the whole run reserved, then each view mapped over its slot */
	base = (u8*)mmap(NULL, size * repeat, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == (u8*)MAP_FAILED)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to reserve %lu bytes for GS memory: %s\n", (unsigned long)(size * repeat),
			strerror(errno));
		close(s_shm_fd);
		s_shm_fd = -1;
		return NULL;
	}
	for (i = 0; i < repeat; i++)
		if (mmap(base + i * size, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s_shm_fd, 0)
			!= (void*)(base + i * size))
		{
			log_cb(RETRO_LOG_ERROR, "Failed to map GS memory repeat %lu: %s\n", (unsigned long)i, strerror(errno));
			munmap(base, size * repeat);
			close(s_shm_fd);
			s_shm_fd = -1;
			return NULL;
		}
	return base;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	if (ptr)
		munmap(ptr, size * repeat);
	if (s_shm_fd >= 0)
		close(s_shm_fd);
	s_shm_fd = -1;
}

#endif
