/* What GSLocalMemory needs of the emulator: its 4MB mapped four times in a
 * row (an access past the end lands at the start, as in the emulator), and
 * the palette paletted texels read through, filled with known colours.
 * The same for the C local memory and the C++ class it replaced. */
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstddef>
#include <cstdlib>

#include "GS/GSLocalMemory.h"

/* (C++ in the oracle: the class it builds called them so) */
#ifdef OLD
#define WRAPPED
#else
#define WRAPPED extern "C"
#endif

WRAPPED void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	int fd = (int)syscall(SYS_memfd_create, "gslmpixel", 0);
	unsigned char* base;
	size_t i;

	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)size) != 0)
	{
		close(fd);
		return NULL;
	}
	base = (unsigned char*)mmap(NULL, size * repeat, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
	{
		close(fd);
		return NULL;
	}
	for (i = 0; i < repeat; i++)
		if (mmap(base + i * size, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
		{
			close(fd);
			return NULL;
		}
	close(fd);
	return base;
}

WRAPPED void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	munmap(ptr, size * repeat);
}

static u32 s_palette[512];

GSClut::GSClut(GSLocalMemory* mem)
	: m_mem(mem)
{
	for (u32 i = 0; i < 512; i++)
		s_palette[i] = i * 0x9E3779B9u ^ 0x5A5A5A5Au;
	m_c.buff32 = s_palette;
}
GSClut::~GSClut() {}

#include "GS/GSTables.h"
#include "GS/GSPsm.h"
static const struct gslmpixel_tables
{
	gslmpixel_tables()
	{
		gs_tables_init();
		gs_psm_init();
	}
} s_tables;
