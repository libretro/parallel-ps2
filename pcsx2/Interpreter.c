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

#include "Common.h"
#include <fastjmp.h>
#include "R5900OpcodeTables.h"
#include "Elfheader.h"


#include <float.h>
#include "HotConfig.h"

static int branch2 = 0;
#ifdef ARCH_ARM64
u32 cpuBlockCycles = 0;			/* 3 bit fixed point version of cycle count (shared with the arm64 EE JIT) */
#else
static u32 cpuBlockCycles = 0;		/* 3 bit fixed point version of cycle count */
#endif
static bool intExitExecution = false;
static fastjmp_buf intJmpBuf;
static u32 intLastBranchTo;

#ifdef ARCH_ARM64
#include "arm64/aR5900Glue.h"
/* arm64 EE recompiler hook: when set, the GAME_RUNNING loop runs
 * JIT-dispatched blocks instead of one interpreter instruction per
 * iteration; NULL interprets. The JIT cache lives in
 * arm64/recR5900_arm64.cpp; recCpu is assembled at the bottom of this file
 * so it can reuse the static execute/exit/cancel helpers and intJmpBuf. */
void (*g_ee_block_runner)(void) = NULL;
#endif

static void intEventTest(void);

void intUpdateCPUCycles(void)
{
	const bool lowcycles = (cpuBlockCycles <= 40);
	const s8 cyclerate = (s8)emu_hot_ee_cycle_rate;
	u32 scale_cycles = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = cpuBlockCycles >> 3;

	else if (cyclerate > 1)
		scale_cycles = cpuBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = (cpuBlockCycles >> 3) / 1.3f; /* Adds a mild 30% increase in clockspeed for value 1. */

	else if (cyclerate == -1) /* the mildest value. */
		/* These values were manually tuned to yield mild speedup with high compatibility */
		scale_cycles = (cpuBlockCycles <= 80 || cpuBlockCycles > 168 ? 5 : 7) * cpuBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * cpuBlockCycles) >> 5;

	/* Ensure block cycle count is never less than 1. */
	cpuRegs.cycle += (scale_cycles < 1) ? 1 : scale_cycles;

	if (cyclerate > 1)
		cpuBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	else
		cpuBlockCycles &= 0x7;
}

/* These macros are used to assemble the repassembler functions */

static void execI(void)
{
	/* execI is called for every instruction so it must remains as light as possible. */
	/* If you enable the next define, Interpreter will be much slower (around */
	/* ~4fps on 3.9GHz Haswell vs ~8fps (even 10fps on dev build)) */
	/* Extra note: due to some cycle count issue PCSX2's internal debugger is */
	/* not yet usable with the interpreter */
	const u32 pc = cpuRegs.pc;
	const OPCODE* opcode;
	/* We need to increase the pc before executing the memRead32. An exception could appears */
	/* and it expects the PC counter to be pre-incremented */
	cpuRegs.pc += 4;

	/* interprete instruction */
	cpuRegs.code = memRead32( pc );

	opcode = R5900_GetCurrentInstruction();
	cpuBlockCycles += opcode->cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));

	opcode->interpret();
}

static __fi void _doBranch_shared(u32 tar)
{
	branch2 = cpuRegs.branch = 1;
	execI();

	/* branch being 0 means an exception was thrown, since only the exception */
	/* handler should ever clear it. */

	if( cpuRegs.branch != 0 )
	{
		if (Cpu == &intCpu)
		{
			if (intLastBranchTo == tar && (emu_hot & EMU_HOT_WAITLOOP))
			{
				bool can_skip = true;
				u32 i;
				intUpdateCPUCycles();
				if (tar != 0x81fc0)
				{
					if ((cpuRegs.pc - tar) < (4 * 10))
					{
						for (i = tar; i < cpuRegs.pc; i += 4)
						{
							if (PSM(i) != 0)
							{
								can_skip = false;
								break;
							}
						}
					}
					else
						can_skip = false;
				}

				if (can_skip)
				{
					if ((s64)(cpuRegs.nextEventCycle - cpuRegs.cycle) > 0)
						cpuRegs.cycle = cpuRegs.nextEventCycle;
					else
						cpuRegs.nextEventCycle = cpuRegs.cycle;
				}
			}
		}
		intLastBranchTo = tar;
		cpuRegs.pc = tar;
		cpuRegs.branch = 0;
	}
}

static void doBranch( u32 target )
{
	_doBranch_shared( target );
	intUpdateCPUCycles();
	intEventTest();
}

