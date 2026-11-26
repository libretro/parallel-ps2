/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+ */

#pragma once

/* ARM64 microVU — Lower opcode handlers (Phase 7, task 7.5b).
 *
 * AArch64 port of pcsx2/x86/microVU_Lower.inl. The Lower pipe is the VU's integer /
 * load-store / EFU / flag-register / branch ISA:
 * * VI integer ALU      — IADD/IADDI/IADDIU/IAND/IOR/ISUB/ISUBIU
 * * load/store          — LQ/LQD/LQI, SQ/SQD/SQI, ILW/ILWR, ISW/ISWR
 * * EFU estimates        — DIV/SQRT/RSQRT, EATAN* /EEXP/ELENG/ERCPR/ERLENG/
 * ERSADD/ERSQRT/ESADD/ESIN/ESQRT/ESUM, WAITP/WAITQ
 * * register moves       — MFIR/MFP/MOVE/MR32/MTIR
 * * random generator     — RINIT/RGET/RNEXT/RXOR
 * * flag-register ops    — FCxxx / FMxxx / FSxxx
 * * VIF window           — XTOP/XITOP, XGKICK
 * * branches/jumps       — B/BAL, IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ, JR/JALR
 * (the branch *drivers* normBranch/normJump/condBranch
 * live in aVU_Branch.inl)
 *
 * Key x86->NEON translations (in addition to the Upper-file set):
 * * xMOVD(gpr, xmm) / xMOVDZX(xmm, gpr)  -> Fmov(gpr.W(), xmm.S()) / Fmov(xmm.S(), gpr.W())
 * * xMOVSS(d, s)                          -> Ins(d.V4S(), 0, s.V4S(), 0)
 * * xMOVSSZX(xmm, ptr32[c])               -> Ldr(xmm.S(), [c])  (zeroes upper lanes)
 * * xMOVAPS(d, s)                         -> Mov(d.V16B(), s.V16B())
 * * xSQRT.SS(d, s)                        -> Fsqrt(d.S(), s.S())
 * * xMUL/xADD/xSUB.SS(r, ptr32[c])        -> load c into scratch S, F{mul,add,sub}
 * * xCMPEQ.SS(0, r) + xPTEST              -> Fcmeq(.S) + Fmov->W + Cmp (testZero)
 * * xMOVMSKPS                             -> mVUmovemask (aVU_Upper.inl)
 * * absolute ptr16/ptr32 mem ops          -> mvuLdr* /mvuStr* (aVU_Misc.inl)
 * * xComplexAddress(tmp, base, idx)       -> materialize base, Add idx, base+off MemOperand
 *
 * The constant-VU-address fast path (mVUoptimizeConstantAddr, aVU_Misc.inl)
 * returns an absolute host pointer; the runtime path computes the byte offset in
 * gprT1 and runs it through mVUaddrFix (vuRegs[0]/vuRegs[1] wrap + window remap). */

/* 64-bit views of the two emit scratch GPRs (x86: gprT1q/gprT2q). mVUaddrFix and
 * the complex-address arithmetic need the full 64-bit register. */
#define gprT1q A64_X(9)
#define gprT2q A64_X(10)

/* ------------------------------------------------------------------
 * Base+offset load/store helpers (LQ/SQ family — arbitrary VU-mem base reg)
 * ------------------------------------------------------------------
 * mVUloadReg/mVUsaveReg (aVU_IR.h) are hard-wired to the RVUSTATE base; the VU
 * data-memory pointer (vuRegs[mVU->index].Mem) is a separate allocation, so LQ/SQ need
 * versions that take an already-materialized 64-bit base register. Same lane
 * semantics + modXYZW convention as mVUsaveReg. */
static void mvuLoadRegBase(a64r reg, a64r base, int xyzw)
{
	switch (xyzw)
	{
		case 8:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(base, 0));  break; /* X */
		case 4:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(base, 4));  break; /* Y */
		case 2:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(base, 8));  break; /* Z */
		case 1:  a64_ldr(armAsm, a64_as(reg, A64K_S), a64_mem_off(base, 12)); break; /* W */
		default: a64_ldr(armAsm, a64_as(reg, A64K_Q), a64_mem_off(base, 0));     break;
	}
}

static void mvuSaveRegBase(a64r reg, a64r base, int xyzw, bool modXYZW)
{
	if (xyzw == 0xf)
	{
		a64_str(armAsm, a64_as(reg, A64K_Q), a64_mem_off(base, 0));
		return;
	}
	if (modXYZW && (xyzw == 4 || xyzw == 2 || xyzw == 1))
	{
		const u32 coff = (xyzw == 4) ? 4u : (xyzw == 2) ? 8u : 12u;
		a64_str(armAsm, a64_as(reg, A64K_S), a64_mem_off(base, coff));
		return;
	}
	if (xyzw & 8)
		a64_str(armAsm, a64_as(reg, A64K_S), a64_mem_off(base, 0));
	if (xyzw & 4)
	{
		a64_add_imm(armAsm, RVUADDR, base, 4);
		a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 1, a64_mem_off(RVUADDR, 0));
	}
	if (xyzw & 2)
	{
		a64_add_imm(armAsm, RVUADDR, base, 8);
		a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 2, a64_mem_off(RVUADDR, 0));
	}
	if (xyzw & 1)
	{
		a64_add_imm(armAsm, RVUADDR, base, 12);
		a64_st1_lane(armAsm, a64_as(reg, A64K_4S), 3, a64_mem_off(RVUADDR, 0));
	}
}

/* Materialize host base `p` into baseReg, then baseReg += indexReg (the byte
 * offset from mVUaddrFix). x86: xComplexAddress(tmp, base, idx). */
static INLINE void mvuComplexAddr(a64r baseReg, const void* p, a64r indexReg)
{
	armMoveAddressToReg(baseReg, p);
	a64_add(armAsm, baseReg, baseReg, indexReg);
}

/* 16-bit zero-extending absolute load (x86: xMOVZX(reg, ptr16[addr])). */
static INLINE void mvuLdrhZ(microVU* mVU, a64r wreg, const void* addr)
{
	a64_ldrh(armAsm, a64_asw(wreg), mvuAbsMem(mVU, addr, 2));
}

/* ------------------------------------------------------------------
 * Branch-attribute setup (x86: microVU_Lower.inl setBranchA)
 * ------------------------------------------------------------------
 * Records the branch type (x) on the lower op + handles the "branch to next
 * instruction" NOP optimization. No emit — pure IR bookkeeping across all passes.
 * Defined here (this .inl is #included before aVU_Tables.inl) so both the Lower
 * branch handlers below and B/BAL in the tables file can call it. */
static void setBranchA(mP, int x, int _x_)
{
	bool isBranchDelaySlot = false;

	incPC(-2);
	if (mVUlow.branch)
		isBranchDelaySlot = true;
	incPC(2);

	pass1
	{
		if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot)
		{
			log_cb(RETRO_LOG_DEBUG, "microVU%d: Branch Optimization\n", mVU->index);
			mVUlow.isNOP = true;
			return;
		}
		mVUbranch     = x;
		mVUlow.branch = x;
	}
	pass2 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
	pass3 { mVUbranch = x; }
	pass4 { if (_Imm11_ == 1 && !_x_ && !isBranchDelaySlot) { return; } mVUbranch = x; }
}

/* ------------------------------------------------------------------
 * DIV/SQRT/RSQRT
 * ------------------------------------------------------------------ */

/* Test if Vector lane0 is +/- Zero. Leaves the result in gprTemp and sets the
 * condition flags (eq == "reg != 0", matching the x86 PTEST+JZ pattern: the
 * caller's B(eq) skips when the value is non-zero). */
static __fi void testZero(a64r xmmReg, a64r xmmTemp, a64r gprTemp)
{
	a64_eor(armAsm, a64_as(xmmTemp, A64K_16B), a64_as(xmmTemp, A64K_16B), a64_as(xmmTemp, A64K_16B));
	a64_fcmeq(armAsm, a64_as(xmmTemp, A64K_S), a64_as(xmmTemp, A64K_S), a64_as(xmmReg, A64K_S)); /* lane0 = (0 == reg) ? ~0 : 0 */
	a64_fmov(armAsm, a64_asw(gprTemp), a64_as(xmmTemp, A64K_S));
	a64_cmp_imm(armAsm, a64_asw(gprTemp), 0); /* ZF(eq) set when reg != 0 */
}

