#include "common/emitter/c89ops.h"
#include <cstring>
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

#include "../microVU/microVU_Pipeline.inl"

//------------------------------------------------------------------
// Execute VU Opcode/Instruction (Upper and Lower)
//------------------------------------------------------------------

/* Block-local VI constant tracking, rooted at vi00 and immediates.
 * The storage - constReg[16] - shipped with the IR struct for a vi15
 * jump experiment that was never enabled; nothing else ever wrote or
 * read it. The tracker runs in pass two right after each lower op is
 * emitted, so the op consumed its sources before its own write lands
 * in the table. Constants derive only from vi00 and instruction
 * immediates, never from block-entry values, which keeps a compiled
 * block correct across executions with different entry VIs; any VI
 * writer outside the whitelist invalidates its destination, with the
 * written register taken from the analysis rather than re-decoded. */
static void mVUtrackVIConst(microVU* mVU)
{
	const u32 wreg = mVUlow.VI_write.reg;
	u32 op, valid = 0, value = 0;
	if (!doViConstProp || !wreg || wreg >= 16)
		return;
	#define vicKnown(r) ((r) == 0 || mVUconstReg[r].isValid)
	#define vicVal(r)   ((r) == 0 ? 0u : mVUconstReg[r].regValue)
	op = (mVU->code >> 25) & 0x7f;
	if (op == 0x08 && vicKnown(_Is_))      { valid = 1; value = (u16)(vicVal(_Is_) + _Imm15_); } /* IADDIU */
	else if (op == 0x09 && vicKnown(_Is_)) { valid = 1; value = (u16)(vicVal(_Is_) - _Imm15_); } /* ISUBIU */
	else if (op == 0x40)
	{
		const u32 l = mVU->code & 0x3f;
		if      (l == 0x32 && vicKnown(_Is_))                     { valid = 1; value = (u16)(vicVal(_Is_) + _Imm5_); }         /* IADDI */
		else if (l == 0x30 && vicKnown(_Is_) && vicKnown(_It_))   { valid = 1; value = (u16)(vicVal(_Is_) + vicVal(_It_)); }   /* IADD  */
		else if (l == 0x31 && vicKnown(_Is_) && vicKnown(_It_))   { valid = 1; value = (u16)(vicVal(_Is_) - vicVal(_It_)); }   /* ISUB  */
		else if (l == 0x34 && vicKnown(_Is_) && vicKnown(_It_))   { valid = 1; value = (u16)(vicVal(_Is_) & vicVal(_It_)); }   /* IAND  */
		else if (l == 0x35 && vicKnown(_Is_) && vicKnown(_It_))   { valid = 1; value = (u16)(vicVal(_Is_) | vicVal(_It_)); }   /* IOR   */
		else if (l >= 0x3c && ((mVU->code >> 6) & 0x1f) == 0x0d && vicKnown(wreg))
		{
			/* LQI and SQI post-increment their address register, LQD
			 * and SQD pre-decrement it; a known register stays known
			 * through the adjustment, which is what lets an unrolled
			 * streaming chain fold every link to its own absolute
			 * address. */
			valid = 1;
			value = (u16)(vicVal(wreg) + ((l <= 0x3d) ? 1 : -1));
		}
	}
	mVUconstReg[wreg].isValid  = valid;
	mVUconstReg[wreg].regValue = value;
	#undef vicKnown
	#undef vicVal
}

#define doUpperOp(mV) \
	mVUopU(mVU, 1); \
	mVUdivSet(mVU)

#define doLowerOp(mV) \
	incPC(-1); \
	mVUopL(mVU, 1); \
	mVUtrackVIConst(mVU); \
	incPC(1)

#define flushRegs(mV) if (!doRegAlloc) mVUra_flushAll(mVU->regAlloc, 1);

