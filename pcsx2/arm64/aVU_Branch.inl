/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+ */

#pragma once

/* ARM64 microVU recompiler — branch / program-exit emission (Phase 7, task 7.7).
 *
 * AArch64 port of pcsx2/x86/microVU_Branch.inl. This slice brings up the two
 * *program-exit* emitters first, because they make NO opcode-table calls
 * (mVUopU/mVUopL) and so compile standalone ahead of the Tables/Compile big-bang:
 *
 * * getLastFlagInst          — pure analysis (which flag instance to flush)
 * * mVU{0,1}clearlpStateJIT  — C thunks called from emitted code
 * * mVUendProgram            — end-of-block: save P/Q + the 4 flag instances back
 * to VURegs, then jump to mVU->exitFunct
 * * mVUDTendProgram          — same, for the D/T-bit early-exit path
 *
 * The branch *drivers* that DO need the opcode tables + mVUcompile
 * (mVUsetupBranch / normBranch{,Compile} / normJump{,Compile} / condBranch) come
 * over with the Tables + Compile slice and are NOT in this file yet.
 *
 * x86 -> a64emit translations used here:
 * xMOVSS(ptr32[a], xmmPQ)   -> Str(mVU_xmmPQ.S(), [a])          (store P/Q lane0)
 * xPSHUF.D(xmmPQ,xmmPQ,imm) -> mVUshufflePS(mVU_xmmPQ, …, imm)  (lane permute)
 * xMOVAPS(xmm, ptr128[a])   -> Ldr(xmm.Q(), [a]) / Str          (128-bit move)
 * xMOVDZX(xmm, ptr32[a])    -> Ldr(xmm.S(), [a])                (32-bit, zero hi)
 * xMOVDZX(xmm, gpr)         -> Fmov(xmm.S(), gpr)               (W->S, zero hi)
 * xSHUF.PS(xmm,xmm,0)       -> Dup(xmm.V4S(), xmm.V4S(), 0)     (lane0 broadcast)
 * xMOV(ptr32[a], gpr/imm)   -> Str / (Mov tmp,imm + Str)
 * xAND(ptr32[a], imm)       -> Ldr + And + Str
 * xJMP(mVU->exitFunct)       -> armEmitJmp(mVU->exitFunct)
 * xFastCall((void*)fn)      -> armEmitCall(fn)
 * Absolute &vuRegs[mVU->index]… / &vuRegs[0].VI[…] addresses are materialized with
 * armMoveAddressToReg (clobbers x16/x17), exactly like the flag/alloc helpers. */

/* mVUincCycles is defined later in aVU.c (after this .inl is included), so it
 * needs a forward declaration here — same as x86 microVU_Branch.inl. */
static void mVUincCycles(microVU* mVU, int x);

/* The small absolute-address memory-access helpers (mvuStr32/mvuLdr32/mvuStrImm32/
 * mvuStrSS/mvuLdrSS/mvuLdrQ/mvuStrQ/mvuMemAndImm32) moved to aVU_Misc.inl so the
 * Tables/Flags slices (included earlier) can share them. */

/* The real XGKICK emit helpers (mVU_XGKICK_DELAY / mVU_XGKICK_SYNC) + the GIF
 * transfer C functions live in aVU_Lower.inl (task 7.5b), which is #included
 * before this file; the branch drivers below call them. */

/* ------------------------------------------------------------------
 * C thunks called from emitted code
 * ------------------------------------------------------------------ */
static void mVUTBit()
{
	const u32 old = (u32)retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, MTVU_INT_VU_TBIT); /* acq_rel >= release */
	if (old & MTVU_INT_VU_TBIT)
		log_cb(RETRO_LOG_WARN, "Old TBit not registered\n");
}

static void mVUEBit()
{
	retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, MTVU_INT_VU_EBIT); /* acq_rel >= release */
}

void mVU0clearlpStateJIT() { if (!microVU0.prog.cleared) memset(&microVU0.prog.lpState, 0, sizeof(microVU0.prog.lpState)); }
void mVU1clearlpStateJIT() { if (!microVU1.prog.cleared) memset(&microVU1.prog.lpState, 0, sizeof(microVU1.prog.lpState)); }

