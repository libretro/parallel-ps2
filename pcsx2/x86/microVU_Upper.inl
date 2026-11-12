#include "common/emitter/c89ops.h"
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

/* ------------------------------------------------------------------ */
/* mVUupdateFlags() - Updates status/mac flags */
/* ------------------------------------------------------------------ */

#define AND_XYZW ((_XYZW_SS && modXYZW) ? (1) : (mFLAG.doFlag ? (_X_Y_Z_W) : (flipMask[_X_Y_Z_W])))
#define ADD_XYZW ((_XYZW_SS && modXYZW) ? (_X ? 3 : (_Y ? 2 : (_Z ? 1 : 0))) : 0)
#define SHIFT_XYZW(gprReg) \
	do { \
		if (_XYZW_SS && modXYZW && !_W) \
		{ \
			xe_shl32_ri((gprReg), ADD_XYZW); \
		} \
	} while (0)


/* Note: If modXYZW is true, then it adjusts XYZW for Single Scalar operations */
static void mVUupdateFlags(mV, int reg, int regT1in, int regT2in, int modXYZW)
{
	const int mReg = gprT1;
	const int sReg = getFlagReg(sFLAG.write);
	int regT1b = regT1in < 0, regT2b = 0;
	static const u16 flipMask[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
	int regT1;
	int regT2;

	if (!sFLAG.doFlag && !mFLAG.doFlag)
		return;

	regT1 = regT1b ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : regT1in;

	regT2 = reg;
	if ((mFLAG.doFlag && !(_XYZW_SS && modXYZW)))
	{
		regT2 = regT2in;
		if (regT2 < 0)
		{
			regT2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
			regT2b = 1;
		}
		xe_pshufd_xxi(regT2, reg, 0x1B); /* Flip wzyx to xyzw */
	}
	else
		regT2 = reg;

	if (sFLAG.doFlag)
	{
		mVUallocSFLAGa(sReg, sFLAG.lastWrite); /* Get Prev Status Flag */
		if (sFLAG.doNonSticky)
			xe_and32_ri(sReg, 0xfffc00ff); /* Clear O,U,S,Z flags */
	}

	/* -------------------------Check for Signed flags------------------------------ */

	xe_movmskps_rx(mReg,  regT2); /* Move the Sign Bits of the t2reg */
	xe_xorps_xx(regT1, regT1); /* Clear regT1 */
	xe_cmpeqps_xx(regT1, regT2); /* Set all F's if each vector is zero */
	xe_movmskps_rx(gprT2, regT1); /* Used for Zero Flag Calculation */

	xe_and32_ri(mReg, AND_XYZW); /* Grab "Is Signed" bits from the previous calculation */
	xe_shl32_ri(mReg, 4);

	/* -------------------------Check for Zero flags------------------------------ */

	xe_and32_ri(gprT2, AND_XYZW); /* Grab "Is Zero" bits from the previous calculation */
	xe_or32_rr(mReg, gprT2);

	/* -------------------------Overflow Flags----------------------------------- */
	/* We can't really do this because of the limited range of x86 and the value MIGHT genuinely be FLT_MAX (x86) */
	/* so this will need to remain as a gamefix for the one game that needs it (Superman Returns) */
	/* until some sort of soft float implementation. */
	/* The FMAX-result signature is a near-exact overflow proxy for
	 * multiply-family finals (differential rig, 600-case batteries:
	 * MUL status divergence 285 -> 1 against the ps2float oracle,
	 * false positives 1) but is wrong for adds, where FMAX passes
	 * through absorbing additions legitimately (ADD: 2 -> 173, all
	 * false fires). The detector therefore only runs where its
	 * signature holds; callers mark multiply-involved ops. */
	/* Overflow detection historically existed only for the overflow
	 * gamefix; under the accuracy options the results carry genuine
	 * exp-255 patterns (the exact composites' saturation), the
	 * cmpnltps detector fires on them via unordered-true, and the
	 * oracle sets O on exactly these cases (fpaudit flag census:
	 * every adversarial flag divergence was the O bit alone, with
	 * rec-O identically zero in every non-hack mode). */
	/* Overflow detection is unconditional (mul-family scoped by
	 * ovfDetectOK): default recompilation never set O at all -- the
	 * flag census proved rec-O identically zero outside the old
	 * gamefix -- while the class-gated threshold detector matches
	 * the hardware model to 1-in-400 adversarial and the block
	 * measures free on frame throughput (three-rep workload, dead
	 * even). The VuOverflowHack GameDB entries are hereby harmless
	 * no-ops; the exact pattern/event machinery still upgrades the
	 * detector under the accuracy options. */
	if (sFLAG.doFlag && mVU->ovfDetectOK)
	{
		PCSX2_ALIGN(16) static const u32 sse4_compvals[2][4] = {
			{0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff}, /* 1111 */
			{0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff}, /* 1111 */
		};
		uint8_t* oJMP;
		/* Calculate overflow */
		if (mVU->ovfUseEventMask)
		{
			/* Accurate add/sub: the composite exports its per-lane
			 * saturation EVENT (value proxies over-fire on
			 * pass-through all-ones operands); the fused blends OR
			 * their mul-overflow lanes into the same spill. */
			extern u32 s_vuAddSubOvfEvent[4];
			xe_movaps_xm(regT1, &s_vuAddSubOvfEvent[0]);
		}
		else
		{
		xe_movaps_xx(regT1, regT2);
		xe_andps_xm(regT1, &sse4_compvals[1][0]); /* Remove sign flags (we don't care) */
		if (CHECK_VU_ACC_ADDSUB || CHECK_VU_EXACTMUL)
			/* Exact composites: FLT_MAX is a legitimate value and true
			 * overflow is exactly the all-ones exp-255 pattern, so the
			 * detector is an integer equality, not a float threshold
			 * (the threshold form fired on every legitimate FMAX add:
			 * fpaudit, ADD 34-to-358 regression before this). */
			xe_pcmpeqd_xm(regT1, &sse4_compvals[1][0]);
		else
			xe_cmpnltps_xm(regT1, &sse4_compvals[0][0]); /* Compare if T1 == FLT_MAX */
		}
		xe_movmskps_rx(gprT2, regT1); /* Grab sign bits  for equal results */
		xe_and32_ri(gprT2, AND_XYZW); /* Grab "Is FLT_MAX" bits from the previous calculation */
		 xe_fwd_jcc32(Jcc_Zero, oJMP);

		xe_or32_ri(sReg, 0x820000);
		if (mFLAG.doFlag)
		{
			xe_shl32_ri(gprT2, 12); /* Add the results to the MAC Flag */
			xe_or32_rr(mReg, gprT2);
		}

		xe_fwd_set32(oJMP);
	}

	/* -------------------------Write back flags------------------------------ */
	if (mFLAG.doFlag)
	{
		SHIFT_XYZW(mReg); /* If it was Single Scalar, move the flags in to the correct position */
		mVUallocMFLAGb(mVU, mReg, mFLAG.write); /* Set Mac Flag */
	}
	if (sFLAG.doFlag)
	{
		xe_and32_ri(mReg, 0xFF); /* Ignore overflow bits, they're handled separately */
		xe_or32_rr(sReg, mReg);
		if (sFLAG.doNonSticky)
		{
			xe_shl32_ri(mReg, 8);
			xe_or32_rr(sReg, mReg);
		}
	}
	if (regT1b)
		mVUra_clearNeededXMM(mVU->regAlloc, regT1);
	if (regT2b)
		mVUra_clearNeededXMM(mVU->regAlloc, regT2);
}

/* ------------------------------------------------------------------ */
/* Helper Macros and Functions */
/* ------------------------------------------------------------------ */

static void (*const SSE_PS[])(microVU*, int, int, int, int) = {
	SSE_ADDPS, /* 0 */
	SSE_SUBPS, /* 1 */
	SSE_MULPS, /* 2 */
	SSE_MAXPS, /* 3 */
	SSE_MINPS, /* 4 */
	SSE_ADD2PS /* 5 */
};

static void (*const SSE_SS[])(microVU*, int, int, int, int) = {
	SSE_ADDSS, /* 0 */
	SSE_SUBSS, /* 1 */
	SSE_MULSS, /* 2 */
	SSE_MAXSS, /* 3 */
	SSE_MINSS, /* 4 */
	SSE_ADD2SS /* 5 */
};

typedef enum clampModes
{
	cFt = 0x01, /* Clamp Ft / I-reg / Q-reg */
	cFs = 0x02, /* Clamp Fs */
	cACC = 0x04, /* Clamp ACC */
} clampModes;

/* Sets Up Pass1 Info for Normal, BC, I, and Q Cases */
static void setupPass1(microVU* mVU, int opCase, int isACC, int noFlagUpdate)
{
	opCase1 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, _Ft_); }
	opCase2 { mVUanalyzeFMAC3(mVU, ((isACC) ? 0 : _Fd_), _Fs_, _Ft_); }
	opCase3 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, 0); }
	opCase4 { mVUanalyzeFMAC1(mVU, ((isACC) ? 0 : _Fd_), _Fs_, 0); }

	if (noFlagUpdate) /* Max/Min Ops */
		sFLAG.doFlag = 0;
}