/* Test if Vector is Negative (sets I-flag and makes positive). */
static __fi void testNeg(mV, a64r xmmReg, a64r gprTemp)
{
	a64_label skip = A64_LABEL_INIT;
	mVUmovemask(gprTemp, xmmReg);
	a64_tst_imm(armAsm, a64_asw(gprTemp), 1);
	a64_b_cond(armAsm, &skip, A64_EQ); /* bit0 clear -> not negative */
		mvuStrImm32(mVU, &mVU->divFlag, divI, gprT2);
		mvuLdrQ(mVU, RQSCRATCH, mVU->glob.absclip);
		a64_and(armAsm, a64_as(xmmReg, A64K_16B), a64_as(xmmReg, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
	a64_bind(armAsm, &skip);
}

/* ------------------------------------------------------------------
 * Exact DIV/SQRT/RSQRT, the arm64 twin of the x86 path: the SRT
 * quotient's final increment is trajectory-dependent with no closed
 * form, so exact Q runs the model's recurrence through a C call.
 * aVU needs no custom thunk - the tree's own backup/restore around
 * armEmitCall is the established full-save idiom - and the model
 * computes the specials and the invalid and zero-divide flags the
 * branchy emissions below otherwise construct by hand.
 * ------------------------------------------------------------------ */
static void aVUexactDivC(microVU* m)
{
	const u32 a = m->exactDivBuf[0];
	const u32 b = m->exactDivBuf[1];
	const u32 op = m->exactDivBuf[2];
	const ps2f_u64 r = (op == 0) ? ps2f_div(a, b)
	                 : (op == 1) ? ps2f_sqrt(b)
	                             : ps2f_rsqrt(a, b);
	m->exactDivBuf[0] = ps2f_raw(r);
	m->divFlag = (r & PS2F_IV) ? divI : ((r & PS2F_DZ) ? divD : 0);
}

static void mVUexactDivCall(microVU* mVU, a64r fs, a64r ft, int op, a64r dstReg)
{
	if (op != 1)
	{
		a64_umov(armAsm, a64_asw(gprT1), a64_as(fs, A64K_4S), 0);
		mvuStr32(mVU, &mVU->exactDivBuf[0], gprT1);
	}
	a64_umov(armAsm, a64_asw(gprT1), a64_as(ft, A64K_4S), 0);
	mvuStr32(mVU, &mVU->exactDivBuf[1], gprT1);
	mvuStrImm32(mVU, &mVU->exactDivBuf[2], op, gprT1);
	mVUbackupRegs(mVU, true, true);
	a64_mov_imm(armAsm, RWARG1, ((uintptr_t)(mVU)));
	armEmitCall(((const void*)(&aVUexactDivC)));
	mVUrestoreRegs(mVU, true, true);
	mvuLdr32(mVU, gprT1, &mVU->exactDivBuf[0]);
	a64_ins(armAsm, a64_as(dstReg, A64K_4S), 0, a64_asw(gprT1));
}

mVUop(mVU_DIV)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 7); }
	pass2
	{
		a64r Fs;
		a64r t1;
		a64r Ft;
		if (_Ftf_) Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, (1 << (3 - _Ftf_)), true);
		else       Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, -1, 0, true);
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);

		if (CHECK_VU_EXACTDIV)
		{
			mVUexactDivCall(mVU, Fs, Ft, 0, Fs);
			goto div_write;
		}

		{
		a64_label cjmp = A64_LABEL_INIT, ajmp = A64_LABEL_INIT, bjmp = A64_LABEL_INIT, djmp = A64_LABEL_INIT;
		testZero(Ft, t1, gprT1); /* Test if Ft is zero */
		a64_b_cond(armAsm, &cjmp, A64_EQ); /* Skip if not zero */

			testZero(Fs, t1, gprT1); /* Test if Fs is zero */
			a64_b_cond(armAsm, &ajmp, A64_EQ);
				mvuStrImm32(mVU, &mVU->divFlag, divI, gprT1); /* Set invalid flag (0/0) */
				a64_b(armAsm, &bjmp);
			a64_bind(armAsm, &ajmp);
				mvuStrImm32(mVU, &mVU->divFlag, divD, gprT1); /* Zero divide (only when not 0/0) */
			a64_bind(armAsm, &bjmp);

			a64_eor(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(Ft, A64K_16B));
			mvuLdrQ(mVU, RQSCRATCH, mVU->glob.signbit);
			a64_and(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
			mvuLdrQ(mVU, RQSCRATCH, mVU->glob.maxvals);
			a64_orr(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(RQSCRATCH, A64K_16B)); /* If division by zero, then Fs = +/- fmax */

			a64_b(armAsm, &djmp);
		a64_bind(armAsm, &cjmp);
			mvuStrImm32(mVU, &mVU->divFlag, 0, gprT1); /* Clear I/D flags */
			SSE_DIVSS(mVU, Fs, Ft, xEmptyReg, xEmptyReg);
			mVUclamp1(mVU, Fs, t1, 8, true);
		a64_bind(armAsm, &djmp);
		}

div_write:
		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU->cop2)
		{
			a64_and_imm(armAsm, gprF0, gprF0, ~0xc0000);
			mvuLdr32(mVU, gprT1, &mVU->divFlag);
			a64_orr(armAsm, gprF0, gprF0, gprT1);
		}

		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, Ft);
		mVUra_clearNeeded(mVU->regAlloc, t1);
	}
	pass3 {  }
}

mVUop(mVU_SQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, 0, 0, _Ft_, _Ftf_, 7); }
	pass2
	{
		const a64r Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, (1 << (3 - _Ftf_)), true);

		if (CHECK_VU_EXACTDIV)
		{
			mVUexactDivCall(mVU, Ft, Ft, 1, Ft);
			goto sqrt_write;
		}

		mvuStrImm32(mVU, &mVU->divFlag, 0, gprT1); /* Clear I/D flags */
		testNeg(mVU, Ft, gprT1); /* Check for negative sqrt */

		if (CHECK_VU_OVERFLOW(mVU->index)) /* Clamp infinities (only need positive clamp since Ft is positive) */
		{
			/* Fminnm, not Fmin: x86 MIN.SS returns the second operand (fmax) when Ft
			 * is a NaN pattern (a valid huge number on the PS2); Fmin would propagate
			 * the NaN into Fsqrt and poison Q. */
			mvuLdrSS(mVU, RQSCRATCH, mVU->glob.maxvals);
			a64_fminnm(armAsm, a64_as(Ft, A64K_S), a64_as(Ft, A64K_S), a64_as(RQSCRATCH, A64K_S));
		}
		a64_fsqrt(armAsm, a64_as(Ft, A64K_S), a64_as(Ft, A64K_S));

sqrt_write:
		writeQreg(Ft, mVUinfo.writeQ);

		if (mVU->cop2)
		{
			a64_and_imm(armAsm, gprF0, gprF0, ~0xc0000);
			mvuLdr32(mVU, gprT1, &mVU->divFlag);
			a64_orr(armAsm, gprF0, gprF0, gprT1);
		}

		mVUra_clearNeeded(mVU->regAlloc, Ft);
	}
	pass3 {  }
}

mVUop(mVU_RSQRT)
{
	pass1 { mVUanalyzeFDIV(mVU, _Fs_, _Fsf_, _Ft_, _Ftf_, 13); }
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		const a64r Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, (1 << (3 - _Ftf_)), true);
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);

		if (CHECK_VU_EXACTDIV)
		{
			mVUexactDivCall(mVU, Fs, Ft, 2, Fs);
			goto rsqrt_write;
		}

		mvuStrImm32(mVU, &mVU->divFlag, 0, gprT1); /* Clear I/D flags */
		testNeg(mVU, Ft, gprT1); /* Check for negative sqrt */

		a64_fsqrt(armAsm, a64_as(Ft, A64K_S), a64_as(Ft, A64K_S));

		{
		a64_label ajmp = A64_LABEL_INIT, bjmp = A64_LABEL_INIT, cjmp = A64_LABEL_INIT, djmp = A64_LABEL_INIT;
		testZero(Ft, t1, gprT1); /* Test if Ft is zero */
		a64_b_cond(armAsm, &ajmp, A64_EQ); /* Skip if not zero */

			testZero(Fs, t1, gprT1); /* Test if Fs is zero */
			a64_b_cond(armAsm, &bjmp, A64_EQ); /* Skip if none are */
				mvuStrImm32(mVU, &mVU->divFlag, divI, gprT1); /* Set invalid flag (0/0) */
				a64_b(armAsm, &cjmp);
			a64_bind(armAsm, &bjmp);
				mvuStrImm32(mVU, &mVU->divFlag, divD, gprT1); /* Zero divide flag (only when not 0/0) */
			a64_bind(armAsm, &cjmp);

			mvuLdrQ(mVU, RQSCRATCH, mVU->glob.signbit);
			a64_and(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
			mvuLdrQ(mVU, RQSCRATCH, mVU->glob.maxvals);
			a64_orr(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(RQSCRATCH, A64K_16B)); /* Fs = +/-Max */

			a64_b(armAsm, &djmp);
		a64_bind(armAsm, &ajmp);
			SSE_DIVSS(mVU, Fs, Ft, xEmptyReg, xEmptyReg);
			mVUclamp1(mVU, Fs, t1, 8, true);
		a64_bind(armAsm, &djmp);
		}

rsqrt_write:
		writeQreg(Fs, mVUinfo.writeQ);

		if (mVU->cop2)
		{
			a64_and_imm(armAsm, gprF0, gprF0, ~0xc0000);
			mvuLdr32(mVU, gprT1, &mVU->divFlag);
			a64_orr(armAsm, gprF0, gprF0, gprT1);
		}

		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, Ft);
		mVUra_clearNeeded(mVU->regAlloc, t1);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * EATAN/EEXP/ELENG/ERCPR/ERLENG/ERSADD/ERSQRT/ESADD/ESIN/ESQRT/ESUM
 * ------------------------------------------------------------------ */

#define EATANhelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs, xEmptyReg, xEmptyReg); \
		SSE_MULSS(mVU, t2, Fs, xEmptyReg, xEmptyReg); \
		a64_mov(armAsm, a64_as(t1, A64K_16B), a64_as(t2, A64K_16B)); \
		mvuLdrSS(mVU, RQSCRATCH, addr); \
		a64_fmul(armAsm, a64_as(t1, A64K_S), a64_as(t1, A64K_S), a64_as(RQSCRATCH, A64K_S)); \
		SSE_ADDSS(mVU, PQ, t1, xEmptyReg, xEmptyReg); \
	}

static __fi void mVU_EATAN_(mV, a64r PQ, a64r Fs, a64r t1, a64r t2)
{
	a64_ins_elem(armAsm, a64_as(PQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
	mvuLdrSS(mVU, RQSCRATCH, mVU->glob.T1);
	mVUscalarMulKeep(PQ, PQ, RQSCRATCH); /* keep Q lanes (see aVU_IR.h) */
	a64_mov(armAsm, a64_as(t2, A64K_16B), a64_as(Fs, A64K_16B));
	EATANhelper(mVU->glob.T2);
	EATANhelper(mVU->glob.T3);
	EATANhelper(mVU->glob.T4);
	EATANhelper(mVU->glob.T5);
	EATANhelper(mVU->glob.T6);
	EATANhelper(mVU->glob.T7);
	EATANhelper(mVU->glob.T8);
	mvuLdrSS(mVU, RQSCRATCH, mVU->glob.Pi4);
	mVUscalarAddKeep(PQ, PQ, RQSCRATCH); /* keep Q lanes (see aVU_IR.h) */
	mVUshufflePS(PQ, PQ, mVUinfo.writeP ? 0x27 : 0xC6);
}

mVUop(mVU_EATAN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 54);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		const a64r t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.one);
		a64_fsub(armAsm, a64_as(Fs, A64K_S), a64_as(Fs, A64K_S), a64_as(RQSCRATCH, A64K_S));
		mVUscalarAddKeep(mVU_xmmPQ, mVU_xmmPQ, RQSCRATCH); /* keep Q lanes (see aVU_IR.h) */
		SSE_DIVSS(mVU, Fs, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		mVU_EATAN_(mVU, mVU_xmmPQ, Fs, t1, t2);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, t1);
		mVUra_clearNeeded(mVU->regAlloc, t2);
	}
	pass3 {  }
}

