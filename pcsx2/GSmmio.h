/* The EE's view of the GS privileged registers: their storage, the CSR
 * accessors the EE, GIF and GS threads share, and the handlers the vtlb
 * maps over 0x12000000 (GS.c). C, so Memory.c and Gif.c reach them;
 * GS.h includes this. */
#pragma once

#include "MemoryTypes.h"
#include "../common/SingleRegisterTypes.h"

#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MSVC decorates C++ data symbols, so the register storage carries C
 * linkage too. */
PCSX2_ALIGN(16) extern u8 g_RealGSMem[PS2MEM_GS_REGS];

#define PS2MEM_GS g_RealGSMem
#define PS2GS_BASE(mem) (PS2MEM_GS + ((mem) & 0x13ff))

extern void gsWrite8(u32 mem, u8 value);
extern void gsWrite16(u32 mem, u16 value);
extern void gsWrite32(u32 mem, u32 value);

extern void gsWrite64_page_00(u32 mem, u64 value);
extern void gsWrite64_page_01(u32 mem, u64 value);
extern void gsWrite64_generic(u32 mem, u64 value);

#if PCSX2_MINGW_R128_BY_PTR
extern void gsWrite128_page_01(u32 mem, const r128* value);
extern void gsWrite128_generic(u32 mem, const r128* value);
#else
extern void TAKES_R128 gsWrite128_page_01(u32 mem, r128 value);
extern void TAKES_R128 gsWrite128_generic(u32 mem, r128 value);
#endif

extern u8 gsRead8(u32 mem);
extern u16 gsRead16(u32 mem);
extern u32 gsRead32(u32 mem);
extern u64 gsRead64(u32 mem);

#ifdef __cplusplus
}
#endif

enum CSR_FifoState
{
	CSR_FIFO_NORMAL = 0, /* Somewhere in between (neither empty nor almost full) */
	CSR_FIFO_EMPTY, /* Empty */
	CSR_FIFO_FULL, /* Almost full */
	CSR_FIFO_RESERVED /* Reserved / unused */
};

/* The GS CSR is written by the EE and GIF threads - a status bit here, a
 * FIFO level there - and read by the GS thread, which takes the FIELD bit
 * once per vsync as the pageflip id.  ThreadSanitizer, over four
 * load/unload cycles: MTGS::MainLoop against rcntUpdate, on this word.
 *
 * The writes were bit-field assignments and, for FIELD, |= and ^= on the
 * whole word - read-modify-writes with no atomicity, so two threads
 * updating different bits could lose one of them outright, quite apart
 * from the reader seeing a half-written value.
 *
 * These accessors are the only way the register should be touched from
 * code that can run on more than one thread: a mask per field, an atomic
 * read-modify-write per update, and one acquire load for readers.
 * Multi-bit fields go through a CAS loop because the value has to be
 * spliced into bits the other threads own. */
#define GS_CSR_SIGNAL   0x00000001u
#define GS_CSR_FINISH   0x00000002u
#define GS_CSR_HSINT    0x00000004u
#define GS_CSR_VSINT    0x00000008u
#define GS_CSR_EDWINT   0x00000010u
#define GS_CSR_FLUSH    0x00000100u
#define GS_CSR_RESET    0x00000200u
#define GS_CSR_NFIELD   0x00001000u
#define GS_CSR_FIELD    0x00002000u
#define GS_CSR_FIFO     0x0000c000u
#define GS_CSR_FIFO_SH  14
#define GS_CSR_REV      0x00ff0000u
#define GS_CSR_REV_SH   16
#define GS_CSR_ID       0xff000000u
#define GS_CSR_ID_SH    24

static __fi retro_atomic_int_t* gsCSRword(void)
{
	return (retro_atomic_int_t*)(PS2MEM_GS + 0x1000);
}

static __fi u32 gsCSRload(void)
{
	return (u32)retro_atomic_load_acquire_int(gsCSRword());
}

static __fi void gsCSRset(u32 mask)
{
	retro_atomic_fetch_or_int(gsCSRword(), (int)mask);
}

static __fi void gsCSRclear(u32 mask)
{
	retro_atomic_fetch_and_int(gsCSRword(), (int)~mask);
}

/* No fetch_xor in the atomics header; CAS is the portable spelling and
 * this runs once per vsync. */
static __fi void gsCSRflip(u32 mask)
{
	for (;;)
	{
		const int old_v = retro_atomic_load_acquire_int(gsCSRword());
		const int new_v = (int)((u32)old_v ^ mask);
		if (retro_atomic_cas_int(gsCSRword(), old_v, new_v))
			return;
	}
}

/* Replace the bits under `mask' with `value', already shifted. */
static __fi void gsCSRfield(u32 mask, u32 value)
{
	for (;;)
	{
		const int old_v = retro_atomic_load_acquire_int(gsCSRword());
		const int new_v = (int)(((u32)old_v & ~mask) | (value & mask));
		if (retro_atomic_cas_int(gsCSRword(), old_v, new_v))
			return;
	}
}

/* IMR's mask bits and SIGLBLID's two words, as C reaches them (GS.h has
 * the C++ register views). */
#define GS_IMR_WORD      (*(u32*)(PS2MEM_GS + 0x1010))
#define GS_IMR_SIGMSK    (1u << 8)
#define GS_IMR_FINISHMSK (1u << 9)
#define GS_SIGID_WORD    (*(u32*)(PS2MEM_GS + 0x1080))
#define GS_LBLID_WORD    (*(u32*)(PS2MEM_GS + 0x1084))
