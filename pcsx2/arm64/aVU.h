/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+ */

#ifndef ARM64_AVU_H
#define ARM64_AVU_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include "../Config.h"

/* ARM64 microVU recompiler — arch-neutral data structures (Phase 7, task 7.2a).
 *
 * This is the ARM64 counterpart to pcsx2/x86/microVU.h + microVU_IR.h. Per the
 * Phase 7 strategy (PROGRESS.md / [[arm64-microvu-architecture]]) microVU is
 * *parallel-cloned* into pcsx2/arm64/ rather than #included from x86/ — the x86 */
#include "ps2float.h"
/* headers fuse the arch-neutral structs together with x86emitter types and the
 * x86 `microRegAlloc` in the same files, so they cannot be compiled on ARM64.
 *
 * What lives here (arch-neutral, near-verbatim copies of the x86 originals):
 * * the pipeline-state key `microRegInfo` and the IR structs
 * (`microBlock`, `microOp`, `microIR`, the per-op read/write/flag info)
 * * the program/block bookkeeping (`microProgram*`, `microProgManager`,
 * `microBlockManager`) and the top-level `microVU` context
 *
 * What is NOT here:
 * * `microRegAlloc` — the host register allocator (NEON v0-v31 for VF, ARM
 * w-regs for VI). It is arch-specific and is ported in task 7.2b
 * (aVU_IR.h). Only forward-declared below; `microVU` holds it by
 * unique_ptr, so the incomplete type is fine in this header. The microVU0/1
 * globals are *defined* in aVU.c where the allocator is complete.
 * * the emitter / dispatcher / opcode emission (tasks 7.2c onward).
 *
 * Renames vs the x86 original (documented in PROGRESS.md 7.2a):
 * microProgManager::x86ptr / x86start / x86end  -> codePtr / codeStart / codeEnd
 * microBlock::x86ptrStart                        -> codeStart
 * microJumpCache::x86ptrStart                    -> codeStart */

#include "Common.h"
#include "arm64/ArmCompat.h"
#include "VU.h"
#include "MTVU.h"
#include "R5900.h"

/* Arch-neutral opcode enum + (compiled-out by default) opcode profiler. This
 * header has no x86 dependency — it is shared as-is rather than re-cloned. */
#include "x86/microVU_Profiler.h"

typedef struct microRegAlloc microRegAlloc; /* host register allocator: aVU_IR.h */
typedef struct microBlockManager microBlockManager;

/* ------------------------------------------------------------------
 * Pipeline-state key + IR structures (from x86/microVU_IR.h)
 * ------------------------------------------------------------------ */

typedef struct regCycleInfo
{
	u8 x : 4;
	u8 y : 4;
	u8 z : 4;
	u8 w : 4;
} regCycleInfo;

/* microRegInfo is carefully ordered for faster compares.  The "important" information is
 * housed in a union that is accessed via 'quick32' so that several u8 fields can be compared
 * using a pair of 32-bit equalities.
 * vi15 is only used if microVU const-prop is enabled (it is *not* by default).  When constprop
 * is disabled the vi15 field acts as additional padding that is required for 16 byte alignment
 * needed by the (x86 xmm / ARM NEON q) compare. */
typedef union PCSX2_ALIGN(16) microRegInfo
{
	struct
	{
		union
		{
			struct
			{
				u8 needExactMatch; /* If set, block needs an exact match of pipeline state */
				u8 flagInfo; /* xC * 2 | xM * 2 | xS * 2 | 0 * 1 | fullFlag Valid * 1 */
				u8 q;
				u8 p;
				u8 xgkick;
				u8 viBackUp; /* VI reg number that was written to on branch-delay slot */
				u8 blockType; /* 0 = Normal; 1,2 = Compile one instruction (E-bit/Branch Ending) */
				u8 r;
			};
			u64 quick64[1];
			u32 quick32[2];
		};

		u32 xgkickcycles;
		u8 unused;
		u8 vi15v; /* 'vi15' constant is valid */
		u16 vi15; /* Constant Prop Info for vi15 */

		struct
		{
			u8 VI[16];
			regCycleInfo VF[32];
		};
	};

	u128 full128[96 / sizeof(u128)];
	u64  full64[96 / sizeof(u64)];
	u32  full32[96 / sizeof(u32)];
} microRegInfo;