mVUop(mVU_EATANxy)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, 0xf, true);
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		const a64r t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		mVUshufflePS(Fs, t1, 0x01);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		SSE_SUBSS(mVU, Fs, t1, xEmptyReg, xEmptyReg); /* y-x, not y-1? >< */
		SSE_ADDSS(mVU, t1, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		SSE_DIVSS(mVU, Fs, t1, xEmptyReg, xEmptyReg);
		mVU_EATAN_(mVU, mVU_xmmPQ, Fs, t1, t2);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, t1);
		mVUra_clearNeeded(mVU->regAlloc, t2);
	}
	pass3 {  }
}

mVUop(mVU_EATANxz)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 54);
	}
	pass2
	{
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, 0xf, true);
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		const a64r t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		mVUshufflePS(Fs, t1, 0x02);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		SSE_SUBSS(mVU, Fs, t1, xEmptyReg, xEmptyReg);
		SSE_ADDSS(mVU, t1, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		SSE_DIVSS(mVU, Fs, t1, xEmptyReg, xEmptyReg);
		mVU_EATAN_(mVU, mVU_xmmPQ, Fs, t1, t2);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, t1);
		mVUra_clearNeeded(mVU->regAlloc, t2);
	}
	pass3 {  }
}

#define eexpHelper(addr) \
	{ \
		SSE_MULSS(mVU, t2, Fs, xEmptyReg, xEmptyReg); \
		a64_mov(armAsm, a64_as(t1, A64K_16B), a64_as(t2, A64K_16B)); \
		mvuLdrSS(mVU, RQSCRATCH, addr); \
		a64_fmul(armAsm, a64_as(t1, A64K_S), a64_as(t1, A64K_S), a64_as(RQSCRATCH, A64K_S)); \
		SSE_ADDSS(mVU, mVU_xmmPQ, t1, xEmptyReg, xEmptyReg); \
	}

mVUop(mVU_EEXP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 44);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		const a64r t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.E1);
		mVUscalarMulKeep(mVU_xmmPQ, mVU_xmmPQ, RQSCRATCH); /* keep Q lanes (see aVU_IR.h) */
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.one);
		mVUscalarAddKeep(mVU_xmmPQ, mVU_xmmPQ, RQSCRATCH); /* keep Q lanes (see aVU_IR.h) */
		a64_mov(armAsm, a64_as(t1, A64K_16B), a64_as(Fs, A64K_16B));
		SSE_MULSS(mVU, t1, Fs, xEmptyReg, xEmptyReg);
		a64_mov(armAsm, a64_as(t2, A64K_16B), a64_as(t1, A64K_16B));
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.E2);
		a64_fmul(armAsm, a64_as(t1, A64K_S), a64_as(t1, A64K_S), a64_as(RQSCRATCH, A64K_S));
		SSE_ADDSS(mVU, mVU_xmmPQ, t1, xEmptyReg, xEmptyReg);
		eexpHelper(mVU->glob.E3);
		eexpHelper(mVU->glob.E4);
		eexpHelper(mVU->glob.E5);
		SSE_MULSS(mVU, t2, Fs, xEmptyReg, xEmptyReg);
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.E6);
		a64_fmul(armAsm, a64_as(t2, A64K_S), a64_as(t2, A64K_S), a64_as(RQSCRATCH, A64K_S));
		SSE_ADDSS(mVU, mVU_xmmPQ, t2, xEmptyReg, xEmptyReg);
		SSE_MULSS(mVU, mVU_xmmPQ, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		SSE_MULSS(mVU, mVU_xmmPQ, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		mvuLdrSS(mVU, t2, mVU->glob.one);
		SSE_DIVSS(mVU, t2, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(t2, A64K_4S), 0);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, t1);
		mVUra_clearNeeded(mVU->regAlloc, t2);
	}
	pass3 {  }
}

/* sumXYZ(): PQ.x = x^2 + y^2 + z^2 */
static __fi void mVU_sumXYZ(mV, a64r PQ, a64r Fs)
{
	a64_fmul(armAsm, a64_as(Fs, A64K_4S), a64_as(Fs, A64K_4S), a64_as(Fs, A64K_4S)); /* x^2, y^2, z^2, w^2 */
	a64_eor(armAsm, a64_as(RQSCRATCH, A64K_16B), a64_as(RQSCRATCH, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
	a64_ins_elem(armAsm, a64_as(Fs, A64K_4S), 3, a64_as(RQSCRATCH, A64K_4S), 0); /* drop w^2 */
	a64_faddp(armAsm, a64_as(Fs, A64K_4S), a64_as(Fs, A64K_4S), a64_as(Fs, A64K_4S)); /* [x^2+y^2, z^2, ...] */
	a64_faddp_scalar(armAsm, a64_as(Fs, A64K_S), a64_as(Fs, A64K_2S)); /* x^2+y^2+z^2 in lane0 */
	a64_ins_elem(armAsm, a64_as(PQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
}

mVUop(mVU_ELENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mVU_sumXYZ(mVU, mVU_xmmPQ, Fs);
		mVUscalarSqrtKeep(mVU_xmmPQ, mVU_xmmPQ); /* keep Q lanes (see aVU_IR.h) */
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ERCPR)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mvuLdrSS(mVU, Fs, mVU->glob.one);
		SSE_DIVSS(mVU, Fs, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ERLENG)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 24);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mVU_sumXYZ(mVU, mVU_xmmPQ, Fs);
		mVUscalarSqrtKeep(mVU_xmmPQ, mVU_xmmPQ); /* keep Q lanes (see aVU_IR.h) */
		mvuLdrSS(mVU, Fs, mVU->glob.one);
		SSE_DIVSS(mVU, Fs, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ERSADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 18);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mVU_sumXYZ(mVU, mVU_xmmPQ, Fs);
		mvuLdrSS(mVU, Fs, mVU->glob.one);
		SSE_DIVSS(mVU, Fs, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ERSQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 18);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mvuLdrQ(mVU, RQSCRATCH, mVU->glob.absclip);
		a64_and(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
		mVUscalarSqrtKeep(mVU_xmmPQ, Fs); /* keep Q lanes (see aVU_IR.h) */
		mvuLdrSS(mVU, Fs, mVU->glob.one);
		SSE_DIVSS(mVU, Fs, mVU_xmmPQ, xEmptyReg, xEmptyReg);
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ESADD)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 11);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mVU_sumXYZ(mVU, mVU_xmmPQ, Fs);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ESIN)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 29);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		const a64r t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0); /* pq = X */
		SSE_MULSS(mVU, Fs, Fs, xEmptyReg, xEmptyReg); /* fs = X^2 */
		a64_mov(armAsm, a64_as(t1, A64K_16B), a64_as(Fs, A64K_16B)); /* t1 = X^2 */
		SSE_MULSS(mVU, Fs, mVU_xmmPQ, xEmptyReg, xEmptyReg); /* fs = X^3 */
		a64_mov(armAsm, a64_as(t2, A64K_16B), a64_as(Fs, A64K_16B)); /* t2 = X^3 */
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.S2);
		a64_fmul(armAsm, a64_as(Fs, A64K_S), a64_as(Fs, A64K_S), a64_as(RQSCRATCH, A64K_S)); /* fs = s2 * X^3 */
		SSE_ADDSS(mVU, mVU_xmmPQ, Fs, xEmptyReg, xEmptyReg); /* pq = X + s2 * X^3 */

		SSE_MULSS(mVU, t2, t1, xEmptyReg, xEmptyReg); /* t2 = X^3 * X^2 */
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.S3);
		a64_fmul(armAsm, a64_as(Fs, A64K_S), a64_as(t2, A64K_S), a64_as(RQSCRATCH, A64K_S)); /* ps = s3 * X^5 */
		SSE_ADDSS(mVU, mVU_xmmPQ, Fs, xEmptyReg, xEmptyReg); /* pq = X + s2 * X^3 + s3 * X^5 */

		SSE_MULSS(mVU, t2, t1, xEmptyReg, xEmptyReg); /* t2 = X^5 * X^2 */
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.S4);
		a64_fmul(armAsm, a64_as(Fs, A64K_S), a64_as(t2, A64K_S), a64_as(RQSCRATCH, A64K_S)); /* fs = s4 * X^7 */
		SSE_ADDSS(mVU, mVU_xmmPQ, Fs, xEmptyReg, xEmptyReg); /* pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7 */

		SSE_MULSS(mVU, t2, t1, xEmptyReg, xEmptyReg); /* t2 = X^7 * X^2 */
		mvuLdrSS(mVU, RQSCRATCH, mVU->glob.S5);
		a64_fmul(armAsm, a64_as(t2, A64K_S), a64_as(t2, A64K_S), a64_as(RQSCRATCH, A64K_S)); /* t2 = s5 * X^9 */
		SSE_ADDSS(mVU, mVU_xmmPQ, t2, xEmptyReg, xEmptyReg); /* pq = X + s2 * X^3 + s3 * X^5 + s4 * X^7 + s5 * X^9 */
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, t1);
		mVUra_clearNeeded(mVU->regAlloc, t2);
	}
	pass3 {  }
}

