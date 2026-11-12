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

#include "common/Pcsx2Defs.h"
#include <stdlib.h>

typedef struct microVU microVU;

/* ------------------------------------------------------------------ */
/* Global Variables */
/* ------------------------------------------------------------------ */

typedef struct mVU_Globals
{

	u32   absclip [4];
	u32   signbit [4];
	u32   minvals [4];
	u32   maxvals [4];
	u32   exponent[4];
	u32   one     [4];
	u32   addm1   [4];   /* add/sub operand masking */
	u32   addm24  [4];
	u32   addm25  [4];
	u32   addm23  [4];
	u32   addzero [4];
	/* Exact add/sub bit pipeline: PS2 bits -> IEEE double BITS by integer
	 * construction (never through a single, which cannot hold the PS2's
	 * exponent-255 octave), addpd, fields back out. Proven bit-exact
	 * against ps2float by tests/fpaudit (emit ADD/SUB rows, 0/2.03M). */
	u32   xakff   [4];   /* exponent field mask     */
	u32   xak896  [4];   /* single->double rebias   */
	u32   xamantm [4];   /* single mantissa mask    */
	u32   xadblhm [4];   /* double mantissa hi mask */
	u32   xak7ff  [4];   /* double exponent mask    */
	u32   xakfd   [4];   /* detector: exp > 0xFD    */
	u32   multz16 [4];
	u32   mulkeepw[4];
	u32   addmall [4];
	u32   addmn25 [4];   /* -25 */
	u32   mulman  [4];   /* exact multiply */
	u32   mulimp  [4];
	u32   mul127  [4];
	u32   mul255  [4];
	u32   mul1    [4];
	u32   mulg    [4];
	u32   mullo   [4];
	u32   mul64one[4];
	/* exact-multiply tree stub */
	u32   tPack   [4];
	u32   tLUTdbl [4];
	u32   tLUTneg [4];
	u32   tLUTany [4];
	u32   t7      [4];
	u32   tOnesP  [4];
	u32   tOneP   [4];
	u32   tF800   [4];
	u32   tF000   [4];
	u32   t0800   [4];
	u32   t0400   [4];
	u32   t8000   [4];
	u32   tOnesP1 [4];
	u32   tOnesP2 [4];
	u32   tOnesP3 [4];
	u32   tOneP1  [4];
	u32   tOneP2  [4];
	u32   tOneP3  [4];
	u32   Pi4     [4];
	u32   T1      [4];
	u32   T2      [4];
	u32   T3      [4];
	u32   T4      [4];
	u32   T5      [4];
	u32   T6      [4];
	u32   T7      [4];
	u32   T8      [4];
	u32   S2      [4];
	u32   S3      [4];
	u32   S4      [4];
	u32   S5      [4];
	u32   E1      [4];
	u32   E2      [4];
	u32   E3      [4];
	u32   E4      [4];
	u32   E5      [4];
	u32   E6      [4];
	u32   I32MAXF [4];
	float FTOI_4  [4];
	float FTOI_12 [4];
	float FTOI_15 [4];
	float ITOF_4  [4];
	float ITOF_12 [4];
	float ITOF_15 [4];
} mVU_Globals;

