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

#include "../common/Pcsx2Defs.h"

/* The sizes as macros, so a C translation unit can read them; the scoped
 * constants below keep every existing Ps2MemSize:: user working. */
#define PS2MEM_MAIN_RAM      (_32mb)
#define PS2MEM_ROM           (_1mb * 4)
#define PS2MEM_ROM1          (_1mb * 4)
#define PS2MEM_ROM2          (0x00080000)
#define PS2MEM_HARDWARE      (_64kb)
#define PS2MEM_SCRATCH       (_16kb)
#define PS2MEM_IOP_RAM       (_1mb * 2)
#define PS2MEM_IOP_HARDWARE  (_64kb)
#define PS2MEM_GS_REGS       (0x00002000)

#ifdef __cplusplus
namespace Ps2MemSize
{
	static const uint MainRam     = PS2MEM_MAIN_RAM;
	static const uint Rom         = PS2MEM_ROM;
	static const uint Rom1        = PS2MEM_ROM1;
	static const uint Rom2        = PS2MEM_ROM2;
	static const uint Hardware    = PS2MEM_HARDWARE;
	static const uint Scratch     = PS2MEM_SCRATCH;
	static const uint IopRam      = PS2MEM_IOP_RAM;
	static const uint IopHardware = PS2MEM_IOP_HARDWARE;
	static const uint GSregs      = PS2MEM_GS_REGS;
}
#endif


typedef u8 mem8_t;
typedef u16 mem16_t;
typedef u32 mem32_t;
typedef u64 mem64_t;
typedef u128 mem128_t;

typedef struct EEVM_MemoryAllocMess
{
	u8 Main[PS2MEM_MAIN_RAM];			/* Main memory (hard-wired to 32MB) */
	u8 Scratch[PS2MEM_SCRATCH];		/* Scratchpad! */
	u8 ROM[PS2MEM_ROM];				/* Boot rom (4MB) */
	u8 ROM1[PS2MEM_ROM1];				/* DVD player (4MB) */
	u8 ROM2[PS2MEM_ROM2];				/* Chinese extensions */

	/* Two 1 megabyte (max DMA) buffers for reading and writing to high memory (>32MB). */
	/* Such accesses are not documented as causing bus errors but as the memory does */
	/* not exist, reads should continue to return 0 and writes should be discarded. */
	/* Probably. */

	u8 ZeroRead[_1mb];
	u8 ZeroWrite[_1mb];
} EEVM_MemoryAllocMess;

typedef struct IopVM_MemoryAllocMess
{
	u8 Main[PS2MEM_IOP_RAM];			/* Main memory (hard-wired to 2MB) */
	u8 P[_64kb];							/* I really have no idea what this is... --air */
	u8 Sif[0x100];							/* a few special SIF/SBUS registers (likely not needed) */
} IopVM_MemoryAllocMess;


/* DevNote: EE and IOP hardware registers are done as a static array instead of a pointer in */
/* order to allow for simpler macros and reference handles to be defined  (we can safely use */
/* compile-time references to registers instead of having to use instance variables). */

/* MSVC decorates C++ data symbols; the ELF ABI does not. A variable a C
 * unit references therefore has to be declared -- and so, through this
 * header, defined -- with C language linkage, or the Windows link fails
 * on an undecorated reference while every other target is fine. */
#ifdef __cplusplus
extern "C" {
#endif
	PCSX2_ALIGN(__pagealignsize) extern u8 eeHw[PS2MEM_HARDWARE];
	PCSX2_ALIGN(__pagealignsize) extern u8 iopHw[PS2MEM_IOP_HARDWARE];
	extern EEVM_MemoryAllocMess* eeMem;
	extern IopVM_MemoryAllocMess* iopMem;
#ifdef __cplusplus
}
#endif