void intDoBranch(u32 target)
{
	_doBranch_shared( target );

	if( Cpu == &intCpu )
	{
		intUpdateCPUCycles();
		intEventTest();
	}
}

void intSetBranch(void)
{
	branch2 = 1;
}


/* R5900 Branching Instructions! */
/* These are the interpreter versions of the branch instructions.  Unlike other */
/* types of interpreter instructions which can be called safely from the recompilers, */
/* these instructions are not "recSafe" because they may not invoke the */
/* necessary branch test logic that the recs need to maintain sync with the */
/* cpuRegs.pc and delaySlot instruction and such. */


/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
/* fixme: looking at the other branching code, shouldn't those _SetLinks in BGEZAL and such only be set */
/* if the condition is true? --arcum42 */

void intJ(void)
{
	doBranch(_JumpTarget_);
}

void intJAL(void)
{
	/* 0x3563b8 is the start address of the function that invalidate entry in TLB cache */
	/* 0x35c118 is the address in the June 22 prototype */
	/* 0x35d628 is the address in the August 26 prototype */
	if ((emu_hot & EMU_HOT_GOEMON_TLB)) {
		if (_JumpTarget_ == 0x3563b8 || _JumpTarget_ == 0x35d628 || _JumpTarget_ == 0x35c118)
			GoemonUnloadTlb(cpuRegs.GPR.n.a0.UL[0]);
	}
	_SetLink(31);
	doBranch(_JumpTarget_);
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void intBEQ(void)  /* Branch if Rs == Rt */
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

void intBNE(void)  /* Branch if Rs != Rt */
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
		doBranch(_BranchTarget_);
	else
		intEventTest();
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void intBGEZ(void)    /* Branch if Rs >= 0 */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void intBGEZAL(void) /* Branch if Rs >= 0 and link */
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void intBGTZ(void)    /* Branch if Rs >  0 */
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
}

void intBLEZ(void)   /* Branch if Rs <= 0 */
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void intBLTZ(void)    /* Branch if Rs <  0 */
{
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

void intBLTZAL(void)  /* Branch if Rs <  0 and link */
{
	_SetLink(31);
	if (cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
}

/*********************************************************
* Register branch logic  Likely                          *
* Format:  OP rs, offset                                 *
*********************************************************/


void intBEQL(void)    /* Branch if Rs == Rt */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] == cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBNEL(void)     /* Branch if Rs != Rt */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] != cpuRegs.GPR.r[_Rt_].SD[0])
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBLEZL(void)    /* Branch if Rs <= 0 */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] <= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBGTZL(void)     /* Branch if Rs >  0 */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] > 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBLTZL(void)     /* Branch if Rs <  0 */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBGEZL(void)     /* Branch if Rs >= 0 */
{
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBLTZALL(void)   /* Branch if Rs <  0 and link */
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] < 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