/* Note: the host pipeline-state compare (compareStateF) needs to be updated if this is changed */
typedef char microRegInfo_is_96_bytes[sizeof(microRegInfo) == 96 ? 1 : -1];

typedef struct microProgram microProgram;
typedef struct microJumpCache
{
	microProgram* prog; /* Program to which the entry point below is part of */
	void* codeStart; /* Start of code (Entry point for block) */
} microJumpCache;

typedef struct PCSX2_ALIGN(16) microBlock
{
	microRegInfo    pState; /* Detailed State of Pipeline */
	microRegInfo    pStateEnd; /* Detailed State of Pipeline at End of Block (needed by JR/JALR opcodes) */
	u8*             codeStart; /* Start of code (Entry point for block) */
	microJumpCache* jumpCache; /* Will point to an array of entry points of size [16k/8] if block ends in JR/JALR */
} microBlock;

typedef struct microTempRegInfo
{
	regCycleInfo VF[2]; /* Holds cycle info for Fd, VF[0] = Upper Instruction, VF[1] = Lower Instruction */
	u8 VFreg[2]; /* Index of the VF reg */
	u8 VI; /* Holds cycle info for Id */
	u8 VIreg; /* Index of the VI reg */
	u8 q; /* Holds cycle info for Q reg */
	u8 p; /* Holds cycle info for P reg */
	u8 r; /* Holds cycle info for R reg (Will never cause stalls, but useful to know if R is modified) */
	u8 xgkick; /* Holds the cycle info for XGkick */
} microTempRegInfo;

typedef struct microVFreg
{
	u8 reg; /* Reg Index */
	u8 x; /* X vector read/written to? */
	u8 y; /* Y vector read/written to? */
	u8 z; /* Z vector read/written to? */
	u8 w; /* W vector read/written to? */
} microVFreg;

typedef struct microVIreg
{
	u8 reg; /* Reg Index */
	u8 used; /* Reg is Used? (Read/Written) */
} microVIreg;

typedef struct microConstInfo
{
	u8  isValid; /* Is the constant in regValue valid? */
	u32 regValue; /* Constant Value */
} microConstInfo;

typedef struct microUpperOp
{
	bool eBit; /* Has E-bit set */
	bool iBit; /* Has I-bit set */
	bool mBit; /* Has M-bit set */
	bool tBit; /* Has T-bit set */
	bool dBit; /* Has D-bit set */
	microVFreg VF_write; /* VF Vectors written to by this instruction */
	microVFreg VF_read[2]; /* VF Vectors read by this instruction */
} microUpperOp;

typedef struct microLowerOp
{
	microVFreg VF_write; /* VF Vectors written to by this instruction */
	microVFreg VF_read[2]; /* VF Vectors read by this instruction */
	microVIreg VI_write; /* VI reg written to by this instruction */
	microVIreg VI_read[2]; /* VI regs read by this instruction */
	microConstInfo constJump; /* Constant Reg Info for JR/JARL instructions */
	u32  branch; /* Branch Type (0 = Not a Branch, 1 = B. 2 = BAL, 3~8 = Conditional Branches, 9 = JR, 10 = JALR) */
	u32  kickcycles; /* Number of xgkick cycles accumulated by this instruction */
	bool badBranch; /* This instruction is a Branch who has another branch in its Delay Slot */
	bool evilBranch; /* This instruction is a Branch in a Branch Delay Slot (Instruction after badBranch) */
	bool isNOP; /* This instruction is a NOP */
	bool isFSSET; /* This instruction is a FSSET */
	bool noWriteVF; /* Don't write back the result of a lower op to VF reg if upper op writes to same reg (or if VF = 0) */
	bool backupVI; /* Backup VI reg to memory if modified before branch (branch uses old VI value unless opcode is ILW or ILWR) */
	bool memReadIs; /* Read Is (VI reg) from memory (used by branches) */
	bool memReadIt; /* Read If (VI reg) from memory (used by branches) */
	bool readFlags; /* Current Instruction reads Status, Mac, or Clip flags */
	bool isMemWrite; /* Current Instruction writes to VU memory */
	bool isKick; /* Op is a kick so don't count kick cycles */
} microLowerOp;

