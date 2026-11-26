/* SPDX-FileCopyrightText: 2026 isztld <https://isztld.com/>
 * SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
 * SPDX-License-Identifier: GPL-3.0+ */

#pragma once


/* ARM64 microVU — misc emit helpers (Phase 7, task 7.5). AArch64 port of the
 * emit-coupled tail of pcsx2/x86/microVU_Misc.inl.
 *
 * Already ported elsewhere:
 * * the arch-neutral reg shufflers (mVUunpack_xyzw/mVUloadReg/mVUsaveReg/
 * mVUmergeRegs) — aVU_IR.h (task 7.2b / 7.4 part 1b);
 * * the branch/T-Bit/E-Bit/waitMTVU C helpers — aVU.c.
 *
 * Deferred:
 * * mVUoptimizeConstantAddr — its return-type contract (how a constant host
 * address is handed to a load/store op) is defined by its only consumer, the
 * Lower load/store handlers in task 7.5b. */

/* Computes the destination PC (byte address) of a relative VU branch from the
 * current lower-op PC + the signed 11-bit immediate. x86: microVU_Misc.inl
 * branchAddr. Pure (no emit) — used by the branch op handlers (B/BAL/IBxx) and
 * the branch drivers in aVU_Branch.inl. */
static INLINE u32 branchAddr(const mV)
{
	pxAssumeMsg(islowerOP, "MicroVU: Expected Lower OP code for valid branch addr.");
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU->progMemMask) * 4);
}

/* ------------------------------------------------------------------
 * Small absolute-address memory-access helpers (shared by Tables/Flags/Branch)
 * ------------------------------------------------------------------
 * x86 folded an absolute &global into a ptr32/ptr128 memory operand; ARM64 must
 * first materialize the address. armMoveAddressToReg clobbers RSCRATCH (x16/x17),
 * so the value regs passed in must never be x16/x17 (callers use w9/w10 or the
 * flag GPRs / NEON scratch). */

static INLINE void mvuStr32(microVU* mVU, const void* addr, a64r wreg)
{
	a64_str(armAsm, a64_asw(wreg), mvuAbsMem(mVU, addr, 4));
}

static INLINE void mvuLdr32(microVU* mVU, a64r wreg, const void* addr)
{
	a64_ldr(armAsm, a64_asw(wreg), mvuAbsMem(mVU, addr, 4));
}

static INLINE void mvuStrImm32(microVU* mVU, const void* addr, u32 imm, a64r tmp)
{
	a64_mov_imm(armAsm, a64_asw(tmp), imm);
	mvuStr32(mVU, addr, tmp);
}

static INLINE void mvuStrSS(microVU* mVU, const void* addr, a64r vreg)
{
	a64_str(armAsm, a64_as(vreg, A64K_S), mvuAbsMem(mVU, addr, 4));
}

static INLINE void mvuLdrSS(microVU* mVU, a64r vreg, const void* addr)
{
	a64_ldr(armAsm, a64_as(vreg, A64K_S), mvuAbsMem(mVU, addr, 4));
}

static INLINE void mvuLdrQ(microVU* mVU, a64r vreg, const void* addr)
{
	a64_ldr(armAsm, a64_as(vreg, A64K_Q), mvuAbsMem(mVU, addr, 16));
}

static INLINE void mvuStrQ(microVU* mVU, const void* addr, a64r vreg)
{
	a64_str(armAsm, a64_as(vreg, A64K_Q), mvuAbsMem(mVU, addr, 16));
}

static INLINE void mvuMemAndImm32(microVU* mVU, const void* addr, u32 imm, a64r tmp)
{
	const a64_mem m = mvuAbsMem(mVU, addr, 4);
	a64_ldr(armAsm, a64_asw(tmp), m);
	a64_and_imm(armAsm, a64_asw(tmp), a64_asw(tmp), imm);
	a64_str(armAsm, a64_asw(tmp), m);
}

static INLINE void mvuMemOrImm32(microVU* mVU, const void* addr, u32 imm, a64r tmp)
{
	const a64_mem m = mvuAbsMem(mVU, addr, 4);
	a64_ldr(armAsm, a64_asw(tmp), m);
	a64_orr_imm(armAsm, a64_asw(tmp), a64_asw(tmp), imm);
	a64_str(armAsm, a64_asw(tmp), m);
}

/* ------------------------------------------------------------------
 * Volatile-register backup / restore around opaque C calls
 * ------------------------------------------------------------------
 * x86: microVU_Misc.inl mVUbackupRegs/mVUrestoreRegs.
 * * !toMemory (the common case, e.g. normJumpCompile): flush the reg cache and
 * stash only the PQ NEON reg in mVU->vecBackup — the dispatcher's XGKICK resume
 * path (mVUdispatcherCD) reloads it from there.
 * * toMemory (debug-only: handleBadOp / mVUdebugPrintBlocks / DumpVUState): the
 * regAlloc isn't told about the call, so push every caller-saved host reg that
 * can hold live VU state to the stack and pop it after. We save the full fixed
 * caller-saved set (GPR x0-x15, NEON v0-v24 = VF pool + PQ) rather than the x86
 * "onlyNeeded" subset — over-saving is correct and these paths are rare. The
 * frame layout here MUST match mVUrestoreRegs. */