/* ------------------------------------------------------------------
 * Flag-instance selection (pure analysis)
 * ------------------------------------------------------------------ */
static __fi int getLastFlagInst(microRegInfo* pState, int* xFlag, int flagType, int isEbit)
{
	if (isEbit)
		return findFlagInst(xFlag, 0x7fffffff);
	if (pState->needExactMatch & (1 << flagType))
		return 3;
	return (((pState->flagInfo >> (2 * flagType + 2)) & 3) - 1) & 3;
}

/* ------------------------------------------------------------------
 * Program exit (D/T-bit early-exit path)
 * ------------------------------------------------------------------ */
static void mVUDTendProgram(mV, microFlagCycles* mFC, int isEbit)
{
	int fStatus = getLastFlagInst(&mVUpBlock->pState, mFC->xStatus, 0, isEbit);
	int fMac    = getLastFlagInst(&mVUpBlock->pState, mFC->xMac, 1, isEbit);
	int fClip   = getLastFlagInst(&mVUpBlock->pState, mFC->xClip, 2, isEbit);
	int qInst   = 0;
	int pInst   = 0;
	microBlock stateBackup;
	memcpy(&stateBackup, &mVUregs, sizeof(mVUregs)); /* backup the state, it's about to get screwed with. */

	mVUra_TDwritebackAll(mVU->regAlloc); /* Writing back ok, invalidating early kills the rec, so don't do it :P */

	if (isEbit)
	{
		mVUincCycles(mVU, 100); /* Ensures Valid P/Q instances (And sets all cycle data to 0) */
		mVUcycles -= 100;
		qInst = mVU->q;
		pInst = mVU->p;
		mVUregs.xgkickcycles = 0;
		if (mVUinfo.doDivFlag)
		{
			sFLAG.doFlag = true;
			sFLAG.write = fStatus;
			mVUdivSet(mVU);
		}
		/* Run any pending XGKick, providing we've got to it. */
		if (mVUinfo.doXGKICK && xPC >= mVUinfo.XGKICKPC)
		{
			mVU_XGKICK_DELAY(mVU);
		}
		if (isVU1 && CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, true);
		}
		if (!isVU1)
			armEmitCall(((const void*)(mVU0clearlpStateJIT)));
		else
			armEmitCall(((const void*)(mVU1clearlpStateJIT)));
	}

	/* Save P/Q Regs */
	if (qInst)
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xe1);
	mvuStrSS(mVU, &vuRegs[mVU->index].VI[REG_Q].UL, mVU_xmmPQ);
	mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xe1);
	mvuStrSS(mVU, &vuRegs[mVU->index].pending_q, mVU_xmmPQ);
	mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xe1);

	if (isVU1)
	{
		if (pInst)
			mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xb4); /* Swap Pending/Active P */
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xC6); /* 3 0 1 2 */
		mvuStrSS(mVU, &vuRegs[mVU->index].VI[REG_P].UL, mVU_xmmPQ);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0x87); /* 0 2 1 3 */
		mvuStrSS(mVU, &vuRegs[mVU->index].pending_p, mVU_xmmPQ);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0x27); /* 3 2 1 0 */
	}

	/* Save MAC, Status and CLIP Flag Instances */
	mVUallocSFLAGc(gprT1, gprT2, fStatus);
	mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_STATUS_FLAG].UL, gprT1);
	mVUallocMFLAGa(mVU, gprT1, fMac);
	mVUallocCFLAGa(mVU, gprT2, fClip);
	mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_MAC_FLAG].UL, gprT1);
	mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL, gprT2);

	if (!isEbit) /* Backup flag instances */
	{
		mvuLdrQ(mVU, xmmT1, &mVU->macFlag[0]);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_macflags, xmmT1);
		mvuLdrQ(mVU, xmmT1, &mVU->clipFlag[0]);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_clipflags, xmmT1);

		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[0], gprF0);
		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[1], gprF1);
		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[2], gprF2);
		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[3], gprF3);
	}
	else /* Flush flag instances */
	{
		mvuLdrSS(mVU, xmmT1, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL);
		a64_dup_elem(armAsm, a64_as(xmmT1, A64K_4S), a64_as(xmmT1, A64K_4S), 0);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_clipflags, xmmT1);

		mvuLdrSS(mVU, xmmT1, &vuRegs[mVU->index].VI[REG_MAC_FLAG].UL);
		a64_dup_elem(armAsm, a64_as(xmmT1, A64K_4S), a64_as(xmmT1, A64K_4S), 0);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_macflags, xmmT1);

		a64_fmov(armAsm, a64_as(xmmT1, A64K_S), getFlagReg(fStatus));
		a64_dup_elem(armAsm, a64_as(xmmT1, A64K_4S), a64_as(xmmT1, A64K_4S), 0);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_statusflags, xmmT1);
	}

	if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
		mvuStrImm32(mVU, &vuRegs[mVU->index].nextBlockCycles, 0, gprT1);

	mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);

	if (isEbit) /* Clear 'is busy' Flags */
	{
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemAndImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? ~0x100u : ~0x001u), gprT1); /* VBS0/VBS1 flag */
		}
	}

	if (isEbit != 2) /* Save PC, and Jump to Exit Point */
	{
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUTBit)));
		armEmitJmp(mVU->exitFunct);
	}

	memcpy(&mVUregs, &stateBackup, sizeof(mVUregs)); /* Restore the state for the rest of the recompile */
}