void doIbit(mV)
{
	if (mVUup.iBit)
	{
		incPC(-1);
		mVUra_clearRegVF(mVU->regAlloc, 33);
		if (EmuConfig.Gamefixes.IbitHack)
		{
			xe_mov32_rm(gprT1, &curI);
			xe_mov32_mr(&::vuRegs[mVU->index].VI[REG_I], gprT1);
		}
		else
		{
			u32 tempI;
			if (CHECK_VU_OVERFLOW(mVU->index) && ((curI & 0x7fffffff) >= 0x7f800000))
				tempI = (0x80000000 & curI) | 0x7f7fffff; // Clamp I Reg
			else
				tempI = curI;

			xe_mov32_mi(&::vuRegs[mVU->index].VI[REG_I], tempI);
		}
		incPC(1);
	}
}

void doSwapOp(mV)
{
	if (mVUinfo.backupVF && !mVUlow.noWriteVF)
	{
		// Allocate t1 first for better chance of reg-alloc
		const int t1 = mVUra_allocReg(mVU->regAlloc, mVUlow.VF_write.reg, -1, 0, 1);
		const int t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		xe_movaps_xx(t2, t1); // Backup VF reg
		mVUra_clearNeededXMM(mVU->regAlloc, t1);

		mVUopL(mVU, 1);
		mVUtrackVIConst(mVU);

		const int t3 = mVUra_allocReg(mVU->regAlloc, mVUlow.VF_write.reg, mVUlow.VF_write.reg, 0xf, 0);
		xe_xorps_xx(t2, t3); // Swap new and old values of the register
		xe_xorps_xx(t3, t2); // Uses xor swap trick...
		xe_xorps_xx(t2, t3);
		mVUra_clearNeededXMM(mVU->regAlloc, t3);

		incPC(1);
		doUpperOp(mVU);

		const int t4 = mVUra_allocReg(mVU->regAlloc, -1, mVUlow.VF_write.reg, 0xf, 1);
		xe_movaps_xx(t4, t2);
		mVUra_clearNeededXMM(mVU->regAlloc, t4);
		mVUra_clearNeededXMM(mVU->regAlloc, t2);
	}
	else
	{
		mVUopL(mVU, 1);
		mVUtrackVIConst(mVU);
		incPC(1);
		flushRegs(mVU);
		doUpperOp(mVU);
	}
}

void mVUexecuteInstruction(mV)
{
	if (mVUlow.isNOP)
	{
		incPC(1);
		doUpperOp(mVU);
		flushRegs(mVU);
		doIbit(mVU);
	}
	else if (!mVUinfo.swapOps)
	{
		incPC(1);
		doUpperOp(mVU);
		flushRegs(mVU);
		doLowerOp(mVU);
	}
	else
	{
		doSwapOp(mVU);
	}

	flushRegs(mVU);
}

//------------------------------------------------------------------
// Warnings / Errors / Illegal Instructions
//------------------------------------------------------------------

// If 1st op in block is a bad opcode, then don't compile rest of block (Dawn of Mana Level 2)
// The BIOS writes upper and lower NOPs in reversed slots (bug)
//So to prevent spamming we ignore these, however its possible the real VU will bomb out if
//this happens, so we will bomb out without warning.
#define mVUcheckBadOp(mV) if (mVUinfo.isBadOp && mVU->code != 0x8000033c) mVUinfo.isEOB = 1

static __ri void branchWarning(mV)
{
	incPC(-2);
	incPC(2);
	if (mVUup.eBit && mVUbranch)
		mVUlow.isNOP = 1;

	/* Check if VI Reg Written to on Branch Delay Slot Instruction */
	if (mVUinfo.isBdelay && !mVUlow.evilBranch)
	{
		if (mVUlow.VI_write.reg && mVUlow.VI_write.used && !mVUlow.readFlags)
		{
			mVUlow.backupVI = 1;
			mVUregs.viBackUp = mVUlow.VI_write.reg;
		}
	}
}