#define kBakGprSave (16) /* x0..x15 */
#define kBakVecSave (25) /* v0..v24 (VF pool + PQ) */
#define kBakGprBytes (kBakGprSave * 8)
#define kBakFrame (kBakGprBytes + ((kBakVecSave * 16 + 15) & ~15))

static __fi void mVUbackupRegs(microVU* mVU, bool toMemory, bool onlyNeeded)
{
	(void)onlyNeeded;
	if (toMemory)
	{
		int i;
		int voff;
		a64_sub_imm(armAsm, A64_SP, A64_SP, kBakFrame);
		for (i = 0; i < kBakGprSave; i += 2)
			a64_stp(armAsm, armXRegister(i), armXRegister(i + 1), a64_mem_off(A64_SP, i * 8));
		voff = kBakGprBytes;
		for (i = 0; i < kBakVecSave - 1; i += 2, voff += 32)
			a64_stp(armAsm, armQRegister(i), armQRegister(i + 1), a64_mem_off(A64_SP, voff));
		a64_str(armAsm, armQRegister(kBakVecSave - 1), a64_mem_off(A64_SP, voff));
	}
	else
	{
		mVUra_flushAll(mVU->regAlloc, true); /* Flush Regalloc */
		a64_str(armAsm, a64_as(mVU_xmmPQ, A64K_Q), mvuAbsMem(mVU, &mVU->vecBackup[A64_CODE(mVU_xmmPQ)][0], 16));
	}
}

static __fi void mVUrestoreRegs(microVU* mVU, bool fromMemory, bool onlyNeeded)
{
	(void)onlyNeeded;
	if (fromMemory)
	{
		int i;
		int voff = kBakGprBytes;
		for (i = 0; i < kBakVecSave - 1; i += 2, voff += 32)
			a64_ldp(armAsm, armQRegister(i), armQRegister(i + 1), a64_mem_off(A64_SP, voff));
		a64_ldr(armAsm, armQRegister(kBakVecSave - 1), a64_mem_off(A64_SP, voff));
		for (i = 0; i < kBakGprSave; i += 2)
			a64_ldp(armAsm, armXRegister(i), armXRegister(i + 1), a64_mem_off(A64_SP, i * 8));
		a64_add_imm(armAsm, A64_SP, A64_SP, kBakFrame);
	}
	else
	{
		a64_ldr(armAsm, a64_as(mVU_xmmPQ, A64K_Q), mvuAbsMem(mVU, &mVU->vecBackup[A64_CODE(mVU_xmmPQ)][0], 16));
	}
}

/* Transforms the VU address (a quadword index) in gprReg into a byte offset into
 * VU memory, applying the vuRegs[0]/vuRegs[1] wrap and the vuRegs[0]->VU1 register-window remap.
 * x86: mVUaddrFix. Modifies gprReg in place (and tmpReg on the far-offset path).
 * gprReg/tmpReg are 64-bit (X) registers; the masking steps use the 32-bit view
 * (W-writes zero bits [63:32], matching the x86 32-bit AND). */
static __fi void mVUaddrFix(mV, a64r gprReg, a64r tmpReg)
{
	if (isVU1)
	{
		a64_and_imm(armAsm, a64_asw(gprReg), a64_asw(gprReg), 0x3ff); /* wrap around (1024 quadwords) */
		a64_lsl_imm(armAsm, a64_asw(gprReg), a64_asw(gprReg), 4); /* * 16 -> byte offset */
	}
	else
	{
		sptr offset;
		a64_label jmpA = A64_LABEL_INIT, jmpB = A64_LABEL_INIT;
		a64_tst_imm(armAsm, a64_asw(gprReg), 0x400);
		a64_b_cond(armAsm, &jmpA, A64_NE); /* if (addr & 0x400): reads vuRegs[1]'s VF/VI regs */
			a64_and_imm(armAsm, a64_asw(gprReg), a64_asw(gprReg), 0xff); /* else wrap (256 quadwords) */
			a64_b(armAsm, &jmpB);
		a64_bind(armAsm, &jmpA);
			if (THREAD_VU1)
				armEmitCall(((const void*)(mVU->waitMTVU)));
			a64_and_imm(armAsm, a64_asw(gprReg), a64_asw(gprReg), 0x3f); /* ToDo: vuRegs[0] may override vuRegs[1]'s VF0/VI0 */
			offset = (u128*)vuRegs[1].VF - (u128*)vuRegs[0].Mem;
			if (offset == ((s32)(offset)))
			{
				a64_add_imm(armAsm, gprReg, gprReg, ((s32)(offset)));
			}
			else
			{
				a64_mov_imm(armAsm, tmpReg, offset);
				a64_add(armAsm, gprReg, gprReg, tmpReg);
			}
		a64_bind(armAsm, &jmpB);
		a64_lsl_imm(armAsm, gprReg, gprReg, 4); /* * 16 -> byte offset (64-bit) */
	}
}

/* If the VU address source register is the always-zero VI0, the effective VU
 * memory address is a compile-time constant — return the absolute host pointer
 * so the load/store handler can skip the runtime moveVIToGPR + mVUaddrFix dance.
 * x86 returned an xAddressVoid (a folded ptr operand); ARM64 has no implicit
 * memory operands, so the contract is "absolute host address, materialized by
 * the caller via armMoveAddressToReg". NULL ⇒ must compute at runtime. */