typedef struct microFlagInst
{
	bool doFlag; /* Update Flag on this Instruction */
	bool doNonSticky; /* Update O,U,S,Z (non-sticky) bits on this Instruction (status flag only) */
	u8   write; /* Points to the instance that should be written to (s-stage write) */
	u8   lastWrite; /* Points to the instance that was last written to (most up-to-date flag) */
	u8   read; /* Points to the instance that should be read by a lower instruction (t-stage read) */
} microFlagInst;

typedef struct microFlagCycles
{
	int xStatus[4];
	int xMac[4];
	int xClip[4];
	int cycles;
} microFlagCycles;

typedef struct microOp
{
	u8   stall; /* Info on how much current instruction stalled */
	bool isBadOp; /* Cur Instruction is a bad opcode (not a legal instruction) */
	bool isEOB; /* Cur Instruction is last instruction in block (End of Block) */
	bool isBdelay; /* Cur Instruction in Branch Delay slot */
	bool swapOps; /* Run Lower Instruction before Upper Instruction */
	bool backupVF; /* Backup mVUlow.VF_write.reg, and restore it before the Upper Instruction is called */
	bool doXGKICK; /* Do XGKICK transfer on this instruction */
	u32  XGKICKPC; /* The PC in which the XGKick has taken place, so if we break early (before it) we don run it. */
	bool doDivFlag; /* Transfer Div flag to Status Flag on this instruction */
	int  readQ; /* Q instance for reading */
	int  writeQ; /* Q instance for writing */
	int  readP; /* P instance for reading */
	int  writeP; /* P instance for writing */
	microFlagInst sFlag; /* Status Flag Instance Info */
	microFlagInst mFlag; /* Mac    Flag Instance Info */
	microFlagInst cFlag; /* Clip   Flag Instance Info */
	microUpperOp  uOp; /* Upper Op Info */
	microLowerOp  lOp; /* Lower Op Info */
} microOp;

#define mProgSize (0x4000 / 4)
typedef struct microIR
{
	microBlock       block; /* Block/Pipeline info */
	microBlock*      pBlock; /* Pointer to a block in mVUblocks */
	microTempRegInfo regsTemp; /* Temp Pipeline info (used so that new pipeline info isn't conflicting between upper and lower instructions in the same cycle) */
	microOp          info[mProgSize / 2]; /* Info for Instructions in current block */
	microConstInfo   constReg[16]; /* Simple Const Propagation Info for VI regs within blocks */
	u8  branch;
	u32 cycles; /* Cycles for current block */
	u32 count; /* Number of VU 64bit instructions ran (starts at 0 for each block) */
	u32 curPC; /* Current PC */
	u32 startPC; /* Start PC for Cur Block */
	u32 sFlagHack; /* Optimize out all Status flag updates if microProgram doesn't use Status flags */
} microIR;

/* ------------------------------------------------------------------
 * Program / block bookkeeping (from x86/microVU.h)
 * ------------------------------------------------------------------ */

typedef struct microBlockLink
{
	microBlock block;
	struct microBlockLink* next;
} microBlockLink;

typedef struct microBlockLinkRef
{
	microBlock* pBlock;
	u64 quick;
} microBlockLinkRef;

typedef struct microRange
{
	s32 start; /* Start PC (The opcode the block starts at) */
	s32 end; /* End PC   (The opcode the block ends with) */
} microRange;

/* Flat lists in place of std::deque, as pcsx2/microVU/microVU_Structs.h:
 * prepend, erase in place and index, grown by doubling. */
typedef struct microRangeList
{
	microRange* data;
	u32         count;
	u32         capacity;
} microRangeList;

typedef struct microProgramList
{
	microProgram** data;
	u32            count;
	u32            capacity;
} microProgramList;

struct microProgram
{
	u32                data [mProgSize]; /* Holds a copy of the VU microProgram */
	microBlockManager* block[mProgSize / 2]; /* Array of Block Managers */
	microRangeList*    ranges; /* The ranges of the microProgram that have already been recompiled */
	u32 startPC; /* Start PC of this program */
	int idx; /* Program index */
};