#define eBitPass1(mV, branch) \
	if (mVUregs.blockType != 1) \
	{ \
		branch     = 1; \
		mVUup.eBit = 1; \
	}

#define eBitWarning(mV) \
	incPC(2); \
	if (curI & _Ebit_) \
		mVUregs.blockType = 1; \
	incPC(-2)

// Test cycles to see if we need to exit-early...
void mVUtestCycles(microVU* mVU, microFlagCycles* mFC)
{
	iPC = mVUstartPC;

	// If the VUSyncHack is on, we want the VU to run behind, to avoid conditions where the VU is sped up.
	if (isVU0 && EmuConfig.Speedhacks.EECycleRate != 0 && (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
	{
		switch (C89_MIN((int)(EmuConfig.Speedhacks.EECycleRate), (int)(mVUcycles)))
		{
			case -3: // 50%
				mVUcycles *= 2.0f;
				break;
			case -2: // 60%
				mVUcycles *= 1.6666667f;
				break;
			case -1: // 75%
				mVUcycles *= 1.3333333f;
				break;
			case 1: // 130%
				mVUcycles /= 1.3f;
				break;
			case 2: // 180%
				mVUcycles /= 1.8f;
				break;
			case 3: // 300%
				mVUcycles /= 3.0f;
				break;
			default:
				break;
		}
	}
	xe_mov32_rm(XE_AX, &mVU->cycles);
	if (EmuConfig.Gamefixes.VUSyncHack)
		xe_sub32_ri(XE_AX, mVUcycles); /* Running behind, make sure we have time to run the block */
	else
		xe_sub32_ri(XE_AX, 1); /* Running ahead, make sure cycles left are above 0 */

	uint8_t* skip; xe_fwd_jcc32(Jcc_Unsigned, skip);

	xe_lea_far(XE_AX, &mVUpBlock->pState);
	xe_call_ptr(mVU->copyPLState);

	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		xe_mov64_mi_s32(&vuRegs[mVU->index].nextBlockCycles, mVUcycles);
	mVUendProgram(mVU, mFC, 0);

	xe_fwd_set32(skip);

	xe_sub32_mi(&mVU->cycles, mVUcycles);
}

//------------------------------------------------------------------
// Initializing
//------------------------------------------------------------------

// Initialize Variables
static __fi void mVUinitFirstPass(microVU* mVU, uptr pState, u8* thisPtr)
{
	mVUstartPC = iPC; // Block Start PC
	mVUbranch  = 0;   // Branch Type
	mVUcount   = 0;   // Number of instructions ran
	mVUcycles  = 0;   // Skips "M" phase, and starts counting cycles at "T" stage
	mVU->p      = 0;   // All blocks start at p index #0
	mVU->q      = 0;   // All blocks start at q index #0
	if ((uptr)&mVUregs != pState) // Loads up Pipeline State Info
		memcpy((u8*)&mVUregs, (u8*)pState, sizeof(microRegInfo));
	if (((uptr)&mVU->prog.lpState != pState))
		memcpy((u8*)&mVU->prog.lpState, (u8*)pState, sizeof(microRegInfo));
	mVUblock.x86ptrStart = thisPtr;
	mVUpBlock = mVUbm_add(mVUblocks[mVUstartPC / 2], mVU, &mVUblock); // Add this block to block manager
	mVUregs.needExactMatch = (mVUpBlock->pState.blockType) ? 7 : 0; // ToDo: Fix 1-Op block flag linking (MGS2:Demo/Sly Cooper)
	mVUregs.blockType = 0;
	mVUregs.viBackUp  = 0;
	mVUregs.flagInfo  = 0;
	mVUsFlagHack      = CHECK_VU_FLAGHACK;
	mVUinitConstValues();
}

//------------------------------------------------------------------
// Recompiler
//------------------------------------------------------------------

static void mVUDoDBit(microVU* mVU, microFlagCycles* mFC)
{
	if (mVU->index && THREAD_VU1)
		xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x400 : 0x4));
	else
		xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x400 : 0x4));
	uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
	if (!isVU1 || !THREAD_VU1)
	{
		xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2));
		xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
	}
	incPC(1);
	mVUDTendProgram(mVU, mFC, 1);
	incPC(-1);
	xe_fwd_set32(eJMP);
}