static __fi const void* mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	s32 addr;
	/* VI const prop, as the old comment here requested: a known
	 * source register folds to an absolute address exactly like vi00
	 * always has, replicating the runtime sum and masking bit for
	 * bit - zero-extended sixteen-bit register plus sign-extended
	 * immediate under the same wrap. */
	if (srcreg != 0 && !(doViConstProp && srcreg < 16 && mVUconstReg[srcreg].isValid))
		return NULL;

	addr = (s32)(srcreg ? (u32)(u16)mVUconstReg[srcreg].regValue : 0u) + offset;
	if (isVU1)
	{
		return (const void*)(vuRegs[mVU->index].Mem + ((addr & 0x3FFu) << 4) + offsetSS_);
	}
	else
	{
		if (addr & 0x400)
			return NULL;

		return (const void*)(vuRegs[mVU->index].Mem + ((addr & 0xFFu) << 4) + offsetSS_);
	}
}

/* ------------------------------------------------------------------
 * Micro VU - Custom SSE Instructions (x86: microVU_Misc.inl SSE_*)
 * ------------------------------------------------------------------
 * AArch64 port of the VU FMAC arithmetic primitives. The VU's MIN/MAX are NOT IEEE
 * min/max — they're a signed-magnitude *integer* compare on the float bit pattern,
 * so MIN_MAX_PS uses the integer-comparison path (the x86 `if (0)` double path is
 * dropped). MIN_MAX_SS keeps the double-precision trick (it has no integer form):
 * the two float lanes are packed into a finite normal double whose ordering matches
 * the float magnitude/sign ordering, so plain FMIN/FMAX on .V2D() is exact (the
 * constructed doubles are never NaN, so NEON's NaN-propagation is never hit).
 *
 * The add/sub/mul/div primitives go through mVUclampedArith: when the extra-overflow
 * gamefix (clampE) is on, operands are sign-clamped before and the result range-
 * clamped after. clampE is off by default, so it normally emits just the NEON op. */

PCSX2_ALIGN(16) static const u32 mVU_MIN_MAX_1[4] = {0xffffffff, 0x80000000, 0xffffffff, 0x80000000};
PCSX2_ALIGN(16) static const u32 mVU_MIN_MAX_2[4] = {0x00000000, 0x40000000, 0x00000000, 0x40000000};
PCSX2_ALIGN(16) static const u32 mVU_ADD_SS[4]    = {0x80000000, 0xffffffff, 0xffffffff, 0xffffffff};

/* Warning: Modifies t1 and t2 */
static void MIN_MAX_PS(mV, a64r to, a64r from, a64r t1in, a64r t2in, bool min)
{
	const bool t1b = !A64_ISREG(t1in);
	const bool t2b = !A64_ISREG(t2in);
	const a64r t1 = t1b ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true) : t1in;
	const a64r t2 = t2b ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true) : t2in;

	/* integer comparison (signed-magnitude transform of the float bit pattern) */
	a64r c1 = min ? t2 : t1;
	a64r c2 = min ? t1 : t2;

	a64_mov(armAsm, a64_as(t1, A64K_16B), a64_as(to, A64K_16B));
	a64_sshr(armAsm, a64_as(t1, A64K_4S), a64_as(t1, A64K_4S), 31);
	a64_ushr(armAsm, a64_as(t1, A64K_4S), a64_as(t1, A64K_4S), 1);
	a64_eor(armAsm, a64_as(t1, A64K_16B), a64_as(t1, A64K_16B), a64_as(to, A64K_16B));

	a64_mov(armAsm, a64_as(t2, A64K_16B), a64_as(from, A64K_16B));
	a64_sshr(armAsm, a64_as(t2, A64K_4S), a64_as(t2, A64K_4S), 31);
	a64_ushr(armAsm, a64_as(t2, A64K_4S), a64_as(t2, A64K_4S), 1);
	a64_eor(armAsm, a64_as(t2, A64K_16B), a64_as(t2, A64K_16B), a64_as(from, A64K_16B));

	a64_cmgt(armAsm, a64_as(c1, A64K_4S), a64_as(c1, A64K_4S), a64_as(c2, A64K_4S)); /* c1 = (c1 > c2) ? -1 : 0 (signed) */
	a64_and(armAsm, a64_as(to, A64K_16B), a64_as(to, A64K_16B), a64_as(c1, A64K_16B));
	a64_bic(armAsm, a64_as(c1, A64K_16B), a64_as(from, A64K_16B), a64_as(c1, A64K_16B)); /* c1 = from & ~c1 (x86 PANDN) */
	a64_orr(armAsm, a64_as(to, A64K_16B), a64_as(to, A64K_16B), a64_as(c1, A64K_16B));

	if (t1b) mVUra_clearNeeded(mVU->regAlloc, t1);
	if (t2b) mVUra_clearNeeded(mVU->regAlloc, t2);
}