void intBGEZALL(void)   /* Branch if Rs >= 0 and link */
{
	_SetLink(31);
	if(cpuRegs.GPR.r[_Rs_].SD[0] >= 0)
	{
		doBranch(_BranchTarget_);
	}
	else
	{
		cpuRegs.pc +=4;
		intEventTest();
	}
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/
void intJR(void)
{
	/* 0x33ad48 and 0x35060c are the return address of the function (0x356250) that populate the TLB cache */
	/* 0x340600 and 0x356334 are the addresses in the June 22 prototype */
	/* 0x341ad0 and 0x357844 are the addresses in the August 26 prototype */
	if ((emu_hot & EMU_HOT_GOEMON_TLB)) {
		const u32 add = cpuRegs.GPR.r[_Rs_].UL[0];
		if (add == 0x33ad48 || add == 0x35060c || add == 0x341ad0 || add == 0x357844 || add == 0x357844 || add == 0x356334)
			GoemonPreloadTlb();
	}
	doBranch(cpuRegs.GPR.r[_Rs_].UL[0]);
}

void intJALR(void)
{
	u32 temp = cpuRegs.GPR.r[_Rs_].UL[0];

	if (_Rd_)  _SetLink(_Rd_);

	doBranch(temp);
}



/* -------------------------------------------------------------------------------------- */
/* R5900cpu/intCpu interface (implementations) */
/* -------------------------------------------------------------------------------------- */

static void intReserve(void)
{
	/* fixme : detect cpu for use the optimize asm code */
}

static void intReset(void)
{
	cpuRegs.branch = 0;
	branch2 = 0;
}

static void intEventTest(void)
{
	/* Perform counters, ints, and IOP updates: */
	_cpuEventTest_Shared();

	if (intExitExecution)
	{
		intExitExecution = false;
		fastjmp_jmp(&intJmpBuf, 1);
	}
}

static void intSafeExitExecution(void)
{
	/* If we're currently processing events, we can't safely jump out of the interpreter here, because we'll */
	/* leave things in an inconsistent state. So instead, we flag it for exiting once cpuEventTest() returns. */
	if (eeEventTestIsActive)
		intExitExecution = true;
	else
		fastjmp_jmp(&intJmpBuf, 1);
}

static void intCancelInstruction(void)
{
	/* See execute function. */
	fastjmp_jmp(&intJmpBuf, 0);
}

static void eeExecuteLoop(void)
{
	enum ExecuteState {
		EXEC_RESET,
		EXEC_GAME_LOADING,
		EXEC_GAME_RUNNING
	};
	/* The RESET stage interprets until the pc reaches the BIOS's EELOAD, which is */
	/* how a cold boot walks itself to the game's entry point. Re-entering the loop */
	/* with the game ALREADY running -- which is what loading a save state does, */
	/* since it exits execution and comes back with the VM mid-game -- can never */
	/* satisfy that condition: the pc is somewhere in the game, and it never */
	/* returns to EELOAD. The loop then interprets forever and the recompiler is */
	/* never reached (the JIT lives in the GAME_RUNNING stage), so a state load */
	/* silently dropped the EE onto the interpreter for the rest of the session. */
	/* g_GameStarted means exactly "we are past the game's entry point" and is part */
	/* of the save state, so it is the right thing to resume on. */
	/* volatile: a cancelled instruction comes back through fastjmp_set
	 * below, which restores the registers it saved, and the stage reached
	 * since then must survive that rather than revert to this one. */
	volatile enum ExecuteState state = g_GameStarted ? EXEC_GAME_RUNNING : EXEC_RESET;

	/* This will come back as zero the first time it runs, or on instruction cancel. */
	/* It will come back as nonzero when we exit execution. */
	if (fastjmp_set(&intJmpBuf) != 0)
		return;

	/* I hope this doesn't cause issues with the optimizer... infinite loop with a constant expression. */
	for (;;)
	{
		/* The execution was splited in three parts so it is easier to */
		/* resume it after a cancelled instruction. */
		switch (state) {
		case EXEC_RESET:
			{
				do
				{
					execI();
				} while (cpuRegs.pc != (g_eeloadMain ? g_eeloadMain : EELOAD_START));

				if (cpuRegs.pc == EELOAD_START)
				{
					/* The EELOAD _start function is the same across all BIOS versions afaik */
					u32 mainjump = memRead32(EELOAD_START + 0x9c);
					if (mainjump >> 26 == 3) /* JAL */
						g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);
				}
				else if (cpuRegs.pc == g_eeloadMain)
				{
					eeloadHook();
					if (g_SkipBiosHack)
					{
						/* See comments on this code in iR5900-32.c's recRecompile() */
						u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
						u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
						u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
						u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
						if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3)) /* JAL to 0x822B8 */
							g_eeloadExec = EELOAD_START + 0x2B8;
						else if (typeAexecjump >> 26 == 3) /* JAL to 0x82170 */
							g_eeloadExec = EELOAD_START + 0x170;
					}
				}
				else if (cpuRegs.pc == g_eeloadExec)
					eeloadHook2();

				if (!g_GameLoading)
					break;

				state = EXEC_GAME_LOADING;
				PCSX2_FALLTHROUGH;
			}

		case EXEC_GAME_LOADING:
			if (ElfEntry != 0xFFFFFFFF)
			{
				do
				{
					execI();
				} while (cpuRegs.pc != ElfEntry);
				eeGameStarting();
			}
			state = EXEC_GAME_RUNNING;
			/* fallthrough */

		case EXEC_GAME_RUNNING:
#ifdef ARCH_ARM64
			if (g_ee_block_runner)
				for (;;) g_ee_block_runner();
			else
#endif
				for (;;) execI();
			break;
		}
	}
}

static void intExecute(void)
{
#ifdef ARCH_ARM64
	g_ee_block_runner = NULL; /* interpreter: one instruction per iteration */
#endif
	eeExecuteLoop();
}

#ifdef ARCH_ARM64
/* arm64 EE recompiler entry points (provider recCpu, assembled below). They live */
/* here so they can use the static execute loop, branch2, execI and intJmpBuf. */