/* ------------------------------------------------------------------
 * Program exit (normal / E-bit end)
 * ------------------------------------------------------------------ */
static void mVUendProgram(mV, microFlagCycles* mFC, int isEbit)
{
	int fStatus = getLastFlagInst(&mVUpBlock->pState, mFC->xStatus, 0, isEbit && isEbit != 3);
	int fMac    = getLastFlagInst(&mVUpBlock->pState, mFC->xMac, 1, isEbit && isEbit != 3);
	int fClip   = getLastFlagInst(&mVUpBlock->pState, mFC->xClip, 2, isEbit && isEbit != 3);
	int qInst   = 0;
	int pInst   = 0;
	microBlock stateBackup;
	memcpy(&stateBackup, &mVUregs, sizeof(mVUregs)); /* backup the state, it's about to get screwed with. */
	if (!isEbit || isEbit == 3)
		mVUra_TDwritebackAll(mVU->regAlloc); /* Writing back ok, invalidating early kills the rec, so don't do it :P */
	else
		mVUra_flushAll(mVU->regAlloc, true);

	if (isEbit && isEbit != 3)
	{
		memset(&mVUinfo, 0, sizeof(mVUinfo));
		memset(&mVUregsTemp, 0, sizeof(mVUregsTemp));
		mVUincCycles(mVU, 100); /* Ensures Valid P/Q instances (And sets all cycle data to 0) */
		mVUcycles -= 100;
		qInst = mVU->q;
		pInst = mVU->p;
		mVUregs.xgkickcycles = 0;
		if (mVUinfo.doDivFlag)
		{
			sFLAG.doFlag = true;
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
			mVU_XGKICK_SYNC(mVU, true);
		}
		if (!isVU1)
			armEmitCall(((const void*)(mVU0clearlpStateJIT)));
		else
			armEmitCall(((const void*)(mVU1clearlpStateJIT)));
	}

	/* Save P/Q Regs */
	if (qInst)
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xe1);
	mvuStrSS(mVU, &vuRegs[mVU->index].VI[REG_Q].UL, mVU_xmmPQ);
	mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xe1);
	mvuStrSS(mVU, &vuRegs[mVU->index].pending_q, mVU_xmmPQ);
	mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xe1);

	if (isVU1)
	{
		if (pInst)
			mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xb4); /* Swap Pending/Active P */
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0xC6); /* 3 0 1 2 */
		mvuStrSS(mVU, &vuRegs[mVU->index].VI[REG_P].UL, mVU_xmmPQ);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0x87); /* 0 2 1 3 */
		mvuStrSS(mVU, &vuRegs[mVU->index].pending_p, mVU_xmmPQ);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, 0x27); /* 3 2 1 0 */
	}

	/* Save MAC, Status and CLIP Flag Instances */
	mVUallocSFLAGc(gprT1, gprT2, fStatus);
	mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_STATUS_FLAG].UL, gprT1);
	mVUallocMFLAGa(mVU, gprT1, fMac);
	mVUallocCFLAGa(mVU, gprT2, fClip);
	mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_MAC_FLAG].UL, gprT1);
	mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL, gprT2);

	if (!isEbit || isEbit == 3) /* Backup flag instances */
	{
		mvuLdrQ(mVU, xmmT1, &mVU->macFlag[0]);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_macflags, xmmT1);
		mvuLdrQ(mVU, xmmT1, &mVU->clipFlag[0]);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_clipflags, xmmT1);

		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[0], gprF0);
		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[1], gprF1);
		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[2], gprF2);
		mvuStr32(mVU, &vuRegs[mVU->index].micro_statusflags[3], gprF3);
	}
	else /* Flush flag instances */
	{
		mvuLdrSS(mVU, xmmT1, &vuRegs[mVU->index].VI[REG_CLIP_FLAG].UL);
		a64_dup_elem(armAsm, a64_as(xmmT1, A64K_4S), a64_as(xmmT1, A64K_4S), 0);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_clipflags, xmmT1);

		mvuLdrSS(mVU, xmmT1, &vuRegs[mVU->index].VI[REG_MAC_FLAG].UL);
		a64_dup_elem(armAsm, a64_as(xmmT1, A64K_4S), a64_as(xmmT1, A64K_4S), 0);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_macflags, xmmT1);

		a64_fmov(armAsm, a64_as(xmmT1, A64K_S), getFlagReg(fStatus));
		a64_dup_elem(armAsm, a64_as(xmmT1, A64K_4S), a64_as(xmmT1, A64K_4S), 0);
		mvuStrQ(mVU, &vuRegs[mVU->index].micro_statusflags, xmmT1);
	}

	mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);

	if ((isEbit && isEbit != 3)) /* Clear 'is busy' Flags */
	{
		if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
			mvuStrImm32(mVU, &vuRegs[mVU->index].nextBlockCycles, 0, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemAndImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? ~0x100u : ~0x001u), gprT1); /* VBS0/VBS1 flag */
		}
	}
	else if (isEbit)
	{
		if (EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Gamefixes.FullVU0SyncHack)
			mvuStrImm32(mVU, &vuRegs[mVU->index].nextBlockCycles, 0, gprT1);
	}

	if (isEbit != 2 && isEbit != 3) /* Save PC, and Jump to Exit Point */
	{
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
	}
	memcpy(&mVUregs, &stateBackup, sizeof(mVUregs)); /* Restore the state for the rest of the recompile */
}