/* Safer to force 0 as the result for X minus X than to do actual subtraction */
static int doSafeSub(microVU* mVU, int opCase, int opType, int isACC)
{
	opCase1
	{
		if ((opType == 1) && (_Ft_ == _Fs_) && (opCase == 1)) /* Don't do this with BC's! */
		{
			const int Fs = mVUra_allocReg(mVU->regAlloc, -1, isACC ? 32 : _Fd_, _X_Y_Z_W, 1);
			xe_pxor_xx(Fs, Fs); /* Set to Positive 0 */
			mVUupdateFlags(mVU, Fs, -1, -1, 1);
			mVUra_clearNeededXMM(mVU->regAlloc, Fs);
			return 1;
		}
	}
	return 0;
}

/* Sets Up Ft Reg for Normal, BC, I, and Q Cases */
static void setupFtReg(microVU* mVU, int* Ft, int* tempFt, int opCase, int clampType)
{
	opCase1
	{
		/* Based on mVUclamp2 -> mVUclamp1 below. */
		const int willClamp = (clampE || ((clampType & cFt) && !clampE && (CHECK_VU_OVERFLOW(mVU->index) || CHECK_VU_SIGN_OVERFLOW(mVU->index))));

		if (_XYZW_SS2)      { (*Ft) = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, _X_Y_Z_W, 1); (*tempFt) = (*Ft); }
		else if (willClamp) { (*Ft) = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, 0xf, 1);      (*tempFt) = (*Ft); }
		else                { (*Ft) = mVUra_allocReg(mVU->regAlloc, _Ft_, -1, 0, 1);              (*tempFt) = -1;  }
	}
	opCase2
	{
		(*tempFt) = mVUra_allocReg(mVU->regAlloc, _Ft_, -1, 0, 1);
		(*Ft)     = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		mVUunpack_xyzw((*Ft), (*tempFt), _bc_);
		mVUra_clearNeededXMM(mVU->regAlloc, (*tempFt));
		(*tempFt) = (*Ft);
	}
	opCase3
	{
		(*Ft) = mVUra_allocReg(mVU->regAlloc, 33, 0, _X_Y_Z_W, 1);
		(*tempFt) = (*Ft);
	}
	opCase4
	{
		if (!clampE && _XYZW_SS && !mVUinfo.readQ)
		{
			(*Ft) = xmmPQ;
			(*tempFt) = -1;
		}
		else
		{
			(*Ft) = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
			(*tempFt) = (*Ft);
			mVUunpack_xyzw((*Ft), xmmPQ, mVUinfo.readQ);
		}
	}
}