static void mVUDoTBit(microVU* mVU, microFlagCycles* mFC)
{
	if (mVU->index && THREAD_VU1)
		xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x800 : 0x8));
	else
		xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x800 : 0x8));
	uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
	if (!isVU1 || !THREAD_VU1)
	{
		xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4));
		xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
	}
	incPC(1);
	mVUDTendProgram(mVU, mFC, 1);
	incPC(-1);

	xe_fwd_set32(eJMP);
}

static void mvuPreloadRegisters(microVU* mVU, u32 endCount)
{
	static const int REQUIRED_FREE_XMMS = 3; // some space for temps
	static const int REQUIRED_FREE_GPRS = 1; // some space for temps

	u32 vfs_loaded = 0;
	u32 vis_loaded = 0;

	for (int reg = 0; reg < mVUra_getXmmCount(mVU->regAlloc); reg++)
	{
		const int vf = mVUra_getRegVF(mVU->regAlloc, reg);
		if (vf >= 0)
			vfs_loaded |= (1u << vf);
	}

	for (int reg = 0; reg < mVUra_getGPRCount(mVU->regAlloc); reg++)
	{
		const int vi = mVUra_getRegVI(mVU->regAlloc, reg);
		if (vi >= 0)
			vis_loaded |= (1u << vi);
	}

	const u32 orig_pc = iPC;
	const u32 orig_code = mVU->code;
	int free_regs = mVUra_getFreeXmmCount(mVU->regAlloc);
	int free_gprs = mVUra_getFreeGPRCount(mVU->regAlloc);

	/* C89 spellings of the old capture lambdas: everything they captured is
	 * passed or visible; the counters live in this frame. */
#define MVU_PRELOAD_VF(reg) do { \
		if (!(free_regs <= REQUIRED_FREE_XMMS || (reg) == 0 || (vfs_loaded & (1u << (reg))) != 0)) \
		{ \
			mVUra_clearNeededXMM(mVU->regAlloc, mVUra_allocReg(mVU->regAlloc, reg, -1, 0, 1)); \
			vfs_loaded |= (1u << (reg)); \
			free_regs--; \
		} \
	} while (0)
#define MVU_PRELOAD_VI(reg) do { \
		if (!(free_gprs <= REQUIRED_FREE_GPRS || (reg) == 0 || (vis_loaded & (1u << (reg))) != 0)) \
		{ \
			mVUra_clearNeededGPR(mVU->regAlloc, mVUra_allocGPR(mVU->regAlloc, reg, -1, 0, 0)); \
			vis_loaded |= (1u << (reg)); \
			free_gprs--; \
		} \
	} while (0)
#define MVU_CAN_PRELOAD() (free_regs >= REQUIRED_FREE_XMMS || free_gprs >= REQUIRED_FREE_GPRS)

	for (u32 x = 0; x < endCount && MVU_CAN_PRELOAD(); x++)
	{
		incPC(1);

		const microOp* info = &mVUinfo;
		if (info->doXGKICK)
			break;

		for (u32 i = 0; i < 2; i++)
		{
			MVU_PRELOAD_VF(info->uOp.VF_read[i].reg);
			MVU_PRELOAD_VF(info->lOp.VF_read[i].reg);
			if (info->lOp.VI_read[i].used)
				MVU_PRELOAD_VI(info->lOp.VI_read[i].reg);
		}

		const microVFreg* uvfr = &info->uOp.VF_write;
		if (uvfr->reg != 0 && (!uvfr->x || !uvfr->y || !uvfr->z || !uvfr->w))
		{
			// not writing entire vector
			MVU_PRELOAD_VF(uvfr->reg);
		}

		const microVFreg* lvfr = &info->lOp.VF_write;
		if (lvfr->reg != 0 && (!lvfr->x || !lvfr->y || !lvfr->z || !lvfr->w))
		{
			// not writing entire vector
			MVU_PRELOAD_VF(lvfr->reg);
		}

		if (info->lOp.branch)
			break;
	}

	iPC = orig_pc;
	mVU->code = orig_code;
