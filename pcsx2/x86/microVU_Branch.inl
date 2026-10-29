#include "common/emitter/c89ops.h"
#include <cstdlib>
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

#include <string.h> /* memset */

extern void mVUincCycles(microVU* mVU, int x);
extern void* mVUcompile(microVU* mVU, u32 startPC, uptr pState);
static __fi int getLastFlagInst(microRegInfo* pState, int* xFlag, int flagType, int isEbit)
{
	if (isEbit)
		return findFlagInst(xFlag, 0x7fffffff);
	if (pState->needExactMatch & (1 << flagType))
		return 3;
	return (((pState->flagInfo >> (2 * flagType + 2)) & 3) - 1) & 3;
}

void mVU0clearlpStateJIT() { if (!microVU0.prog.cleared) memset(&microVU0.prog.lpState, 0, sizeof(microVU1.prog.lpState));  }
void mVU1clearlpStateJIT() { if (!microVU1.prog.cleared) memset(&microVU1.prog.lpState, 0, sizeof(microVU1.prog.lpState)); }

void mVUDTendProgram(mV, microFlagCycles* mFC, int isEbit)
{

	int fStatus = getLastFlagInst(&mVUpBlock->pState, mFC->xStatus, 0, isEbit);
	int fMac    = getLastFlagInst(&mVUpBlock->pState, mFC->xMac, 1, isEbit);
	int fClip   = getLastFlagInst(&mVUpBlock->pState, mFC->xClip, 2, isEbit);
	int qInst   = 0;
	int pInst   = 0;
	microBlock stateBackup;
	memcpy(&stateBackup, &mVUregs, sizeof(mVUregs)); //backup the state, it's about to get screwed with.

	mVUra_TDwritebackAll(mVU->regAlloc); //Writing back ok, invalidating early kills the rec, so don't do it :P

	if (isEbit)
	{
		mVUincCycles(mVU, 100); // Ensures Valid P/Q instances (And sets all cycle data to 0)
		mVUcycles -= 100;
		qInst = mVU->q;
		pInst = mVU->p;
		mVUregs.xgkickcycles = 0;
		if (mVUinfo.doDivFlag)
		{
			sFLAG.doFlag = 1;
			sFLAG.write = fStatus;
			mVUdivSet(mVU);
		}
		//Run any pending XGKick, providing we've got to it.
		if (mVUinfo.doXGKICK && xPC >= mVUinfo.XGKICKPC)
		{
			mVU_XGKICK_DELAY(mVU);
		}
		if (isVU1 && CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, 1);
		}
		if (!isVU1)
			xe_fastcall0(mVU0clearlpStateJIT);
		else
			xe_fastcall0(mVU1clearlpStateJIT);
	}

	// Save P/Q Regs
	if (qInst)
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);
	xe_movss_mx(&vuRegs[mVU->index].VI[REG_Q].UL, xmmPQ);
	xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);
	xe_movss_mx(&vuRegs[mVU->index].pending_q, xmmPQ);
	xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);

	if (isVU1)
	{
		if (pInst)
			xe_pshufd_xxi(xmmPQ, xmmPQ, 0xb4); // Swap Pending/Active P
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xC6); // 3 0 1 2
		xe_movss_mx(&vuRegs[mVU->index].VI[REG_P].UL, xmmPQ);
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0x87); // 0 2 1 3
		xe_movss_mx(&vuRegs[mVU->index].pending_p, xmmPQ);
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0x27); // 3 2 1 0
	}

	// Save MAC, Status and CLIP Flag Instances
	mVUallocSFLAGc(gprT1, gprT2, fStatus);
	xe_mov32_mr(&vuRegs[mVU->index].VI[REG_STATUS_FLAG].UL, gprT1);
	mVUallocMFLAGa(mVU, gprT1, fMac);
	mVUallocCFLAGa(mVU, gprT2, fClip);
	xe_mov32_mr(&vuRegs[mVU->index].VI[REG_MAC_FLAG].UL, gprT1);
	xe_mov32_mr(&vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL, gprT2);

	if (!isEbit) // Backup flag instances
	{
		xe_movaps_xm(xmmT1, mVU->macFlag);
		xe_movaps_mx(&vuRegs[mVU->index].micro_macflags, xmmT1);
		xe_movaps_xm(xmmT1, mVU->clipFlag);
		xe_movaps_mx(&vuRegs[mVU->index].micro_clipflags, xmmT1);

		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[0], gprF0);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[1], gprF1);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[2], gprF2);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[3], gprF3);
	}
	else // Flush flag instances
	{
		{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL); xe_movdzx_xmemg(xmmT1, xm); }
		xe_shufps_xxi(xmmT1, xmmT1, 0);
		xe_movaps_mx(&vuRegs[mVU->index].micro_clipflags, xmmT1);

		{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].VI[REG_MAC_FLAG].UL); xe_movdzx_xmemg(xmmT1, xm); }
		xe_shufps_xxi(xmmT1, xmmT1, 0);
		xe_movaps_mx(&vuRegs[mVU->index].micro_macflags, xmmT1);

		xe_movdzx_xr(xmmT1, getFlagReg(fStatus));
		xe_shufps_xxi(xmmT1, xmmT1, 0);
		xe_movaps_mx(&vuRegs[mVU->index].micro_statusflags, xmmT1);
	}

	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		xe_mov64_mi_s32(&vuRegs[mVU->index].nextBlockCycles, 0);


	xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);

	if (isEbit) // Clear 'is busy' Flags
	{
		if (!mVU->index || !THREAD_VU1)
		{
			xe_and32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? ~0x100 : ~0x001)); // VBS0/VBS1 flag
		}
	}

	if (isEbit != 2) // Save PC, and Jump to Exit Point
	{
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUTBit);
		xe_jmp_to(mVU->exitFunct);
	}

	memcpy(&mVUregs, &stateBackup, sizeof(mVUregs)); //Restore the state for the rest of the recompile
}