static INLINE microRangeList* mvu_rangelist_new(void)
{
	return (microRangeList*)calloc(1, sizeof(microRangeList));
}
static INLINE void mvu_rangelist_delete(microRangeList* l)
{
	if (l)
	{
		free(l->data);
		free(l);
	}
}
static INLINE microProgramList* mvu_proglist_new(void)
{
	return (microProgramList*)calloc(1, sizeof(microProgramList));
}
static INLINE void mvu_proglist_delete(microProgramList* l)
{
	if (l)
	{
		free(l->data);
		free(l);
	}
}
static INLINE void mvu_proglist_clear(microProgramList* l)
{
	l->count = 0;
}
static INLINE void mvu_rangelist_reserve(microRangeList* l, u32 need)
{
	if (need > l->capacity)
	{
		u32 cap = l->capacity ? l->capacity * 2 : 8;
		while (cap < need)
			cap *= 2;
		l->data = (microRange*)realloc(l->data, cap * sizeof(microRange));
		l->capacity = cap;
	}
}
static INLINE void mvu_rangelist_push_front(microRangeList* l, microRange r)
{
	mvu_rangelist_reserve(l, l->count + 1);
	memmove(&l->data[1], &l->data[0], l->count * sizeof(microRange));
	l->data[0] = r;
	l->count++;
}
static INLINE void mvu_rangelist_erase(microRangeList* l, u32 i)
{
	memmove(&l->data[i], &l->data[i + 1], (l->count - i - 1) * sizeof(microRange));
	l->count--;
}
static INLINE void mvu_proglist_reserve(microProgramList* l, u32 need)
{
	if (need > l->capacity)
	{
		u32 cap = l->capacity ? l->capacity * 2 : 8;
		while (cap < need)
			cap *= 2;
		l->data = (microProgram**)realloc(l->data, cap * sizeof(microProgram*));
		l->capacity = cap;
	}
}
static INLINE void mvu_proglist_push_front(microProgramList* l, microProgram* p)
{
	mvu_proglist_reserve(l, l->count + 1);
	memmove(&l->data[1], &l->data[0], l->count * sizeof(microProgram*));
	l->data[0] = p;
	l->count++;
}
static INLINE void mvu_proglist_erase(microProgramList* l, u32 i)
{
	memmove(&l->data[i], &l->data[i + 1], (l->count - i - 1) * sizeof(microProgram*));
	l->count--;
}

typedef struct microProgramQuick
{
	microBlockManager* block; /* Quick reference to valid microBlockManager for current startPC */
	microProgram*      prog; /* The microProgram who is the owner of 'block' */
} microProgramQuick;

typedef struct microProgManager
{
	microIR            IRinfo; /* IR information */
	microProgramList*  prog [mProgSize/2]; /* List of microPrograms indexed by startPC values */
	microProgramQuick  quick[mProgSize/2]; /* Quick reference to valid microPrograms for current execution */
	microProgram*      cur; /* Pointer to currently running MicroProgram */
	int                total; /* Total Number of valid MicroPrograms */
	int                isSame; /* Current cached microProgram is Exact Same program as vuRegs[mVU->index].Micro (-1 = unknown, 0 = No, 1 = Yes) */
	int                cleared; /* Micro Program is Indeterminate so must be searched for (and if no matches are found then recompile a new one) */
	u32                curFrame; /* Frame Counter */
	u8*                codePtr; /* Pointer to program's recompilation code  (x86: x86ptr) */
	u8*                codeStart; /* Start of program's rec-cache             (x86: x86start) */
	u8*                codeEnd; /* Reset threshold (x86: x86end) — 3MB short of the physical end so a single in-flight compile can spill into the safe-zone and be caught afterward */
	u8*                codeReserveEnd; /* Physical end of the rec-cache reserve — the hard capacity handed to the emitter (x86 has no equivalent: it writes raw bytes with no buffer cap) */
	microRegInfo       lpState; /* Pipeline state from where program left off (useful for continuing execution) */
} microProgManager;

#define mVUcacheSafeZone 3 /* Safe-Zone for program recompilation (in megabytes) */

/* Const-prop of the vi15 register is off by default (matches the x86 rec). */
#define doConstProp 0
#define doViConstProp 1 /* Block-local VI const propagation into memory-op addressing */

/* C.75: emit-constant table (defined here so microVU can embed a copy; the
 * canonical instance mVUglob lives in aVU_Misc.h). */