/* ------------------------------------------------------------------
 * Block-linking setup (flag instances + P/Q reset)
 * ------------------------------------------------------------------
 * Recompiles code for proper flags and Q/P regs on block linkings. Table-
 * independent: only needs mVUsetupFlags (ported) + a PQ lane permute, so it comes
 * over now; its callers (normBranch/normJump/condBranch) arrive with the Compile
 * slice. x86 xPSHUF.D(xmmPQ, xmmPQ, shufflePQ) -> mVUshufflePS. */
static void mVUsetupBranch(mV, microFlagCycles* mFC)
{
	mVUra_flushAll(mVU->regAlloc, true); /* Flush Allocated Regs */
	mVUsetupFlags(mVU, mFC); /* Shuffle Flag Instances */

	/* Shuffle P/Q regs since every block starts at instance #0 */
	if (mVU->p || mVU->q)
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, shufflePQ);
	mVU->p = 0, mVU->q = 0;
}

/* ------------------------------------------------------------------
 * Branch drivers (the cross-referencing core)
 * ------------------------------------------------------------------
 * These emit the block-exit / block-linking code for every branch/jump type.
 * They mutually recurse with mVUcompile (defined later in aVU_Compile.inl), so it
 * is forward-declared here exactly like x86 microVU_Branch.inl. mVUcompileJIT (the
 * runtime JR/JALR thunk) is likewise forward-declared to take its address. */