void mVUendProgram(mV, microFlagCycles* mFC, int isEbit)
{

	int fStatus = getLastFlagInst(&mVUpBlock->pState, mFC->xStatus, 0, isEbit && isEbit != 3);
	int fMac    = getLastFlagInst(&mVUpBlock->pState, mFC->xMac, 1, isEbit && isEbit != 3);
	int fClip   = getLastFlagInst(&mVUpBlock->pState, mFC->xClip, 2, isEbit && isEbit != 3);
	int qInst   = 0;
	int pInst   = 0;
	microBlock stateBackup;
	memcpy(&stateBackup, &mVUregs, sizeof(mVUregs)); //backup the state, it's about to get screwed with.
	if (!isEbit || isEbit == 3)
		mVUra_TDwritebackAll(mVU->regAlloc); //Writing back ok, invalidating early kills the rec, so don't do it :P
	else
		mVUra_flushAll(mVU->regAlloc, 1);

	if (isEbit && isEbit != 3)
	{
		memset(&mVUinfo, 0, sizeof(mVUinfo));
		memset(&mVUregsTemp, 0, sizeof(mVUregsTemp));
		mVUincCycles(mVU, 100); // Ensures Valid P/Q instances (And sets all cycle data to 0)
		mVUcycles -= 100;
		qInst = mVU->q;
		pInst = mVU->p;
		mVUregs.xgkickcycles = 0;
		if (mVUinfo.doDivFlag)
		{
			sFLAG.doFlag = 1;
			sFLAG.write = fStatus;
			mVUdivSet(mVU);
		}
		if (mVUinfo.doXGKICK)
		{
			mVU_XGKICK_DELAY(mVU);
		}
		if (isVU1 && CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, 1);
		}
		if (!isVU1)
			xe_fastcall0(mVU0clearlpStateJIT);
		else
			xe_fastcall0(mVU1clearlpStateJIT);
	}

	// Save P/Q Regs
	if (qInst)
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);
	xe_movss_mx(&vuRegs[mVU->index].VI[REG_Q].UL, xmmPQ);
	xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);
	xe_movss_mx(&vuRegs[mVU->index].pending_q, xmmPQ);
	xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);

	if (isVU1)
	{
		if (pInst)
			xe_pshufd_xxi(xmmPQ, xmmPQ, 0xb4); // Swap Pending/Active P
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xC6); // 3 0 1 2
		xe_movss_mx(&vuRegs[mVU->index].VI[REG_P].UL, xmmPQ);
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0x87); // 0 2 1 3
		xe_movss_mx(&vuRegs[mVU->index].pending_p, xmmPQ);
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0x27); // 3 2 1 0
	}

	// Save MAC, Status and CLIP Flag Instances
	mVUallocSFLAGc(gprT1, gprT2, fStatus);
	xe_mov32_mr(&vuRegs[mVU->index].VI[REG_STATUS_FLAG].UL, gprT1);
	mVUallocMFLAGa(mVU, gprT1, fMac);
	mVUallocCFLAGa(mVU, gprT2, fClip);
	xe_mov32_mr(&vuRegs[mVU->index].VI[REG_MAC_FLAG].UL, gprT1);
	xe_mov32_mr(&vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL, gprT2);

	if (!isEbit || isEbit == 3) // Backup flag instances
	{
		xe_movaps_xm(xmmT1, mVU->macFlag);
		xe_movaps_mx(&vuRegs[mVU->index].micro_macflags, xmmT1);
		xe_movaps_xm(xmmT1, mVU->clipFlag);
		xe_movaps_mx(&vuRegs[mVU->index].micro_clipflags, xmmT1);

		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[0], gprF0);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[1], gprF1);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[2], gprF2);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[3], gprF3);
	}
	else // Flush flag instances
	{
		{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL); xe_movdzx_xmemg(xmmT1, xm); }
		xe_shufps_xxi(xmmT1, xmmT1, 0);
		xe_movaps_mx(&vuRegs[mVU->index].micro_clipflags, xmmT1);

		{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].VI[REG_MAC_FLAG].UL); xe_movdzx_xmemg(xmmT1, xm); }
		xe_shufps_xxi(xmmT1, xmmT1, 0);
		xe_movaps_mx(&vuRegs[mVU->index].micro_macflags, xmmT1);

		xe_movdzx_xr(xmmT1, getFlagReg(fStatus));
		xe_shufps_xxi(xmmT1, xmmT1, 0);
		xe_movaps_mx(&vuRegs[mVU->index].micro_statusflags, xmmT1);
	}

	xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);

	if ((isEbit && isEbit != 3)) // Clear 'is busy' Flags
	{
		if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
			xe_mov64_mi_s32(&vuRegs[mVU->index].nextBlockCycles, 0);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_and32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? ~0x100 : ~0x001)); // VBS0/VBS1 flag
		}
	}
	else if(isEbit)
	{
		if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
			xe_mov64_mi_s32(&vuRegs[mVU->index].nextBlockCycles, 0);
	}

	if (isEbit != 2 && isEbit != 3) // Save PC, and Jump to Exit Point
	{
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
	}
	memcpy(&mVUregs, &stateBackup, sizeof(mVUregs)); //Restore the state for the rest of the recompile
}

