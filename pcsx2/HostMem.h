/*
 * HostMem - the core's policy over libretro-common's memmap.
 *
 * This replaces common/HostSys.h's memory functions. The primitives are
 * libretro-common's -- mmap/munmap/mprotect through memmap's shims,
 * memreserve_at/memcommit/memrelease, memshm_*, memjit_*, memsync -- and
 * what is here is only what a core decides for itself:
 *
 *   - MAP_JIT on Apple Silicon for a region that will hold code. It has
 *     to be given at mmap time; a plain region cannot be made RWX later.
 *   - A hint that the kernel did not honour is a failure, not a silent
 *     relocation: the recompilers assume the address they asked for.
 *   - PageProtectionMode to PROT_ bits, the same way HostSys did it: a
 *     write-only mode gets PROT_WRITE alone, exec needs read.
 *
 * Nothing in this header is a class over a primitive, and nothing in it
 * lives in common/.
 */

#ifndef PCSX2_HOSTMEM_H
#define PCSX2_HOSTMEM_H

#include <stdio.h>
#include <memmap.h>
#include <retro_inline.h>
#include "common/General.h"

#if defined(_WIN32)
/* NOMINMAX first: the max macro would break every
 * std::numeric_limits<>::max() in a C++ unit including this one. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef MAP_JIT
#define MAP_JIT 0
#endif

static INLINE int host_prot(PageProtectionMode m)
{
	int ret = 0;
	if (m.m_read)
	{
		ret |= PROT_READ;
		if (m.m_exec)
			ret |= PROT_EXEC;
	}
	if (m.m_write)
		ret |= PROT_WRITE;
	return ret;
}

/* Anonymous private memory, committed, at a hint. NULL if the mode has
 * neither read nor write, or if a hint was given and not honoured. */
static INLINE void* host_mmap(void* hint, size_t size, PageProtectionMode m)
{
	void* p;
	if (!m.m_read && !m.m_write)
		return NULL;
#if defined(_WIN32)
	/* memmap's mmap shim on Windows is a file mapping with no hint;
	 * a reservation at the hint, committed and protected, is the same
	 * thing HostSys's VirtualAlloc(MEM_RESERVE | MEM_COMMIT) was. */
	p = memreserve_at(hint, size);
	if (!p)
		return NULL;
	if (hint && p != hint)
	{
		memrelease(p, size);
		return NULL;
	}
	if (!memcommit(p, size))
	{
		memrelease(p, size);
		return NULL;
	}
	mprotect(p, size, host_prot(m));
	return p;
#else
	{
		int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__APPLE__) && defined(__aarch64__)
		if (m.m_read && m.m_exec)
			flags |= MAP_JIT;
#endif
		p = mmap(hint, size, host_prot(m), flags, -1, 0);
		if (p == MAP_FAILED)
			return NULL;
		if (hint && p != hint)
		{
			munmap(p, size);
			return NULL;
		}
		return p;
	}
#endif
}

static INLINE void host_munmap(void* p, size_t size)
{
	if (!p)
		return;
#if defined(_WIN32)
	memrelease(p, size);
#else
	munmap(p, size);
#endif
}

/* A shared-memory name unique to this process. FreeBSD's shm_open needs
 * an absolute path; the others take a bare name. */
static INLINE void host_shm_name(char* buf, size_t len, const char* prefix)
{
#if defined(_WIN32)
	const unsigned pid = (unsigned)GetCurrentProcessId();
	snprintf(buf, len, "%s_%u", prefix, pid);
#elif defined(__FreeBSD__)
	const unsigned pid = (unsigned)getpid();
	snprintf(buf, len, "/tmp/%s_%u", prefix, pid);
#else
	const unsigned pid = (unsigned)getpid();
	snprintf(buf, len, "%s_%u", prefix, pid);
#endif
}

#endif