mVUop(mVU_ESQRT)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU1(mVU, _Fs_, _Fsf_, 12);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mvuLdrQ(mVU, RQSCRATCH, mVU->glob.absclip);
		a64_and(armAsm, a64_as(Fs, A64K_16B), a64_as(Fs, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
		mVUscalarSqrtKeep(mVU_xmmPQ, Fs); /* keep Q lanes (see aVU_IR.h) */
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_ESUM)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeEFU2(mVU, _Fs_, 12);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, true);
		const a64r t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip xmmPQ to get Valid P instance */
		mVUshufflePS(t1, Fs, 0x1b);
		SSE_ADDPS(mVU, Fs, t1, xEmptyReg, xEmptyReg);
		mVUshufflePS(t1, Fs, 0x01);
		SSE_ADDSS(mVU, Fs, t1, xEmptyReg, xEmptyReg);
		a64_ins_elem(armAsm, a64_as(mVU_xmmPQ, A64K_4S), 0, a64_as(Fs, A64K_4S), 0);
		mVUshufflePS(mVU_xmmPQ, mVU_xmmPQ, mVUinfo.writeP ? 0x27 : 0xC6); /* Flip back */
		mVUra_clearNeeded(mVU->regAlloc, Fs);
		mVUra_clearNeeded(mVU->regAlloc, t1);
	}
	pass3 {  }
}

#undef EATANhelper
#undef eexpHelper

/* ------------------------------------------------------------------
 * FCAND/FCEQ/FCGET/FCOR/FCSET
 * ------------------------------------------------------------------ */

mVUop(mVU_FCAND)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const a64r dst = mVUra_allocGPR(mVU->regAlloc, -1, 1, mVUlow.backupVI, false);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		a64_and_imm(armAsm, dst, dst, _Imm24_);
		a64_add_imm(armAsm, dst, dst, 0xffffff);
		a64_lsr_imm(armAsm, dst, dst, 24);
		mVUra_clearNeeded(mVU->regAlloc, dst);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCEQ)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const a64r dst = mVUra_allocGPR(mVU->regAlloc, -1, 1, mVUlow.backupVI, false);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		a64_eor_imm(armAsm, dst, dst, _Imm24_);
		a64_sub_imm(armAsm, dst, dst, 1);
		a64_lsr_imm(armAsm, dst, dst, 31);
		mVUra_clearNeeded(mVU->regAlloc, dst);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCGET)
{
	pass1 { mVUanalyzeCflag(mVU, _It_); }
	pass2
	{
		const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		mVUallocCFLAGa(mVU, regT, cFLAG.read);
		a64_and_imm(armAsm, regT, regT, 0xfff);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCOR)
{
	pass1 { mVUanalyzeCflag(mVU, 1); }
	pass2
	{
		const a64r dst = mVUra_allocGPR(mVU->regAlloc, -1, 1, mVUlow.backupVI, false);
		mVUallocCFLAGa(mVU, dst, cFLAG.read);
		a64_orr_imm(armAsm, dst, dst, _Imm24_);
		a64_add_imm(armAsm, dst, dst, 1); /* If 24 1's will make 25th bit 1, else 0 */
		a64_lsr_imm(armAsm, dst, dst, 24); /* Get the 25th bit (also clears the rest of the garbage in the reg) */
		mVUra_clearNeeded(mVU->regAlloc, dst);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 4; }
}

mVUop(mVU_FCSET)
{
	pass1 { cFLAG.doFlag = true; }
	pass2
	{
		a64_mov_imm(armAsm, gprT1, _Imm24_);
		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * FMAND/FMEQ/FMOR
 * ------------------------------------------------------------------ */

mVUop(mVU_FMAND)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		a64r regT;
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		regT = mVUra_allocGPR(mVU->regAlloc, _Is_, _It_, mVUlow.backupVI, false);
		a64_and(armAsm, regT, regT, gprT1);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMEQ)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		a64r regT;
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		regT = mVUra_allocGPR(mVU->regAlloc, _Is_, _It_, mVUlow.backupVI, false);
		a64_eor(armAsm, regT, regT, gprT1);
		a64_sub_imm(armAsm, regT, regT, 1);
		a64_lsr_imm(armAsm, regT, regT, 31);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 2; }
}

mVUop(mVU_FMOR)
{
	pass1 { mVUanalyzeMflag(mVU, _Is_, _It_); }
	pass2
	{
		a64r regT;
		mVUallocMFLAGa(mVU, gprT1, mFLAG.read);
		regT = mVUra_allocGPR(mVU->regAlloc, _Is_, _It_, mVUlow.backupVI, false);
		a64_orr(armAsm, regT, regT, gprT1);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 2; }
}

/* ------------------------------------------------------------------
 * FSAND/FSEQ/FSOR/FSSET
 * ------------------------------------------------------------------ */

mVUop(mVU_FSAND)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		a64r reg;
		if (_Imm12_ & 0x0c30) log_cb(RETRO_LOG_DEBUG, "mVU_FSAND: Checking I/D/IS/DS Flags\n");
		if (_Imm12_ & 0x030c) log_cb(RETRO_LOG_DEBUG, "mVU_FSAND: Checking U/O/US/OS Flags\n");
		reg = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		mVUallocSFLAGc(reg, gprT1, sFLAG.read);
		a64_and_imm(armAsm, reg, reg, _Imm12_);
		mVUra_clearNeeded(mVU->regAlloc, reg);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSOR)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		const a64r reg = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		mVUallocSFLAGc(reg, gprT2, sFLAG.read);
		a64_orr_imm(armAsm, reg, reg, _Imm12_);
		mVUra_clearNeeded(mVU->regAlloc, reg);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSEQ)
{
	pass1 { mVUanalyzeSflag(mVU, _It_); }
	pass2
	{
		a64r reg;
		int imm = 0;
		if (_Imm12_ & 0x0c30) log_cb(RETRO_LOG_DEBUG, "mVU_FSEQ: Checking I/D/IS/DS Flags\n");
		if (_Imm12_ & 0x030c) log_cb(RETRO_LOG_DEBUG, "mVU_FSEQ: Checking U/O/US/OS Flags\n");
		if (_Imm12_ & 0x0001) imm |= 0x0000f00; /* Z */
		if (_Imm12_ & 0x0002) imm |= 0x000f000; /* S */
		if (_Imm12_ & 0x0004) imm |= 0x0010000; /* U */
		if (_Imm12_ & 0x0008) imm |= 0x0020000; /* O */
		if (_Imm12_ & 0x0010) imm |= 0x0040000; /* I */
		if (_Imm12_ & 0x0020) imm |= 0x0080000; /* D */
		if (_Imm12_ & 0x0040) imm |= 0x000000f; /* ZS */
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; /* SS */
		if (_Imm12_ & 0x0100) imm |= 0x0400000; /* US */
		if (_Imm12_ & 0x0200) imm |= 0x0800000; /* OS */
		if (_Imm12_ & 0x0400) imm |= 0x1000000; /* IS */
		if (_Imm12_ & 0x0800) imm |= 0x2000000; /* DS */

		reg = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		mVUallocSFLAGa(reg, sFLAG.read);
		setBitFSEQ(reg, 0x0f00); /* Z  bit */
		setBitFSEQ(reg, 0xf000); /* S  bit */
		setBitFSEQ(reg, 0x000f); /* ZS bit */
		setBitFSEQ(reg, 0x00f0); /* SS bit */
		a64_eor_imm(armAsm, reg, reg, imm);
		a64_sub_imm(armAsm, reg, reg, 1);
		a64_lsr_imm(armAsm, reg, reg, 31);
		mVUra_clearNeeded(mVU->regAlloc, reg);
	}
	pass3 {  }
	pass4 { mVUregs.needExactMatch |= 1; }
}

mVUop(mVU_FSSET)
{
	pass1 { mVUanalyzeFSSET(mVU); }
	pass2
	{
		int imm = 0;
		if (_Imm12_ & 0x0040) imm |= 0x000000f; /* ZS */
		if (_Imm12_ & 0x0080) imm |= 0x00000f0; /* SS */
		if (_Imm12_ & 0x0100) imm |= 0x0400000; /* US */
		if (_Imm12_ & 0x0200) imm |= 0x0800000; /* OS */
		if (_Imm12_ & 0x0400) imm |= 0x1000000; /* IS */
		if (_Imm12_ & 0x0800) imm |= 0x2000000; /* DS */
		if (!(sFLAG.doFlag || mVUinfo.doDivFlag))
		{
			mVUallocSFLAGa(getFlagReg(sFLAG.write), sFLAG.lastWrite); /* Get Prev Status Flag */
		}
		a64_and_imm(armAsm, getFlagReg(sFLAG.write), getFlagReg(sFLAG.write), 0xfff00); /* Keep Non-Sticky Bits */
		if (imm)
			a64_orr_imm(armAsm, getFlagReg(sFLAG.write), getFlagReg(sFLAG.write), imm);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * IADD/IADDI/IADDIU/IAND/IOR/ISUB/ISUBIU
 * ------------------------------------------------------------------ */

mVUop(mVU_IADD)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_Is_ == 0 || _It_ == 0)
		{
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_ ? _Is_ : _It_, -1, false, false);
			const a64r regD = mVUra_allocGPR(mVU->regAlloc, -1, _Id_, mVUlow.backupVI, false);
			a64_mov(armAsm, a64_asw(regD), a64_asw(regS));
			mVUra_clearNeeded(mVU->regAlloc, regD);
			mVUra_clearNeeded(mVU->regAlloc, regS);
		}
		else
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, false);
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _Id_, mVUlow.backupVI, false);
			a64_add(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(regT));
			mVUra_clearNeeded(mVU->regAlloc, regS);
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
	}
	pass3 {  }
}

mVUop(mVU_IADDI)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm5_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					a64_mov_imm(armAsm, a64_asw(regT), _Imm5_);
				else
					a64_mov(armAsm, a64_asw(regT), A64_WZR);
			}
			else
			{
				mvuLdr32(mVU, regT, &curI);
				a64_lsl_imm(armAsm, a64_asw(regT), a64_asw(regT), 21);
				a64_asr_imm(armAsm, a64_asw(regT), a64_asw(regT), 27);
			}
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
		else
		{
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _It_, mVUlow.backupVI, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm5_ != 0)
					a64_add_imm(armAsm, a64_asw(regS), a64_asw(regS), _Imm5_);
			}
			else
			{
				mvuLdr32(mVU, gprT1, &curI);
				a64_lsl_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 21);
				a64_asr_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 27);
				a64_add(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(gprT1));
			}
			mVUra_clearNeeded(mVU->regAlloc, regS);
		}
	}
	pass3 {  }
}