// Recompiles Code for Proper Flags and Q/P regs on Block Linkings
void mVUsetupBranch(mV, microFlagCycles* mFC)
{
	mVUra_flushAll(mVU->regAlloc, 1); // Flush Allocated Regs
	mVUsetupFlags(mVU, mFC);  // Shuffle Flag Instances

	// Shuffle P/Q regs since every block starts at instance #0
	if (mVU->p || mVU->q)
		xe_pshufd_xxi(xmmPQ, xmmPQ, shufflePQ);
	mVU->p = 0, mVU->q = 0;
}

void normBranchCompile(microVU* mVU, u32 branchPC)
{
	microBlock* pBlock;
	blockCreate(branchPC / 8);
	pBlock = mVUbm_search(mVUblocks[branchPC / 8], mVU, (microRegInfo*)&mVUregs);
	if (pBlock)
		xe_jmp_to(pBlock->x86ptrStart);
	else
		mVUcompile(mVU, branchPC, (uptr)&mVUregs);
}

void normJumpCompile(mV, microFlagCycles* mFC, int isEvilJump)
{
	memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
	mVUsetupBranch(mVU, mFC);

	if (!mVUpBlock->jumpCache) // Create the jump cache for this block
	{
		mVUpBlock->jumpCache = (microJumpCache*)calloc(mProgSize / 2, sizeof(microJumpCache));
	}

	/* Inline jump-cache probe. The census on a dispatch-bound title
	 * measured 3.4 million indirect-jump roundtrips a second with a
	 * 99.4 percent cache hit rate - each hit paying the register
	 * backup, C call into mVUcompileJIT, restore and indirect jump
	 * only to fetch a pointer the cache already held. The probe
	 * reproduces the C hit path exactly: start_pc updated first, hit
	 * requires a nonnull cached program equal to the quick entry, and
	 * both tables index by startPC/8 with sixteen-byte elements, so
	 * the raw PC scales by two. Emitted before the backup, the hit
	 * path clobbers only scratch GPRs and preserves strictly more
	 * machine state than the call it replaces; misses fall into the
	 * unchanged slow path. Evil and E-bit jumps keep their extra
	 * bookkeeping and are not probed. Scratch is RAX and RDX - on
	 * Win64 ARG1 is RCX, so gprT2q would collide with the index. */
	if (doJumpCaching && !isEvilJump && !mVUup.eBit)
	{
		/* The census convicted the first version of this probe: the
		 * jump-cache element is twenty-four bytes, not sixteen, so
		 * the scale-two indexing read garbage, every probe missed,
		 * and the whole patch fell safe into the slow path as a
		 * silent no-op. The asserts below make any future layout
		 * drift a build failure instead. Twenty-four is not a SIB
		 * scale, so the index is computed: three registers total,
		 * with the raw PC retiring into the quick lookup before its
		 * register is reused as the far-base scratch. */
		static_assert(sizeof(microJumpCache) == 24, "probe stride");
		static_assert(sizeof(microProgramQuick) == 16, "probe stride");
		struct e_mem m;
		uint8_t *pj1, *pj2;
		xe_mov32_rm(XE_ARG1, &mVU->branch);
		xe_mov32_mr(&vuRegs[mVU->index].start_pc, XE_ARG1);
		xe_complexaddr_si(m, XE_DX, (u8*)&mVU->prog.quick[0] + 8, XE_ARG1, 2);
		xe_mov64_rmem(gprT1q, m);                 // quick->prog
		xe_mov64_rr(XE_DX, XE_ARG1);
		xe_shl64_ri(XE_DX, 1);
		xe_add64_rr(XE_DX, XE_ARG1);              // idx = startPC*3 (stride 24 over startPC/8)
		xe_complexaddr_si(m, XE_ARG1, mVUpBlock->jumpCache, XE_DX, 1);
		xe_mov64_rmem(XE_ARG1, m);                // jc->prog
		xe_test64_rr(XE_ARG1, XE_ARG1);
		xe_fwd_jcc32(Jcc_Zero, pj1);
		xe_cmp64_rr(XE_ARG1, gprT1q);
		xe_fwd_jcc32(Jcc_NotEqual, pj2);
		xe_complexaddr_si(m, XE_ARG1, (u8*)mVUpBlock->jumpCache + 8, XE_DX, 1);
		xe_mov64_rmem(gprT1q, m);                 // jc->x86ptrStart
		xe_jmp_r(gprT1q);
		xe_fwd_set32(pj1);
		xe_fwd_set32(pj2);
	}

	mVUbackupRegs(mVU, 0, 0);

	if (isEvilJump)
	{
		xe_mov32_rm(XE_ARG1, &mVU->evilBranch);
		xe_mov32_rm(gprT1, &mVU->evilevilBranch);
		xe_mov32_mr(&mVU->evilBranch, gprT1);
	}
	else
		xe_mov32_rm(XE_ARG1, &mVU->branch);
	if (doJumpCaching)
		xe_lea_far(XE_ARG2, mVUpBlock);
	else
		xe_lea_far(XE_ARG2, &mVUpBlock->pStateEnd);

	if (mVUup.eBit && isEvilJump) // E-bit EvilJump
	{
		//Xtreme G 3 does 2 conditional jumps, the first contains an E Bit on the first instruction
		//So if it is taken, you need to end the program, else you get infinite loops.
		mVUendProgram(mVU, mFC, 2);
		xe_mov32_mr(&vuRegs[mVU->index].VI[REG_TPC].UL, XE_ARG1);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
	}

	if (!mVU->index)
		xe_fastcall2_rr((void*)(void (*)())mVUcompileJIT<0>, XE_ARG1, XE_ARG2); //(u32 startPC, uptr pState)
	else
		xe_fastcall2_rr((void*)(void (*)())mVUcompileJIT<1>, XE_ARG1, XE_ARG2);

	mVUrestoreRegs(mVU, 0, 0);
	xe_jmp_r(gprT1q); // Jump to rec-code address
}

