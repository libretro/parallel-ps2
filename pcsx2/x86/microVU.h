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

#include <string.h> /* memset/memcpy */
#include "ps2float.h"
#include "common/CpuFeatures.h"

#include "Common.h"
#include "VU.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "iR5900.h"
#include "R5900OpcodeTables.h"
#include "VirtualMemory.h"
#include "common/emitter/x86emitter.h"
#include "microVU_Misc.h"
#include "microVU_IR.h"

#include "../microVU/microVU_Structs.h"

/* One slot per startPC: the last (program, pipeline-state) resolved at
 * that PC and the block entry it produced. Program identity is checked
 * against the live quick table - the same trick as the jump cache, which
 * also absorbs mVUclear for free since a clear nulls every quick pointer -
 * and the state by the emitted full-state comparator, which is strictly
 * conservative against the simple search's partial matching. */
struct microMscalMemo
{
	microProgram* prog;
	void* entry;
	u32 startPC;      /* the section PC: programs run in sections, so the
	                   * same start_pc slot serves multiple entry PCs and
	                   * the entry depends on which one */
	microRegInfo state;
};

struct microVU
{

	alignas(16) u32 statFlag[4]; // 4 instances of status flag (backup for xgkick)
	alignas(16) u32 exactMulBuf[16];      // exact-multiply stub I/O: ma, mb, p02, p13
	alignas(16) u32 exactMulSave[16][4];  // stub xmm spill area (all of xmm0-15)
	u8* exactMulStub;                     // emitted tree stub (in dispCache)
	alignas(16) u32 exactDivBuf[4];       // exact div/sqrt/rsqrt I/O: a, b, op-in result-out
	u32 exactDivMxcsr;                    // MXCSR across the stub's C call
	/* Recompile-time mask elision. FMACa raises the one-shot flag when
	 * the current op's operands are provably exponent-equal per lane
	 * (register-form Fs == Ft), the SSE_ADD/SUB wrapper consumes it,
	 * skips the mask stage (a no-op on equal exponents), and keeps the
	 * detector -- equal operands can still saturate. The counters
	 * report how much of real content the static proof covers; they
	 * print with the JIT hash dump. */
	bool addsubMaskNoop;
	bool ovfDetectOK;                     // overflow-hack detector valid for the current op (multiply-family finals)
	bool ovfUseEventMask;                 // flag block should read the composite's saturation-EVENT mask (accurate add/sub)
	u32  addsubRecTotal;
	u32  addsubRecElided;
	alignas(16) u32 macFlag [4]; // 4 instances of mac    flag (used in execution)
	alignas(16) u32 clipFlag[4]; // 4 instances of clip   flag (used in execution)
	alignas(16) u32 xmmCTemp[4];     // Backup used in mVUclamp2()
	alignas(16) u32 xmmBackup[16][4]; // Backup for xmm0~xmm15

	u32 index;        // VU Index (VU0 or VU1)
	u32 cop2;         // VU is in COP2 mode?  (No/Yes)
	u32 vuMemSize;    // VU Main  Memory Size (in bytes)
	u32 microMemSize; // VU Micro Memory Size (in bytes)
	u32 progSize;     // VU Micro Memory Size (in u32's)
	u32 progMemMask;  // VU Micro Memory Size (in u32's)
	u32 cacheSize;    // VU Cache Size

	microProgManager               prog;     // Micro Program Data
	struct microRegAlloc* regAlloc; // Reg Alloc state

	struct CodeReserve* cache_reserve;
	u8* cache;        // Dynarec Cache Start (where we will start writing the recompiled code to)
	u8* dispCache;    // Dispatchers Cache (where startFunct and exitFunct are written to)
	u8* startFunct;   // Function Ptr to the recompiler dispatcher (start)
	u8* exitFunct;    // Function Ptr to the recompiler dispatcher (exit)
	u8* startFunctXG; // Function Ptr to the recompiler dispatcher (xgkick resume)
	u8* exitFunctXG;  // Function Ptr to the recompiler dispatcher (xgkick exit)
	u8* compareStateF;// Function Ptr to search which compares all state.
	struct microMscalMemo* mscalMemo; // Per-startPC (prog, state) -> entry memo for program invocations
	u8* waitMTVU;     // Ptr to function to save registers/sync VU1 thread
	u8* copyPLState;  // Ptr to function to copy pipeline state into microVU
	u8* resumePtrXG;  // Ptr to recompiled code position to resume xgkick
	u32 code;         // Contains the current Instruction
	u32 divFlag;      // 1 instance of I/D flags
	u32 VIbackup;     // Holds a backup of a VI reg if modified before a branch
	u32 VIxgkick;     // Holds a backup of a VI reg used for xgkick-delays
	u32 branch;       // Holds branch compare result (IBxx) OR Holds address to Jump to (JALR/JR)
	u32 badBranch;    // For Branches in Branch Delay Slots, holds Address the first Branch went to + 8
	u32 evilBranch;   // For Branches in Branch Delay Slots, holds Address to Jump to
	u32 evilevilBranch;// For Branches in Branch Delay Slots (chained), holds Address to Jump to
	u32 p;            // Holds current P instance index
	u32 q;            // Holds current Q instance index
	u32 totalCycles;  // Total Cycles that mVU is expected to run for
	s32 cycles;       // Cycles Counter
};

#include "../microVU/microVU_BlockManager.inl"

// microVU rec structs
alignas(16) microVU microVU0;
alignas(16) microVU microVU1;

// Main Functions
static __fi void mVUclear(mV, u32, u32);
extern void mVUreset(microVU* mVU, int resetReserve);
static __fi void* mVUblockFetch(microVU* mVU, u32 startPC, uptr pState);
_mVUt extern void* mVUcompileJIT(u32 startPC, uptr ptr);

// Prototypes for Linux
extern void mVUcleanUpVU0();
extern void mVUcleanUpVU1();
mVUop(mVUopU);
mVUop(mVUopL);

// Private Functions
static __ri void mVUcacheProg(microVU* mVU, microProgram* prog);
static __ri void mVUdeleteProg(microVU* mVU, microProgram** prog);
_mVUt extern void* mVUsearchProg(u32 startPC, uptr pState);
extern void* mVUexecuteVU0(u32 startPC, u32 cycles);
extern void* mVUexecuteVU1(u32 startPC, u32 cycles);

// recCall Function Pointer
typedef void (*mVUrecCall)(u32, u32);
typedef void (*mVUrecCallXG)(void);

// Include all the *.inl files (microVU compiles as 1 Translation Unit)
#include "microVU_Clamp.inl"
#include "microVU_Misc.inl"
#include "../microVU/microVU_Analyze.inl"
#include "microVU_Alloc.inl"
#include "microVU_Upper.inl"
#include "microVU_Lower.inl"
#include "microVU_Tables.inl"
#include "microVU_Flags.inl"
#include "microVU_Branch.inl"
#include "microVU_Compile.inl"
#include "microVU_Execute.inl"
#include "microVU_Macro.inl"
