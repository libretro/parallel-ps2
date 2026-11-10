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
#include "HotConfig.h"

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

/* The VU micro units are C (VUmicro.c, VU0micro.c, VU1micro.c, the two
 * interpreters and VUmicroMem.c); everything here carries C linkage. */
#ifdef __cplusplus
extern "C" {
#endif

/* Executes a block based on EE delta time (see VUmicro.c); SPR.c
 * catches the VUs up through it. */
void vucpu_execute_block(const struct VUmicroCpu* cpu, int startUp);

/* Called from recompiled code after a COP2 transfer; see VUmicro.c. */
void vucpu_execute_block_jit(const struct VUmicroCpu* cpu, int interlocked);

extern const struct VUmicroCpu vucpu_interp_vu0;
extern const struct VUmicroCpu vucpu_interp_vu1;
extern const struct VUmicroCpu vucpu_rec_vu0;
extern const struct VUmicroCpu vucpu_rec_vu1;

void vucpu_rec_vu0_reserve(void);
void vucpu_rec_vu1_reserve(void);

extern const struct VUmicroCpu* CpuVU0;
extern const struct VUmicroCpu* CpuVU1;

extern void vu0ExecMicro(u32 addr);
extern void _vu0WaitMicro(void);
extern void _vu0FinishMicro(void);

/* The VU memory block in the main map: points vuRegs at it, clears it, and
 * hands it back. */
struct VirtualMemoryManager;
extern void vu_memory_assign(const struct VirtualMemoryManager* main_map);
extern void vu_memory_reset(void);
extern void vu_memory_release(const struct VirtualMemoryManager* main_map);

/* MTVU (MTVU.c): wait for the VU1 thread to drain its ring, and queue
 * writes to VU1 memory through it. A write of a known width takes a slot in
 * the ring with mtvu_write_mem_begin, stores the data straight into it and
 * publishes it with mtvu_write_mem_commit; the _micro/_data_mem forms copy
 * from a buffer. */
extern void mtvu_wait_vu(void);
/* Pcsx2Config.cpp: the host rounding mode a VU unit runs under: set
 * returns the mode it replaced, for restore to put back. Whether VU1 runs
 * on its own thread is EMU_THREAD_VU1, in HotConfig.h. */
extern u64  emu_fpcr_set_vu(int unit);
/* The EE FPU's control register; returns the previous one. */
extern u64  emu_fpcr_set_fpu(void);
extern void emu_fpcr_restore(u64 saved);
/* MTVU.c: start a VU1 program on the VU1 thread. */
extern void mtvu_execute_vu(u32 addr, u32 vif_top, u32 vif_itop, u32 fbrst);
extern void* mtvu_write_mem_begin(int data_mem, u32 addr, u32 size);
extern void mtvu_write_mem_commit(void);
extern void mtvu_write_micro_mem(u32 vu_micro_addr, const void* data, u32 size);
extern void mtvu_write_data_mem(u32 vu_data_addr, const void* data, u32 size);
/* VIF1's STCOL/STROW: hand vif1's column or row to the VU1 thread. */
extern void mtvu_write_col(void);
extern void mtvu_write_row(void);
/* The VU1 thread's copies of VIF1's state, which its unpacks write, and an
 * UNPACK's data handed to it (Vif_Unpack.c). */
extern struct vifStruct* mtvu_vif(void);
extern struct VIFregisters* mtvu_vif_regs(void);
extern void mtvu_vif_unpack(const u8* data, u32 size);
/* The VU1 thread's eventcounts MTGS waits on: a path-1 packet queued
 * (xgkick), and one popped (p1_progress). Fixed for the process. */
struct retro_asym_eventcount;
extern struct retro_asym_eventcount* mtvu_ec_xgkick(void);
extern struct retro_asym_eventcount* mtvu_ec_p1_progress(void);

/* VU0 */
extern void vu0ResetRegs(void);
extern void vu0Exec(VURegs* VU);
/* VU0.c; Vif0_Dma.c finishes VU0 through it. */
extern void vu0Finish(void);
/* VU0.c: runs VU0 until it catches up with the EE. */
extern void vu0Sync(void);

/* VU1 */
/* VU1micro.c; Dmac.c finishes VU1 through it. */
extern void vu1Finish(bool add_cycles);
/* VU1micro.c; Vif_Codes.c starts VU1 programs through it. */
extern void vu1ExecMicro(u32 addr);
extern void vu1ResetRegs(void);
extern void vu1Exec(VURegs* VU);

#ifdef __cplusplus
}
#endif
