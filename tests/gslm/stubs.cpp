/* The emulator gives GSLocalMemory a 4MB block mapped four times in a row,
 * so an access that runs off the end lands back at the start rather than
 * faulting. A plain malloc would make the harness report faults the
 * emulator never sees -- and, worse, would make an out-of-bounds write
 * look like a crash instead of the silent corruption it really is. */
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstddef>
#include <cstdlib>

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	int fd = (int)syscall(SYS_memfd_create, "gslm", 0);
	unsigned char *base;
	size_t i;

	if (fd < 0)
		return NULL;
	if (ftruncate(fd, (off_t)size) != 0)
	{
		close(fd);
		return NULL;
	}

	base = (unsigned char *)mmap(NULL, size * repeat, PROT_NONE,
	                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
	{
		close(fd);
		return NULL;
	}

	for (i = 0; i < repeat; i++)
	{
		if (mmap(base + i * size, size, PROT_READ | PROT_WRITE,
		         MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED)
		{
			close(fd);
			return NULL;
		}
	}

	close(fd);
	return base;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	munmap(ptr, size * repeat);
}

/* GSLocalMemory holds a GSClut by value, so its constructor has to exist
 * for the object to be built -- but the real one pulls in the render
 * device and the whole renderer. The swizzle paths under test never touch
 * the palette, so an empty pair is enough to link and honest about what
 * is and is not being exercised. */
#include "GS/GSClut.h"
GSClut::GSClut(GSLocalMemory* mem) : m_mem(mem) {}
GSClut::~GSClut() {}

/* The tables and the format table are filled as the GS starts
 * (gs_util_init); there is no GS here, so they are filled before main. */
#include "GS/GSTables.h"
#include "GS/GSPsm.h"
static const struct gslm_tables
{
	gslm_tables()
	{
		gs_tables_init();
		gs_psm_init();
	}
} s_gslm_tables;