mVUop(mVU_IADDIU)
{
	pass1 { mVUanalyzeIADDI(mVU, _Is_, _It_, _Imm15_); }
	pass2
	{
		if (_Is_ == 0)
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					a64_mov_imm(armAsm, a64_asw(regT), _Imm15_);
				else
					a64_mov(armAsm, a64_asw(regT), A64_WZR);
			}
			else
			{
				mvuLdr32(mVU, regT, &curI);
				a64_mov(armAsm, a64_asw(gprT1), a64_asw(regT));
				a64_lsr_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 10);
				a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x7800);
				a64_and_imm(armAsm, a64_asw(regT), a64_asw(regT), 0x7FF);
				a64_orr(armAsm, a64_asw(regT), a64_asw(regT), a64_asw(gprT1));
			}
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
		else
		{
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _It_, mVUlow.backupVI, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm15_ != 0)
					a64_add_imm(armAsm, a64_asw(regS), a64_asw(regS), _Imm15_);
			}
			else
			{
				mvuLdr32(mVU, gprT1, &curI);
				a64_mov(armAsm, a64_asw(gprT2), a64_asw(gprT1));
				a64_lsr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 10);
				a64_and_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 0x7800);
				a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x7FF);
				a64_orr(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
				a64_add(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(gprT1));
			}
			mVUra_clearNeeded(mVU->regAlloc, regS);
		}
	}
	pass3 {  }
}

mVUop(mVU_IAND)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, false);
		const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _Id_, mVUlow.backupVI, false);
		if (_It_ != _Is_)
			a64_and(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(regT));
		mVUra_clearNeeded(mVU->regAlloc, regS);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
}

mVUop(mVU_IOR)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, false);
		const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _Id_, mVUlow.backupVI, false);
		if (_It_ != _Is_)
			a64_orr(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(regT));
		mVUra_clearNeeded(mVU->regAlloc, regS);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
}

mVUop(mVU_ISUB)
{
	pass1 { mVUanalyzeIALU1(mVU, _Id_, _Is_, _It_); }
	pass2
	{
		if (_It_ != _Is_)
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, false);
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _Id_, mVUlow.backupVI, false);
			a64_sub(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(regT));
			mVUra_clearNeeded(mVU->regAlloc, regS);
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
		else
		{
			const a64r regD = mVUra_allocGPR(mVU->regAlloc, -1, _Id_, mVUlow.backupVI, false);
			a64_mov(armAsm, a64_asw(regD), A64_WZR);
			mVUra_clearNeeded(mVU->regAlloc, regD);
		}
	}
	pass3 {  }
}

mVUop(mVU_ISUBIU)
{
	pass1 { mVUanalyzeIALU2(mVU, _Is_, _It_); }
	pass2
	{
		const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _It_, mVUlow.backupVI, false);
		if (!EmuConfig.Gamefixes.IbitHack)
		{
			if (_Imm15_ != 0)
				a64_sub_imm(armAsm, a64_asw(regS), a64_asw(regS), _Imm15_);
		}
		else
		{
			mvuLdr32(mVU, gprT1, &curI);
			a64_mov(armAsm, a64_asw(gprT2), a64_asw(gprT1));
			a64_lsr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 10);
			a64_and_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 0x7800);
			a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x7FF);
			a64_orr(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
			a64_sub(armAsm, a64_asw(regS), a64_asw(regS), a64_asw(gprT1));
		}
		mVUra_clearNeeded(mVU->regAlloc, regS);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * MFIR/MFP/MOVE/MR32/MTIR
 * ------------------------------------------------------------------ */

mVUop(mVU_MFIR)
{
	pass1
	{
		if (!_Ft_)
		{
			mVUlow.isNOP = true;
		}
		analyzeVIreg1(mVU, _Is_, &mVUlow.VI_read[0]);
		analyzeReg2  (mVU, _Ft_, &mVUlow.VF_write, 1);
	}
	pass2
	{
		const a64r Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
		if (_Is_ != 0)
		{
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, -1, false, false);
			a64_sxth(armAsm, a64_asw(regS), a64_asw(regS));
			/* TODO: Broadcast instead */
			a64_fmov(armAsm, a64_as(Ft, A64K_S), a64_asw(regS));
			if (!_XYZW_SS)
				mVUunpack_xyzw(Ft, Ft, 0);
			mVUra_clearNeeded(mVU->regAlloc, regS);
		}
		else
		{
			a64_eor(armAsm, a64_as(Ft, A64K_16B), a64_as(Ft, A64K_16B), a64_as(Ft, A64K_16B));
		}
		mVUra_clearNeeded(mVU->regAlloc, Ft);
	}
	pass3 {  }
}

mVUop(mVU_MFP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeMFP(mVU, _Ft_);
	}
	pass2
	{
		const a64r Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
		getPreg(mVU, Ft);
		mVUra_clearNeeded(mVU->regAlloc, Ft);
	}
	pass3 {  }
}

mVUop(mVU_MOVE)
{
	pass1 { mVUanalyzeMOVE(mVU, _Fs_, _Ft_); }
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _Ft_, _X_Y_Z_W, true);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_MR32)
{
	pass1 { mVUanalyzeMR32(mVU, _Fs_, _Ft_); }
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, -1, 0, true);
		const a64r Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
		if (_XYZW_SS)
			mVUunpack_xyzw(Ft, Fs, (_X ? 1 : (_Y ? 2 : (_Z ? 3 : 0))));
		else
			mVUshufflePS(Ft, Fs, 0x39);
		mVUra_clearNeeded(mVU->regAlloc, Ft);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_MTIR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeReg5(mVU, _Fs_, _Fsf_, &mVUlow.VF_read[0]);
		analyzeVIreg2(mVU, _It_, &mVUlow.VI_write, 1);
	}
	pass2
	{
		const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
		const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		a64_fmov(armAsm, a64_asw(regT), a64_as(Fs, A64K_S));
		mVUra_clearNeeded(mVU->regAlloc, regT);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * ILW/ILWR
 * ------------------------------------------------------------------ */

mVUop(mVU_ILW)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, &mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, &mVUlow.VI_write, 4);
	}
	pass2
	{
		a64r regT;
		void* ptr = (void*)(vuRegs[mVU->index].Mem + offsetSS);
		const void* optaddr = EmuConfig.Gamefixes.IbitHack ? NULL : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, offsetSS);
		if (!(optaddr != NULL))
		{
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), _Imm11_);
			}
			else
			{
				mvuLdr32(mVU, gprT2, &curI);
				a64_lsl_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_asr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_add(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		if ((optaddr != NULL))
		{
			mvuLdrhZ(mVU, regT, optaddr);
		}
		else
		{
			mvuComplexAddr(gprT2q, ptr, gprT1q);
			a64_ldrh(armAsm, a64_asw(regT), a64_mem_off(gprT2q, 0));
		}
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
}

mVUop(mVU_ILWR)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg1(mVU, _Is_, &mVUlow.VI_read[0]);
		analyzeVIreg2(mVU, _It_, &mVUlow.VI_write, 4);
	}
	pass2
	{
		void* ptr = (void*)(vuRegs[mVU->index].Mem + offsetSS);
		if (_Is_)
		{
			a64r regT;
			const void* optaddr = EmuConfig.Gamefixes.IbitHack ? NULL : mVUoptimizeConstantAddr(mVU, _Is_, 0, offsetSS);
			if (!(optaddr != NULL))
			{
				mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
				mVUaddrFix(mVU, gprT1q, gprT2q);
			}

			regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			if ((optaddr != NULL))
			{
				armMoveAddressToReg(gprT2q, optaddr);
			}
			else
			{
				mvuComplexAddr(gprT2q, ptr, gprT1q);
			}
			a64_ldrh(armAsm, a64_asw(regT), a64_mem_off(gprT2q, 0));
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
		else
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			mvuLdrhZ(mVU, regT, ptr);
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * ISW/ISWR
 * ------------------------------------------------------------------ */

mVUop(mVU_ISW)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, &mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, &mVUlow.VI_read[1]);
	}
	pass2
	{
		a64r regT;
		a64r base;
		const void* optaddr = EmuConfig.Gamefixes.IbitHack ? NULL : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0);
		if (!(optaddr != NULL))
		{
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), _Imm11_);
			}
			else
			{
				mvuLdr32(mVU, gprT2, &curI);
				a64_lsl_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_asr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_add(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		/* If regT is dirty, the high bits might not be zero. */
		regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, true);
		base = gprT2q;
		if ((optaddr != NULL))
			armMoveAddressToReg(base, optaddr);
		else
			mvuComplexAddr(base, vuRegs[mVU->index].Mem, gprT1q);
		if (_X) a64_str(armAsm, a64_asw(regT), a64_mem_off(base, 0));
		if (_Y) a64_str(armAsm, a64_asw(regT), a64_mem_off(base, 4));
		if (_Z) a64_str(armAsm, a64_asw(regT), a64_mem_off(base, 8));
		if (_W) a64_str(armAsm, a64_asw(regT), a64_mem_off(base, 12));
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
}