#undef MVU_PRELOAD_VF
#undef MVU_PRELOAD_VI
#undef MVU_CAN_PRELOAD
}

void* mVUcompile(microVU* mVU, u32 startPC, uptr pState)
{
	microFlagCycles mFC;
	u8* thisPtr = x86Ptr;
	const u32 endCount = (((microRegInfo*)pState)->blockType) ? 1 : (mVU->microMemSize / 8);

	/* First Pass */
	iPC = startPC / 4;
	mVUsetupRange(mVU, startPC, 1); /* Setup Program Bounds/Range */
	mVUra_reset(mVU->regAlloc, 0); /* Reset regAlloc */
	mVUinitFirstPass(mVU, pState, thisPtr);
	mVUbranch = 0;
	for (int branch = 0; mVUcount < endCount;)
	{
		incPC(1);
		startLoop(mVU);
		mVUincCycles(mVU, 1);
		mVUopU(mVU, 0);
		mVUcheckBadOp(mVU);
		if (curI & _Ebit_)
		{
			eBitPass1(mVU, branch);
			// VU0 end of program MAC results can be read by COP2, so best to make sure the last instance is valid
			// Needed for State of Emergency 2 and Driving Emotion Type-S
			if (isVU0)
				mVUregs.needExactMatch |= 7;
		}

		if ((curI & _Mbit_) && isVU0)
		{
			if (xPC > 0)
			{
				incPC(-2);
				if (!(curI & _Mbit_)) //If the last instruction was also M-Bit we don't need to sync again
				{
					incPC(2);
					mVUup.mBit = 1;
				}
				else
					incPC(2);
			}
			else
				mVUup.mBit = 1;
		}

		if (curI & _Ibit_)
		{
			mVUlow.isNOP = 1;
			mVUup.iBit = 1;
			if (EmuConfig.Gamefixes.IbitHack)
			{
				mVUsetupRange(mVU, xPC, 0);
				if (branch < 2)
					mVUsetupRange(mVU, xPC + 8, 1); // Ideally we'd do +4 but the mmx compare only works in 64bits, this should be fine
			}
		}
		else
		{
			incPC(-1);
			mVUopL(mVU, 0);
			incPC(1);
		}
		if (curI & _Dbit_)
			mVUup.dBit = 1;
		if (curI & _Tbit_)
			mVUup.tBit = 1;
		mVUsetCycles(mVU);
		// Update XGKick information
		if (!mVUlow.isKick)
		{
			mVUregs.xgkickcycles += 1 + mVUstall;
			if (mVUlow.isMemWrite)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
		}
		else
		{
			// XGKick command counts as one cycle for the transfer.
			// Can be tested with Resident Evil: Outbreak, Kingdom Hearts, CART Fury.
			mVUregs.xgkickcycles = 1;
			mVUlow.kickcycles = 0;
		}

		mVUinfo.readQ = mVU->q;
		mVUinfo.writeQ = !mVU->q;
		mVUinfo.readP = mVU->p && isVU1;
		mVUinfo.writeP = !mVU->p && isVU1;
		mVUcount++;

		if (branch >= 2)
		{
			mVUinfo.isEOB = 1;

			if (branch == 3)
			{
				mVUinfo.isBdelay = 1;
			}

			branchWarning(mVU);
			if (mVUregs.xgkickcycles)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
			break;
		}
		else if (branch == 1)
		{
			branch = 2;
		}

		if (mVUbranch)
		{
			mVUsetFlagInfo(mVU);
			eBitWarning(mVU);
			branch = 3;
			mVUbranch = 0;
		}

		if (mVUup.mBit && !branch && !mVUup.eBit)
		{
			mVUregs.needExactMatch |= 7;
			if (mVUregs.xgkickcycles)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
			break;
		}

		if (mVUinfo.isEOB)
		{
			if (mVUregs.xgkickcycles)
			{
				mVUlow.kickcycles = mVUregs.xgkickcycles;
				mVUregs.xgkickcycles = 0;
			}
			break;
		}

		incPC(1);
	}

	// Fix up vi15 const info for propagation through blocks
	mVUregs.vi15 = (doConstProp && mVUconstReg[15].isValid) ? (u16)mVUconstReg[15].regValue : 0;
	mVUregs.vi15v = (doConstProp && mVUconstReg[15].isValid) ? 1 : 0;
	mVUsetFlags(mVU, &mFC);           // Sets Up Flag instances
	mVUoptimizePipeState(mVU);       // Optimize the End Pipeline State for nicer Block Linking
	mVUtestCycles(mVU, &mFC);         // Update VU Cycles and Exit Early if Necessary

	// Second Pass
	memset(mVUconstReg, 0, sizeof(mVU->prog.IRinfo.constReg)); /* block entry: no VI constants known */
	iPC = mVUstartPC;
	setCode();
	mVUbranch = 0;
	u32 x = 0;

	mvuPreloadRegisters(mVU, endCount);

	for (; x < endCount; x++)
	{
		if (mVUinfo.isEOB)
			x = 0xffff;
		if (mVUup.mBit)
		{
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_MFLAGSET);
		}

		if (isVU1 && mVUlow.kickcycles && CHECK_XGKICKHACK)
		{
			mVU_XGKICK_SYNC(mVU, 0);
		}

		mVUexecuteInstruction(mVU);
		if (!mVUinfo.isBdelay && !mVUlow.branch) //T/D Bit on branch is handled after the branch, branch delay slots are executed.
		{
			if (mVUup.tBit)
			{
				mVUDoTBit(mVU, &mFC);
			}
			else if (mVUup.dBit && doDBitHandling)
			{
				mVUDoDBit(mVU, &mFC);
			}
			else if (mVUup.mBit && !mVUup.eBit && !mVUinfo.isEOB)
			{
				// Need to make sure the flags are exact, Gungrave does FCAND with Mbit, then directly after FMAND with M-bit
				// Also call setupBranch to sort flag instances

				mVUsetupBranch(mVU, &mFC);
				// Make sure we save the current state so it can come back to it
				u32* cpS = (u32*)&mVUregs;
				u32* lpS = (u32*)&mVU->prog.lpState;
				for (size_t i = 0; i < (sizeof(microRegInfo) - 4) / 4; i++, lpS++, cpS++)
				{
					xe_mov32_mi(lpS, cpS[0]);
				}
				incPC(2);
				mVUsetupRange(mVU, xPC, 0);
				if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
					xe_mov64_mi_s32(&vuRegs[mVU->index].nextBlockCycles, 0);
				mVUendProgram(mVU, &mFC, 0);
				normBranchCompile(mVU, xPC);
				incPC(-2);
				return thisPtr;
			}
		}

		if (mVUinfo.doXGKICK)
		{
			mVU_XGKICK_DELAY(mVU);
		}

		if (isEvilBlock)
		{
			mVUsetupRange(mVU, xPC + 8, 0);
			normJumpCompile(mVU, &mFC, 1);
			return thisPtr;
		}
		else if (!mVUinfo.isBdelay)
		{
			// Handle range wrapping
			if ((xPC + 8) == mVU->microMemSize)
			{
				mVUsetupRange(mVU, xPC + 8, 0);
				mVUsetupRange(mVU, 0, 1);
			}
			incPC(1);
		}
		else
		{
			incPC(1);
			mVUsetupRange(mVU, xPC, 0);
			incPC(-4); // Go back to branch opcode

			switch (mVUlow.branch)
			{
				case 1: // B/BAL
				case 2:
					normBranch(mVU, &mFC);
					return thisPtr;
				case 9: // JR/JALR
				case 10:
					normJump(mVU, &mFC);
					return thisPtr;
				case 3: // IBEQ
					condBranch(mVU, &mFC, Jcc_Equal);
					return thisPtr;
				case 4: // IBGEZ
					condBranch(mVU, &mFC, Jcc_GreaterOrEqual);
					return thisPtr;
				case 5: // IBGTZ
					condBranch(mVU, &mFC, Jcc_Greater);
					return thisPtr;
				case 6: // IBLEQ
					condBranch(mVU, &mFC, Jcc_LessOrEqual);
					return thisPtr;
				case 7: // IBLTZ
					condBranch(mVU, &mFC, Jcc_Less);
					return thisPtr;
				case 8: // IBNEQ
					condBranch(mVU, &mFC, Jcc_NotEqual);
					return thisPtr;
			}
		}
	}

	/* E-bit End */
	mVUsetupRange(mVU, xPC, 0);
	mVUendProgram(mVU, &mFC, 1);

	return thisPtr;
}