/* Normal FMAC Opcodes */
static void mVU_FMACa(microVU* mVU, int recPass, int opCase, int opType, int isACC, int clampType)
{
	pass1 { setupPass1(mVU, opCase, isACC, ((opType == 3) || (opType == 4))); }
	pass2
	{
		int Fs;
		int Ft;
		int ACC;
		int tempFt;
		if (doSafeSub(mVU, opCase, opType, isACC))
			return;

		setupFtReg(mVU, &Ft, &tempFt, opCase, clampType);

		if (isACC)
		{
			Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, 1);
			ACC = mVUra_allocReg(mVU->regAlloc, (_X_Y_Z_W == 0xf) ? -1 : 32, 32, 0xf, 0);
			if (_XYZW_SS2)
				xe_pshufd_xxi(ACC, ACC, shuffleSS(_X_Y_Z_W));
		}
		else
		{
			Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _Fd_, _X_Y_Z_W, 1);
		}

		/* The multiply-family operand clamps exist because a host
		 * multiply of an exponent-255 operand propagates infinities;
		 * the exact multiplier has no such failure by construction,
		 * and the clamp only destroys information there (closure
		 * census: every Fd-form residual against the oracle carried
		 * an exp-255 operand flattened by this clamp). Elide them on
		 * the exact path; SUB-family clamps are untouched, they are
		 * not this option's domain. */
		{
			/* Input clamps are elided where an accuracy option's exact
			 * implementation subsumes them: exactmul owns multiply
			 * operands, and the accurate add/sub composite implements
			 * the hardware adder outright, exponent-255 operands
			 * included -- ADD (which carries no clamp flags) is exact
			 * on raw exp-255 register operands through the same
			 * composite, while SUB's Kingdom Hearts-era input clamps
			 * were flattening those operands before the composite's
			 * detector saw them (fpaudit: 205/400 divergent lanes, all
			 * with an exp-255 operand, several with the wrong sign). */
			const int elide = ((opType == 2) && CHECK_VU_EXACTMUL)
			               || ((opType <= 1) && CHECK_VU_ACC_ADDSUB);
			if ((clampType & cFt) && !elide) mVUclamp2(mVU, Ft, -1, _X_Y_Z_W, 0);
			if ((clampType & cFs) && !elide) mVUclamp2(mVU, Fs, -1, _X_Y_Z_W, 0);
		}

		/* broadcast add/sub of a vf0 x, y or z lane is an identity;
		 * opCase 2 is the broadcast form, read from setupFtReg's own
		 * body this time. */
		/* provable mask no-op: register-form add/sub with Fs == Ft has
		 * per-lane exponent distance zero on every computed lane */
		mVU->addsubMaskNoop = ((opType <= 1) && (opCase == 1) && (_Fs_ == _Ft_));
		/* Mul-family only: for multiplies the all-ones result pattern
		 * is equivalent to a genuine overflow event (exp-255 inputs
		 * yield exp-255-with-mantissa products, never all-ones), so
		 * the pattern detector is exact there. Add/sub results can be
		 * all-ones by PASS-THROUGH of an exp-255 operand with no
		 * overflow event, so the value proxy over-fires (fpaudit: ADD
		 * 34 missing-O became 141 mixed under the extension) --
		 * closing add/sub O needs the composite's internal
		 * saturation-event mask exported to the flag block, ledgered
		 * as the flag front's remaining design item. */
		mVU->ovfDetectOK = (opType == 2) || ((opType <= 1) && CHECK_VU_ACC_ADDSUB);
		mVU->ovfUseEventMask = ((opType <= 1) && CHECK_VU_ACC_ADDSUB);

		if ((opType <= 1) && (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK) && (opCase == 2) && (_Ft_ == 0) && (_bc_ != 3))
			mVUaddSubVF0(mVU, Fs, opType == 1, _XYZW_SS);
		else if (opType != 2 || !CHECK_VU_EXACTMUL || !mVUexactMulVF0(mVU, Fs, opCase, _XYZW_SS))
		{
			if (_XYZW_SS) SSE_SS[opType](mVU, Fs, Ft, -1, -1);
			else          SSE_PS[opType](mVU, Fs, Ft, -1, -1);
		}

		if (isACC)
		{
			if (_XYZW_SS)
				xe_movss_xx(ACC, Fs);
			else
				mVUmergeRegs(ACC, Fs, _X_Y_Z_W, 0);
			mVUupdateFlags(mVU, ACC, Fs, tempFt, 1);
			if (_XYZW_SS2)
				xe_pshufd_xxi(ACC, ACC, shuffleSS(_X_Y_Z_W));
			mVUra_clearNeededXMM(mVU->regAlloc, ACC);
		}
		else if (opType < 3 || opType == 5) /* Not Min/Max or is ADDi(5) (TODO: Reorganise this so its < 4 including ADDi) */
			mVUupdateFlags(mVU, Fs, tempFt, -1, 1);

		mVUra_clearNeededXMM(mVU->regAlloc, Fs); /* Always Clear Written Reg First */
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
	}
	pass4
	{
		if ((opType != 3) && (opType != 4))
			mVUregs.needExactMatch |= 8;
	}
}