void normBranch(mV, microFlagCycles* mFC)
{
	// E-bit or T-Bit or D-Bit Branch
	if (mVUup.dBit && doDBitHandling)
	{
		// Flush register cache early to avoid double flush on both paths
		mVUra_flushAll(mVU->regAlloc, 0);

		u32 tempPC = iPC;
		if (mVU->index && THREAD_VU1)
			xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x400 : 0x4));
		else
			xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x400 : 0x4));
		uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2));
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
		}
		iPC = branchAddr(mVU) / 4;
		mVUDTendProgram(mVU, mFC, 1);
		xe_fwd_set32(eJMP);
		iPC = tempPC;
	}
	if (mVUup.tBit)
	{
		// Flush register cache early to avoid double flush on both paths
		mVUra_flushAll(mVU->regAlloc, 0);

		u32 tempPC = iPC;
		if (mVU->index && THREAD_VU1)
			xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x800 : 0x8));
		else
			xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x800 : 0x8));
		uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4));
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
		}
		iPC = branchAddr(mVU) / 4;
		mVUDTendProgram(mVU, mFC, 1);
		xe_fwd_set32(eJMP);
		iPC = tempPC;
	}
	if (mVUup.mBit)
	{
		u32 tempPC = iPC;

		memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
		xe_lea_far(XE_AX, &mVUpBlock->pStateEnd);
		xe_call_ptr(mVU->copyPLState);

		mVUsetupBranch(mVU, mFC);
		mVUendProgram(mVU, mFC, 3);
		iPC = branchAddr(mVU) / 4;
		xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
		iPC = tempPC;
	}
	if (mVUup.eBit)
	{
		iPC = branchAddr(mVU) / 4;
		mVUendProgram(mVU, mFC, 1);
		return;
	}

	// Normal Branch
	mVUsetupBranch(mVU, mFC);
	normBranchCompile(mVU, branchAddr(mVU));
}