static void* mVUcompile(microVU* mVU, u32 startPC, uptr pState);
static void* mVUblockFetch(microVU* mVU, u32 startPC, uptr pState);
static void* mVUcompileJIT0(u32 startPC, uptr ptr);
static void* mVUcompileJIT1(u32 startPC, uptr ptr);

/* Small file-local helpers for the absolute-addr ops the drivers need.
 * Sign-extending 16-bit load: every caller loads mVU->branch and compares it
 * against 0 with signed conditions (IBLTZ/IBGEZ/IBGTZ/IBLEZ → lt/ge/gt/le), so
 * it must mirror x86's 16-bit *signed* `xCMP(ptr16[&mVU->branch], 0)` — a VI value
 * with bit15 set is negative. (eq/ne for IBEQ/IBNE are unaffected by the extend.) */
static INLINE void mvuLdrsh16(microVU* mVU, a64r wreg, const void* addr)
{
	a64_ldrsh(armAsm, a64_asw(wreg), mvuAbsMem(mVU, addr, 2));
}
/* xTEST(ptr32[addr], imm) + xForwardJump32(Jcc_Zero): load, test, branch-if-zero. */
static INLINE void mvuTestMemBranchZero(microVU* mVU, const void* addr, u32 imm, a64_label* tgt, a64r tmp)
{
	mvuLdr32(mVU, tmp, addr);
	a64_tst_imm(armAsm, a64_asw(tmp), imm);
	a64_b_cond(armAsm, tgt, A64_EQ); /* ZF set == (val & imm) == 0 */
}

static void normBranchCompile(microVU* mVU, u32 branchPC)
{
	microBlock* pBlock;
	blockCreate(branchPC / 8);
	pBlock = mVUbm_search(mVUblocks[branchPC / 8], mVU, (microRegInfo*)&mVUregs);
	if (pBlock)
		armEmitJmp(pBlock->codeStart);
	else
		mVUcompile(mVU, branchPC, (uptr)&mVUregs);
}

static void normJumpCompile(mV, microFlagCycles* mFC, bool isEvilJump)
{
	memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
	mVUsetupBranch(mVU, mFC);
	mVUbackupRegs(mVU, false, false);

	if (!mVUpBlock->jumpCache) /* Create the jump cache for this block */
	{
		mVUpBlock->jumpCache = (microJumpCache*)calloc(mProgSize / 2, sizeof(microJumpCache));
	}

	if (isEvilJump)
	{
		mvuLdr32(mVU, RWARG1, &mVU->evilBranch); /* startPC (arg1) */
		mvuLdr32(mVU, gprT1, &mVU->evilevilBranch);
		mvuStr32(mVU, &mVU->evilBranch, gprT1);
	}
	else
		mvuLdr32(mVU, RWARG1, &mVU->branch);
	if (doJumpCaching)
		armMoveAddressToReg(RXARG2, mVUpBlock);
	else
		armMoveAddressToReg(RXARG2, &mVUpBlock->pStateEnd);

	if (mVUup.eBit && isEvilJump) /* E-bit EvilJump */
	{
		/* Xtreme G 3 does 2 conditional jumps, the first contains an E Bit on the first instruction
		 * So if it is taken, you need to end the program, else you get infinite loops. */
		mVUendProgram(mVU, mFC, 2);
		mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, RWARG1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
	}

	if (!mVU->index)
		armEmitCall(((const void*)(mVUcompileJIT0))); /* (u32 startPC, uptr pState) */
	else
		armEmitCall(((const void*)(mVUcompileJIT1)));

	mVUrestoreRegs(mVU, false, false);
	a64_br(armAsm, RXRET); /* Jump to rec-code address (returned in x0) */
}