mVUop(mVU_ISWR)
{
	pass1
	{
		mVUlow.isMemWrite = true;
		analyzeVIreg1(mVU, _Is_, &mVUlow.VI_read[0]);
		analyzeVIreg1(mVU, _It_, &mVUlow.VI_read[1]);
	}
	pass2
	{
		bool hasIs = false;
		a64r regT;
		a64r baseR;
		void* base = (void*)vuRegs[mVU->index].Mem;
		if (_Is_)
		{
			const void* optaddr = EmuConfig.Gamefixes.IbitHack ? NULL : mVUoptimizeConstantAddr(mVU, _Is_, 0, 0);
			if ((optaddr != NULL))
			{
				/* The folded address becomes the store base; the lane
				 * stores below run unchanged on the absolute form. */
				base = (void*)optaddr;
			}
			else
			{
				mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
				mVUaddrFix(mVU, gprT1q, gprT2q);
				hasIs = true;
			}
		}
		regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, true);
		baseR = gprT2q;
		armMoveAddressToReg(baseR, base);
		if (hasIs)
			a64_add(armAsm, baseR, baseR, gprT1q);
		if (_X) a64_str(armAsm, a64_asw(regT), a64_mem_off(baseR, 0));
		if (_Y) a64_str(armAsm, a64_asw(regT), a64_mem_off(baseR, 4));
		if (_Z) a64_str(armAsm, a64_asw(regT), a64_mem_off(baseR, 8));
		if (_W) a64_str(armAsm, a64_asw(regT), a64_mem_off(baseR, 12));
		mVUra_clearNeeded(mVU->regAlloc, regT);

	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * LQ/LQD/LQI
 * ------------------------------------------------------------------ */

mVUop(mVU_LQ)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, false); }
	pass2
	{
		a64r Ft;
		a64r base;
		const void* optaddr = EmuConfig.Gamefixes.IbitHack ? NULL : mVUoptimizeConstantAddr(mVU, _Is_, _Imm11_, 0);
		if (!(optaddr != NULL))
		{
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), _Imm11_);
			}
			else
			{
				mvuLdr32(mVU, gprT2, &curI);
				a64_lsl_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_asr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_add(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
		base = gprT2q;
		if ((optaddr != NULL))
			armMoveAddressToReg(base, optaddr);
		else
			mvuComplexAddr(base, vuRegs[mVU->index].Mem, gprT1q);
		mvuLoadRegBase(Ft, base, _X_Y_Z_W);
		mVUra_clearNeeded(mVU->regAlloc, Ft);
	}
	pass3 {  }
}

mVUop(mVU_LQD)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		bool hasIs = false;
		void* ptr = (void*)vuRegs[mVU->index].Mem;
		if (_Is_ || isVU0) /* Access vuRegs[1] regs mem-map in !_Is_ case */
		{
			const a64r regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _Is_, mVUlow.backupVI, false);
			a64_sub_imm(armAsm, a64_asw(regS), a64_asw(regS), 1);
			a64_sxth(armAsm, a64_asw(gprT1), a64_asw(regS)); /* TODO: Confirm */
			mVUra_clearNeeded(mVU->regAlloc, regS);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			hasIs = true;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU->microMemSize - 8)));
		}
		if (!mVUlow.noWriteVF)
		{
			const a64r Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
			const a64r base = gprT2q;
			if (!hasIs)
				armMoveAddressToReg(base, ptr);
			else
				mvuComplexAddr(base, ptr, gprT1q);
			mvuLoadRegBase(Ft, base, _X_Y_Z_W);
			mVUra_clearNeeded(mVU->regAlloc, Ft);
		}
	}
	pass3 {  }
}

mVUop(mVU_LQI)
{
	pass1 { mVUanalyzeLQ(mVU, _Ft_, _Is_, true); }
	pass2
	{
		bool hasIs = false;
		void* ptr = (void*)vuRegs[mVU->index].Mem;
		const void* optaddr = NULL;
		if (_Is_)
		{
			a64r regS;
			/* A const-known address register folds the load to an
			 * absolute access; the post-increment still runs. */
			if (!EmuConfig.Gamefixes.IbitHack)
				optaddr = mVUoptimizeConstantAddr(mVU, _Is_, 0, 0);
			regS = mVUra_allocGPR(mVU->regAlloc, _Is_, _Is_, mVUlow.backupVI, false);
			if (!(optaddr != NULL))
				a64_sxth(armAsm, a64_asw(gprT1), a64_asw(regS)); /* TODO: Confirm */
			a64_add_imm(armAsm, a64_asw(regS), a64_asw(regS), 1);
			mVUra_clearNeeded(mVU->regAlloc, regS);
			if (!(optaddr != NULL))
			{
				mVUaddrFix(mVU, gprT1q, gprT2q);
				hasIs = true;
			}
		}
		if (!mVUlow.noWriteVF)
		{
			const a64r Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
			const a64r base = gprT2q;
			if ((optaddr != NULL))
				armMoveAddressToReg(base, optaddr);
			else if (!hasIs)
				armMoveAddressToReg(base, ptr);
			else
				mvuComplexAddr(base, ptr, gprT1q);
			mvuLoadRegBase(Ft, base, _X_Y_Z_W);
			mVUra_clearNeeded(mVU->regAlloc, Ft);
		}
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * SQ/SQD/SQI
 * ------------------------------------------------------------------ */

mVUop(mVU_SQ)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, false); }
	pass2
	{
		a64r Fs;
		a64r base;
		const void* optptr = EmuConfig.Gamefixes.IbitHack ? NULL : mVUoptimizeConstantAddr(mVU, _It_, _Imm11_, 0);
		if (!(optptr != NULL))
		{
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _It_, false);
			if (!EmuConfig.Gamefixes.IbitHack)
			{
				if (_Imm11_ != 0)
					a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), _Imm11_);
			}
			else
			{
				mvuLdr32(mVU, gprT2, &curI);
				a64_lsl_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_asr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 21);
				a64_add(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
			}
			mVUaddrFix(mVU, gprT1q, gprT2q);
		}

		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W, true);
		base = gprT2q;
		if ((optptr != NULL))
			armMoveAddressToReg(base, optptr);
		else
			mvuComplexAddr(base, vuRegs[mVU->index].Mem, gprT1q);
		mvuSaveRegBase(Fs, base, _X_Y_Z_W, 1);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_SQD)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		bool hasIt = false;
		a64r Fs;
		a64r base;
		void* ptr = (void*)vuRegs[mVU->index].Mem;
		if (_It_ || isVU0) /* Access vuRegs[1] regs mem-map in !_It_ case */
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, _It_, mVUlow.backupVI, false);
			a64_sub_imm(armAsm, a64_asw(regT), a64_asw(regT), 1);
			a64_uxth(armAsm, a64_asw(gprT1), a64_asw(regT));
			mVUra_clearNeeded(mVU->regAlloc, regT);
			mVUaddrFix(mVU, gprT1q, gprT2q);
			hasIt = true;
		}
		else
		{
			ptr = (void*)((sptr)ptr + (0xffff & (mVU->microMemSize - 8)));
		}
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W, true);
		base = gprT2q;
		if (!hasIt)
			armMoveAddressToReg(base, ptr);
		else
			mvuComplexAddr(base, ptr, gprT1q);
		mvuSaveRegBase(Fs, base, _X_Y_Z_W, 1);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

mVUop(mVU_SQI)
{
	pass1 { mVUanalyzeSQ(mVU, _Fs_, _It_, true); }
	pass2
	{
		a64r Fs;
		a64r base;
		void* ptr = (void*)vuRegs[mVU->index].Mem;
		const void* optaddr = NULL;
		if (_It_)
		{
			a64r regT;
			if (!EmuConfig.Gamefixes.IbitHack)
				optaddr = mVUoptimizeConstantAddr(mVU, _It_, 0, 0);
			regT = mVUra_allocGPR(mVU->regAlloc, _It_, _It_, mVUlow.backupVI, false);
			if (!(optaddr != NULL))
				a64_uxth(armAsm, a64_asw(gprT1), a64_asw(regT));
			a64_add_imm(armAsm, a64_asw(regT), a64_asw(regT), 1);
			mVUra_clearNeeded(mVU->regAlloc, regT);
			if (!(optaddr != NULL))
				mVUaddrFix(mVU, gprT1q, gprT2q);
		}
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _XYZW_PS ? -1 : 0, _X_Y_Z_W, true);
		base = gprT2q;
		if ((optaddr != NULL))
			armMoveAddressToReg(base, optaddr);
		else if (_It_)
			mvuComplexAddr(base, ptr, gprT1q);
		else
			armMoveAddressToReg(base, ptr);
		mvuSaveRegBase(Fs, base, _X_Y_Z_W, 1);
		mVUra_clearNeeded(mVU->regAlloc, Fs);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * RINIT/RGET/RNEXT/RXOR
 * ------------------------------------------------------------------ */

mVUop(mVU_RINIT)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
			a64_fmov(armAsm, a64_asw(gprT1), a64_as(Fs, A64K_S));
			a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x007fffff);
			a64_orr_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x3f800000);
			mvuStr32(mVU, Rmem, gprT1);
			mVUra_clearNeeded(mVU->regAlloc, Fs);
		}
		else
		{
			mvuStrImm32(mVU, Rmem, 0x3f800000, gprT1);
		}
	}
	pass3 {  }
}

static __fi void mVU_RGET_(mV, a64r Rreg)
{
	if (!mVUlow.noWriteVF)
	{
		const a64r Ft = mVUra_allocReg(mVU->regAlloc, -1, _Ft_, _X_Y_Z_W, true);
		a64_fmov(armAsm, a64_as(Ft, A64K_S), a64_asw(Rreg));
		if (!_XYZW_SS)
			mVUunpack_xyzw(Ft, Ft, 0);
		mVUra_clearNeeded(mVU->regAlloc, Ft);
	}
}

mVUop(mVU_RGET)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, true); }
	pass2
	{
		mvuLdr32(mVU, gprT1, Rmem);
		mVU_RGET_(mVU, gprT1);
	}
	pass3 {  }
}

mVUop(mVU_RNEXT)
{
	pass1 { mVUanalyzeR2(mVU, _Ft_, false); }
	pass2
	{
		/* algorithm from www.project-fao.org */
		const a64r temp3 = mVUra_allocGPR(mVU->regAlloc, -1, -1, false, false);
		mvuLdr32(mVU, temp3, Rmem);
		a64_mov(armAsm, a64_asw(gprT1), a64_asw(temp3));
		a64_lsr_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 4);
		a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 1);

		a64_mov(armAsm, a64_asw(gprT2), a64_asw(temp3));
		a64_lsr_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 22);
		a64_and_imm(armAsm, a64_asw(gprT2), a64_asw(gprT2), 1);

		a64_lsl_imm(armAsm, a64_asw(temp3), a64_asw(temp3), 1);
		a64_eor(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
		a64_eor(armAsm, a64_asw(temp3), a64_asw(temp3), a64_asw(gprT1));
		a64_and_imm(armAsm, a64_asw(temp3), a64_asw(temp3), 0x007fffff);
		a64_orr_imm(armAsm, a64_asw(temp3), a64_asw(temp3), 0x3f800000);
		mvuStr32(mVU, Rmem, temp3);
		mVU_RGET_(mVU, temp3);
		mVUra_clearNeeded(mVU->regAlloc, temp3);
	}
	pass3 {  }
}