/* Hardware fused-MAC overflow rule (ps2f_mac_finish): when the mul
 * stage overflows, the fused result is the sign-selected all-ones
 * saturation pattern regardless of the add -- distinct from adding
 * the saturated float, which yields FMAX arithmetic (fpaudit
 * decomposition probe: the two semantics split on 158/400
 * adversarial cases). Under exactmul the product register already
 * holds the exp-255 saturation pattern, so detection is a per-lane
 * exponent compare on the product and the fix is a post-add blend.
 * Active only when both accuracy options own their stages. MADD's
 * pattern sign follows the product's; MSUB's is flipped
 * (round_to_max = sign(product) there). Flags are the flag
 * campaign's ledger, not this blend's. */
/* One register is held across the add: the saturation pattern on the
 * overflow lanes and zero elsewhere. The pattern is never zero (its
 * magnitude is 0x7FFFFFFF), so the lane mask is recovered from it after
 * the add, when the add's own temps are released. */
static __fi int mVUfusedOvfPre(mV, int prod, bool msub)
{
	int ovf, pat;
	if (!(CHECK_VU_EXACTMUL && CHECK_VU_ACC_ADDSUB))
		return -1;
	ovf = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	pat = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	/* True mul-stage overflow saturates to the all-ones pattern
	 * (sign|0x7FFFFFFF -- plain-MUL rows prove the exact mul emits
	 * exactly this); exp-255 products with a computed mantissa come
	 * from exp-255 INPUTS, carry no overflow in the model, and flow
	 * into the add normally, so the detector matches the full
	 * magnitude pattern, not the exponent field. */
	xe_movaps_xm(pat, mVUglob.absclip);
	xe_movaps_xx(ovf, prod);
	xe_pand_xx(ovf, pat);
	xe_pcmpeqd_xx(ovf, pat);                  /* |prod| == 0x7FFFFFFF */
	xe_movaps_xx(pat, prod);
	xe_pand_xm(pat, mVUglob.signbit);
	if (msub)
		xe_pxor_xm(pat, mVUglob.signbit);
	xe_por_xm(pat, mVUglob.absclip);          /* sign | 0x7FFFFFFF */
	xe_pand_xx(ovf, pat);                     /* pattern on ovf lanes */
	mVUra_clearNeededXMM(mVU->regAlloc, pat);
	return ovf;
}

static __fi void mVUfusedOvfPost(mV, int dst, int ovf)
{
	int keep;
	if (ovf < 0)
		return;
	keep = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	xe_pxor_xx(keep, keep);
	xe_pcmpeqd_xx(keep, ovf);                 /* lanes the add keeps */
	{
		/* mac_finish sets O on mul-overflow lanes regardless of the
		 * add; fold them into the composite's event spill so the
		 * flag block sees the union. The spill is stored in the
		 * reversed convention the flag block's movmsk/shl path
		 * expects, so the lane mask is reversed for the OR, or the
		 * blend lanes land mirrored in the mac nibble. */
		extern u32 s_vuAddSubOvfEvent[4];
		const int t = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		xe_pcmpeqd_xx(t, t);
		xe_pxor_xx(t, keep);                  /* overflow lanes */
		xe_pshufd_xxi(t, t, 0x1B);
		xe_por_xm(t, &s_vuAddSubOvfEvent[0]);
		xe_movaps_mx(&s_vuAddSubOvfEvent[0], t);
		mVUra_clearNeededXMM(mVU->regAlloc, t);
	}
	xe_pand_xx(dst, keep);
	xe_por_xx(dst, ovf);
	mVUra_clearNeededXMM(mVU->regAlloc, keep);
	mVUra_clearNeededXMM(mVU->regAlloc, ovf);
}