/* Warning: Modifies to's upper 3 vectors, and t1 */
static void MIN_MAX_SS(mV, a64r to, a64r from, a64r t1in, bool min)
{
	const bool t1b = !A64_ISREG(t1in);
	const a64r t1 = t1b ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, true) : t1in;

	/* to = { to0, to0, from0, from0 }  (x86 xSHUF.PS(to, from, 0)) */
	a64_ins_elem(armAsm, a64_as(to, A64K_4S), 1, a64_as(to, A64K_4S), 0);
	a64_ins_elem(armAsm, a64_as(to, A64K_4S), 2, a64_as(from, A64K_4S), 0);
	a64_ins_elem(armAsm, a64_as(to, A64K_4S), 3, a64_as(from, A64K_4S), 0);

	mvuLdrQ(mVU, RQSCRATCH, mVU_MIN_MAX_1);
	a64_and(armAsm, a64_as(to, A64K_16B), a64_as(to, A64K_16B), a64_as(RQSCRATCH, A64K_16B));
	mvuLdrQ(mVU, RQSCRATCH, mVU_MIN_MAX_2);
	a64_orr(armAsm, a64_as(to, A64K_16B), a64_as(to, A64K_16B), a64_as(RQSCRATCH, A64K_16B));

	mVUshufflePS(t1, to, 0xee); /* t1 = { to2, to3, to2, to3 } */
	if (min) a64_fmin(armAsm, a64_as(to, A64K_2D), a64_as(to, A64K_2D), a64_as(t1, A64K_2D));
	else     a64_fmax(armAsm, a64_as(to, A64K_2D), a64_as(to, A64K_2D), a64_as(t1, A64K_2D));

	if (t1b) mVUra_clearNeeded(mVU->regAlloc, t1);
}


/* Exponent-distance operand masking for VU ADD/SUB, fused single-keep
 * schedule (x86: mVUmaskAddSubFused, whose proofs this port shares: the
 * fused algebra measured identical to the reference over 80.7 million
 * lanes, and the x86 twin runs bit-identical to the two-pass emission
 * it replaces in the emitted-bytes differential). NEON makes it short:
 * native Abs, three-operand forms, per-lane Ushl for the keep mask with
 * out-of-range counts collapsing safely under the sign-OR, and the
 * zero-immediate compare forms are exactly the side tests. Twenty-one
 * instructions against the two-pass thirty-two, no recompute.
 * Masks a in place and leaves the masked copy of b in vc; b is not
 * modified. vm and vs are scratch, as is v0 (free here, like the
 * scalar path's use of v2 established). */
/* vf0 identities, the aVU twins of the x86 specializations, same
 * oracle proofs: a broadcast add of +0.0 is the value except zero-
 * exponent inputs which pack to +0.0 (subtract keeps the sign), and
 * a multiply by vf0.w is the value under the same zero-exponent
 * select while the x, y and z lanes give the sign alone. The NEON
 * forms lean on two tricks: an all-ones compare mask shifted right
 * once is exactly the magnitude mask the sign-preserving cases
 * need, and Bic folds the select into one op. Fixed scratch
 * registers, zero allocator pressure. Broadcast forms only - the
 * census puts the whole measured class there. */