typedef struct mVU_Globals
{
	u32 absclip[4];
	u32 tPackCnt[4];
	u32 tCnt0[4];
	u32 tCnt1[4];
	u32 tCnt2[4];
	u32 tCnt3[4];
	u32 tLUTdbl[4];
	u32 tLUTneg[4];
	u32 tLUTany[4];
	u32 tOnesP[4];
	u32 tOneP[4];
	u32 tF800[4];
	u32 tF000[4];
	u32 t0800[4];
	u32 t0400[4];
	u32 t8000[4];
	u32 t64one[4];
	u32 signbit[4];
	u32 minvals[4];
	u32 maxvals[4];
	u32 exponent[4];
	u32 one[4];
	u32 Pi4[4];
	u32 T1[4];
	u32 T2[4];
	u32 T3[4];
	u32 T4[4];
	u32 T5[4];
	u32 T6[4];
	u32 T7[4];
	u32 T8[4];
	u32 S2[4];
	u32 S3[4];
	u32 S4[4];
	u32 S5[4];
	u32 E1[4];
	u32 E2[4];
	u32 E3[4];
	u32 E4[4];
	u32 E5[4];
	u32 E6[4];
	u32 I32MAXF[4];
	float FTOI_4[4];
	float FTOI_12[4];
	float FTOI_15[4];
	float ITOF_4[4];
	float ITOF_12[4];
	float ITOF_15[4];
} mVU_Globals;



/* One slot per program start_pc: the last (program, section PC, entry
 * state) resolved there and the block entry it produced. The section PC
 * is part of the key from day one on this side: programs run in
 * sections, re-entering mid-program at a later PC while start_pc keeps
 * the base, which is the false-hit the x86 memo shipped and a real
 * title convicted. Program identity checks against the live quick
 * table, which mVUclear nulls wholesale. */
typedef struct microMscalMemoA
{
	microProgram* prog;
	void* entry;
	u32 startPC;
	microRegInfo state;
} microMscalMemoA;