/* MADDA/MSUBA Opcodes */
static void mVU_FMACb(microVU* mVU, int recPass, int opCase, int opType, int clampType)
{
	pass1 { setupPass1(mVU, opCase, 1, 0); }
	pass2
	{
		int Fs;
		int Ft;
		int ACC;
		int tempFt;
		mVU->ovfDetectOK = true;
		mVU->ovfUseEventMask = (CHECK_VU_ACC_ADDSUB && CHECK_VU_EXACTMUL);
		setupFtReg(mVU, &Ft, &tempFt, opCase, clampType);

		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, _X_Y_Z_W, 1);
		ACC = mVUra_allocReg(mVU->regAlloc, 32, 32, 0xf, 0);

		if (_XYZW_SS2)
			xe_pshufd_xxi(ACC, ACC, shuffleSS(_X_Y_Z_W));

		if ((clampType & cFt) && !CHECK_VU_EXACTMUL) mVUclamp2(mVU, Ft, -1, _X_Y_Z_W, 0);
		if ((clampType & cFs) && !CHECK_VU_EXACTMUL) mVUclamp2(mVU, Fs, -1, _X_Y_Z_W, 0);

		if (!CHECK_VU_EXACTMUL || !mVUexactMulVF0(mVU, Fs, opCase, _XYZW_SS))
		{
			if (_XYZW_SS) SSE_SS[2](mVU, Fs, Ft, -1, -1);
			else          SSE_PS[2](mVU, Fs, Ft, -1, -1);
		}

		if (_XYZW_SS || _X_Y_Z_W == 0xf)
		{
			const int fov = mVUfusedOvfPre(mVU, Fs, opType == 1);
			if (_XYZW_SS) SSE_SS[opType](mVU, ACC, Fs, tempFt, -1);
			else          SSE_PS[opType](mVU, ACC, Fs, tempFt, -1);
			mVUfusedOvfPost(mVU, ACC, fov);
			mVUupdateFlags(mVU, ACC, Fs, tempFt, 1);
			if (_XYZW_SS && _X_Y_Z_W != 8)
				xe_pshufd_xxi(ACC, ACC, shuffleSS(_X_Y_Z_W));
		}
		else
		{
			const int tempACC = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
			const int fov = mVUfusedOvfPre(mVU, Fs, opType == 1);
			xe_movaps_xx(tempACC, ACC);
			SSE_PS[opType](mVU, tempACC, Fs, tempFt, -1);
			mVUfusedOvfPost(mVU, tempACC, fov);
			mVUmergeRegs(ACC, tempACC, _X_Y_Z_W, 0);
			mVUupdateFlags(mVU, ACC, Fs, tempFt, 1);
			mVUra_clearNeededXMM(mVU->regAlloc, tempACC);
		}

		mVUra_clearNeededXMM(mVU->regAlloc, ACC);
		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

/* MADD Opcodes */
static void mVU_FMACc(microVU* mVU, int recPass, int opCase, int clampType)
{
	pass1 { setupPass1(mVU, opCase, 0, 0); }
	pass2
	{
		int Fs;
		int Ft;
		int ACC;
		int tempFt;
		mVU->ovfDetectOK = true;
		mVU->ovfUseEventMask = (CHECK_VU_ACC_ADDSUB && CHECK_VU_EXACTMUL);
		setupFtReg(mVU, &Ft, &tempFt, opCase, clampType);

		ACC = mVUra_allocReg(mVU->regAlloc, 32, -1, 0, 1);
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _Fd_, _X_Y_Z_W, 1);

		if (_XYZW_SS2)
			xe_pshufd_xxi(ACC, ACC, shuffleSS(_X_Y_Z_W));

		if ((clampType & cFt) && !CHECK_VU_EXACTMUL)  mVUclamp2(mVU, Ft,  -1, _X_Y_Z_W, 0);
		if ((clampType & cFs) && !CHECK_VU_EXACTMUL)  mVUclamp2(mVU, Fs,  -1, _X_Y_Z_W, 0);
		if (clampType & cACC) mVUclamp2(mVU, ACC, -1, _X_Y_Z_W, 0);


		if (!CHECK_VU_EXACTMUL || !mVUexactMulVF0(mVU, Fs, opCase, _XYZW_SS))
		{
			if (_XYZW_SS) SSE_SS[2](mVU, Fs, Ft, -1, -1);
			else          SSE_PS[2](mVU, Fs, Ft, -1, -1);
		}
		{
			const int fov = mVUfusedOvfPre(mVU, Fs, false);
			if (_XYZW_SS) SSE_SS[0](mVU, Fs, ACC, tempFt, -1);
			else          SSE_PS[0](mVU, Fs, ACC, tempFt, -1);
			mVUfusedOvfPost(mVU, Fs, fov);
		}

		if (_XYZW_SS2)
			xe_pshufd_xxi(ACC, ACC, shuffleSS(_X_Y_Z_W));

		mVUupdateFlags(mVU, Fs, tempFt, -1, 1);

		mVUra_clearNeededXMM(mVU->regAlloc, Fs); /* Always Clear Written Reg First */
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
		mVUra_clearNeededXMM(mVU->regAlloc, ACC);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

/* MSUB Opcodes */
static void mVU_FMACd(microVU* mVU, int recPass, int opCase, int clampType)
{
	pass1 { setupPass1(mVU, opCase, 0, 0); }
	pass2
	{
		int Fs;
		int Ft;
		int Fd;
		int tempFt;
		mVU->ovfDetectOK = true;
		mVU->ovfUseEventMask = (CHECK_VU_ACC_ADDSUB && CHECK_VU_EXACTMUL);
		setupFtReg(mVU, &Ft, &tempFt, opCase, clampType);

		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_,  0, _X_Y_Z_W, 1);
		Fd = mVUra_allocReg(mVU->regAlloc, 32, _Fd_, _X_Y_Z_W, 1);

		if ((clampType & cFt) && !CHECK_VU_EXACTMUL)  mVUclamp2(mVU, Ft, -1, _X_Y_Z_W, 0);
		if ((clampType & cFs) && !CHECK_VU_EXACTMUL)  mVUclamp2(mVU, Fs, -1, _X_Y_Z_W, 0);
		if (clampType & cACC) mVUclamp2(mVU, Fd, -1, _X_Y_Z_W, 0);

		if (!CHECK_VU_EXACTMUL || !mVUexactMulVF0(mVU, Fs, opCase, _XYZW_SS))
		{
			if (_XYZW_SS) SSE_SS[2](mVU, Fs, Ft, -1, -1);
			else          SSE_PS[2](mVU, Fs, Ft, -1, -1);
		}
		{
			const int fov = mVUfusedOvfPre(mVU, Fs, true);
			if (_XYZW_SS) SSE_SS[1](mVU, Fd, Fs, tempFt, -1);
			else          SSE_PS[1](mVU, Fd, Fs, tempFt, -1);
			mVUfusedOvfPost(mVU, Fd, fov);
		}

		mVUupdateFlags(mVU, Fd, Fs, tempFt, 1);

		mVUra_clearNeededXMM(mVU->regAlloc, Fd); /* Always Clear Written Reg First */
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

/* ABS Opcode */
mVUop(mVU_ABS)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		int Fs;
		if (!_Ft_)
			return;
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));
		xe_andps_xm(Fs, mVUglob.absclip);
		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
	}
}

