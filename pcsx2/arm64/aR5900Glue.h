/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

/* What the arm64 EE recompiler (recR5900_arm64.cpp) and the interpreter
 * (Interpreter.c) share: the interpreter's loop and memory paths, which
 * JIT blocks call into, and the JIT's lifecycle, which the interpreter
 * assembles recCpu from. */

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Interpreter.c */

/* 3 bit fixed point cycle count of the current block. */
extern u32 cpuBlockCycles;
/* The block runner the GAME_RUNNING loop calls, or NULL to interpret. */
extern void (*g_ee_block_runner)(void);

void eeRunBasicBlock_arm64(void);
void eeRecExecute_arm64(void);
void eeEventTest_arm64(void);
void eeUpdateCycles_arm64(void);
void eeCancelInstruction_arm64(void);

/* Memory paths for the JIT's native loads and stores. */
u32  eeRead8_arm64(u32 addr);
u32  eeRead16_arm64(u32 addr);
u32  eeRead32_arm64(u32 addr);
u64  eeRead64_arm64(u32 addr);
void eeRead128_arm64(u32 addr, u128* dst);
void eeWrite8_arm64(u32 addr, u32 val);
void eeWrite16_arm64(u32 addr, u32 val);
void eeWrite32_arm64(u32 addr, u32 val);
void eeWrite64_arm64(u32 addr, u64 val);
void eeWrite128_arm64(u32 addr, const u128* src);

/* The unaligned family, with merge semantics and no misalign trap. */
void eeLWL_arm64(u32 addr, u32 rt);
void eeLWR_arm64(u32 addr, u32 rt);
void eeLDL_arm64(u32 addr, u32 rt);
void eeLDR_arm64(u32 addr, u32 rt);
void eeSWL_arm64(u32 addr, u32 rt);
void eeSWR_arm64(u32 addr, u32 rt);
void eeSDL_arm64(u32 addr, u32 rt);
void eeSDR_arm64(u32 addr, u32 rt);

/* Slow paths of the fused unaligned pairs, both accesses in guest order. */
u32  eeReadU32_arm64(u32 addr);
u64  eeReadU64_arm64(u32 addr);
void eeWriteU32_arm64(u32 addr, u32 val);
void eeWriteU64_arm64(u32 addr, u64 val);

/* recR5900_arm64.cpp */
void eeJitReserve_arm64(void);
void eeJitShutdown_arm64(void);
void eeJitReset_arm64(void);
void eeJitClear_arm64(u32 addr, u32 size);
void eeJitRunBlock_arm64(void);
/* vtlb.c's fault handler hands fastmem faults in JIT code here. */
bool eeFastmemFault_arm64(uptr code_address);

#ifdef __cplusplus
}
#endif