typedef struct microVU
{
	PCSX2_ALIGN(16) u32 statFlag[4]; /* 4 instances of status flag (backup for xgkick) */
	PCSX2_ALIGN(16) u32 macFlag [4]; /* 4 instances of mac    flag (used in execution) */
	PCSX2_ALIGN(16) u32 clipFlag[4]; /* 4 instances of clip   flag (used in execution) */
	PCSX2_ALIGN(16) u32 vecCTemp[4]; /* Backup used in mVUclamp2()                  (x86: xmmCTemp) */
	PCSX2_ALIGN(16) u32 exactMulBuf[16]; /* exact-multiply stub I/O: fs, ft, p01, p23 (x86: same shape) */
	PCSX2_ALIGN(16) u32 exactDivBuf[4]; /* exact div/sqrt/rsqrt I/O: a, b, op-in result-out */
	PCSX2_ALIGN(16) u32 exactMulSave[16][4]; /* stub vector spill area */
	u8* exactMulStub; /* emitted tree stub */
	PCSX2_ALIGN(16) u32 vecBackup[32][4]; /* Backup for host vector regs across XGKICK   (x86: xmmBackup[16][4]; sized for NEON v0-v31) */

	/* C.75: block-local copies of the emitter constant tables (filled in
	 * mVUinit), so the hot constant loads (clamps, FTOI/ITOF, EFU polynomials)
	 * encode as [x27 + imm] in micro mode instead of an adrp+ldr pair. */
	PCSX2_ALIGN(32) mVU_Globals glob;
	PCSX2_ALIGN(16) u32 sse4Min[2][4]; /* copies of aVU_Clamp.inl sse4_min/maxvals */
	PCSX2_ALIGN(16) u32 sse4Max[2][4];

	u32 index; /* VU Index (vuRegs[0] or vuRegs[1]) */
	u32 cop2; /* VU is in COP2 mode?  (No/Yes) */
	u32 vuMemSize; /* VU Main  Memory Size (in bytes) */
	u32 microMemSize; /* VU Micro Memory Size (in bytes) */
	u32 progSize; /* VU Micro Memory Size (in u32's) */
	u32 progMemMask; /* VU Micro Memory Size (in u32's) */
	u32 cacheSize; /* VU Cache Size */

	/* C.74: the hot per-op scalars live BEFORE the ~290KB `prog` member so they
	 * sit in the first KB of the struct, inside the unsigned scaled-immediate
	 * window of the pinned x27 (&mVU) base -- see mvuAbsMem. (The x86 rec keeps
	 * them after prog; with absolute addressing the layout never mattered.) */
	u32 code; /* Contains the current Instruction */
	u32 divFlag; /* 1 instance of I/D flags */
	u32 VIbackup; /* Holds a backup of a VI reg if modified before a branch */
	u32 VIxgkick; /* Holds a backup of a VI reg used for xgkick-delays */
	u32 branch; /* Holds branch compare result (IBxx) OR Holds address to Jump to (JALR/JR) */
	u32 badBranch; /* For Branches in Branch Delay Slots, holds Address the first Branch went to + 8 */
	u32 evilBranch; /* For Branches in Branch Delay Slots, holds Address to Jump to */
	u32 evilevilBranch; /* For Branches in Branch Delay Slots (chained), holds Address to Jump to */
	u32 p; /* Holds current P instance index */
	u32 q; /* Holds current Q instance index */
	u32 totalCycles; /* Total Cycles that mVU is expected to run for */
	s32 cycles; /* Cycles Counter */

	microProgManager               prog; /* Micro Program Data */
	microRegAlloc*                 regAlloc; /* Reg Alloc state */
	bool ovfDetectOK; /* overflow-hack detector valid for the current op (multiply-family finals) */
	FILE*                          logFile; /* Log File Pointer */

	u8* cache; /* Dynarec Cache Start (where we will start writing the recompiled code to) */
	u8* startFunct; /* Function Ptr to the recompiler dispatcher (start) */
	u8* exitFunct; /* Function Ptr to the recompiler dispatcher (exit) */
	u8* startFunctXG; /* Function Ptr to the recompiler dispatcher (xgkick resume) */
	u8* exitFunctXG; /* Function Ptr to the recompiler dispatcher (xgkick exit) */
	u8* compareStateF; /* Function Ptr to search which compares all state. */
	struct microMscalMemoA* mscalMemo; /* Per-startPC (prog, section PC, state) -> entry memo */
	u8* waitMTVU; /* Ptr to function to save registers/sync vuRegs[1] thread */
	u8* copyPLState; /* Ptr to function to copy pipeline state into microVU */
	u8* resumePtrXG; /* Ptr to recompiled code position to resume xgkick */
} microVU;

static __fi void* mVUtextPtr(const microVU* mVU)
{
	return (mVU->index && THREAD_VU1) ? (void*)&vuRegs[mVU->index].VF[9] : (void*)&cpuRegs.GPR.r[9];
}
static __fi VIFregisters* mVUvifRegs(const microVU* mVU)
{
	return (mVU->index && THREAD_VU1) ? &vu1Thread.vifRegs : (mVU->index ? &vif1Regs : &vif0Regs);
}
/* On Apple Silicon the JIT-generated compareStateF must NOT run here:
 * microVU runs its whole recursive compile inside one write session of the
 * MAP_JIT region, writable-but-not-executable, and the block search runs
 * mid-compile. A plain compare is the same test (sizeof(microRegInfo) ==
 * 96, 0 iff equal). */
static __fi u32 mVUcompareState(microRegInfo* lhs, microRegInfo* rhs)
{
	return memcmp(lhs, rhs, sizeof(microRegInfo)) ? 1u : 0u;
}


/* Per-startPC block table: a plain struct with prefixed functions, as
 * pcsx2/microVU/microVU_BlockManager.inl. quickLookup is an array grown by
 * doubling, as the vector it was. */
struct microBlockManager
{
	microBlockLink *qBlockList, *qBlockEnd; /* Quick Search */
	microBlockLink *fBlockList, *fBlockEnd; /* Full  Search */
	microBlockLinkRef* quickLookup;
	u32 quickCount;
	u32 quickCapacity;
	int qListI, fListI;
};

static void mVUbm_init(microBlockManager* m)
{
	memset(m, 0, sizeof(*m));
}