/* OPMULA Opcode */
mVUop(mVU_OPMULA)
{
	pass1 { mVUanalyzeFMAC1(mVU, 0, _Fs_, _Ft_); }
	pass2
	{
		int Ft;
		int Fs;
		mVU->ovfDetectOK = true;
		mVU->ovfUseEventMask = (CHECK_VU_ACC_ADDSUB && CHECK_VU_EXACTMUL);
		Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, _X_Y_Z_W, 1);
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 32, _X_Y_Z_W, 1);

		xe_pshufd_xxi(Fs, Fs, 0xC9); /* WXZY */
		xe_pshufd_xxi(Ft, Ft, 0xD2); /* WYXZ */
		SSE_MULPS(mVU, Fs, Ft, -1, -1);
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
		mVUupdateFlags(mVU, Fs, -1, -1, 1);
		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

/* OPMSUB Opcode */
mVUop(mVU_OPMSUB)
{
	pass1 { mVUanalyzeFMAC1(mVU, _Fd_, _Fs_, _Ft_); }
	pass2
	{
		int Ft;
		int Fs;
		int ACC;
		mVU->ovfDetectOK = true;
		mVU->ovfUseEventMask = (CHECK_VU_ACC_ADDSUB && CHECK_VU_EXACTMUL);
		Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, 0xf, 1);
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, 0xf, 1);
		ACC = mVUra_allocReg(mVU->regAlloc, 32, _Fd_, _X_Y_Z_W, 1);

		xe_pshufd_xxi(Fs, Fs, 0xC9); /* WXZY */
		xe_pshufd_xxi(Ft, Ft, 0xD2); /* WYXZ */
		SSE_MULPS(mVU, Fs,  Ft, -1, -1);
		SSE_SUBPS(mVU, ACC, Fs, -1, -1);
		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
		mVUupdateFlags(mVU, ACC, -1, -1, 1);
		mVUra_clearNeededXMM(mVU->regAlloc, ACC);
	}
	pass4 { mVUregs.needExactMatch |= 8; }
}

/* FTOI0/FTIO4/FTIO12/FTIO15 Opcodes */
static void mVU_FTOIx(mP, const float* addr)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		int Fs;
		int t1;
		if (!_Ft_)
			return;
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));
		t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);

		/* cvttps2dq returns 0x8000000 for any unrepresentable values.
		 * We want it to return 0x8000000 for negative and 0x7fffffff for positive.
		 * So for unrepresentable positive values, xor with 0xffffffff to 
		 * turn 0x80000000 into 0x7fffffff. */
		if (addr)
			xe_mulps_xm(Fs, addr);
		xe_movaps_xx(t1, Fs);
		xe_pcmpgtd_xm(t1, mVUglob.I32MAXF);
		xe_cvttps2dq_xx(Fs, Fs);
		xe_pxor_xx(Fs, t1);

		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
		mVUra_clearNeededXMM(mVU->regAlloc, t1);
	}
}

/* ITOF0/ITOF4/ITOF12/ITOF15 Opcodes */
static void mVU_ITOFx(mP, const float* addr)
{
	pass1 { mVUanalyzeFMAC2(mVU, _Fs_, _Ft_); }
	pass2
	{
		int Fs;
		if (!_Ft_)
			return;
		Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, _Ft_, _X_Y_Z_W, !((_Fs_ == _Ft_) && (_X_Y_Z_W == 0xf)));

		xe_cvtdq2ps_xx(Fs, Fs);
		if (addr)
			xe_mulps_xm(Fs, addr);
		/* mVUclamp2(Fs, xmmT1.Id, 15); // Clamp (not sure if this is needed) */

		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
	}
}

/* Clip Opcode */
mVUop(mVU_CLIP)
{
	pass1 { mVUanalyzeFMAC4(mVU, _Fs_, _Ft_); }
	pass2
	{
		const int Fs = mVUra_allocReg(mVU->regAlloc, _Fs_, 0, 0xf, 1);
		const int Ft = mVUra_allocReg(mVU->regAlloc, _Ft_, 0, 0x1, 1);
		const int t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);

		mVUunpack_xyzw(Ft, Ft, 0);
		mVUallocCFLAGa(mVU, gprT1, cFLAG.lastWrite);
		xe_shl32_ri(gprT1, 6);

		xe_movaps_xm(t1, mVUglob.exponent);
		xe_pand_xx(t1, Fs);
		xe_pxor_xx(t2, t2);
		xe_pcmpeqd_xx(t1, t2); /* Denormal check */
		xe_pandn_xx(t1, Fs); /* If denormal, set to zero, which can't be greater than any nonnegative denormal in Ft */
		xe_pand_xm(Ft, mVUglob.absclip);

		xe_movaps_xm(Fs, mVUglob.signbit);
		xe_pxor_xx(Fs, t1); /* Negate */
		xe_pcmpgtd_xx(t1, Ft); /* +w, +z, +y, +x */
		xe_pcmpgtd_xx(Fs, Ft); /* -w, -z, -y, -x */

		xe_pblendw_xxi(Fs, t1, 0x55); /* Squish together */
		xe_packsswb_xx(Fs, Fs);       /* Convert u16 to u8 */
		xe_pmovmskb_rx(gprT2, Fs);    /* Get bitmask */
		xe_and32_ri(gprT2, 0x3f);  /* Mask unused stuff */
		xe_and32_ri(gprT1, 0xffffff);
		xe_or32_rr(gprT1, gprT2);

		mVUallocCFLAGb(mVU, gprT1, cFLAG.write);
		mVUra_clearNeededXMM(mVU->regAlloc, Fs);
		mVUra_clearNeededXMM(mVU->regAlloc, Ft);
		mVUra_clearNeededXMM(mVU->regAlloc, t1);
		mVUra_clearNeededXMM(mVU->regAlloc, t2);
	}
}