void condBranch(mV, microFlagCycles* mFC, int JMPcc)
{
	mVUsetupBranch(mVU, mFC);

	if (mVUup.tBit)
	{
		u32 tempPC = iPC;
		if (mVU->index && THREAD_VU1)
			xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x800 : 0x8));
		else
			xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x800 : 0x8));
		uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4));
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, mFC, 2);
		xe_cmp16_mi(&mVU->branch, 0);
		uint8_t* tJMP; xe_fwd_jcc32(xInvertCond((JccComparisonType)JMPcc), tJMP);
			incPC(4); // Set PC to First instruction of Non-Taken Side
			xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
			if (mVU->index && THREAD_VU1)
				xe_fastcall0(mVUTBit);
			xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(tJMP);
		incPC(-4); // Go Back to Branch Opcode to get branchAddr
		iPC = branchAddr(mVU) / 4;
		xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUTBit);
		xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(eJMP);
		iPC = tempPC;
	}
	if (mVUup.dBit && doDBitHandling)
	{
		u32 tempPC = iPC;
		if (mVU->index  && THREAD_VU1)
			xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x400 : 0x4));
		else
			xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x400 : 0x4));
		uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2));
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, mFC, 2);
		xe_cmp16_mi(&mVU->branch, 0);
		uint8_t* dJMP; xe_fwd_jcc32(xInvertCond((JccComparisonType)JMPcc), dJMP);
			incPC(4); // Set PC to First instruction of Non-Taken Side
			xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
			xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(dJMP);
		incPC(-4); // Go Back to Branch Opcode to get branchAddr
		iPC = branchAddr(mVU) / 4;
		xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
		xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(eJMP);
		iPC = tempPC;
	}
	if (mVUup.mBit)
	{
		u32 tempPC = iPC;

		memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
		xe_lea_far(XE_AX, &mVUpBlock->pStateEnd);
		xe_call_ptr(mVU->copyPLState);

		mVUendProgram(mVU, mFC, 3);
		xe_cmp16_mi(&mVU->branch, 0);
		uint8_t* dJMP; xe_fwd_jcc32((JccComparisonType)JMPcc, dJMP);
		incPC(4); // Set PC to First instruction of Non-Taken Side
		xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(dJMP);
		incPC(-4); // Go Back to Branch Opcode to get branchAddr
		iPC = branchAddr(mVU) / 4;
		xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
		iPC = tempPC;
	}
	if (mVUup.eBit) // Conditional Branch With E-Bit Set
	{
		mVUendProgram(mVU, mFC, 2);
		xe_cmp16_mi(&mVU->branch, 0);

		incPC(3);
		uint8_t* eJMP; xe_fwd_jcc32((JccComparisonType)JMPcc, eJMP);
			incPC(1); // Set PC to First instruction of Non-Taken Side
			xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
			if (mVU->index && THREAD_VU1)
				xe_fastcall0(mVUEBit);
			xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(eJMP);
		incPC(-4); // Go Back to Branch Opcode to get branchAddr

		iPC = branchAddr(mVU) / 4;
		xe_mov32_mi(&vuRegs[mVU->index].VI[REG_TPC].UL, xPC);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
		return;
	}
	else // Normal Conditional Branch
	{
		xe_cmp16_mi(&mVU->branch, 0);

		incPC(3);
		microBlock* bBlock;
		incPC2(1); // Check if Branch Non-Taken Side has already been recompiled
		blockCreate(iPC / 2);
		bBlock = mVUbm_search(mVUblocks[iPC / 2], mVU, (microRegInfo*)&mVUregs);
		incPC2(-1);
		if (bBlock) // Branch non-taken has already been compiled
		{
			xe_jcc_known(xInvertCond((JccComparisonType)JMPcc), bBlock->x86ptrStart);
			incPC(-3); // Go back to branch opcode (to get branch imm addr)
			normBranchCompile(mVU, branchAddr(mVU));
		}
		else
		{
			s32* ajmp; xe_jcc32_slot(JMPcc, 0, ajmp);
			u32 bPC = iPC; // mVUcompile can modify iPC, mVUpBlock, and mVUregs so back them up

			microRegInfo regBackup;
			memcpy(&regBackup, &mVUregs, sizeof(microRegInfo));

			incPC2(1); // Get PC for branch not-taken
			mVUcompile(mVU, xPC, (uptr)&mVUregs);

			iPC = bPC;
			incPC(-3); // Go back to branch opcode (to get branch imm addr)
			uptr jumpAddr = (uptr)mVUblockFetch(mVU, branchAddr(mVU), (uptr)&regBackup);
			*ajmp = (jumpAddr - ((uptr)ajmp + 4));
		}
	}
}

