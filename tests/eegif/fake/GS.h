/* tests/eegif: the GS CSR as the C++ GIF DMA reached it through GS.h,
 * written to the same word GSmmio.h's accessors write. Found ahead of the
 * real GS.h because the old source is compiled from this directory. */
#pragma once
#include "Common.h"
#include "Config.h"
#include "GSmmio.h"
enum CSR_FifoState
{
	CSR_FIFO_NORMAL = 0,
	CSR_FIFO_EMPTY,
	CSR_FIFO_FULL,
	CSR_FIFO_RESERVED
};
#define GS_CSR_FIFO    0x0000c000u
#define GS_CSR_FIFO_SH 14
static inline void gsCSRfield(u32 mask, u32 value)
{
	u32* w = (u32*)(g_RealGSMem + 0x1000);
	*w = (*w & ~mask) | (value & mask);
}