/* ------------------------------------------------------------------ */
/* Micro VU Micromode Upper instructions */
/* ------------------------------------------------------------------ */

mVUop(mVU_ADD)    { mVU_FMACa(mVU, recPass, 1, 0, 0,    0);  }
mVUop(mVU_ADDi)   { mVU_FMACa(mVU, recPass, 3, 5, 0,   0);  }
mVUop(mVU_ADDq)   { mVU_FMACa(mVU, recPass, 4, 0, 0,   0);  }
mVUop(mVU_ADDx)   { mVU_FMACa(mVU, recPass, 2, 0, 0,   0);  }
mVUop(mVU_ADDy)   { mVU_FMACa(mVU, recPass, 2, 0, 0,   0);  }
mVUop(mVU_ADDz)   { mVU_FMACa(mVU, recPass, 2, 0, 0,   0);  }
mVUop(mVU_ADDw)   { mVU_FMACa(mVU, recPass, 2, 0, 0,   0);  }
mVUop(mVU_ADDA)   { mVU_FMACa(mVU, recPass, 1, 0, 1,   0);  }
mVUop(mVU_ADDAi)  { mVU_FMACa(mVU, recPass, 3, 0, 1,  0);  }
mVUop(mVU_ADDAq)  { mVU_FMACa(mVU, recPass, 4, 0, 1,  0);  }
mVUop(mVU_ADDAx)  { mVU_FMACa(mVU, recPass, 2, 0, 1,  0);  }
mVUop(mVU_ADDAy)  { mVU_FMACa(mVU, recPass, 2, 0, 1,  0);  }
mVUop(mVU_ADDAz)  { mVU_FMACa(mVU, recPass, 2, 0, 1,  0);  }
mVUop(mVU_ADDAw)  { mVU_FMACa(mVU, recPass, 2, 0, 1,  0);  }
mVUop(mVU_SUB)    { mVU_FMACa(mVU, recPass, 1, 1, 0,  (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBi)   { mVU_FMACa(mVU, recPass, 3, 1, 0, (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBq)   { mVU_FMACa(mVU, recPass, 4, 1, 0, (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBx)   { mVU_FMACa(mVU, recPass, 2, 1, 0, (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBy)   { mVU_FMACa(mVU, recPass, 2, 1, 0, (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBz)   { mVU_FMACa(mVU, recPass, 2, 1, 0, (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBw)   { mVU_FMACa(mVU, recPass, 2, 1, 0, (_XYZW_PS)?(cFs|cFt):0);   } /* Clamp (Kingdom Hearts I (VU0)) */
mVUop(mVU_SUBA)   { mVU_FMACa(mVU, recPass, 1, 1, 1,   0);  }
mVUop(mVU_SUBAi)  { mVU_FMACa(mVU, recPass, 3, 1, 1,  0);  }
mVUop(mVU_SUBAq)  { mVU_FMACa(mVU, recPass, 4, 1, 1,  0);  }
mVUop(mVU_SUBAx)  { mVU_FMACa(mVU, recPass, 2, 1, 1,  0);  }
mVUop(mVU_SUBAy)  { mVU_FMACa(mVU, recPass, 2, 1, 1,  0);  }
mVUop(mVU_SUBAz)  { mVU_FMACa(mVU, recPass, 2, 1, 1,  0);  }
mVUop(mVU_SUBAw)  { mVU_FMACa(mVU, recPass, 2, 1, 1,  0);  }
mVUop(mVU_MUL)    { mVU_FMACa(mVU, recPass, 1, 2, 0,  (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (VU0)) */
mVUop(mVU_MULi)   { mVU_FMACa(mVU, recPass, 3, 2, 0, (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (VU0)) */
mVUop(mVU_MULq)   { mVU_FMACa(mVU, recPass, 4, 2, 0, (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (VU0)) */
mVUop(mVU_MULx)   { mVU_FMACa(mVU, recPass, 2, 2, 0, (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (vu0)) */
mVUop(mVU_MULy)   { mVU_FMACa(mVU, recPass, 2, 2, 0, (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (VU0)) */
mVUop(mVU_MULz)   { mVU_FMACa(mVU, recPass, 2, 2, 0, (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (VU0)) */
mVUop(mVU_MULw)   { mVU_FMACa(mVU, recPass, 2, 2, 0, (_XYZW_PS)?(cFs|cFt):cFs); } /* Clamp (TOTA, DoM, Ice Age (VU0)) */
mVUop(mVU_MULA)   { mVU_FMACa(mVU, recPass, 1, 2, 1,   0);  }
mVUop(mVU_MULAi)  { mVU_FMACa(mVU, recPass, 3, 2, 1,  0);  }
mVUop(mVU_MULAq)  { mVU_FMACa(mVU, recPass, 4, 2, 1,  0);  }
mVUop(mVU_MULAx)  { mVU_FMACa(mVU, recPass, 2, 2, 1,  cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MULAy)  { mVU_FMACa(mVU, recPass, 2, 2, 1,  cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MULAz)  { mVU_FMACa(mVU, recPass, 2, 2, 1,  cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MULAw)  { mVU_FMACa(mVU, recPass, 2, 2, 1, (_XYZW_PS) ? (cFs | cFt) : cFs); } /* Clamp (TOTA, DoM, ...)- Ft for Superman - Shadow Of Apokolips */
mVUop(mVU_MADD)   { mVU_FMACc(mVU, recPass, 1,   0); }
mVUop(mVU_MADDi)  { mVU_FMACc(mVU, recPass, 3,  0); }
mVUop(mVU_MADDq)  { mVU_FMACc(mVU, recPass, 4,  0); }
mVUop(mVU_MADDx)  { mVU_FMACc(mVU, recPass, 2,  cFs); } /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MADDy)  { mVU_FMACc(mVU, recPass, 2,  cFs); } /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MADDz)  { mVU_FMACc(mVU, recPass, 2,  cFs); } /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MADDw)  { mVU_FMACc(mVU, recPass, 2, (isCOP2)?(cACC|cFt|cFs):cFs);} /* Clamp (ICO (COP2), TOTA, DoM) */
mVUop(mVU_MADDA)  { mVU_FMACb(mVU, recPass, 1, 0, 0);  }
mVUop(mVU_MADDAi) { mVU_FMACb(mVU, recPass, 3, 0, 0);  }
mVUop(mVU_MADDAq) { mVU_FMACb(mVU, recPass, 4, 0, 0);  }
mVUop(mVU_MADDAx) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MADDAy) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MADDAz) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MADDAw) { mVU_FMACb(mVU, recPass, 2, 0, cFs);} /* Clamp (TOTA, DoM, ...) */
mVUop(mVU_MSUB)   { mVU_FMACd(mVU, recPass, 1, (isCOP2) ? cFs : 0); } /* Clamp ( Superman - Shadow Of Apokolips) */
mVUop(mVU_MSUBi)  { mVU_FMACd(mVU, recPass, 3, 0);  }
mVUop(mVU_MSUBq)  { mVU_FMACd(mVU, recPass, 4, 0);  }
mVUop(mVU_MSUBx)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBy)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBz)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBw)  { mVU_FMACd(mVU, recPass, 2, 0);  }
mVUop(mVU_MSUBA)  { mVU_FMACb(mVU, recPass, 1, 1, 0);  }
mVUop(mVU_MSUBAi) { mVU_FMACb(mVU, recPass, 3, 1, 0);  }
mVUop(mVU_MSUBAq) { mVU_FMACb(mVU, recPass, 4, 1, 0);  }
mVUop(mVU_MSUBAx) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MSUBAy) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MSUBAz) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MSUBAw) { mVU_FMACb(mVU, recPass, 2, 1, 0);  }
mVUop(mVU_MAX)    { mVU_FMACa(mVU, recPass, 1, 3, 0,    0);  }
mVUop(mVU_MAXi)   { mVU_FMACa(mVU, recPass, 3, 3, 0,   0);  }
mVUop(mVU_MAXx)   { mVU_FMACa(mVU, recPass, 2, 3, 0,   0);  }
mVUop(mVU_MAXy)   { mVU_FMACa(mVU, recPass, 2, 3, 0,   0);  }
mVUop(mVU_MAXz)   { mVU_FMACa(mVU, recPass, 2, 3, 0,   0);  }
mVUop(mVU_MAXw)   { mVU_FMACa(mVU, recPass, 2, 3, 0,   0);  }
mVUop(mVU_MINI)   { mVU_FMACa(mVU, recPass, 1, 4, 0,   0);  }
mVUop(mVU_MINIi)  { mVU_FMACa(mVU, recPass, 3, 4, 0,  0);  }
mVUop(mVU_MINIx)  { mVU_FMACa(mVU, recPass, 2, 4, 0,  0);  }
mVUop(mVU_MINIy)  { mVU_FMACa(mVU, recPass, 2, 4, 0,  0);  }
mVUop(mVU_MINIz)  { mVU_FMACa(mVU, recPass, 2, 4, 0,  0);  }
mVUop(mVU_MINIw)  { mVU_FMACa(mVU, recPass, 2, 4, 0,  0);  }
mVUop(mVU_FTOI0)  { mVU_FTOIx(mX, NULL);      }
mVUop(mVU_FTOI4)  { mVU_FTOIx(mX, mVUglob.FTOI_4);      }
mVUop(mVU_FTOI12) { mVU_FTOIx(mX, mVUglob.FTOI_12);     }
mVUop(mVU_FTOI15) { mVU_FTOIx(mX, mVUglob.FTOI_15);     }
mVUop(mVU_ITOF0)  { mVU_ITOFx(mX, NULL);      }
mVUop(mVU_ITOF4)  { mVU_ITOFx(mX, mVUglob.ITOF_4);      }
mVUop(mVU_ITOF12) { mVU_ITOFx(mX, mVUglob.ITOF_12);     }
mVUop(mVU_ITOF15) { mVU_ITOFx(mX, mVUglob.ITOF_15);     }
mVUop(mVU_NOP)    { }
