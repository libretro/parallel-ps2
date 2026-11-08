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

#include "MemoryTypes.h"

#ifdef __cplusplus
#include <string>
#endif


#define psxSs8(mem)	iopMem->Sif[(mem) & 0x00ff]
#define psxSs16(mem)	(*(s16*)&iopMem->Sif[(mem) & 0x00ff])
#define psxSs32(mem)	(*(s32*)&iopMem->Sif[(mem) & 0x00ff])
#define psxSu8(mem)	(*(u8*) &iopMem->Sif[(mem) & 0x00ff])
#define psxSu16(mem)	(*(u16*)&iopMem->Sif[(mem) & 0x00ff])
#define psxSu32(mem)	(*(u32*)&iopMem->Sif[(mem) & 0x00ff])

#define psxPs8(mem)	iopMem->P[(mem) & 0xffff]
#define psxPs16(mem)	(*(s16*)&iopMem->P[(mem) & 0xffff])
#define psxPs32(mem)	(*(s32*)&iopMem->P[(mem) & 0xffff])
#define psxPu8(mem)	(*(u8*) &iopMem->P[(mem) & 0xffff])
#define psxPu16(mem)	(*(u16*)&iopMem->P[(mem) & 0xffff])
#define psxPu32(mem)	(*(u32*)&iopMem->P[(mem) & 0xffff])

#define psxHs8(mem)	iopHw[(mem) & 0xffff]
#define psxHs16(mem)	(*(s16*)&iopHw[(mem) & 0xffff])
#define psxHs32(mem)	(*(s32*)&iopHw[(mem) & 0xffff])
#define psxHu8(mem)	(*(u8*) &iopHw[(mem) & 0xffff])
#define psxHu16(mem)	(*(u16*)&iopHw[(mem) & 0xffff])
#define psxHu32(mem)	(*(u32*)&iopHw[(mem) & 0xffff])

#ifdef __cplusplus
extern "C" {
#endif

extern const uptr *psxMemRLUT;
extern u8   iopMemRead8_slow (u32 mem);
extern u16  iopMemRead16_slow(u32 mem);

/* Inline fast paths for 8- and 16-bit IOP reads, same split as the 32-bit
 * one below. The excluded pages differ per width and are taken from each
 * function's own control flow rather than assumed to match:
 *
 *   read8    skips 0x1f80 (hardware) and 0x1f40 (the extra dev page it
 *            handles before the LUT lookup); no SBUS check
 *   read16   skips 0x1f80 and, after the lookup, 0x1d00 -- as read32 does
 *
 * Getting these two confused would produce a fast path that silently
 * bypassed a hardware register, so tests/iopmem_split.c checks each
 * predicate against its own original. */
static __fi u8 iopMemRead8(u32 mem)
{
	mem &= 0x1fffffff;
	{
		const u32 t = mem >> 16;
		if (t != 0x1f80 && t != 0x1f40)
		{
			const u8* const p = (const u8*)(psxMemRLUT[t]);
			if (p != NULL)
				return *(const u8*)(p + (mem & 0xffff));
		}
	}
	return iopMemRead8_slow(mem);
}

static __fi u16 iopMemRead16(u32 mem)
{
	mem &= 0x1fffffff;
	{
		const u32 t = mem >> 16;
		if (t != 0x1f80 && t != 0x1d00)
		{
			const u8* const p = (const u8*)(psxMemRLUT[t]);
			if (p != NULL)
				return *(const u16*)(p + (mem & 0xffff));
		}
	}
	return iopMemRead16_slow(mem);
}
extern u32  iopMemRead32_slow(u32 mem);

/* Inline fast path for IOP 32-bit reads.
 *
 * Every guest load that is not a hardware register comes through here, and
 * out of line it costs a call plus the compiler's inability to see that the
 * LUT entry it just loaded is the one it is about to dereference. The
 * conditions below are exactly the ones the out-of-line version tests before
 * reaching its `return *(const u32 *)(p + (mem & 0xffff));`:
 *
 *   t != 0x1f80   not the hardware page
 *   t != 0x1d00   not the SBUS register window, which aliases into psHu32
 *   p != NULL     the page is mapped
 *
 * Anything else falls through to iopMemRead32_slow, which re-masks harmlessly
 * and handles the rest unchanged. */
static __fi u32 iopMemRead32(u32 mem)
{
	mem &= 0x1fffffff;
	{
		const u32 t = mem >> 16;
		if (t != 0x1f80 && t != 0x1d00)
		{
			const u8* const p = (const u8*)(psxMemRLUT[t]);
			if (p != NULL)
				return *(const u32*)(p + (mem & 0xffff));
		}
	}
	return iopMemRead32_slow(mem);
}
extern void iopMemWrite8 (u32 mem, u8 value);
extern void iopMemWrite16(u32 mem, u16 value);
extern void iopMemWrite32(u32 mem, u32 value);

#ifdef __cplusplus
std::string iopMemReadString(u32 mem, int maxlen = 65536);
#endif
/* C89-callable variant: fills dst (NUL-terminated, cap includes the NUL). */
void iopMemReadStringBuf(char* dst, int cap, u32 mem, int maxlen);

/* IOP main memory in the main map: points iopMem at it and allocates the
 * LUTs, clears and maps it, and hands it back. */
struct VirtualMemoryManager;
extern void iop_memory_assign(const struct VirtualMemoryManager* main_map);
extern void iop_memory_reset(void);
extern void iop_memory_release(const struct VirtualMemoryManager* main_map);

#ifdef __cplusplus
}
#endif

/* The IOP hardware register handlers (ps2/Iop/IopHwRead.c, IopHwWrite.c):
 * the EE's vtlb maps them and the IOP's memory map calls them. */
#ifdef __cplusplus
extern "C" {
#endif
	extern mem8_t iopHwRead8_generic( u32 addr );
	extern mem16_t iopHwRead16_generic( u32 addr );
	extern mem32_t iopHwRead32_generic( u32 addr );
	extern void iopHwWrite8_generic( u32 addr, mem8_t val );
	extern void iopHwWrite16_generic( u32 addr, mem16_t val );
	extern void iopHwWrite32_generic( u32 addr, mem32_t val );


	extern mem8_t iopHwRead8_Page1( u32 iopaddr );
	extern mem8_t iopHwRead8_Page3( u32 iopaddr );
	extern mem8_t iopHwRead8_Page8( u32 iopaddr );
	extern mem16_t iopHwRead16_Page1( u32 iopaddr );
	extern mem16_t iopHwRead16_Page3( u32 iopaddr );
	extern mem16_t iopHwRead16_Page8( u32 iopaddr );
	extern mem32_t iopHwRead32_Page1( u32 iopaddr );
	extern mem32_t iopHwRead32_Page3( u32 iopaddr );
	extern mem32_t iopHwRead32_Page8( u32 iopaddr );

	extern void iopHwWrite8_Page1( u32 iopaddr, mem8_t data );
	extern void iopHwWrite8_Page3( u32 iopaddr, mem8_t data );
	extern void iopHwWrite8_Page8( u32 iopaddr, mem8_t data );
	extern void iopHwWrite16_Page1( u32 iopaddr, mem16_t data );
	extern void iopHwWrite16_Page3( u32 iopaddr, mem16_t data );
	extern void iopHwWrite16_Page8( u32 iopaddr, mem16_t data );
	extern void iopHwWrite32_Page1( u32 iopaddr, mem32_t data );
	extern void iopHwWrite32_Page3( u32 iopaddr, mem32_t data );
	extern void iopHwWrite32_Page8( u32 iopaddr, mem32_t data );
#ifdef __cplusplus
}
#endif