static void normBranch(mV, microFlagCycles* mFC)
{
	/* E-bit or T-Bit or D-Bit Branch */
	if (mVUup.dBit && doDBitHandling)
	{
		u32 tempPC;
		a64_label eJMP = A64_LABEL_INIT;
		/* Flush register cache early to avoid double flush on both paths */
		mVUra_flushAll(mVU->regAlloc, false);

		tempPC = iPC;
		mvuTestMemBranchZero(mVU, (mVU->index && THREAD_VU1) ? (const void*)&vu1Thread.vuFBRST : (const void*)&vuRegs[0].VI[REG_FBRST].UL,
			(isVU1 ? 0x400 : 0x4), &eJMP, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemOrImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2), gprT1);
			mvuMemOrImm32(mVU, &vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT, gprT1);
		}
		iPC = branchAddr(mVU) / 4;
		mVUDTendProgram(mVU, mFC, 1);
		a64_bind(armAsm, &eJMP);
		iPC = tempPC;
	}
	if (mVUup.tBit)
	{
		u32 tempPC;
		a64_label eJMP = A64_LABEL_INIT;
		/* Flush register cache early to avoid double flush on both paths */
		mVUra_flushAll(mVU->regAlloc, false);

		tempPC = iPC;
		mvuTestMemBranchZero(mVU, (mVU->index && THREAD_VU1) ? (const void*)&vu1Thread.vuFBRST : (const void*)&vuRegs[0].VI[REG_FBRST].UL,
			(isVU1 ? 0x800 : 0x8), &eJMP, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemOrImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4), gprT1);
			mvuMemOrImm32(mVU, &vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT, gprT1);
		}
		iPC = branchAddr(mVU) / 4;
		mVUDTendProgram(mVU, mFC, 1);
		a64_bind(armAsm, &eJMP);
		iPC = tempPC;
	}
	if (mVUup.mBit)
	{
		u32 tempPC;
		log_cb(RETRO_LOG_WARN, "M-Bit on normal branch, report if broken\n");
		tempPC = iPC;

		memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
		armMoveAddressToReg(RXARG1, &mVUpBlock->pStateEnd);
		armEmitCall(mVU->copyPLState);

		mVUsetupBranch(mVU, mFC);
		mVUendProgram(mVU, mFC, 3);
		iPC = branchAddr(mVU) / 4;
		mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
		iPC = tempPC;
	}
	if (mVUup.eBit)
	{
		if (mVUlow.badBranch)
			log_cb(RETRO_LOG_WARN, "End on evil Unconditional branch! - Not implemented! - If game broken report to PCSX2 Team\n");

		iPC = branchAddr(mVU) / 4;
		mVUendProgram(mVU, mFC, 1);
		return;
	}

	/* Normal Branch */
	mVUsetupBranch(mVU, mFC);
	normBranchCompile(mVU, branchAddr(mVU));
}