static void mVUaddSubVF0(microVU* mVU, a64r reg, bool isSub, bool isSS)
{
	a64r T = RQSCRATCH;
	a64_shl(armAsm, a64_as(T, A64K_4S), a64_as(reg, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(T, A64K_4S), a64_as(T, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(T, A64K_4S), a64_as(T, A64K_4S));
	if (isSub)
		a64_ushr(armAsm, a64_as(T, A64K_4S), a64_as(T, A64K_4S), 1);
	if (isSS)
	{
		a64r U = RQSCRATCH2;
		a64_bic(armAsm, a64_as(U, A64K_16B), a64_as(reg, A64K_16B), a64_as(T, A64K_16B));
		a64_ins_elem(armAsm, a64_as(reg, A64K_4S), 0, a64_as(U, A64K_4S), 0);
	}
	else
		a64_bic(armAsm, a64_as(reg, A64K_16B), a64_as(reg, A64K_16B), a64_as(T, A64K_16B));
}

static bool mVUexactMulVF0(microVU* mVU, a64r reg, int opCase, bool isSS)
{
	a64r T = RQSCRATCH;
	if (opCase != 2 || _Ft_ != 0)
		return false;
	if (_bc_ != 3)
	{
		a64_movi_shift(armAsm, a64_as(T, A64K_4S), 0x80, A64_LSL, 24);
		if (isSS)
		{
			a64r U = RQSCRATCH2;
			a64_and(armAsm, a64_as(U, A64K_16B), a64_as(reg, A64K_16B), a64_as(T, A64K_16B));
			a64_ins_elem(armAsm, a64_as(reg, A64K_4S), 0, a64_as(U, A64K_4S), 0);
		}
		else
			a64_and(armAsm, a64_as(reg, A64K_16B), a64_as(reg, A64K_16B), a64_as(T, A64K_16B));
		return true;
	}
	a64_shl(armAsm, a64_as(T, A64K_4S), a64_as(reg, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(T, A64K_4S), a64_as(T, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(T, A64K_4S), a64_as(T, A64K_4S));
	a64_ushr(armAsm, a64_as(T, A64K_4S), a64_as(T, A64K_4S), 1);
	if (isSS)
	{
		a64r U = RQSCRATCH2;
		a64_bic(armAsm, a64_as(U, A64K_16B), a64_as(reg, A64K_16B), a64_as(T, A64K_16B));
		a64_ins_elem(armAsm, a64_as(reg, A64K_4S), 0, a64_as(U, A64K_4S), 0);
	}
	else
		a64_bic(armAsm, a64_as(reg, A64K_16B), a64_as(reg, A64K_16B), a64_as(T, A64K_16B));
	return true;
}

static void mVUmaskAddSubPS(mV, a64r a, a64r b,
	a64r vc, a64r vm, a64r vs)
{
	a64_shl(armAsm, a64_as(vc, A64K_4S), a64_as(a, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vc, A64K_4S), a64_as(vc, A64K_4S), 24);
	a64_shl(armAsm, a64_as(vs, A64K_4S), a64_as(b, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vs, A64K_4S), a64_as(vs, A64K_4S), 24);
	a64_sub(armAsm, a64_as(vc, A64K_4S), a64_as(vc, A64K_4S), a64_as(vs, A64K_4S)); /* d */
	a64_abs(armAsm, a64_as(vs, A64K_4S), a64_as(vc, A64K_4S));
	a64_movi(armAsm, a64_as(vm, A64K_4S), 1);
	a64_sub(armAsm, a64_as(vs, A64K_4S), a64_as(vs, A64K_4S), a64_as(vm, A64K_4S)); /* c = |d| - 1 */
	a64_movi(armAsm, a64_as(vm, A64K_16B), 0xff);
	a64_ushl(armAsm, a64_as(vm, A64K_4S), a64_as(vm, A64K_4S), a64_as(vs, A64K_4S)); /* keep = ~0 << c */
	a64_orr_vimm(armAsm, a64_as(vm, A64K_4S), 0x80, 24); /* keep' (sign survives garbage) */
	a64_movi(armAsm, a64_as(A64_Q(0), A64K_4S), 24);
	a64_cmgt(armAsm, a64_as(A64_Q(0), A64K_4S), a64_as(A64_Q(0), A64K_4S), a64_as(vs, A64K_4S)); /* 24 > c */
	a64_orr_vimm(armAsm, a64_as(A64_Q(0), A64K_4S), 0x80, 24);
	a64_and(armAsm, a64_as(vm, A64K_16B), a64_as(vm, A64K_16B), a64_as(A64_Q(0), A64K_16B)); /* M */
	a64_cmge_zero(armAsm, a64_as(A64_Q(0), A64K_4S), a64_as(vc, A64K_4S)); /* d >= 0: a side inactive */
	a64_orr(armAsm, a64_as(A64_Q(0), A64K_16B), a64_as(A64_Q(0), A64K_16B), a64_as(vm, A64K_16B));
	a64_and(armAsm, a64_as(a, A64K_16B), a64_as(a, A64K_16B), a64_as(A64_Q(0), A64K_16B));
	a64_cmle_zero(armAsm, a64_as(A64_Q(0), A64K_4S), a64_as(vc, A64K_4S)); /* d <= 0: b side inactive */
	a64_orr(armAsm, a64_as(A64_Q(0), A64K_16B), a64_as(A64_Q(0), A64K_16B), a64_as(vm, A64K_16B));
	a64_and(armAsm, a64_as(vc, A64K_16B), a64_as(b, A64K_16B), a64_as(A64_Q(0), A64K_16B)); /* masked b copy */
}


/* to (op)= from, sign-clamping operands and range-clamping the result when the
 * extra-overflow gamefix is enabled (x86: clampOp macro). isPS selects 4-lane vs
 * single-scalar. t1 is the caller-provided clamp scratch (xEmptyReg ⇒ RQSCRATCH). */
enum mVUarithOp { mVU_ADD_OP, mVU_SUB_OP, mVU_MUL_OP, mVU_DIV_OP };

static void mVUclampedArith(mV, a64r to, a64r from, a64r t1, int op, bool isPS)
{
	const a64r ct = !A64_ISREG(t1) ? RQSCRATCH : t1;
	const int xyzw = isPS ? 0xf : 0x8;
	const bool isAddSub = (op == mVU_ADD_OP) || (op == mVU_SUB_OP);
	mVUclamp3(mVU, to, ct, xyzw);
	mVUclamp3(mVU, from, ct, xyzw);
	if (isPS)
	{
		if (isAddSub && (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK))
		{
			/* the fused mask leaves the masked copy in RQSCRATCH and
			 * never touches the cached source register. */
			mVUmaskAddSubPS(mVU, to, from, RQSCRATCH, RQSCRATCH2, RQSCRATCH3);
			if (op == mVU_SUB_OP) a64_fsub(armAsm, a64_as(to, A64K_4S), a64_as(to, A64K_4S), a64_as(RQSCRATCH, A64K_4S));
			else                  a64_fadd(armAsm, a64_as(to, A64K_4S), a64_as(to, A64K_4S), a64_as(RQSCRATCH, A64K_4S));
			mVUclamp4(mVU, to, ct, xyzw);
			return;
		}
		switch (op)
		{
			case mVU_ADD_OP: a64_fadd(armAsm, a64_as(to, A64K_4S), a64_as(to, A64K_4S), a64_as(from, A64K_4S)); break;
			case mVU_SUB_OP: a64_fsub(armAsm, a64_as(to, A64K_4S), a64_as(to, A64K_4S), a64_as(from, A64K_4S)); break;
			case mVU_MUL_OP: a64_fmul(armAsm, a64_as(to, A64K_4S), a64_as(to, A64K_4S), a64_as(from, A64K_4S)); break;
			case mVU_DIV_OP: a64_fdiv(armAsm, a64_as(to, A64K_4S), a64_as(to, A64K_4S), a64_as(from, A64K_4S)); break;
		}
	}
	else if (isAddSub && (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK))
	{
		/* Lane 0 result only, through the same fused vector mask: compute
		 * on a copy of the destination so its upper lanes survive. No
		 * GPRs anywhere - the x86 rework's GPR scalar path broke
		 * rendering through a mechanism never identified, and the
		 * empirical boundary applies here too. */
		a64_mov(armAsm, a64_as(A64_Q(1), A64K_16B), a64_as(to, A64K_16B));
		mVUmaskAddSubPS(mVU, A64_Q(1), from, RQSCRATCH, RQSCRATCH2, RQSCRATCH3);
		if (op == mVU_SUB_OP) a64_fsub(armAsm, a64_as(A64_Q(1), A64K_S), a64_as(A64_Q(1), A64K_S), a64_as(RQSCRATCH, A64K_S));
		else                  a64_fadd(armAsm, a64_as(A64_Q(1), A64K_S), a64_as(A64_Q(1), A64K_S), a64_as(RQSCRATCH, A64K_S));
		/* not ct: with no caller scratch ct aliases RQSCRATCH */
		mVUclamp4(mVU, A64_Q(1), RQSCRATCH2, 0x8);
		a64_ins_elem(armAsm, a64_as(to, A64K_4S), 0, a64_as(A64_Q(1), A64K_4S), 0);
		return;
	}
	else
	{
		/* AArch64 scalar FP ops write the result to Sd and ZERO the upper bits
		 * [127:32] of the V register — unlike x86 ADDSS/MULSS, which preserve the
		 * upper 3 lanes. The microVU single-scalar (_XYZW_SS) model shuffles the
		 * target lane into lane0, operates, then shuffles back, and DEPENDS on the
		 * other lanes surviving (e.g. mVU_FMACb's MADDAw.z accumulates into ACC.z
		 * while ACC.x/.y must be preserved). Writing `to.S()` in place would wipe
		 * them. Compute into a scratch and insert only lane0 of `to`. */
		const a64r sres = RQSCRATCH;
		switch (op)
		{
			case mVU_ADD_OP: a64_fadd(armAsm, a64_as(sres, A64K_S), a64_as(to, A64K_S), a64_as(from, A64K_S)); break;
			case mVU_SUB_OP: a64_fsub(armAsm, a64_as(sres, A64K_S), a64_as(to, A64K_S), a64_as(from, A64K_S)); break;
			case mVU_MUL_OP: a64_fmul(armAsm, a64_as(sres, A64K_S), a64_as(to, A64K_S), a64_as(from, A64K_S)); break;
			case mVU_DIV_OP: a64_fdiv(armAsm, a64_as(sres, A64K_S), a64_as(to, A64K_S), a64_as(from, A64K_S)); break;
		}
		a64_ins_elem(armAsm, a64_as(to, A64K_4S), 0, a64_as(sres, A64K_4S), 0);
	}
	mVUclamp4(mVU, to, ct, xyzw);
}

static void SSE_MAXPS(mV, a64r to, a64r from, a64r t1, a64r t2) { MIN_MAX_PS(mVU, to, from, t1, t2, false); }
static void SSE_MINPS(mV, a64r to, a64r from, a64r t1, a64r t2) { MIN_MAX_PS(mVU, to, from, t1, t2, true); }
static void SSE_MAXSS(mV, a64r to, a64r from, a64r t1, a64r t2) { MIN_MAX_SS(mVU, to, from, t1, false); }
static void SSE_MINSS(mV, a64r to, a64r from, a64r t1, a64r t2) { MIN_MAX_SS(mVU, to, from, t1, true); }
/* The TriAce ADDi hack is gone: the exponent-distance mask reproduces the
 * discard it approximated, for every distance. */
static void SSE_ADD2SS(mV, a64r to, a64r from, a64r t1, a64r t2)
{
	mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, false);
}
static void SSE_ADD2PS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, true); }
static void SSE_ADDPS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, true); }
static void SSE_ADDSS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_ADD_OP, false); }
static void SSE_SUBPS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_SUB_OP, true); }
static void SSE_SUBSS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_SUB_OP, false); }

