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

#include "VU.h"
#include "VUops.h"
#include "R5900.h"

/* Macros rather than static const: in C a const object is not a constant
 * expression, so MSVC refuses the masks below as initializers. */
#define VU0_MEMSIZE	0x1000u		/* 4kb */
#define VU0_PROGSIZE	0x1000u		/* 4kb */
#define VU1_MEMSIZE	0x4000u		/* 16kb */
#define VU1_PROGSIZE	0x4000u		/* 16kb */

#define VU0_MEMMASK	(VU0_MEMSIZE-1)
#define VU0_PROGMASK	(VU0_PROGSIZE-1)
#define VU1_MEMMASK	(VU1_MEMSIZE-1)

/* VU0/VU1 micro and data memory, one block in the main map. */
#define VU_MEMORY_RESERVE_SIZE (VU1_PROGSIZE + VU1_MEMSIZE + VU0_PROGSIZE + VU0_MEMSIZE)
#define VU1_PROGMASK	(VU1_PROGSIZE-1)

#define vu1RunCycles (3000000) /* mVU1 uses this for inf loop detection on dev builds */


/* --------------------------------------------------------------------------
 *  VU micro CPU provider
 * --------------------------------------------------------------------------
 * VU0 and VU1 each run either the interpreter or the recompiler. That was a
 * class hierarchy with five virtuals; it is a struct of function pointers
 * now, one static instance per (unit, provider) pair. Same indirect call at
 * the dispatch point, minus the vtable indirection to find it -- and the
 * unit index is baked into each provider's functions instead of being
 * branched on at run time.
 *
 * ResumeXGkick is NULL for every provider except the VU1 recompiler; the
 * call sites test it, which is what the empty virtual overrides did.
 */
struct VUmicroCpu
{
	int  idx;              /* 0 = VU0, 1 = VU1 */
	int  is_interpreter;   /* logging facilities ask this */
	void (*Shutdown)(void);
	void (*Reset)(void);
	void (*SetStartPC)(u32 startPC);
	void (*Execute)(u32 cycles);
	void (*Clear)(u32 addr, u32 size);
	void (*ResumeXGkick)(void); /* NULL when the provider has none */
};

/* Executes a block based on EE delta time (see VUmicro.cpp); SPR.c
 * catches the VUs up through it. */
#ifdef __cplusplus
extern "C" {
#endif
void vucpu_execute_block(const struct VUmicroCpu* cpu, int startUp);
#ifdef __cplusplus
}
#endif

/* Called from recompiled code after a COP2 transfer; see VUmicro.cpp. */
void vucpu_execute_block_jit(const struct VUmicroCpu* cpu, int interlocked);

extern const struct VUmicroCpu vucpu_interp_vu0;
extern const struct VUmicroCpu vucpu_interp_vu1;
extern const struct VUmicroCpu vucpu_rec_vu0;
extern const struct VUmicroCpu vucpu_rec_vu1;

void vucpu_rec_vu0_reserve(void);
void vucpu_rec_vu1_reserve(void);

/* COP2.c and Memory.c reach these, so they carry C linkage. */
#ifdef __cplusplus
extern "C" {
#endif
extern const struct VUmicroCpu* CpuVU0;
extern const struct VUmicroCpu* CpuVU1;

extern void vu0ExecMicro(u32 addr);
extern void _vu0FinishMicro(void);

/* The VU memory block in the main map: points vuRegs at it, clears it, and
 * hands it back. */
struct VirtualMemoryManager;
extern void vu_memory_assign(const struct VirtualMemoryManager* main_map);
extern void vu_memory_reset(void);
extern void vu_memory_release(const struct VirtualMemoryManager* main_map);

/* MTVU (MTVU.cpp): wait for the VU1 thread to drain its ring, and queue
 * writes to VU1 memory through it. A write of a known width takes a slot in
 * the ring with mtvu_write_mem_begin, stores the data straight into it and
 * publishes it with mtvu_write_mem_commit; the _micro/_data_mem forms copy
 * from a buffer. */
extern void mtvu_wait_vu(void);
/* Pcsx2Config.cpp: whether VU1 runs on its own thread (THREAD_VU1). */
extern bool emu_config_thread_vu1(void);
extern void* mtvu_write_mem_begin(int data_mem, u32 addr, u32 size);
extern void mtvu_write_mem_commit(void);
extern void mtvu_write_micro_mem(u32 vu_micro_addr, const void* data, u32 size);
extern void mtvu_write_data_mem(u32 vu_data_addr, const void* data, u32 size);
#ifdef __cplusplus
}
#endif

/* VU0 */
extern void vu0ResetRegs();
extern void vu0Exec(VURegs* VU);
extern void vu0Finish();

/* VU1 */
extern void vu1Finish(bool add_cycles);
extern void vu1ResetRegs();
extern void vu1ExecMicro(u32 addr);
extern void vu1Exec(VURegs* VU);