static void mVUbm_reset(microBlockManager* m)
{
	microBlockLink* linkI;
	for (linkI = m->qBlockList; linkI != NULL;)
	{
		microBlockLink* freeI = linkI;
		free(linkI->block.jumpCache);
		linkI->block.jumpCache = NULL;
		linkI = linkI->next;
		memalign_free(freeI);
	}
	for (linkI = m->fBlockList; linkI != NULL;)
	{
		microBlockLink* freeI = linkI;
		free(linkI->block.jumpCache);
		linkI->block.jumpCache = NULL;
		linkI = linkI->next;
		memalign_free(freeI);
	}
	m->qListI = m->fListI = 0;
	m->qBlockEnd = m->qBlockList = NULL;
	m->fBlockEnd = m->fBlockList = NULL;
	m->quickCount = 0; /* the buffer is kept for reuse */
}

static void mVUbm_destroy(microBlockManager* m)
{
	mVUbm_reset(m);
	free(m->quickLookup);
	m->quickLookup   = NULL;
	m->quickCapacity = 0;
}

static __ri microBlock* mVUbm_search(microBlockManager* m, microVU* mVU, microRegInfo* pState)
{
	if (pState->needExactMatch) /* Needs Detailed Search (Exact Match of Pipeline State) */
	{
		microBlockLink* prevI = NULL;
		microBlockLink* linkI;
		for (linkI = m->fBlockList; linkI != NULL; prevI = linkI, linkI = linkI->next)
		{
			if (mVUcompareState(pState, &linkI->block.pState) == 0)
			{
				if (linkI != m->fBlockList)
				{
					prevI->next = linkI->next;
					linkI->next = m->fBlockList;
					m->fBlockList = linkI;
				}
				return &linkI->block;
			}
		}
	}
	else /* Can do Simple Search (Only Matches the Important Pipeline Stuff) */
	{
		const u64 quick64 = pState->quick64[0];
		u32 i;
		for (i = 0; i < m->quickCount; i++)
		{
			const microBlockLinkRef* ref = &m->quickLookup[i];
			/* if we're using the flag hack, ignore the mac flags going in to the new block too if an exact match wasn't requested. */
			if (mVU->prog.IRinfo.sFlagHack)
			{
				if ((ref->quick & ~0x0C04) != (quick64 & ~0x0C04)) continue;
			}
			else if (ref->quick != quick64) continue;

			if (doConstProp && (ref->pBlock->pState.vi15 != pState->vi15))  continue;
			if (doConstProp && (ref->pBlock->pState.vi15v != pState->vi15v)) continue;
			return ref->pBlock;
		}
	}
	return NULL;
}

static microBlock* mVUbm_add(microBlockManager* m, microVU* mVU, microBlock* pBlock)
{
	microBlock* thisBlock = mVUbm_search(m, mVU, &pBlock->pState);
	if (!thisBlock)
	{
		const u8 fullCmp = pBlock->pState.needExactMatch;
		microBlockLink** blockList = fullCmp ? &m->fBlockList : &m->qBlockList;
		microBlockLink** blockEnd  = fullCmp ? &m->fBlockEnd  : &m->qBlockEnd;
		microBlockLink*  newBlock  = (microBlockLink*)memalign_alloc(32, sizeof(microBlockLink));
		if (fullCmp)
			m->fListI++;
		else
			m->qListI++;
		newBlock->block.jumpCache = NULL;
		newBlock->next            = NULL;

		if (*blockEnd)
		{
			(*blockEnd)->next = newBlock;
			*blockEnd         = newBlock;
		}
		else
			*blockEnd = *blockList = newBlock;

		memcpy(&newBlock->block, pBlock, sizeof(microBlock));
		thisBlock = &newBlock->block;

		if (m->quickCount == m->quickCapacity)
		{
			const u32 newcap = m->quickCapacity ? m->quickCapacity * 2 : 64;
			m->quickLookup   = (microBlockLinkRef*)realloc(m->quickLookup, newcap * sizeof(microBlockLinkRef));
			m->quickCapacity = newcap;
		}
		m->quickLookup[m->quickCount].pBlock = &newBlock->block;
		m->quickLookup[m->quickCount].quick  = pBlock->pState.quick64[0];
		m->quickCount++;
	}
	return thisBlock;
}

/* microVU rec contexts (aVU.c) */
extern microVU microVU0;
extern microVU microVU1;

#endif