static void condBranch(mV, microFlagCycles* mFC, unsigned JMPcc)
{
	mVUsetupBranch(mVU, mFC);

	if (mVUup.tBit)
	{
		u32 tempPC;
		a64_label eJMP = A64_LABEL_INIT;
		a64_label tJMP = A64_LABEL_INIT;
		log_cb(RETRO_LOG_WARN, "T-Bit on branch, please report if broken\n");
		tempPC = iPC;
		mvuTestMemBranchZero(mVU, (mVU->index && THREAD_VU1) ? (const void*)&vu1Thread.vuFBRST : (const void*)&vuRegs[0].VI[REG_FBRST].UL,
			(isVU1 ? 0x800 : 0x8), &eJMP, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemOrImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4), gprT1);
			mvuMemOrImm32(mVU, &vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT, gprT1);
		}
		mVUDTendProgram(mVU, mFC, 2);
		mvuLdrsh16(mVU, gprT1, &mVU->branch);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);
		a64_b_cond(armAsm, &tJMP, A64_INVERT(JMPcc));
			incPC(4); /* Set PC to First instruction of Non-Taken Side */
			mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
			if (mVU->index && THREAD_VU1)
				armEmitCall(((const void*)(mVUTBit)));
			armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &tJMP);
		incPC(-4); /* Go Back to Branch Opcode to get branchAddr */
		iPC = branchAddr(mVU) / 4;
		mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUTBit)));
		armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &eJMP);
		iPC = tempPC;
	}
	if (mVUup.dBit && doDBitHandling)
	{
		a64_label dJMP = A64_LABEL_INIT;
		u32 tempPC = iPC;
		a64_label eJMP = A64_LABEL_INIT;
		mvuTestMemBranchZero(mVU, (mVU->index && THREAD_VU1) ? (const void*)&vu1Thread.vuFBRST : (const void*)&vuRegs[0].VI[REG_FBRST].UL,
			(isVU1 ? 0x400 : 0x4), &eJMP, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemOrImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2), gprT1);
			mvuMemOrImm32(mVU, &vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT, gprT1);
		}
		mVUDTendProgram(mVU, mFC, 2);
		mvuLdrsh16(mVU, gprT1, &mVU->branch);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);
		a64_b_cond(armAsm, &dJMP, A64_INVERT(JMPcc));
			incPC(4); /* Set PC to First instruction of Non-Taken Side */
			mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
			armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &dJMP);
		incPC(-4); /* Go Back to Branch Opcode to get branchAddr */
		iPC = branchAddr(mVU) / 4;
		mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
		armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &eJMP);
		iPC = tempPC;
	}
	if (mVUup.mBit)
	{
		a64_label dJMP = A64_LABEL_INIT;
		u32 tempPC = iPC;

		memcpy(&mVUpBlock->pStateEnd, &mVUregs, sizeof(microRegInfo));
		armMoveAddressToReg(RXARG1, &mVUpBlock->pStateEnd);
		armEmitCall(mVU->copyPLState);

		mVUendProgram(mVU, mFC, 3);
		mvuLdrsh16(mVU, gprT1, &mVU->branch);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);
		a64_b_cond(armAsm, &dJMP, JMPcc);
		incPC(4); /* Set PC to First instruction of Non-Taken Side */
		mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &dJMP);
		incPC(-4); /* Go Back to Branch Opcode to get branchAddr */
		iPC = branchAddr(mVU) / 4;
		mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
		iPC = tempPC;
	}
	if (mVUup.eBit) /* Conditional Branch With E-Bit Set */
	{
		a64_label eJMP = A64_LABEL_INIT;
		if (mVUlow.evilBranch)
			log_cb(RETRO_LOG_WARN, "End on evil branch! - Not implemented! - If game broken report to PCSX2 Team\n");

		mVUendProgram(mVU, mFC, 2);
		mvuLdrsh16(mVU, gprT1, &mVU->branch);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);

		incPC(3);
		a64_b_cond(armAsm, &eJMP, JMPcc);
			incPC(1); /* Set PC to First instruction of Non-Taken Side */
			mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
			if (mVU->index && THREAD_VU1)
				armEmitCall(((const void*)(mVUEBit)));
			armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &eJMP);
		incPC(-4); /* Go Back to Branch Opcode to get branchAddr */

		iPC = branchAddr(mVU) / 4;
		mvuStrImm32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, xPC, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
		return;
	}
	else /* Normal Conditional Branch */
	{
		microBlock* bBlock;
		mvuLdrsh16(mVU, gprT1, &mVU->branch);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);

		incPC(3);
		incPC2(1); /* Check if Branch Non-Taken Side has already been recompiled */
		blockCreate(iPC / 2);
		bBlock = mVUbm_search(mVUblocks[iPC / 2], mVU, (microRegInfo*)&mVUregs);
		incPC2(-1);
		if (bBlock) /* Branch non-taken has already been compiled */
		{
			armEmitCondBranch(A64_INVERT(JMPcc), bBlock->codeStart);
			incPC(-3); /* Go back to branch opcode (to get branch imm addr) */
			normBranchCompile(mVU, branchAddr(mVU));
		}
		else
		{
			void* jumpAddr;
			microRegInfo regBackup;
			u32 bPC;
			u8* beforeFetch;
			a64_label takenLabel = A64_LABEL_INIT;
			a64_b_cond(armAsm, &takenLabel, JMPcc);
			bPC = iPC; /* mVUcompile can modify iPC, mVUpBlock, and mVUregs so back them up */

			memcpy(&regBackup, &mVUregs, sizeof(microRegInfo));

			incPC2(1); /* Get PC for branch not-taken */
			mVUcompile(mVU, xPC, (uptr)&mVUregs);

			iPC = bPC;
			incPC(-3); /* Go back to branch opcode (to get branch imm addr) */
			a64_bind(armAsm, &takenLabel);
			beforeFetch = armGetCurrentCodePointer();
			jumpAddr = mVUblockFetch(mVU, branchAddr(mVU), (uptr)&regBackup);
			/* If mVUblockFetch found an existing block (emitted nothing), bridge to
			 * it; if it compiled inline, the block starts right here (fall through). */
			if (jumpAddr != beforeFetch)
				armEmitJmp(jumpAddr);
		}
	}
}