/* Run a single EE basic block through the interpreter (instructions until the */
/* next taken branch). The arm64 JIT's per-PC blocks call this for now (Phase */
/* C.3-1); later phases translate the instructions natively. */
void eeRunBasicBlock_arm64(void)
{
	branch2 = 0;
	do { execI(); } while (!branch2);
}

void eeRecExecute_arm64(void)
{
	g_ee_block_runner = &eeJitRunBlock_arm64;
	eeExecuteLoop();
}

/* Non-template EE memory wrappers + misalign cancel, called from the arm64 EE */
/* JIT's natively-translated loads/stores (Phase C.3-3). Defined here so they see */
/* Memory.h / Cpu. Cpu->CancelInstruction() fastjmps to intJmpBuf, which is safe */
/* from JIT code (fastjmp restores sp and callee-saved regs). */
u32  eeRead8_arm64 (u32 a) { return memRead8(a); }
u32  eeRead16_arm64(u32 a) { return memRead16(a); }
u32  eeRead32_arm64(u32 a) { return memRead32(a); }
u64  eeRead64_arm64(u32 a) { return memRead64(a); }
/* 128-bit LQ/SQ wrappers (Phase C.12). LQ/SQ silently align (addr & ~0xf on the */
/* caller side); dst/src is &cpuRegs.GPR.r[rt]. These are the slow path -- the */
/* JIT inlines the vtlb direct-pointer case and only calls here for handlers. */
void eeRead128_arm64 (u32 a, u128* dst)       { memRead128(a, dst); }
void eeWrite128_arm64(u32 a, const u128* src) { memWrite128(a, src); }
void eeWrite8_arm64 (u32 a, u32 v) { memWrite8(a, (u8)v); }
void eeWrite16_arm64(u32 a, u32 v) { memWrite16(a, (u16)v); }
void eeWrite32_arm64(u32 a, u32 v) { memWrite32(a, v); }
void eeWrite64_arm64(u32 a, u64 v) { memWrite64(a, v); }
void eeCancelInstruction_arm64(void) { Cpu->CancelInstruction(); }
/* Unaligned load/store family (LWL/LWR/LDL/LDR, SWL/SWR/SDL/SDR), called from */
/* the arm64 EE JIT (C.16) with the decoded rt index. Line-for-line mirrors of */
/* the interpreter ops (R5900OpcodeImpl.c) -- they merge with the existing */
/* register/memory contents at the aligned address and never take a misalign */
/* exception, so translating them is a plain helper call. */
void eeLWL_arm64(u32 addr, u32 rt)
{
	static const u32 MASK[4]  = { 0xffffff, 0x0000ffff, 0x000000ff, 0x00000000 };
	static const u8  SHIFT[4] = { 24, 16, 8, 0 };
	const u32 shift = addr & 3;
	const u32 mem   = memRead32(addr & ~3);
	if (!rt) return;
	cpuRegs.GPR.r[rt].SD[0] = (s32)((cpuRegs.GPR.r[rt].UL[0] & MASK[shift]) | (mem << SHIFT[shift]));
}
void eeLWR_arm64(u32 addr, u32 rt)
{
	static const u32 MASK[4]  = { 0x000000, 0xff000000, 0xffff0000, 0xffffff00 };
	static const u8  SHIFT[4] = { 0, 8, 16, 24 };
	const u32 shift = addr & 3;
	u32 mem         = memRead32(addr & ~3);
	if (!rt) return;
	mem = (cpuRegs.GPR.r[rt].UL[0] & MASK[shift]) | (mem >> SHIFT[shift]);
	if (shift == 0) cpuRegs.GPR.r[rt].SD[0] = (s32)mem; /* sign-extends into 64 */
	else            cpuRegs.GPR.r[rt].UL[0] = mem;      /* upper 32 preserved */
}
static const u64 s_LDL_MASK[8] =
{	0x00ffffffffffffffULL, 0x0000ffffffffffffULL, 0x000000ffffffffffULL, 0x00000000ffffffffULL,
	0x0000000000ffffffULL, 0x000000000000ffffULL, 0x00000000000000ffULL, 0x0000000000000000ULL
};
static const u64 s_LDR_MASK[8] =
{	0x0000000000000000ULL, 0xff00000000000000ULL, 0xffff000000000000ULL, 0xffffff0000000000ULL,
	0xffffffff00000000ULL, 0xffffffffff000000ULL, 0xffffffffffff0000ULL, 0xffffffffffffff00ULL
};
void eeLDL_arm64(u32 addr, u32 rt)
{
	static const u8 SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	const u32 shift = addr & 7;
	const u64 mem   = memRead64(addr & ~7);
	if (!rt) return;
	cpuRegs.GPR.r[rt].UD[0] = (cpuRegs.GPR.r[rt].UD[0] & s_LDL_MASK[shift]) | (mem << SHIFT[shift]);
}
void eeLDR_arm64(u32 addr, u32 rt)
{
	static const u8 SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	const u32 shift = addr & 7;
	const u64 mem   = memRead64(addr & ~7);
	if (!rt) return;
	cpuRegs.GPR.r[rt].UD[0] = (cpuRegs.GPR.r[rt].UD[0] & s_LDR_MASK[shift]) | (mem >> SHIFT[shift]);
}
/* ---- C.67: fused unaligned pairs ---- */
/* The canonical MIPS unaligned idioms -- LDL rt,off+7(b); LDR rt,off(b) and the */
/* LWL/LWR, SDL/SDR, SWL/SWR equivalents -- fully overwrite rt (or the memory */
/* range), so the JIT emits them as ONE host unaligned access through fastmem. */
/* These wrappers are the slow path a faulting fused access lands in (hardware */
/* registers / unmapped pages). They replicate the two per-op helpers above */
/* EXACTLY, in the guest's left-then-right order, including the double read of */
/* the SAME word when the address is aligned -- an MMIO read can have side */
/* effects, and the unfused sequence performed both reads. */
u32 eeReadU32_arm64(u32 addr) /* == LWL rt,addr+3 ; LWR rt,addr (full pair) */
{
	const u32 s    = addr & 3;
	const u32 memL = memRead32((addr + 3) & ~3); /* LWL's read */
	const u32 memR = memRead32(addr & ~3);       /* LWR's read */
	if (!s)
		return memR; /* LWL's mask is 0 at shift 3: the pair degenerates to the plain word */
	return (memL << (32 - 8 * s)) | (memR >> (8 * s));
}
u64 eeReadU64_arm64(u32 addr) /* == LDL rt,addr+7 ; LDR rt,addr (full pair) */
{
	const u32 s    = addr & 7;
	const u64 memL = memRead64((addr + 7) & ~7); /* LDL's read */
	const u64 memR = memRead64(addr & ~7);       /* LDR's read */
	if (!s)
		return memR;
	return (memL << (64 - 8 * s)) | (memR >> (8 * s));
}
void eeWriteU32_arm64(u32 addr, u32 val) /* == SWL rt,addr+3 ; SWR rt,addr */
{
	/* SWL at addr+3 (shift (s+3)&3), then SWR at addr (shift s) -- each a */
	/* read-modify-write of its aligned word, in that order, like the helpers. */
	static const u32 SWL_MASK[4]  = { 0xffffff00, 0xffff0000, 0xff000000, 0x00000000 };
	static const u8  SWL_SHIFT[4] = { 24, 16, 8, 0 };
	static const u32 SWR_MASK[4]  = { 0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff };
	static const u8  SWR_SHIFT[4] = { 0, 8, 16, 24 };
	const u32 aL = addr + 3, sL = aL & 3, sR = addr & 3;
	const u32 mL = memRead32(aL & ~3);
	u32 mR;
	memWrite32(aL & ~3, (val >> SWL_SHIFT[sL]) | (mL & SWL_MASK[sL]));
	mR = memRead32(addr & ~3);
	memWrite32(addr & ~3, (val << SWR_SHIFT[sR]) | (mR & SWR_MASK[sR]));
}
void eeWriteU64_arm64(u32 addr, u64 val) /* == SDL rt,addr+7 ; SDR rt,addr */
{
	static const u64 SDL_MASK[8] =
	{	0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL, 0xffffffff00000000ULL,
		0xffffff0000000000ULL, 0xffff000000000000ULL, 0xff00000000000000ULL, 0x0000000000000000ULL };
	static const u8  SDL_SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	static const u64 SDR_MASK[8] =
	{	0x0000000000000000ULL, 0x00000000000000ffULL, 0x000000000000ffffULL, 0x0000000000ffffffULL,
		0x00000000ffffffffULL, 0x000000ffffffffffULL, 0x0000ffffffffffffULL, 0x00ffffffffffffffULL };
	static const u8  SDR_SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	const u32 aL = addr + 7, sL = aL & 7, sR = addr & 7;
	const u64 mL = memRead64(aL & ~7);
	u64 mR;
	memWrite64(aL & ~7, (val >> SDL_SHIFT[sL]) | (mL & SDL_MASK[sL]));
	mR = memRead64(addr & ~7);
	memWrite64(addr & ~7, (val << SDR_SHIFT[sR]) | (mR & SDR_MASK[sR]));
}