/* Returns the entry point of the block (compiles it if not found) */
static __fi void* mVUentryGet(microVU* mVU, microBlockManager* block, u32 startPC, uptr pState)
{
	microBlock* pBlock = mVUbm_search(block, mVU, (microRegInfo*)pState);
	if (pBlock)
		return pBlock->x86ptrStart;
	return mVUcompile(mVU, startPC, pState);
}

/* Search for Existing Compiled Block (if found, return x86ptr;
 * else, compile and return x86ptr) */
static __fi void* mVUblockFetch(microVU* mVU, u32 startPC, uptr pState)
{
	startPC &= mVU->microMemSize - 8;

	blockCreate(startPC / 8);
	return mVUentryGet(mVU, mVUblocks[startPC / 8], startPC, pState);
}

// mVUcompileJIT() - Called By JR/JALR during execution
_mVUt void* mVUcompileJIT(u32 startPC, uptr ptr)
{
	if (doJumpAsSameProgram) // Treat jump as part of same microProgram
	{
		if (doJumpCaching) // When doJumpCaching, ptr is a microBlock pointer
		{
			microVU* mVU = mVUx;
			microBlock* pBlock = (microBlock*)ptr;
			microJumpCache* jc = &pBlock->jumpCache[startPC / 8];
			if (jc->prog && jc->prog == mVU->prog.quick[startPC / 8].prog)
				return jc->x86ptrStart;
			void* v = mVUblockFetch(mVUx, startPC, (uptr)&pBlock->pStateEnd);
			jc->prog = mVU->prog.quick[startPC / 8].prog;
			jc->x86ptrStart = v;
			return v;
		}
		return mVUblockFetch(mVUx, startPC, ptr);
	}
	vuRegs[mVUx->index].start_pc = startPC;
	if (doJumpCaching) // When doJumpCaching, ptr is a microBlock pointer
	{
		microVU* mVU = mVUx;
		microBlock* pBlock = (microBlock*)ptr;
		microJumpCache* jc = &pBlock->jumpCache[startPC / 8];
		if (jc->prog && jc->prog == mVU->prog.quick[startPC / 8].prog)
			return jc->x86ptrStart;
		void* v = mVUsearchProg<vuIndex>(startPC, (uptr)&pBlock->pStateEnd);
		jc->prog = mVU->prog.quick[startPC / 8].prog;
		jc->x86ptrStart = v;
		return v;
	}
	/* When !doJumpCaching, pBlock param is really a microRegInfo pointer */
	return mVUsearchProg<vuIndex>(startPC, ptr); /* Find and set correct program */
}