PCSX2_ALIGN(32) static const mVU_Globals mVUglob = {
#define __four(val) { val, val, val, val }
	__four(0x7fffffff),
	__four(0x80000000),
	__four(0xff7fffff),
	__four(0x7f7fffff),
	__four(0x7f800000),
	__four(0x3f800000),
	__four(0x00000001),
	__four(0x00000018),
	__four(0x00000019),
	__four(0x00000017),
	__four(0x00000000),
	__four(0x000000ff),
	__four(0x00000380),
	__four(0x007fffff),
	__four(0x000fffff),
	__four(0x000007ff),
	__four(0x000000fd),
	__four(0x0000ffff),
	{0, 0, 0, 0x7fffffff},
	__four(0xffffffff),
	__four(0xffffffe7),
	__four(0x007fffff),
	__four(0x00800000),
	__four(127),
	__four(255),
	__four(1),
	{0x007f8000, 0, 0x007f8000, 0},
	{0xffffffff, 0, 0xffffffff, 0},
	{1, 0, 1, 0},
	{0x05040100, 0x0d0c0908, 0x80808080, 0x80808080},
	{0xff000000, 0x000000ff, 0xff000000, 0x000000ff},
	{0x00000000, 0x00ffffff, 0x00000000, 0x00ffffff},
	{0xffffff00, 0x00ffffff, 0xffffff00, 0x00ffffff},
	__four(0x00070007),
	{0xffffffff, 0xffffffff, 0xff00ff00, 0xff00ff00},
	{0x00010001, 0x00010001, 0x01000100, 0x01000100},
	__four(0xf800f800),
	__four(0xf000f000),
	__four(0x08000800),
	__four(0x04000400),
	__four(0x80008000),
	{0xfffcfffc, 0xfffcfffc, 0xfc00fc00, 0xfc00fc00},
	{0xfff0fff0, 0xfff0fff0, 0xf000f000, 0xf000f000},
	{0xffc0ffc0, 0xffc0ffc0, 0xc000c000, 0xc000c000},
	{0x00040004, 0x00040004, 0x04000400, 0x04000400},
	{0x00100010, 0x00100010, 0x10001000, 0x10001000},
	{0x00400040, 0x00400040, 0x40004000, 0x40004000},
	__four(0x3f490fdb),
	__four(0x3f7ffff5),
	__four(0xbeaaa61c),
	__four(0x3e4c40a6),
	__four(0xbe0e6c63),
	__four(0x3dc577df),
	__four(0xbd6501c4),
	__four(0x3cb31652),
	__four(0xbb84d7e7),
	__four(0xbe2aaaa4),
	__four(0x3c08873e),
	__four(0xb94fb21f),
	__four(0x362e9c14),
	__four(0x3e7fffa8),
	__four(0x3d0007f4),
	__four(0x3b29d3ff),
	__four(0x3933e553),
	__four(0x36b63510),
	__four(0x353961ac),
	__four(0x4effffff),
	__four(16.0),
	__four(4096.0),
	__four(32768.0),
	__four(0.0625f),
	__four(0.000244140625),
	__four(0.000030517578125),
#undef __four
};

#define _Ibit_ ((uint)(1 << 31))
#define _Ebit_ ((uint)(1 << 30))
#define _Mbit_ ((uint)(1 << 29))
#define _Dbit_ ((uint)(1 << 28))
#define _Tbit_ ((uint)(1 << 27))

#define divI ((uint)(0x1040000))
#define divD ((uint)0x2080000)

/* ------------------------------------------------------------------ */
/* Helper Macros */
/* ------------------------------------------------------------------ */

#define _Ft_ ((mVU->code >> 16) & 0x1F) /* The ft part of the instruction register */
#define _Fs_ ((mVU->code >> 11) & 0x1F) /* The fs part of the instruction register */
#define _Fd_ ((mVU->code >>  6) & 0x1F) /* The fd part of the instruction register */

#define _It_ ((mVU->code >> 16) & 0xF)  /* The it part of the instruction register */
#define _Is_ ((mVU->code >> 11) & 0xF)  /* The is part of the instruction register */
#define _Id_ ((mVU->code >>  6) & 0xF)  /* The id part of the instruction register */

#define _X ((mVU->code >> 24) & 0x1)
#define _Y ((mVU->code >> 23) & 0x1)
#define _Z ((mVU->code >> 22) & 0x1)
#define _W ((mVU->code >> 21) & 0x1)

#define _cX ((cpuRegs.code >> 24) & 0x1)
#define _cY ((cpuRegs.code >> 23) & 0x1)
#define _cZ ((cpuRegs.code >> 22) & 0x1)
#define _cW ((cpuRegs.code >> 21) & 0x1)

#define _X_Y_Z_W   (((mVU->code >> 21) & 0xF))
#define _cX_Y_Z_W   (((cpuRegs.code >> 21) & 0xF))
#define _cXYZW_SS  (_cX + _cY + _cZ + _cW == 1)
#define _cXYZW_SS2  (_cXYZW_SS && (_cX_Y_Z_W != 8))