void eeSWL_arm64(u32 addr, u32 rt)
{
	static const u32 MASK[4]  = { 0xffffff00, 0xffff0000, 0xff000000, 0x00000000 };
	static const u8  SHIFT[4] = { 24, 16, 8, 0 };
	const u32 shift = addr & 3;
	const u32 mem   = memRead32(addr & ~3);
	memWrite32(addr & ~3, (cpuRegs.GPR.r[rt].UL[0] >> SHIFT[shift]) | (mem & MASK[shift]));
}
void eeSWR_arm64(u32 addr, u32 rt)
{
	static const u32 MASK[4]  = { 0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff };
	static const u8  SHIFT[4] = { 0, 8, 16, 24 };
	const u32 shift = addr & 3;
	const u32 mem   = memRead32(addr & ~3);
	memWrite32(addr & ~3, (cpuRegs.GPR.r[rt].UL[0] << SHIFT[shift]) | (mem & MASK[shift]));
}
void eeSDL_arm64(u32 addr, u32 rt)
{
	static const u64 MASK[8] =
	{	0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL, 0xffffffff00000000ULL,
		0xffffff0000000000ULL, 0xffff000000000000ULL, 0xff00000000000000ULL, 0x0000000000000000ULL
	};
	static const u8 SHIFT[8] = { 56, 48, 40, 32, 24, 16, 8, 0 };
	const u32 shift = addr & 7;
	const u64 mem   = memRead64(addr & ~7);
	memWrite64(addr & ~7, (cpuRegs.GPR.r[rt].UD[0] >> SHIFT[shift]) | (mem & MASK[shift]));
}
void eeSDR_arm64(u32 addr, u32 rt)
{
	static const u64 MASK[8] =
	{	0x0000000000000000ULL, 0x00000000000000ffULL, 0x000000000000ffffULL, 0x0000000000ffffffULL,
		0x00000000ffffffffULL, 0x000000ffffffffffULL, 0x0000ffffffffffffULL, 0x00ffffffffffffffULL
	};
	static const u8 SHIFT[8] = { 0, 8, 16, 24, 32, 40, 48, 56 };
	const u32 shift = addr & 7;
	const u64 mem   = memRead64(addr & ~7);
	memWrite64(addr & ~7, (cpuRegs.GPR.r[rt].UD[0] << SHIFT[shift]) | (mem & MASK[shift]));
}
/* EE event test, called by the JIT after BEQ/BNE not-taken and (with the cycle */
/* update) after every taken/unconditional branch -- matching doBranch() and the */
/* interpreter branch handlers. intUpdateCPUCycles() flushes cpuBlockCycles into */
/* cpuRegs.cycle so events actually fire (without it the JIT livelocks). */
void eeEventTest_arm64(void)   { intEventTest(); }
void eeUpdateCycles_arm64(void){ intUpdateCPUCycles(); }
#endif

static void intClear(u32 Addr, u32 Size) { }
static void intShutdown(void) { }

R5900cpu intCpu =
{
	intReserve,
	intShutdown,

	intReset,
	intExecute,

	intSafeExitExecution,
	intCancelInstruction,

	intClear
};

#ifdef ARCH_ARM64
/* arm64 EE recompiler provider (Phase C.3). Execution loop/exit/cancel are shared */
/* with the interpreter (same intJmpBuf); only Reserve/Reset/Shutdown/Clear and the */
/* per-PC block dispatch (eeRecExecute_arm64 -> eeJitRunBlock_arm64) are the JIT's. */
R5900cpu recCpu =
{
	eeJitReserve_arm64,
	eeJitShutdown_arm64,

	eeJitReset_arm64,
	eeRecExecute_arm64,

	intSafeExitExecution,
	intCancelInstruction,

	eeJitClear_arm64
};
#endif

/* Bounded interpreter stepping for the EE COP1 audit rig (design in
 * tests/fpaudit/EE-COP1-DESIGN.txt): the oracle half needs no event
 * machinery, just execI a fixed instruction count. */
PS2_AUDIT_EXPORT
void ps2_audit_ee_stepn(unsigned n)
{
	while (n--)
		execI();
}