/* ------------------------------------------------------------------
 * Hardware-exact multiply (x86: mVUexactMulPS). Same architecture:
 * the Booth correction can reach the kept mantissa only when the
 * product's bits 22:15 are all zero - about one lane in 256 - so the
 * fast path is the widening products, that guard, and a chop-pack;
 * a tripped guard drops the quad to the scalar soft model through
 * the per-VU buffer. NEON compresses the x86 sequence considerably:
 * Umull pairs make the products, Xtn narrows the low words for a
 * constant-free guard (shift up, compare against zero), Shrn extracts
 * the mantissas, the carry-normalize is one per-lane Ushl by negated
 * counts, and Bic with immediates does the field masking.
 * Scratch: v0, v1, q29-q31; from is not modified; dst may equal to.
 * The stub's operand words are stored on the slow path only, so the hot
 * path carries no stores at all.
 * ------------------------------------------------------------------ */

static void mVUexactMulPS(mV, a64r dst, a64r to, a64r from)
{
	const a64r vA = A64_Q(0);
	const a64r vB = A64_Q(1);
	const a64r vC = RQSCRATCH;
	const a64r vD = RQSCRATCH2;
	const a64r vE = RQSCRATCH3;
	u32* buf = mVU->exactMulBuf;
	a64_label slow = A64_LABEL_INIT, pack = A64_LABEL_INIT, done = A64_LABEL_INIT;

	/* mantissas with implicit bit: vA = ma, vB = mb */
	a64_movi_shift(armAsm, a64_as(vC, A64K_4S), 0x80, A64_LSL, 16); /* implicit */
	a64_movi(armAsm, a64_as(vD, A64K_4S), 1);
	a64_sub(armAsm, a64_as(vD, A64K_4S), a64_as(vC, A64K_4S), a64_as(vD, A64K_4S)); /* mantissa mask */
	a64_and(armAsm, a64_as(vA, A64K_16B), a64_as(to, A64K_16B), a64_as(vD, A64K_16B));
	a64_orr(armAsm, a64_as(vA, A64K_16B), a64_as(vA, A64K_16B), a64_as(vC, A64K_16B));
	a64_and(armAsm, a64_as(vB, A64K_16B), a64_as(from, A64K_16B), a64_as(vD, A64K_16B));
	a64_orr(armAsm, a64_as(vB, A64K_16B), a64_as(vB, A64K_16B), a64_as(vC, A64K_16B));

	/* products: vC = p01 (lanes 0,1), vA = p23 */
	a64_umull(armAsm, a64_as(vC, A64K_2D), a64_as(vA, A64K_2S), a64_as(vB, A64K_2S));
	a64_umull(armAsm, a64_as(vA, A64K_2D), a64_as(vA, A64K_4S), a64_as(vB, A64K_4S));

	/* guard: low words narrowed, bits 22:15 tested constant-free */
	a64_xtn(armAsm, a64_as(vB, A64K_2S), a64_as(vC, A64K_2D));
	a64_xtn(armAsm, a64_as(vB, A64K_4S), a64_as(vA, A64K_2D));
	a64_ushr(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S), 15);
	a64_shl(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S));
	/* zero-operand lanes are decided by the final select whatever the
	 * product says; with the implicit bit forced on, 0.0 times anything
	 * zeroes the guard field, and game data is full of zeros. */
	a64_shl(armAsm, a64_as(vD, A64K_4S), a64_as(to, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S));
	a64_shl(armAsm, a64_as(vE, A64K_4S), a64_as(from, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vE, A64K_4S), a64_as(vE, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(vE, A64K_4S), a64_as(vE, A64K_4S));
	a64_orr(armAsm, a64_as(vD, A64K_16B), a64_as(vD, A64K_16B), a64_as(vE, A64K_16B));
	/* sixteen or more trailing zeros in the recoded mantissa empty the
	 * whole low half of the recode, so the tree is exact and the fast
	 * path already right - oracle-proven at that threshold with zero
	 * violations and measurably unsound below it (the partial-source
	 * side is not sound at any threshold and is not excluded). */
	a64_movi_shift(armAsm, a64_as(vE, A64K_4S), 0xff, A64_LSL, 8);
	a64_orr_vimm(armAsm, a64_as(vE, A64K_4S), 0xff, 0);
	a64_and(armAsm, a64_as(vE, A64K_16B), a64_as(from, A64K_16B), a64_as(vE, A64K_16B));
	a64_cmeq_zero(armAsm, a64_as(vE, A64K_4S), a64_as(vE, A64K_4S));
	a64_orr(armAsm, a64_as(vD, A64K_16B), a64_as(vD, A64K_16B), a64_as(vE, A64K_16B));
	a64_bic(armAsm, a64_as(vB, A64K_16B), a64_as(vB, A64K_16B), a64_as(vD, A64K_16B));
	a64_umaxv(armAsm, A64_S(2), a64_as(vB, A64K_4S)); /* v2 low used transiently */
	a64_fmov(armAsm, gprT1, A64_S(2));
	a64_cbnz(armAsm, gprT1, &slow);

	/* ---- shared pack (fast path falls in; slow path branches back) ---- */
	a64_bind(armAsm, &pack);
	/* rm merged: (p >> 23) narrowed into 4S */
	a64_shrn(armAsm, a64_as(vB, A64K_2S), a64_as(vC, A64K_2D), 23);
	a64_shrn(armAsm, a64_as(vB, A64K_4S), a64_as(vA, A64K_2D), 23);
	/* carry-normalize with a per-lane negative shift */
	a64_ushr(armAsm, a64_as(vC, A64K_4S), a64_as(vB, A64K_4S), 24); /* carry */
	a64_neg(armAsm, a64_as(vD, A64K_4S), a64_as(vC, A64K_4S));
	a64_ushl(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S), a64_as(vD, A64K_4S));
	/* e = expA + expB - 127 + carry */
	a64_shl(armAsm, a64_as(vD, A64K_4S), a64_as(to, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S), 24);
	a64_add(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S), a64_as(vC, A64K_4S));
	a64_shl(armAsm, a64_as(vC, A64K_4S), a64_as(from, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vC, A64K_4S), a64_as(vC, A64K_4S), 24);
	a64_add(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S), a64_as(vC, A64K_4S));
	a64_movi(armAsm, a64_as(vC, A64K_4S), 127);
	a64_sub(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S), a64_as(vC, A64K_4S)); /* e */
	/* sign */
	a64_eor(armAsm, a64_as(vE, A64K_16B), a64_as(to, A64K_16B), a64_as(from, A64K_16B));
	a64_ushr(armAsm, a64_as(vE, A64K_4S), a64_as(vE, A64K_4S), 31);
	a64_shl(armAsm, a64_as(vE, A64K_4S), a64_as(vE, A64K_4S), 31); /* sign */
	/* normal result in vA */
	a64_bic_vimm(armAsm, a64_as(vB, A64K_4S), 0xff, 24); /* rm &= 0x007fffff */
	a64_bic_vimm(armAsm, a64_as(vB, A64K_4S), 0x80, 16);
	a64_shl(armAsm, a64_as(vA, A64K_4S), a64_as(vD, A64K_4S), 23);
	a64_orr(armAsm, a64_as(vA, A64K_16B), a64_as(vA, A64K_16B), a64_as(vB, A64K_16B));
	a64_orr(armAsm, a64_as(vA, A64K_16B), a64_as(vA, A64K_16B), a64_as(vE, A64K_16B));
	/* overflow: e > 255 -> sign | 0x7fffffff */
	a64_movi(armAsm, a64_as(vC, A64K_4S), 255);
	a64_cmgt(armAsm, a64_as(vC, A64K_4S), a64_as(vD, A64K_4S), a64_as(vC, A64K_4S)); /* mo */
	a64_mvni(armAsm, a64_as(vB, A64K_4S), 0x80, A64_LSL, 24); /* 0x7fffffff */
	a64_orr(armAsm, a64_as(vB, A64K_16B), a64_as(vB, A64K_16B), a64_as(vE, A64K_16B));
	a64_bsl(armAsm, a64_as(vC, A64K_16B), a64_as(vB, A64K_16B), a64_as(vA, A64K_16B)); /* mo ? clamp : normal */
	/* underflow (e < 1) or a zero-exponent operand -> sign */
	a64_cmlt_zero(armAsm, a64_as(vD, A64K_4S), a64_as(vD, A64K_4S)); /* e <= 0 */
	a64_shl(armAsm, a64_as(vB, A64K_4S), a64_as(to, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S));
	a64_orr(armAsm, a64_as(vD, A64K_16B), a64_as(vD, A64K_16B), a64_as(vB, A64K_16B));
	a64_shl(armAsm, a64_as(vB, A64K_4S), a64_as(from, A64K_4S), 1);
	a64_ushr(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S), 24);
	a64_cmeq_zero(armAsm, a64_as(vB, A64K_4S), a64_as(vB, A64K_4S));
	a64_orr(armAsm, a64_as(vD, A64K_16B), a64_as(vD, A64K_16B), a64_as(vB, A64K_16B));
	a64_bsl(armAsm, a64_as(vD, A64K_16B), a64_as(vE, A64K_16B), a64_as(vC, A64K_16B)); /* mzu ? sign : selected */
	a64_mov(armAsm, a64_as(dst, A64K_16B), a64_as(vD, A64K_16B));
	a64_b(armAsm, &done);

	/* ---- slow path: hand the products to the tree stub, take the
	 * corrected ones back, and rejoin the pack. The stub saves every
	 * vector register itself, so nothing here needs a flush. ---- */
	a64_bind(armAsm, &slow);
	/* The stub narrows its two operand words with Xtn and never reads
	 * above bit 15, and setting the implicit bit only touches bit 23, so
	 * the raw operands serve where the mantissas used to: the stores that
	 * were on every multiply now happen only here. `to` and `from` are
	 * both still live -- the pack below reads them, and dst is not
	 * written until the very end. */
	a64_str(armAsm, to, armAbsMemOperand(RSCRATCHADDR, &buf[0], 128));
	a64_str(armAsm, from, armAbsMemOperand(RSCRATCHADDR, &buf[4], 128));
	a64_str(armAsm, vC, armAbsMemOperand(RSCRATCHADDR, &buf[8], 128));
	a64_str(armAsm, vA, armAbsMemOperand(RSCRATCHADDR, &buf[12], 128));
	armEmitCall(((const void*)(mVU->exactMulStub)));
	a64_ldr(armAsm, vC, armAbsMemOperand(RSCRATCHADDR, &buf[8], 128));
	a64_ldr(armAsm, vA, armAbsMemOperand(RSCRATCHADDR, &buf[12], 128));
	a64_b(armAsm, &pack);
	a64_bind(armAsm, &done);
}

static void SSE_MULPS(mV, a64r to, a64r from, a64r t1, a64r t2)
{
	if (CHECK_VU_EXACTMUL)
	{
		mVUexactMulPS(mVU, to, to, from);
		return;
	}
	mVUclampedArith(mVU, to, from, t1, mVU_MUL_OP, true);
}
static void SSE_MULSS(mV, a64r to, a64r from, a64r t1, a64r t2)
{
	if (CHECK_VU_EXACTMUL)
	{
		/* lane 0 through the same pipeline into a scratch, then insert:
		 * the upper lanes of the destination must survive. v2 is free
		 * here alongside the pipeline's own scratch. */
		mVUexactMulPS(mVU, A64_Q(2), to, from);
		a64_ins_elem(armAsm, a64_as(to, A64K_4S), 0, a64_as(A64_Q(2), A64K_4S), 0);
		return;
	}
	mVUclampedArith(mVU, to, from, t1, mVU_MUL_OP, false);
}
static void SSE_DIVPS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_DIV_OP, true); }
static void SSE_DIVSS(mV, a64r to, a64r from, a64r t1, a64r t2) { mVUclampedArith(mVU, to, from, t1, mVU_DIV_OP, false); }