#define _XYZW_SS   (_X + _Y + _Z + _W == 1)
#define _XYZW_SS2  (_XYZW_SS && (_X_Y_Z_W != 8))
#define _XYZW_PS   (_X_Y_Z_W == 0xf)
#define _XYZWss(x) ((x == 8) || (x == 4) || (x == 2) || (x == 1))

#define _bc_   (mVU->code & 0x3)
#define _bc_x ((mVU->code & 0x3) == 0)
#define _bc_y ((mVU->code & 0x3) == 1)
#define _bc_z ((mVU->code & 0x3) == 2)
#define _bc_w ((mVU->code & 0x3) == 3)

#define _Fsf_ ((mVU->code >> 21) & 0x03)
#define _Ftf_ ((mVU->code >> 23) & 0x03)

#define _Imm5_  ((s16) (((mVU->code & 0x400) ? 0xfff0 : 0) | ((mVU->code >> 6) & 0xf)))
#define _Imm11_ ((s32)  ((mVU->code & 0x400) ? (0xfffffc00 |  (mVU->code & 0x3ff)) : (mVU->code & 0x3ff)))
#define _Imm12_ ((u32)((((mVU->code >> 21) & 0x1) << 11)   |  (mVU->code & 0x7ff)))
#define _Imm15_ ((u32) (((mVU->code >> 10) & 0x7800)       |  (mVU->code & 0x7ff)))
#define _Imm24_ ((u32)   (mVU->code & 0xffffff))

#define isCOP2      (mVU->cop2 != 0)
#define isVU1       (mVU->index != 0)
#define isVU0       (mVU->index == 0)
#define getIndex    (isVU1 ? 1 : 0)
#define getVUmem(x) (((isVU1) ? (x & 0x3ff) : ((x >= 0x400) ? (x & 0x43f) : (x & 0xff))) * 16)
#define offsetSS    ((_X) ? (0) : ((_Y) ? (4) : ((_Z) ? 8 : 12)))
#define offsetReg   ((_X) ? (0) : ((_Y) ? (1) : ((_Z) ? 2 :  3)))

/* Function Params */
#define mP microVU* mVU, int recPass
#define mV microVU* mVU
#define mF int recPass
#define mX mVU, recPass

typedef void Fntype_mVUrecInst(microVU* mVU, int recPass);
typedef Fntype_mVUrecInst* Fnptr_mVUrecInst;

/* Function/Template Stuff */
#define mVUx (vuIndex ? &microVU1 : &microVU0)
#define mVUop(opName) static void opName(mP)

/* Define Passes */
#define pass1 if (recPass == 0) /* Analyze */
#define pass2 if (recPass == 1) /* Recompile */
#define pass4 if (recPass == 3) /* Flag stuff */

/* Upper Opcode Cases */
#define opCase1 if (opCase == 1) /* Normal Opcodes */
#define opCase2 if (opCase == 2) /* BC Opcodes */
#define opCase3 if (opCase == 3) /* I  Opcodes */
#define opCase4 if (opCase == 4) /* Q  Opcodes */

/* ------------------------------------------------------------------ */