static void normJump(mV, microFlagCycles* mFC)
{
	if (mVUup.mBit)
	{
		log_cb(RETRO_LOG_WARN, "M-Bit on Jump! Please report if broken\n");
	}
	if (mVUlow.constJump.isValid) /* Jump Address is Constant */
	{
		int jumpAddr;
		if (mVUup.eBit) /* E-bit Jump */
		{
			iPC = (mVUlow.constJump.regValue * 2) & (mVU->progMemMask);
			mVUendProgram(mVU, mFC, 1);
			return;
		}
		jumpAddr = (mVUlow.constJump.regValue * 8) & (mVU->microMemSize - 8);
		mVUsetupBranch(mVU, mFC);
		normBranchCompile(mVU, jumpAddr);
		return;
	}
	if (mVUup.dBit && doDBitHandling)
	{
		a64_label eJMP = A64_LABEL_INIT;
		/* Flush register cache early to avoid double flush on both paths */
		mVUra_flushAll(mVU->regAlloc, false);

		mvuTestMemBranchZero(mVU, THREAD_VU1 ? (const void*)&vu1Thread.vuFBRST : (const void*)&vuRegs[0].VI[REG_FBRST].UL,
			(isVU1 ? 0x400 : 0x4), &eJMP, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemOrImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x200 : 0x2), gprT1);
			mvuMemOrImm32(mVU, &vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT, gprT1);
		}
		mVUDTendProgram(mVU, mFC, 2);
		mvuLdr32(mVU, gprT1, &mVU->branch);
		mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, gprT1);
		armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &eJMP);
	}
	if (mVUup.tBit)
	{
		a64_label eJMP = A64_LABEL_INIT;
		/* Flush register cache early to avoid double flush on both paths */
		mVUra_flushAll(mVU->regAlloc, false);

		mvuTestMemBranchZero(mVU, (mVU->index && THREAD_VU1) ? (const void*)&vu1Thread.vuFBRST : (const void*)&vuRegs[0].VI[REG_FBRST].UL,
			(isVU1 ? 0x800 : 0x8), &eJMP, gprT1);
		if (!mVU->index || !THREAD_VU1)
		{
			mvuMemOrImm32(mVU, &vuRegs[0].VI[REG_VPU_STAT].UL, (isVU1 ? 0x400 : 0x4), gprT1);
			mvuMemOrImm32(mVU, &vuRegs[mVU->index].flags, VUFLAG_INTCINTERRUPT, gprT1);
		}
		mVUDTendProgram(mVU, mFC, 2);
		mvuLdr32(mVU, gprT1, &mVU->branch);
		mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUTBit)));
		armEmitJmp(mVU->exitFunct);
		a64_bind(armAsm, &eJMP);
	}
	if (mVUup.eBit) /* E-bit Jump */
	{
		mVUendProgram(mVU, mFC, 2);
		mvuLdr32(mVU, gprT1, &mVU->branch);
		mvuStr32(mVU, &vuRegs[mVU->index].VI[REG_TPC].UL, gprT1);
		if (mVU->index && THREAD_VU1)
			armEmitCall(((const void*)(mVUEBit)));
		armEmitJmp(mVU->exitFunct);
	}
	else
	{
		normJumpCompile(mVU, mFC, false);
	}
}