mVUop(mVU_RXOR)
{
	pass1 { mVUanalyzeR1(mVU, _Fs_, _Fsf_); }
	pass2
	{
		if (_Fs_ || (_Fsf_ == 3))
		{
			const a64r Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, (1 << (3 - _Fsf_)), true);
			a64_fmov(armAsm, a64_asw(gprT1), a64_as(Fs, A64K_S));
			a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x7fffff);
			mvuLdr32(mVU, gprT2, Rmem);
			a64_eor(armAsm, a64_asw(gprT2), a64_asw(gprT2), a64_asw(gprT1));
			mvuStr32(mVU, Rmem, gprT2);
			mVUra_clearNeeded(mVU->regAlloc, Fs);
		}
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * WaitP/WaitQ
 * ------------------------------------------------------------------ */

mVUop(mVU_WAITP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUstall = mVUmax(mVUstall, (u8)((mVUregs.p) ? (mVUregs.p - 1) : 0));
	}
}

mVUop(mVU_WAITQ)
{
	pass1 { mVUstall = mVUmax(mVUstall, mVUregs.q); }
}

/* ------------------------------------------------------------------
 * XTOP/XITOP
 * ------------------------------------------------------------------ */

mVUop(mVU_XTOP)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}

		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, &mVUlow.VI_write, 1);
	}
	pass2
	{
		const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		mvuLdrhZ(mVU, regT, &(*mVUvifRegs(mVU)).top);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
}

mVUop(mVU_XITOP)
{
	pass1
	{
		if (!_It_)
			mVUlow.isNOP = true;

		analyzeVIreg2(mVU, _It_, &mVUlow.VI_write, 1);
	}
	pass2
	{
		const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
		mvuLdrhZ(mVU, regT, &(*mVUvifRegs(mVU)).itop);
		a64_and_imm(armAsm, a64_asw(regT), a64_asw(regT), isVU1 ? 0x3ff : 0xff);
		mVUra_clearNeeded(mVU->regAlloc, regT);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * XGkick
 * ------------------------------------------------------------------ */

static void mVU_XGKICK_(u32 addr)
{
	u32 diff;
	u32 size;
	addr = (addr & 0x3ff) * 16;
	diff = 0x4000 - addr;
	size = gif_unit_get_packet_size(GIF_PATH_1, vuRegs[1].Mem, addr, ~0u, true);

	if (size > diff)
	{
		gif_path_copy(&gifUnit.gifPath[GIF_PATH_1], &vuRegs[1].Mem[addr], diff);
		gif_unit_transfer(GIF_TRANS_XGKICK, &vuRegs[1].Mem[0], size - diff);
	}
	else
	{
		gif_unit_transfer(GIF_TRANS_XGKICK, &vuRegs[1].Mem[addr], size);
	}
}

static void _vuXGKICKTransfermVU(bool flush)
{
	while (vuRegs[1].xgkickenable && (flush || vuRegs[1].xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;

		if (vuRegs[1].xgkicksizeremaining == 0)
		{
			u32 size = gif_unit_get_packet_size(GIF_PATH_1, vuRegs[1].Mem, vuRegs[1].xgkickaddr, ~0u, flush);
			vuRegs[1].xgkicksizeremaining = size & 0xFFFF;
			vuRegs[1].xgkickendpacket = size >> 31;
			vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

			if (vuRegs[1].xgkicksizeremaining == 0)
			{
				vuRegs[1].xgkickenable = false;
				break;
			}
		}

		if (!flush)
		{
			transfersize = mVUmin(vuRegs[1].xgkicksizeremaining, vuRegs[1].xgkickcyclecount * 8);
			transfersize = mVUmin(transfersize, vuRegs[1].xgkickdiff);
		}
		else
		{
			transfersize = vuRegs[1].xgkicksizeremaining;
			transfersize = mVUmin(transfersize, vuRegs[1].xgkickdiff);
		}

		/* Would be "nicer" to do the copy until it's all up, however this really screws up PATH3 masking stuff
		 * So lets just do it the other way :) */
		if (THREAD_VU1)
		{
			if (transfersize < vuRegs[1].xgkicksizeremaining)
				gif_path_copy(&gifUnit.gifPath[GIF_PATH_1], &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize);
			else
				gif_unit_transfer(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize);
		}
		else
		{
			gif_unit_transfer(GIF_TRANS_XGKICK, &vuRegs[1].Mem[vuRegs[1].xgkickaddr], transfersize);
		}

		if (flush)
			vuRegs[1].cycle += transfersize / 8;

		vuRegs[1].xgkickcyclecount -= transfersize / 8;

		vuRegs[1].xgkickaddr = (vuRegs[1].xgkickaddr + transfersize) & 0x3FFF;
		vuRegs[1].xgkicksizeremaining -= transfersize;
		vuRegs[1].xgkickdiff = 0x4000 - vuRegs[1].xgkickaddr;

		if (vuRegs[1].xgkickendpacket && !vuRegs[1].xgkicksizeremaining)
		{
			vuRegs[1].xgkickenable = false;
		}
	}
}

static __fi void mVU_XGKICK_SYNC(mV, bool flush)
{
	a64_label skipxgkick = A64_LABEL_INIT;
	a64_label needcycles = A64_LABEL_INIT;
	mVUra_flushCallerSavedRegisters(mVU->regAlloc, false);

	/* Add the single cycle remainder after this instruction, some games do the store
	 * on the second instruction after the kick and that needs to go through first
	 * but that's VERY close.. */
	mvuLdr32(mVU, gprT1, &vuRegs[1].xgkickenable);
	a64_tst_imm(armAsm, a64_asw(gprT1), 0x1);
	a64_b_cond(armAsm, &skipxgkick, A64_EQ);

	mvuLdr32(mVU, gprT1, &vuRegs[1].xgkickcyclecount);
	a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), mVUlow.kickcycles - 1);
	mvuStr32(mVU, &vuRegs[1].xgkickcyclecount, gprT1);
	a64_cmp_imm(armAsm, a64_asw(gprT1), 2);
	a64_b_cond(armAsm, &needcycles, A64_LT);
	mVUbackupRegs(mVU, true, true);
	a64_mov_imm(armAsm, a64_asw(RWARG1), flush ? 1 : 0);
	armEmitCall(((const void*)(&_vuXGKICKTransfermVU)));
	mVUrestoreRegs(mVU, true, true);
	a64_bind(armAsm, &needcycles);
	mvuLdr32(mVU, gprT1, &vuRegs[1].xgkickcyclecount);
	a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 1);
	mvuStr32(mVU, &vuRegs[1].xgkickcyclecount, gprT1);
	a64_bind(armAsm, &skipxgkick);
}

static __fi void mVU_XGKICK_DELAY(mV)
{
	mVUra_flushCallerSavedRegisters(mVU->regAlloc, false);

	mVUbackupRegs(mVU, true, true);
	mvuLdr32(mVU, RWARG1, &mVU->VIxgkick);
	armEmitCall(((const void*)(&mVU_XGKICK_)));
	mVUrestoreRegs(mVU, true, true);
}

mVUop(mVU_XGKICK)
{
	pass1
	{
		if (isVU0)
		{
			mVUlow.isNOP = true;
			return;
		}
		mVUanalyzeXGkick(mVU, _Is_, 1);
	}
	pass2
	{
		a64r regS;
		if (CHECK_XGKICKHACK)
		{
			mVUlow.kickcycles = 99;
			mVU_XGKICK_SYNC(mVU, true);
			mVUlow.kickcycles = 0;
		}
		if (mVUinfo.doXGKICK) /* check for XGkick Transfer */
		{
			mVU_XGKICK_DELAY(mVU);
			mVUinfo.doXGKICK = false;
		}

		regS = mVUra_allocGPR(mVU->regAlloc, _Is_, -1, false, false);
		if (!CHECK_XGKICKHACK)
		{
			mvuStr32(mVU, &mVU->VIxgkick, regS);
		}
		else
		{
			mvuStrImm32(mVU, &vuRegs[1].xgkickenable, 1, gprT1);
			mvuStrImm32(mVU, &vuRegs[1].xgkickendpacket, 0, gprT1);
			mvuStrImm32(mVU, &vuRegs[1].xgkicksizeremaining, 0, gprT1);
			mvuStrImm32(mVU, &vuRegs[1].xgkickcyclecount, 0, gprT1);
			mvuLdr32(mVU, gprT2, &mVU->totalCycles);
			mvuLdr32(mVU, gprT1, &mVU->cycles);
			a64_sub(armAsm, a64_asw(gprT2), a64_asw(gprT2), a64_asw(gprT1));
			armMoveAddressToReg(RSCRATCHADDR, &vuRegs[1].cycle);
			a64_ldr(armAsm, gprT1q, a64_mem_off(RSCRATCHADDR, 0));
			a64_add(armAsm, gprT2q, gprT2q, gprT1q);
			mvuStr32(mVU, &vuRegs[1].xgkicklastcycle, gprT2);
			a64_mov(armAsm, a64_asw(gprT1), a64_asw(regS));
			a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 0x3FF);
			a64_lsl_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 4);
			mvuStr32(mVU, &vuRegs[1].xgkickaddr, gprT1);
		}
		mVUra_clearNeeded(mVU->regAlloc, regS);
	}
	pass3 {  }
}