/* Misc Macros... */
#define mVUcurProg   mVU->prog.cur[0]
#define mVUblocks    mVU->prog.cur->block
#define mVUir        mVU->prog.IRinfo
#define mVUbranch    mVU->prog.IRinfo.branch
#define mVUcycles    mVU->prog.IRinfo.cycles
#define mVUcount     mVU->prog.IRinfo.count
#define mVUpBlock    mVU->prog.IRinfo.pBlock
#define mVUblock     mVU->prog.IRinfo.block
#define mVUregs      mVU->prog.IRinfo.block.pState
#define mVUregsTemp  mVU->prog.IRinfo.regsTemp
#define iPC          mVU->prog.IRinfo.curPC
#define mVUsFlagHack mVU->prog.IRinfo.sFlagHack
#define mVUconstReg  mVU->prog.IRinfo.constReg
#define mVUstartPC   mVU->prog.IRinfo.startPC
#define mVUinfo      mVU->prog.IRinfo.info[iPC / 2]
#define mVUstall     mVUinfo.stall
#define mVUup        mVUinfo.uOp
#define mVUlow       mVUinfo.lOp
#define sFLAG        mVUinfo.sFlag
#define mFLAG        mVUinfo.mFlag
#define cFLAG        mVUinfo.cFlag
#define mVUrange     (mVUcurProg.ranges->data[0])
#define isEvilBlock  (mVUpBlock->pState.blockType == 2)
#define isBadOrEvil  (mVUlow.badBranch || mVUlow.evilBranch)
#define isConditional (mVUlow.branch > 2 && mVUlow.branch < 9)
#define xPC          ((iPC / 2) * 8)
#define curI         ((u32*)vuRegs[mVU->index].Micro)[iPC] /* mVUcurProg.data[iPC] */
#define setCode()    { mVU->code = curI; }
#define bSaveAddr    (((xPC + 16) & (mVU->microMemSize-8)) / 8)
#define shufflePQ    (((mVU->p) ? 0xb0 : 0xe0) | ((mVU->q) ? 0x01 : 0x04))
#define Rmem         &vuRegs[mVU->index].VI[REG_R].UL
#define aWrap(x, m)  ((x > m) ? 0 : x)
#define shuffleSS(x) ((x == 1) ? (0x27) : ((x == 2) ? (0xc6) : ((x == 4) ? (0xe1) : (0xe4))))
#define clampE       CHECK_VU_EXTRA_OVERFLOW(mVU->index)
#define islowerOP    ((iPC & 1) == 0)

#define blockCreate(addr) \
	{ \
		if (!mVUblocks[addr]) \
		{ \
			mVUblocks[addr] = (microBlockManager*)malloc(sizeof(microBlockManager)); \
			mVUbm_init(mVUblocks[addr]); \
		} \
	}

/* Fetches the PC and instruction opcode relative to the current PC.  Used to rewind and */
/* fast-forward the IR state while calculating VU pipeline conditions (branches, writebacks, etc) */
#define incPC(x)  { iPC = ((iPC + (x)) & mVU->progMemMask); mVU->code = curI; }
#define incPC2(x) { iPC = ((iPC + (x)) & mVU->progMemMask); }

/* Flag Info (Set if next-block's first 4 ops will read current-block's flags) */
#define __Status (mVUregs.needExactMatch & 1)
#define __Mac    (mVUregs.needExactMatch & 2)
#define __Clip   (mVUregs.needExactMatch & 4)

/* Pass 3 Helper Macros (Used for program logging) */
#define _Fsf_String ((_Fsf_ == 3) ? "w" : ((_Fsf_ == 2) ? "z" : ((_Fsf_ == 1) ? "y" : "x")))
#define _Ftf_String ((_Ftf_ == 3) ? "w" : ((_Ftf_ == 2) ? "z" : ((_Ftf_ == 1) ? "y" : "x")))
#define xyzwStr(x, s) (_X_Y_Z_W == x) ? s:
#define _XYZW_String (xyzwStr(1, "w") (xyzwStr(2, "z") (xyzwStr(3, "zw") (xyzwStr(4, "y") (xyzwStr(5, "yw") (xyzwStr(6, "yz") (xyzwStr(7, "yzw") (xyzwStr(8, "x") (xyzwStr(9, "xw") (xyzwStr(10, "xz") (xyzwStr(11, "xzw") (xyzwStr(12, "xy") (xyzwStr(13, "xyw") (xyzwStr(14, "xyz") "xyzw"))))))))))))))
#define _BC_String   (_bc_x ? "x" : (_bc_y ? "y" : (_bc_z ? "z" : "w")))

/* ------------------------------------------------------------------ */
/* Optimization / Debug Options */
/* ------------------------------------------------------------------ */

