/* The C++ the surface-search harness cannot avoid: the swizzle lives in
 * GSLocalMemory, whose format table is filled as the GS starts. The
 * memory it allocates is never touched here, so a plain allocation
 * stands in for the emulator's wrapped mapping, and GSClut, held by value,
 * gets an empty constructor pair. Everything else is in main.c. */
#include <cstdlib>

#include "GS/GSLocalMemory.h"
#include "GS/GSClut.h"
#include "GS/GSTables.h"
#include "GS/GSPsm.h"

void* GSAllocateWrappedMemory(size_t size, size_t repeat) { return calloc(size, repeat); }
void GSFreeWrappedMemory(void* ptr, size_t, size_t) { free(ptr); }
GSClut::GSClut(GSLocalMemory* mem) : m_mem(mem) {}
GSClut::~GSClut() {}

extern "C" {
int gss_init(void)
{
	static GSLocalMemory* mem;
	if (!mem)
	{
		/* (the tables and formats, as gs_util_init fills them when the GS
		 * starts) */
		gs_tables_init();
		gs_psm_init();
		mem = new GSLocalMemory();
	}
	return mem != NULL;
}

unsigned gss_bn(unsigned psm, int x, int y, unsigned bp, unsigned bw)
{
	return gs_swizzle_bn(&GSLocalMemory::m_psm[psm].info, x, y, bp, bw);
}

void gss_dims(unsigned psm, int* bs_x, int* bs_y, int* pgs_x, int* pgs_y)
{
	const GSLocalMemory::psm_t& p = GSLocalMemory::m_psm[psm];
	*bs_x  = p.bs.x;
	*bs_y  = p.bs.y;
	*pgs_x = p.pgs.x;
	*pgs_y = p.pgs.y;
}
}