/* ------------------------------------------------------------------
 * Branches/Jumps
 * ------------------------------------------------------------------
 * setBranchA (the branch-attribute setup) + mVU_B / mVU_BAL live in aVU_Tables.inl
 * (ported with the table big-bang). The conditional/jump *drivers*
 * (normBranch/normJump/condBranch) are in aVU_Branch.inl; the op handlers below
 * just record the comparison value / target into the mVU branch fields, which the
 * drivers (invoked by mVUcompile) consume. */

static void condEvilBranch(mV, unsigned JMPcc)
{
	if (mVUlow.badBranch)
	{
		a64_label cJMP = A64_LABEL_INIT;
		mvuStr32(mVU, &mVU->branch, gprT1);
		mvuStrImm32(mVU, &mVU->badBranch, branchAddr(mVU), gprT2);

		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);
		a64_b_cond(armAsm, &cJMP, JMPcc);
			incPC(4); /* Branch Not Taken Addr */
			mvuStrImm32(mVU, &mVU->badBranch, xPC, gprT1);
			incPC(-4);
		a64_bind(armAsm, &cJMP);
		return;
	}
	if (isEvilBlock)
	{
		a64_label cJMP = A64_LABEL_INIT;
		mvuStrImm32(mVU, &mVU->evilevilBranch, branchAddr(mVU), gprT2);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);
		a64_b_cond(armAsm, &cJMP, JMPcc);
		mvuLdr32(mVU, gprT1, &mVU->evilBranch); /* Branch Not Taken */
		a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 8); /* We have already executed 1 instruction from the original branch */
		mvuStr32(mVU, &mVU->evilevilBranch, gprT1);
		a64_bind(armAsm, &cJMP);
	}
	else
	{
		a64_label cJMP = A64_LABEL_INIT;
		mvuStrImm32(mVU, &mVU->evilBranch, branchAddr(mVU), gprT2);
		a64_cmp_imm(armAsm, a64_asw(gprT1), 0);
		a64_b_cond(armAsm, &cJMP, JMPcc);
		mvuLdr32(mVU, gprT1, &mVU->badBranch); /* Branch Not Taken */
		a64_add_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 8); /* We have already executed 1 instruction from the original branch */
		mvuStr32(mVU, &mVU->evilBranch, gprT1);
		a64_bind(armAsm, &cJMP);
		incPC(-2);
		if (mVUlow.branch >= 9)
			log_cb(RETRO_LOG_WARN, "Conditional in JALR/JR delay slot - If game broken report to PCSX2 Team\n");
		incPC(2);
	}
}

mVUop(mVU_B)
{
	setBranchA(mX, 1, 0);
	pass1 { mVUanalyzeNormBranch(mVU, 0, false); }
	pass2
	{
		if (mVUlow.badBranch)  { mvuStrImm32(mVU, &mVU->badBranch, branchAddr(mVU), gprT1); }
		if (mVUlow.evilBranch) { if (isEvilBlock) mvuStrImm32(mVU, &mVU->evilevilBranch, branchAddr(mVU), gprT1); else mvuStrImm32(mVU, &mVU->evilBranch, branchAddr(mVU), gprT1); }
	}
	pass3 {  }
}

mVUop(mVU_BAL)
{
	setBranchA(mX, 2, _It_);
	pass1 { mVUanalyzeNormBranch(mVU, _It_, true); }
	pass2
	{
		if (!mVUlow.evilBranch)
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			a64_mov_imm(armAsm, a64_asw(regT), bSaveAddr);
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
		else
		{
			a64r regT;
			incPC(-2);
			log_cb(RETRO_LOG_WARN, "Linking BAL from %s branch taken/not taken target! - If game broken report to PCSX2 Team\n", branchSTR[mVUlow.branch & 0xf]);
			incPC(2);

			regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			if (isEvilBlock)
				mvuLdr32(mVU, regT, &mVU->evilBranch);
			else
				mvuLdr32(mVU, regT, &mVU->badBranch);

			a64_add_imm(armAsm, a64_asw(regT), a64_asw(regT), 8);
			a64_lsr_imm(armAsm, a64_asw(regT), a64_asw(regT), 3);
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}

		if (mVUlow.badBranch)  { mvuStrImm32(mVU, &mVU->badBranch, branchAddr(mVU), gprT1); }
		if (mVUlow.evilBranch) { if (isEvilBlock) mvuStrImm32(mVU, &mVU->evilevilBranch, branchAddr(mVU), gprT1); else mvuStrImm32(mVU, &mVU->evilBranch, branchAddr(mVU), gprT1); }
	}
	pass3 {  }
}

mVUop(mVU_IBEQ)
{
	setBranchA(mX, 3, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			mvuLdr32(mVU, gprT1, &mVU->VIbackup);
		else
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);

		if (mVUlow.memReadIt)
		{
			mvuLdr32(mVU, gprT2, &mVU->VIbackup);
			a64_eor(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
		}
		else
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, false);
			a64_eor(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(regT));
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}

		if (!(isBadOrEvil))
			mvuStr32(mVU, &mVU->branch, gprT1);
		else
			condEvilBranch(mVU, A64_EQ);
	}
	pass3 {  }
}

mVUop(mVU_IBGEZ)
{
	setBranchA(mX, 4, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			mvuLdr32(mVU, gprT1, &mVU->VIbackup);
		else
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
		if (!(isBadOrEvil))
			mvuStr32(mVU, &mVU->branch, gprT1);
		else
			condEvilBranch(mVU, A64_GE);
	}
	pass3 {  }
}

mVUop(mVU_IBGTZ)
{
	setBranchA(mX, 5, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			mvuLdr32(mVU, gprT1, &mVU->VIbackup);
		else
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
		if (!(isBadOrEvil))
			mvuStr32(mVU, &mVU->branch, gprT1);
		else
			condEvilBranch(mVU, A64_GT);
	}
	pass3 {  }
}

mVUop(mVU_IBLEZ)
{
	setBranchA(mX, 6, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			mvuLdr32(mVU, gprT1, &mVU->VIbackup);
		else
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
		if (!(isBadOrEvil))
			mvuStr32(mVU, &mVU->branch, gprT1);
		else
			condEvilBranch(mVU, A64_LE);
	}
	pass3 {  }
}

mVUop(mVU_IBLTZ)
{
	setBranchA(mX, 7, 0);
	pass1 { mVUanalyzeCondBranch1(mVU, _Is_); }
	pass2
	{
		if (mVUlow.memReadIs)
			mvuLdr32(mVU, gprT1, &mVU->VIbackup);
		else
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
		if (!(isBadOrEvil))
			mvuStr32(mVU, &mVU->branch, gprT1);
		else
			condEvilBranch(mVU, A64_LT);
	}
	pass3 {  }
}

mVUop(mVU_IBNE)
{
	setBranchA(mX, 8, 0);
	pass1 { mVUanalyzeCondBranch2(mVU, _Is_, _It_); }
	pass2
	{
		if (mVUlow.memReadIs)
			mvuLdr32(mVU, gprT1, &mVU->VIbackup);
		else
			mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);

		if (mVUlow.memReadIt)
		{
			mvuLdr32(mVU, gprT2, &mVU->VIbackup);
			a64_eor(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(gprT2));
		}
		else
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, _It_, -1, false, false);
			a64_eor(armAsm, a64_asw(gprT1), a64_asw(gprT1), a64_asw(regT));
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}

		if (!(isBadOrEvil))
			mvuStr32(mVU, &mVU->branch, gprT1);
		else
			condEvilBranch(mVU, A64_NE);
	}
	pass3 {  }
}

static void normJumpPass2(mV)
{
	if (!mVUlow.constJump.isValid || mVUlow.evilBranch)
	{
		mVUra_moveVIToGPR(mVU->regAlloc, gprT1, _Is_, false);
		a64_lsl_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), 3);
		a64_and_imm(armAsm, a64_asw(gprT1), a64_asw(gprT1), mVU->microMemSize - 8);

		if (!mVUlow.evilBranch)
		{
			mvuStr32(mVU, &mVU->branch, gprT1);
		}
		else
		{
			if (isEvilBlock)
				mvuStr32(mVU, &mVU->evilevilBranch, gprT1);
			else
				mvuStr32(mVU, &mVU->evilBranch, gprT1);
		}
		/* If delay slot is conditional, it uses badBranch to go to its target */
		if (mVUlow.badBranch)
		{
			mvuStr32(mVU, &mVU->badBranch, gprT1);
		}
	}
}

mVUop(mVU_JR)
{
	mVUbranch = 9;
	pass1 { mVUanalyzeJump(mVU, _Is_, 0, false); }
	pass2
	{
		normJumpPass2(mVU);
	}
	pass3 {  }
}

mVUop(mVU_JALR)
{
	mVUbranch = 10;
	pass1 { mVUanalyzeJump(mVU, _Is_, _It_, 1); }
	pass2
	{
		normJumpPass2(mVU);
		if (!mVUlow.evilBranch)
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			a64_mov_imm(armAsm, a64_asw(regT), bSaveAddr);
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}
		if (mVUlow.evilBranch)
		{
			const a64r regT = mVUra_allocGPR(mVU->regAlloc, -1, _It_, mVUlow.backupVI, false);
			if (isEvilBlock)
			{
				mvuLdr32(mVU, regT, &mVU->evilBranch);
				a64_add_imm(armAsm, a64_asw(regT), a64_asw(regT), 8);
				a64_lsr_imm(armAsm, a64_asw(regT), a64_asw(regT), 3);
			}
			else
			{
				incPC(-2);
				log_cb(RETRO_LOG_WARN, "Linking JALR from %s branch taken/not taken target! - If game broken report to PCSX2 Team\n", branchSTR[mVUlow.branch & 0xf]);
				incPC(2);

				mvuLdr32(mVU, regT, &mVU->badBranch);
				a64_add_imm(armAsm, a64_asw(regT), a64_asw(regT), 8);
				a64_lsr_imm(armAsm, a64_asw(regT), a64_asw(regT), 3);
			}
			mVUra_clearNeeded(mVU->regAlloc, regT);
		}

	}
	pass3 {  }
}

#undef gprT1q
#undef gprT2q