/* Reg Alloc */
#define doRegAlloc 1 /* Set to false to flush every 32bit Instruction */
/* This turns off reg alloc for the most part, but reg alloc will still */
/* be done within instructions... Also on doSwapOp() regAlloc is needed between */
/* Lower and Upper instructions, so in this case it flushes after the full */
/* 64bit instruction (lower and upper) */

/* No Flag Optimizations */
#define noFlagOpts 0 /* Set to true to disable all flag setting optimizations */
/* Note: The flag optimizations this disables should all be harmless, so */
/* this option is mainly just for debugging... it effectively forces mVU */
/* to always update Mac and Status Flags (both sticky and non-sticky) whenever */
/* an Upper Instruction updates them. It also always transfers the 4 possible */
/* flag instances between blocks... */

/* Multiple Flag Instances */
#define doSFlagInsts 1 /* Set to true to enable multiple status flag instances */
#define doMFlagInsts 1 /* Set to true to enable multiple mac    flag instances */
#define doCFlagInsts 1 /* Set to true to enable multiple clip   flag instances */
/* This is the correct behavior of the VU's. Due to the pipeline of the VU's */
/* there can be up to 4 different instances of values to keep track of */
/* for the 3 different types of flags: Status, Mac, Clip flags. */
/* Setting one of these to 0 acts as if there is only 1 instance of the */
/* corresponding flag, which may be useful when debugging flag pipeline bugs. */

/* Branch in Branch Delay Slots */
#define doBranchInDelaySlot 1 /* Set to true to enable evil-branches */
/* This attempts to emulate the correct behavior for branches in branch delay */
/* slots. It is evil that games do this, and handling the different possible */
/* cases is tricky and bug prone. If this option is disabled then the second */
/* branch is treated as a NOP and effectively ignored. */

/* Constant Propagation */
#define doConstProp 0 /* Set to true to turn on vi15 const propagation */
#define doViConstProp 1 /* Block-local VI const propagation into memory-op addressing */
/* Enables Constant Propagation for Jumps based on vi15 'link-register' */
/* allowing us to know many indirect jump target addresses. */
/* Makes GoW a lot slower due to extra recompilation time and extra code-gen! */

/* Indirect Jump Caching */
#define doJumpCaching 1 /* Set to true to enable jump caching */
/* Indirect jumps (JR/JALR) will remember the entry points to their previously */
/* jumped-to addresses. This allows us to skip the microBlockManager::search() */
/* routine that is performed every indirect jump in order to find a block within a */
/* program that matches the correct pipeline state. */

/* Indirect Jumps are part of same cached microProgram */
#define doJumpAsSameProgram 0 /* Set to true to treat jumps as same program */
/* Enabling this treats indirect jumps (JR/JALR) as part of the same microProgram */
/* when determining the valid ranges for the microProgram cache. Disabling this */
/* counts indirect jumps as separate cached microPrograms which generally leads */
/* to more microPrograms being cached, but the programs created are smaller and */
/* the overall cache usage ends up being more optimal; it can also help prevent */
/* constant recompilation problems in certain games. */
/* Note: You MUST disable doJumpCaching if you enable this option. */

/* Handling of D-Bit in Micro Programs */
#define doDBitHandling 0
/* This flag shouldn't be enabled in released versions of games. Any games which */
/* need this method of pausing the VU should be using the T-Bit instead, however */
/* this could prove useful for VU debugging. */

/* Whole program comparison on search */
#define doWholeProgCompare 0
/* This shouldn't be needed and could inflate program generation. */
/* Compares the entire VU memory with the stored micro program's memory, regardless of if it's used. */
/* Generally slower but may be useful for debugging. */

/* ------------------------------------------------------------------ */
/* Speed Hacks (can cause infinite loops, SPS, Black Screens, etc...) */
/* ------------------------------------------------------------------ */

/* Status Flag Speed Hack */
#define CHECK_VU_FLAGHACK (EmuConfig.Speedhacks.vuFlagHack)
/* This hack only updates the Status Flag on blocks that will read it. */
/* Most blocks do not read status flags, so this is a big speedup. */