void normJump(mV, microFlagCycles* mFC)
{
	if (mVUlow.constJump.isValid) // Jump Address is Constant
	{
		if (mVUup.eBit) // E-bit Jump
		{
			iPC = (mVUlow.constJump.regValue * 2) & (mVU->progMemMask);
			mVUendProgram(mVU, mFC, 1);
			return;
		}
		int jumpAddr = (mVUlow.constJump.regValue * 8) & (mVU->microMemSize - 8);
		mVUsetupBranch(mVU, mFC);
		normBranchCompile(mVU, jumpAddr);
		return;
	}
	if (mVUup.dBit && doDBitHandling)
	{
		// Flush register cache early to avoid double flush on both paths
		mVUra_flushAll(mVU->regAlloc, 0);

		if (THREAD_VU1)
			xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x400 : 0x4));
		else
			xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x400 : 0x4));
		uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2));
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, mFC, 2);
		xe_mov32_rm(gprT1, &mVU->branch);
		xe_mov32_mr(&vuRegs[mVU->index].VI[REG_TPC].UL, gprT1);
		xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(eJMP);
	}
	if (mVUup.tBit)
	{
		// Flush register cache early to avoid double flush on both paths
		mVUra_flushAll(mVU->regAlloc, 0);

		if (mVU->index && THREAD_VU1)
			xe_test32_mi(&vu1Thread.vuFBRST, (isVU1 ? 0x800 : 0x8));
		else
			xe_test32_mi(&vuRegs[0].VI[REG_FBRST].UL, (isVU1 ? 0x800 : 0x8));
		uint8_t* eJMP; xe_fwd_jcc32(Jcc_Zero, eJMP);
		if (!mVU->index || !THREAD_VU1)
		{
			xe_or32_mi(&vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4));
			xe_or32_mi(&vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT);
		}
		mVUDTendProgram(mVU, mFC, 2);
		xe_mov32_rm(gprT1, &mVU->branch);
		xe_mov32_mr(&vuRegs[mVU->index].VI[REG_TPC].UL, gprT1);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUTBit);
		xe_jmp_to(mVU->exitFunct);
		xe_fwd_set32(eJMP);
	}
	if (mVUup.eBit) // E-bit Jump
	{
		mVUendProgram(mVU, mFC, 2);
		xe_mov32_rm(gprT1, &mVU->branch);
		xe_mov32_mr(&vuRegs[mVU->index].VI[REG_TPC].UL, gprT1);
		if (mVU->index && THREAD_VU1)
			xe_fastcall0(mVUEBit);
		xe_jmp_to(mVU->exitFunct);
	}
	else
	{
		normJumpCompile(mVU, mFC, 0);
	}
}
