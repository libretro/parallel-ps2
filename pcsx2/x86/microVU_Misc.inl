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

//------------------------------------------------------------------
// Micro VU - Reg Loading/Saving/Shuffling/Unpacking/Merging...
//------------------------------------------------------------------

void mVUunpack_xyzw(int dstreg, int srcreg, int xyzw)
{
	switch (xyzw)
	{
		case 0: xe_pshufd_xxi(dstreg, srcreg, 0x00); break; // XXXX
		case 1: xe_pshufd_xxi(dstreg, srcreg, 0x55); break; // YYYY
		case 2: xe_pshufd_xxi(dstreg, srcreg, 0xaa); break; // ZZZZ
		case 3: xe_pshufd_xxi(dstreg, srcreg, 0xff); break; // WWWW
	}
}

void mVUloadReg(int reg, struct e_mem ptr, int xyzw)
{
	switch (xyzw)
	{
		case 8:  { struct e_mem xm = e_mem_off(ptr, 0); xe_movss_xmemg(reg, xm); } break; // X
		case 4:  { struct e_mem xm = e_mem_off(ptr, 4); xe_movss_xmemg(reg, xm); } break; // Y
		case 2:  { struct e_mem xm = e_mem_off(ptr, 8); xe_movss_xmemg(reg, xm); } break; // Z
		case 1:  { struct e_mem xm = e_mem_off(ptr, 12); xe_movss_xmemg(reg, xm); } break; // W
		default: { struct e_mem xm = e_mem_off(ptr, 0); xe_movaps_xmemg(reg, xm); } break;
	}
}

// Modifies the Source Reg!
void mVUsaveReg(int reg, struct e_mem ptr, int xyzw, int modXYZW)
{
	switch (xyzw)
	{
		case 5: // YW
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_extractps_memxi(xm, reg, 1); }
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_extractps_memxi(xm, reg, 3); }
			break;
		case 6: // YZ
			xe_pshufd_xxi(reg, reg, 0xc9);
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_movlps_memxg(xm, reg); }
			break;
		case 7: // YZW
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movhps_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_extractps_memxi(xm, reg, 1); }
			break;
		case 9: // XW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_extractps_memxi(xm, reg, 3); }
			break;
		case 10: // XZ
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_extractps_memxi(xm, reg, 2); }
			break;
		case 11: // XZW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movhps_memxg(xm, reg); }
			break;
		case 13: // XYW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movlps_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_extractps_memxi(xm, reg, 3); }
			break;
		case 14: // XYZ
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movlps_memxg(xm, reg); }
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_extractps_memxi(xm, reg, 2); }
			break;
		case 4: // Y
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 1);
			{ struct e_mem xm = e_mem_off(ptr, 4); xe_movss_memxg(xm, reg); }
			break;
		case 2: // Z
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 2);
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movss_memxg(xm, reg); }
			break;
		case 1: // W
			if (!modXYZW)
				mVUunpack_xyzw(reg, reg, 3);
			{ struct e_mem xm = e_mem_off(ptr, 12); xe_movss_memxg(xm, reg); }
			break;
		case 8: // X
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movss_memxg(xm, reg); }
			break;
		case 12: // XY
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movlps_memxg(xm, reg); }
			break;
		case 3: // ZW
			{ struct e_mem xm = e_mem_off(ptr, 8); xe_movhps_memxg(xm, reg); }
			break;
		default: // XYZW
			{ struct e_mem xm = e_mem_off(ptr, 0); xe_movaps_memxg(xm, reg); }
			break;
	}
}

// Modifies the Source Reg! (ToDo: Optimize modXYZW = 1 cases)
void mVUmergeRegs(int dest, int src, int xyzw, int modXYZW)
{
	xyzw &= 0xf;
	if ((dest != src) && (xyzw != 0))
	{
		if (xyzw == 0x8)
			xe_movss_xx(dest, src);
		else if (xyzw == 0xf)
			xe_movaps_xx(dest, src);
		else
		{
			if (modXYZW)
			{
				if      (xyzw == 1) { xe_insertps_xxi(dest, src, _MM_MK_INSERTPS_NDX(0, 3, 0)); return; }
				else if (xyzw == 2) { xe_insertps_xxi(dest, src, _MM_MK_INSERTPS_NDX(0, 2, 0)); return; }
				else if (xyzw == 4) { xe_insertps_xxi(dest, src, _MM_MK_INSERTPS_NDX(0, 1, 0)); return; }
			}
			xyzw = ((xyzw & 1) << 3) | ((xyzw & 2) << 1) | ((xyzw & 4) >> 1) | ((xyzw & 8) >> 3);
			xe_blendps_xxi(dest, src, xyzw);
		}
	}
}

//------------------------------------------------------------------
// Micro VU - Misc Functions
//------------------------------------------------------------------

// Backup Volatile Regs (EAX, ECX, EDX, MM0~7, XMM0~7, are all volatile according to 32bit Win/Linux ABI)
static void mVUexactDivC(microVU* mVU)
{
	const u32 a = mVU->exactDivBuf[0];
	const u32 b = mVU->exactDivBuf[1];
	const u32 op = mVU->exactDivBuf[2];
	const ps2f_u64 r = (op == 0) ? ps2f_div(a, b)
	                 : (op == 1) ? ps2f_sqrt(b)
	                             : ps2f_rsqrt(a, b);
	mVU->exactDivBuf[0] = ps2f_raw(r);
	mVU->divFlag = (r & PS2F_IV) ? divI : ((r & PS2F_DZ) ? divD : 0);
}
static void mVUexactDivVU0() { mVUexactDivC(&microVU0); }
static void mVUexactDivVU1() { mVUexactDivC(&microVU1); }

static __fi void mVUbackupRegs(microVU* mVU, int toMemory, int onlyNeeded)
{
	if (toMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		for (int i = 0; i < (int)(iREGCNT_GPR); i++)
		{
			if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
				continue;

			if (!onlyNeeded || mVUra_checkCachedGPR(mVU->regAlloc, i))
			{
				num_gprs++;
				xe_push64_r(i);
			}
		}

		u32 save_xmms = 0; /* bit per xmm */
		for (int i = 0; i < (int)(iREGCNT_XMM); i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			if (!onlyNeeded || mVUra_checkCachedReg(mVU->regAlloc, i) || xmmPQ == i)
			{
				save_xmms |= (1u << i);
				num_xmms++;
			}
		}

		// we need 16 byte alignment on the stack
#ifdef _WIN32
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64)) + 32;
		int stack_offset = 32;
#else
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64));
		int stack_offset = 0;
#endif
		if (stack_size > 0)
		{
			xe_sub64_ri(XE_SP, stack_size);
			for (int i = 0; i < (int)(iREGCNT_XMM); i++)
			{
				if ((save_xmms & (1u << i)))
				{
					{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_memxg(xm, i); }
					stack_offset += sizeof(u128);
				}
			}
		}
	}
	else
	{
		// TODO(Stenzek): get rid of xmmbackup
		mVUra_flushAll(mVU->regAlloc, 1); // Flush Regalloc
		xe_movaps_mx(&mVU->xmmBackup[xmmPQ][0], xmmPQ);
	}
}

/* Restore Volatile Regs */
static __fi void mVUrestoreRegs(microVU* mVU, int fromMemory, int onlyNeeded)
{
	if (fromMemory)
	{
		int num_xmms = 0, num_gprs = 0;

		u32 save_gprs = 0; /* bit per gpr */
		for (int i = 0; i < (int)(iREGCNT_GPR); i++)
		{
			if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
				continue;

			if (!onlyNeeded || mVUra_checkCachedGPR(mVU->regAlloc, i))
			{
				save_gprs |= (1u << i);
				num_gprs++;
			}
		}

		u32 save_xmms = 0; /* bit per xmm */
		for (int i = 0; i < (int)(iREGCNT_XMM); i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			if (!onlyNeeded || mVUra_checkCachedReg(mVU->regAlloc, i) || xmmPQ == i)
			{
				save_xmms |= (1u << i);
				num_xmms++;
			}
		}

#ifdef _WIN32
		const int stack_extra = 32;
#else
		const int stack_extra = 0;
#endif
		const int stack_size = (num_xmms * sizeof(u128)) + ((num_gprs & 1) * sizeof(u64)) + stack_extra;
		if (num_xmms > 0)
		{
			int stack_offset = (num_xmms - 1) * sizeof(u128) + stack_extra;
			for (int i = (int)(iREGCNT_XMM - 1); i >= 0; i--)
			{
				if (!(save_xmms & (1u << i)))
					continue;

				{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_xmemg(i, xm); }
				stack_offset -= sizeof(u128);
			}
		}
		if (stack_size > 0)
			xe_add64_ri(XE_SP, stack_size);

		for (int i = (int)(iREGCNT_GPR - 1); i >= 0; i--)
		{
			if ((save_gprs & (1u << i)))
				xe_pop64_r(i);
		}
	}
	else
	{
		xe_movaps_xm(xmmPQ, &mVU->xmmBackup[xmmPQ][0]);
	}
}

static void mVUTBit(void)
{
	retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagVUTBit); /* acq_rel >= release */
}

static void mVUEBit(void)
{
	retro_atomic_fetch_or_int(&vu1Thread.mtvuInterrupts, VU_Thread::InterruptFlagVUEBit); /* acq_rel >= release */
}

static inline u32 branchAddr(const mV)
{
	return ((((iPC + 2) + (_Imm11_ * 2)) & mVU->progMemMask) * 4);
}

static void mVUwaitMTVU(void) { mtvu_wait_vu(); }

/* Transforms the Address in gprReg to valid VU0/VU1 Address */
static __fi void mVUaddrFix(mV, int gprReg)
{
	if (isVU1)
	{
		xe_and32_ri(gprReg, 0x3ff); // wrap around
		xe_shl32_ri(gprReg, 4);
	}
	else
	{
		xe_test32_ri(gprReg, 0x400);
		uint8_t* jmpA; xe_fwd_jcc8(Jcc_NotZero, jmpA); // if addr & 0x4000, reads VU1's VF regs and VI regs
			xe_and32_ri(gprReg, 0xff); // if !(addr & 0x4000), wrap around
			uint8_t* jmpB; xe_fwd_jcc32(Jcc_Unconditional, jmpB);
		xe_fwd_set8(jmpA);
			if (THREAD_VU1)
			{
				xe_fastcall0(mVU->waitMTVU);
			}
			xe_and32_ri(gprReg, 0x3f); // ToDo: theres a potential problem if VU0 overrides VU1's VF0/VI0 regs!
			xe_add64_ri(gprReg, (u128*)vuRegs[1].VF - (u128*)vuRegs[0].Mem);
		xe_fwd_set32(jmpB);
		xe_shl64_ri(gprReg, 4); // multiply by 16 (shift left by 4)
	}
}

static __fi struct e_memopt mVUoptimizeConstantAddr(mV, u32 srcreg, s32 offset, s32 offsetSS_)
{
	/* The comment below asked for VI const prop; the tracker in the
	 * compile pass now provides it, so a known source register folds
	 * to an absolute address exactly like vi00 always has. The sum
	 * and masking replicate the runtime path bit for bit: the VI is
	 * a zero-extended sixteen-bit value, the offset a sign-extended
	 * immediate, and the wrap masks are applied to the same sum. */
	/* COP2 macro mode never consults the tracker: mVUconstReg is
	 * micro-program compile state, wiped at each micro compile's start
	 * but left populated at its end, so a macro LQI/SQI/LQD/SQD/ILWR/
	 * ISWR compiling after a micro block would fold its address to a
	 * constant from that block's compile-time tracking -- a value with
	 * no relation to the EE-visible VI the macro op actually reads.
	 * This is the read-side mirror of the setConstReg cop2 guard (the
	 * Star Ocean 3 write-side fix); both directions of macro/micro
	 * tracker sharing are now closed. The vi00 fold (srcreg == 0) is a
	 * true constant in every mode and stays. */
	if (srcreg != 0 && (mVU->cop2 || !(doViConstProp && srcreg < 16 && mVUconstReg[srcreg].isValid)))
		return e_memopt_none();
	const s32 addr = (s32)(srcreg ? (u32)(u16)mVUconstReg[srcreg].regValue : 0u) + offset;
	if (isVU1)
		return e_memopt_of(e_mem_abs(vuRegs[mVU->index].Mem + ((addr & 0x3FFu) << 4) + offsetSS_));
	if (addr & 0x400)
		return e_memopt_none();
	return e_memopt_of(e_mem_abs(vuRegs[mVU->index].Mem + ((addr & 0xFFu) << 4) + offsetSS_));
}

//------------------------------------------------------------------
// Micro VU - Custom SSE Instructions
//------------------------------------------------------------------

struct SSEMasks
{
	u32 MIN_MAX_1[4], MIN_MAX_2[4], ADD_SS[4];
};

alignas(16) static const SSEMasks sseMasks =
{
	{0xffffffff, 0x80000000, 0xffffffff, 0x80000000},
	{0x00000000, 0x40000000, 0x00000000, 0x40000000},
	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
};


// Warning: Modifies t1 and t2
void MIN_MAX_PS(microVU* mVU, int to, int from, int t1in, int t2in, int min)
{
	int t1 = (t1in < 0) ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : t1in;
	int t2 = (t2in < 0) ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : t2in;

	/* Use integer comparison */
	{
		int c1 = min ? t2 : t1;
		int c2 = min ? t1 : t2;

		xe_movaps_xx(t1, to);
		xe_psrad_xi(t1, 31);
		xe_psrld_xi(t1, 1);
		xe_pxor_xx(t1, to);

		xe_movaps_xx(t2, from);
		xe_psrad_xi(t2, 31);
		xe_psrld_xi(t2, 1);
		xe_pxor_xx(t2, from);

		xe_pcmpgtd_xx(c1, c2);
		xe_pand_xx(to, c1);
		xe_pandn_xx(c1, from);
		xe_por_xx(to, c1);
	}

	if (t1 != t1in) mVUra_clearNeededXMM(mVU->regAlloc, t1);
	if (t2 != t2in) mVUra_clearNeededXMM(mVU->regAlloc, t2);
}

// Warning: Modifies to's upper 3 vectors, and t1
void MIN_MAX_SS(mV, int to, int from, int t1in, int min)
{
	int t1 = (t1in < 0) ? mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1) : t1in;
	xe_shufps_xxi(to, from, 0);
	xe_pand_xm(to, sseMasks.MIN_MAX_1);
	xe_por_xm(to, sseMasks.MIN_MAX_2);
	xe_pshufd_xxi(t1, to, 0xee);
	if (min) xe_minpd_xx(to, t1);
	else	 xe_maxpd_xx(to, t1);
	if (t1 != t1in)
		mVUra_clearNeededXMM(mVU->regAlloc, t1);
}

// Turns out only this is needed to get TriAce games booting with mVU
// Modifies from's lower vector

//------------------------------------------------------------------
// Exponent-distance operand masking for VU ADD/SUB. The hardware's
// adder truncates the smaller operand's low bits by exponent distance
// before adding; masking those bits and then doing the IEEE add under
// the VU's chop rounding reproduces it. Measured against the ps2float
// model, the plain clamped addps was wrong on 44.9% of random operand
// pairs and the masked add on 0.79% - the remainder is the
// exponent-255 range single precision cannot represent. The per-lane
// variable mask has no SSE2 shift, so 2^(d-1) comes from building the
// float with exponent field d-1 and truncating it back to integer;
// the identity against the scalar mask measures zero mismatches over
// 16.7 million lanes across every exponent distance. Both stages are
// AND-chains: OR-ing the sign bit into the keep mask makes the
// distance >= 25 stage safe even where the power construction
// overflows, so no blend and no extra register.
// a and b are masked in place; t1..t3 are scratch.
//------------------------------------------------------------------
static void mVUmaskAddSub(mV, int a, int b, int t1, int t2, int t3)
{
	// t1 = exponent difference d = exp(a) - exp(b), per lane
	xe_movaps_xx(t1, a);
	xe_pand_xm(t1, mVUglob.exponent);
	xe_psrld_xi(t1, 23);
	xe_movaps_xx(t2, b);
	xe_pand_xm(t2, mVUglob.exponent);
	xe_psrld_xi(t2, 23);
	xe_psubd_xx(t1, t2);

	// ---- b side: b &= (keep' | (d <= 0)) ; b &= (sign | (d <= 24)) ----
	xe_pcmpeqd_xx(t2, t2);            // -1
	xe_paddd_xx(t2, t1);              // d - 1
	xe_pslld_xi(t2, 23);
	xe_paddd_xm(t2, mVUglob.one);     // float with exponent d-1
	xe_cvttps2dq_xx(t2, t2);          // 2^(d-1)
	xe_psubd_xm(t2, mVUglob.addm1);   // low mask
	xe_pxor_xm(t2, mVUglob.addmall);  // keep = ~low
	xe_por_xm(t2, mVUglob.signbit);   // keep' (sign always survives)
	xe_movaps_xm(t3, mVUglob.addm1);
	xe_pcmpgtd_xx(t3, t1);            // 1 > d  <=>  d <= 0
	xe_por_xx(t2, t3);
	xe_pand_xx(b, t2);
	xe_movaps_xm(t3, mVUglob.addm25);
	xe_pcmpgtd_xx(t3, t1);            // 25 > d  <=>  d <= 24: distance 24 keeps bit 23
	xe_por_xm(t3, mVUglob.signbit);
	xe_pand_xx(b, t3);

	// ---- a side, with -d-1 == ~d ----
	xe_movaps_xx(t2, t1);
	xe_pxor_xm(t2, mVUglob.addmall);  // ~d = -d - 1
	xe_pslld_xi(t2, 23);
	xe_paddd_xm(t2, mVUglob.one);
	xe_cvttps2dq_xx(t2, t2);
	xe_psubd_xm(t2, mVUglob.addm1);
	xe_pxor_xm(t2, mVUglob.addmall);
	xe_por_xm(t2, mVUglob.signbit);
	xe_movaps_xx(t3, t1);
	xe_pcmpgtd_xm(t3, mVUglob.addmall); // d > -1  <=>  d >= 0
	xe_por_xx(t2, t3);
	xe_pand_xx(a, t2);
	xe_movaps_xx(t3, t1);
	xe_pcmpgtd_xm(t3, mVUglob.addmn25); // d > -25  <=>  d >= -24
	xe_por_xm(t3, mVUglob.signbit);
	xe_pand_xx(a, t3);
}

// Fused single-keep mask, packed forms only: (X|~p) & (Y|~p) = (X&Y)|~p
// collapses the two per-side chains around one keep-and-not-huge mask
// computed from |d|-1, and applying the a side first lets the distance
// register die into the masked from-copy - three temps, no source
// mutation. Proven bit-identical to the two-sided mask above by the
// emitted-bytes differential harness: twelve million executed lanes
// under chop, denormals-are-zero and flush-to-zero, across five
// register patterns including xmm15 operands, zero mismatches. Masks
// `a` in place and leaves the masked copy of b in tD; b is untouched.
static void mVUmaskAddSubFused(mV, int a, int b, int tD, int tM, int tS)
{
	xe_movaps_xx(tD, a);
	xe_pand_xm(tD, mVUglob.exponent);
	xe_psrld_xi(tD, 23);
	xe_movaps_xx(tS, b);
	xe_pand_xm(tS, mVUglob.exponent);
	xe_psrld_xi(tS, 23);
	xe_psubd_xx(tD, tS);              // d
	xe_pabsd_xx(tS, tD);
	xe_psubd_xm(tS, mVUglob.addm1);   // c = |d| - 1
	xe_movaps_xm(tM, mVUglob.addm24);
	xe_pcmpgtd_xx(tM, tS);            // 24 > c  <=>  |d| <= 24
	xe_por_xm(tM, mVUglob.signbit);
	xe_pslld_xi(tS, 23);
	xe_paddd_xm(tS, mVUglob.one);
	xe_cvttps2dq_xx(tS, tS);
	xe_psubd_xm(tS, mVUglob.addm1);
	xe_pxor_xm(tS, mVUglob.addmall);
	xe_por_xm(tS, mVUglob.signbit);   // keep'
	xe_pand_xx(tM, tS);               // M
	xe_movaps_xx(tS, tD);
	xe_pcmpgtd_xm(tS, mVUglob.addmall); // d >= 0: a side inactive
	xe_por_xx(tS, tM);
	xe_pand_xx(a, tS);
	xe_movaps_xx(tS, tM);
	xe_movaps_xx(tM, tD);
	xe_movaps_xm(tD, mVUglob.addm1);
	xe_pcmpgtd_xx(tD, tM);            // d <= 0: b side inactive
	xe_por_xx(tD, tS);
	xe_movaps_xx(tM, b);
	xe_pand_xx(tM, tD);
	xe_movaps_xx(tD, tM);             // masked b copy
}

// AVX-512 form: three ternary-logic folds shorten the AVX2 mask from
// twenty instructions to fifteen. Proven in the emitted-bytes
// differential against the fused SSE mask - twelve million lanes under
// chop, denormals-are-zero and flush-to-zero, five register patterns -
// before any tree consumer existed; landed now that the microcode
// audit puts real weight on the add/sub mask (Tekken's per-vertex
// SUBq and the accumulate stages of its MSUB transform chains).
// Same contract as the other forms.
static void mVUmaskAddSubFusedAVX512(mV, int a, int b, int tD, int tM, int tS, int tF = -1)
{
	xe_vpslld_xxi(tD, a, 1, 0);
	xe_vpsrld_xxi(tD, tD, 24, 0);
	xe_vpslld_xxi(tS, b, 1, 0);
	xe_vpsrld_xxi(tS, tS, 24, 0);
	if (tF >= 0)
	{
		xe_vpcmpeqd_xxm(tF, tD, mVUglob.xakff, 0);
		xe_vpcmpeqd_xxm(tM, tS, mVUglob.xakff, 0);
		xe_vpor_xxx(tF, tF, tM, 0);
	}
	xe_vpsubd_xxx(tD, tD, tS, 0);            // d
	xe_vpabsd_xx(tS, tD, 0);
	xe_vpsubd_xxm(tS, tS, mVUglob.addm1, 0); // c
	xe_vpcmpeqd_xxx(tM, tM, tM, 0);
	xe_vpsllvd_xxx(tM, tM, tS, 0);           // keep
	xe_vpcmpgtd_xxm(tS, tS, mVUglob.addm23, 0); // big
	xe_vpternlogd_xxmi(tM, tS, mVUglob.signbit, 0xBA); // M = (keep & ~big) | sign
	xe_vpcmpgtd_xxm(tS, tD, mVUglob.addmall, 0); // d >= 0
	xe_vpternlogd_xxxi(a, tM, tS, 0xE0);     // a &= M | ge0
	xe_vpcmpgtd_xxm(tD, tD, mVUglob.addzero, 0); // pos = d > 0
	xe_vpternlogd_xxxi(tD, tM, b, 0x8A);     // bcopy = b & (M | ~pos)
}

// AVX2 form of the fused mask: identical algebra, three-operand
// encodings and a per-lane variable shift. vpsllvd makes the keep mask
// directly - counts of 32 and above, including the wrapped negatives of
// the inactive lanes, shift to zero, and the sign-OR keeps that safe -
// so the float construction disappears, and the non-destructive forms
// remove every register shuffle: twenty-two instructions against
// thirty-three, still three temps. Proven bit-identical to the SSE
// fused mask by the emitted-bytes differential harness: twelve million
// executed lanes under chop, denormals-are-zero and flush-to-zero,
// five register patterns, zero mismatches. Same contract: a masked in
// place, masked copy of b left in tD, b untouched.
/* tF, when not -1, receives the operand exponent-255 flag per lane --
 * computed here for free while both exponent fields are still live,
 * instead of re-extracting them in the caller's detector. */
static void mVUmaskAddSubFusedAVX(mV, int a, int b, int tD, int tM, int tS, int tF = -1)
{
	xe_vpslld_xxi(tD, a, 1, 0);
	xe_vpsrld_xxi(tD, tD, 24, 0);
	xe_vpslld_xxi(tS, b, 1, 0);
	xe_vpsrld_xxi(tS, tS, 24, 0);
	if (tF >= 0)
	{
		xe_vpcmpeqd_xxm(tF, tD, mVUglob.xakff, 0);
		xe_vpcmpeqd_xxm(tM, tS, mVUglob.xakff, 0);
		xe_vpor_xxx(tF, tF, tM, 0);
	}
	xe_vpsubd_xxx(tD, tD, tS, 0);               // d
	xe_vpabsd_xx(tS, tD, 0);
	xe_vpsubd_xxm(tS, tS, mVUglob.addm1, 0);    // c
	xe_vpcmpeqd_xxx(tM, tM, tM, 0);
	xe_vpsllvd_xxx(tM, tM, tS, 0);              // keep
	xe_vpor_xxm(tM, tM, mVUglob.signbit, 0);    // keep'
	xe_vpcmpgtd_xxm(tS, tS, mVUglob.addm23, 0); // c > 23: sign-only class
	xe_vpandn_xxx(tM, tS, tM, 0);
	xe_vpor_xxm(tM, tM, mVUglob.signbit, 0);    // M
	xe_vpcmpgtd_xxm(tS, tD, mVUglob.addmall, 0); // d >= 0: a side inactive
	xe_vpor_xxx(tS, tS, tM, 0);
	xe_vpand_xxx(a, a, tS, 0);
	xe_vpcmpgtd_xxm(tD, tD, mVUglob.addzero, 0); // d > 0: b side active
	xe_vpand_xxx(tS, b, tM, 0);
	xe_vpandn_xxx(tD, tD, b, 0);
	xe_vpor_xxx(tD, tD, tS, 0);                 // masked b copy
}

// Masked add/sub. The destination may be masked in place, but the
// source is a cached VF register: mask a copy, never the original.

/* Exact add/sub result stage, replacing the round-to-nearest addps after
 * the fused mask stage when VuAccurateAddSub is on. Consumes the masked
 * operands (X in place, Y = the mask stage's output register), leaves the
 * PS2-exact sum in X. Bit formulas proven by tests/fpaudit emit rows:
 * encode hi = sign | (e+896)<<20 | mant>>3, lo = bits<<29, lanes with
 * e==0 collapse to signed zero; one addpd per double pair (exact: the
 * masked operands carry at most 25 significant bits); decode es =
 * field-896 with flush at es<=0, saturate at es>255 to sign|0x7fffffff,
 * and mant = (hi&fffff)<<3 | lo>>29 -- the >>29 IS the hardware adder's
 * truncation of the surviving guard bit. Integer SSE2 end to end; the
 * PS2's exponent-255 values never exist as IEEE singles here. */
/* All six temps come pre-allocated by the caller, BEFORE any branch is
 * emitted: mVUra_allocReg can evict a cached guest register, and the
 * eviction writeback it emits would land inside the skipped region --
 * the allocator's bookkeeping then disagrees with the fast path's
 * runtime reality, which corrupted the first wired build broadly from
 * frame 3. Allocation is an emit-time act; it must not sit under a
 * runtime-conditional path. */
alignas(16) u32 s_vuAddSubOvfEvent[4];
alignas(16) static const u32 s_vuZeroQuad[4] = {0, 0, 0, 0};

static void mVUexactAddSubStage(mV, int X, int Y, bool issub,
	int tA, int tB, int tC, int tQ, int tE, int tZ)
{
	if (issub)
		xe_pxor_xm(Y, mVUglob.signbit);

	/* encode X -> A01(tA), A23(tC) */
	xe_movaps_xx(tE, X); xe_psrld_xi(tE, 23); xe_pand_xm(tE, mVUglob.xakff);
	xe_movaps_xx(tZ, tE); xe_pcmpeqd_xm(tZ, mVUglob.addzero);
	xe_movaps_xx(tB, X); xe_pand_xm(tB, mVUglob.xamantm); xe_psrld_xi(tB, 3);
	xe_paddd_xm(tE, mVUglob.xak896); xe_pslld_xi(tE, 20); xe_por_xx(tB, tE);
	xe_movaps_xx(tA, X); xe_pslld_xi(tA, 29);
	/* zero-lane collapse: hi keeps only the sign, lo clears */
	xe_movaps_xx(tE, tZ); xe_pandn_xx(tE, tB); xe_movaps_xx(tB, tE);
	xe_movaps_xx(tE, X); xe_pand_xm(tE, mVUglob.signbit); xe_por_xx(tB, tE);
	xe_movaps_xx(tE, tZ); xe_pandn_xx(tE, tA); xe_movaps_xx(tA, tE);
	xe_movaps_xx(tC, tA);
	xe_punpckldq_xx(tA, tB);
	xe_punpckhdq_xx(tC, tB);

	/* encode Y -> B01(tB), B23(tE) */
	xe_movaps_xx(tE, Y); xe_psrld_xi(tE, 23); xe_pand_xm(tE, mVUglob.xakff);
	xe_movaps_xx(tZ, tE); xe_pcmpeqd_xm(tZ, mVUglob.addzero);
	xe_movaps_xx(tQ, Y); xe_pand_xm(tQ, mVUglob.xamantm); xe_psrld_xi(tQ, 3);
	xe_paddd_xm(tE, mVUglob.xak896); xe_pslld_xi(tE, 20); xe_por_xx(tQ, tE);
	xe_movaps_xx(tB, Y); xe_pslld_xi(tB, 29);
	xe_movaps_xx(tE, tZ); xe_pandn_xx(tE, tQ); xe_movaps_xx(tQ, tE);
	xe_movaps_xx(tE, Y); xe_pand_xm(tE, mVUglob.signbit); xe_por_xx(tQ, tE);
	xe_movaps_xx(tE, tZ); xe_pandn_xx(tE, tB); xe_movaps_xx(tB, tE);
	xe_movaps_xx(tE, tB);
	xe_punpckldq_xx(tB, tQ);
	xe_punpckhdq_xx(tE, tQ);

	xe_addpd_xx(tA, tB);
	xe_addpd_xx(tC, tE);

	/* decode: hiV(tB) = odd dwords, loV(tA) = even dwords */
	xe_movaps_xx(tB, tA);
	xe_shufps_xxi(tB, tC, 0xDD);
	xe_shufps_xxi(tA, tC, 0x88);

	xe_movaps_xx(tC, tB); xe_pand_xm(tC, mVUglob.signbit);              /* s   */
	xe_movaps_xx(tQ, tB); xe_psrld_xi(tQ, 20); xe_pand_xm(tQ, mVUglob.xak7ff);
	xe_psubd_xm(tQ, mVUglob.xak896);                                     /* es  */
	xe_pand_xm(tB, mVUglob.xadblhm); xe_pslld_xi(tB, 3);
	xe_psrld_xi(tA, 29); xe_por_xx(tB, tA);                              /* man */
	xe_movaps_xx(tA, tQ); xe_pslld_xi(tA, 23); xe_por_xx(tA, tB); xe_por_xx(tA, tC);
	xe_movaps_xm(tE, mVUglob.addm1); xe_pcmpgtd_xx(tE, tQ);             /* es<=0  */
	xe_movaps_xx(tB, tQ); xe_pcmpgtd_xm(tB, mVUglob.xakff);             /* es>255 */
	/* Export the saturation EVENT per lane for the flag block:
	 * result-value proxies over-fire on pass-through all-ones
	 * operands (fpaudit), so overflow flags for accurate add/sub
	 * come from this mask, spilled here where it exists. The stage's
	 * internal lane order is reversed relative to the mac-nibble
	 * convention at this point (fpaudit: the mac O nibble read back
	 * bit-reversed), so the spill shuffles to architectural order
	 * and restores. */
	xe_pshufd_xxi(tB, tB, 0x1B);
	xe_movaps_mx(&s_vuAddSubOvfEvent[0], tB);
	xe_pshufd_xxi(tB, tB, 0x1B);
	xe_movaps_xx(tZ, tE); xe_pandn_xx(tZ, tA);
	xe_pand_xx(tE, tC); xe_por_xx(tZ, tE);                               /* flush->s */
	xe_movaps_xx(tE, tC); xe_por_xm(tE, mVUglob.absclip); xe_pand_xx(tE, tB);
	xe_pandn_xx(tB, tZ); xe_por_xx(tB, tE);                              /* sat->s|max */
	xe_movaps_xx(X, tB);
}

static void mVUmaskedAddSubOp(mV, int to, int from, bool isPS, bool issub)
{
	/* No-event default for every path that skips the exact stage:
	 * the flag block reads this spill unconditionally for accurate
	 * add/sub, so stale events must be impossible. */
	{
		const int zt = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		xe_movaps_xm(zt, &s_vuZeroQuad[0]);
		xe_movaps_mx(&s_vuAddSubOvfEvent[0], zt);
		mVUra_clearNeededXMM(mVU->regAlloc, zt);
	}
	if (!clampE)
	{
		/* Default mode: clamp3/clamp4 emit nothing, so the lean fused
		 * schedules apply - proven bit-identical by the emitted-bytes
		 * differential harness, and free of GPR use entirely: the
		 * GPR-based scalar rework broke rendering in the wild through a
		 * mechanism still unidentified, so these paths touch none. */
		const int tD = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int tM = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int tS = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const bool avx512 = CPU_HAS_AVX512;
		const bool avx2 = CPU_HAS_AVX2;
		const int dst = isPS ? to : mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		/* Fast path first: the VU MXCSR rounds toward zero (ChopZero is
		 * the default control register), so masked addps already IS the
		 * hardware adder's truncation for every lane whose exponents
		 * stay out of the top octave -- 98.95% of uniform random and
		 * effectively all game content (fpaudit ACC CHOP rows). The
		 * detector below routes only exponent-255 operands and
		 * saturating results through the exact stage; its coverage of
		 * every residual divergence is the composite rows' 0/2.03M.
		 * Subtraction folds into one sign flip on the masked copy so
		 * both paths are pure adds. GPRs stay untouched (see the note
		 * above); the branch test is ptest, so hosts below SSE4.1 take
		 * the exact stage unconditionally instead. */
		/* every allocation this construction will ever need, hoisted
		 * above the branch (see the note on mVUexactAddSubStage) and
		 * above the mask stage, which folds the operand exp-255 flag
		 * into sB on AVX hosts while the exponent fields are live.
		 * The stage's last two temps are tM and tS: both are dead once
		 * the detector has branched (tS restored into dst, tM tested),
		 * and every stage temp is written before it is read. That
		 * keeps the composite at eight registers, so a fused op holds
		 * its operands, ACC and overflow blend within the fifteen the
		 * allocator has. */
		const int sA = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int sB = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int sC = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const int sQ = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		const bool elide = mVU->addsubMaskNoop;
		mVU->addsubMaskNoop = false;
		mVU->addsubRecTotal++;
		if (elide)
			mVU->addsubRecElided++;
		if (!isPS)
			xe_movaps_xx(dst, to);
		if (elide)
		{
			/* proven exponent-equal operands: the mask is a no-op. Copy
			 * the source (the contract keeps cached VF registers
			 * unmasked, and here unmodified) and let the detector guard
			 * saturation as usual. Operand exp-255 still needs testing,
			 * so the SSE-shaped detector runs on AVX hosts too. */
			xe_movaps_xx(tD, from);
		}
		else if (avx512) mVUmaskAddSubFusedAVX512(mVU, dst, from, tD, tM, tS, sB);
		else if (avx2)   mVUmaskAddSubFusedAVX(mVU, dst, from, tD, tM, tS, sB);
		else             mVUmaskAddSubFused(mVU, dst, from, tD, tM, tS);
		if (issub)
			xe_pxor_xm(tD, mVUglob.signbit);
		{
			if (CPU_HAS_SSE41)
			{
				uint8_t* to_fast_done;
				xe_movaps_xx(tS, dst);            /* masked a, for the slow path */
				xe_addps_xx(dst, tD);             /* chop add: the 99% result    */
				if ((avx512 || avx2) && !elide)
				{
					/* operand exp-255 flag already sits in sB, folded
					 * into the mask stage while the exponent fields
					 * were live; only the result test remains */
					xe_movaps_xx(tM, sB);
				}
				else
				{
					xe_movaps_xx(tM, tS); xe_pslld_xi(tM, 1); xe_psrld_xi(tM, 24);
					xe_pcmpeqd_xm(tM, mVUglob.xakff); /* opA exp == 0xFF */
					xe_movaps_xx(sA, tD); xe_pslld_xi(sA, 1); xe_psrld_xi(sA, 24);
					xe_pcmpeqd_xm(sA, mVUglob.xakff); /* opB exp == 0xFF */
					xe_por_xx(tM, sA);
				}
				xe_movaps_xx(sA, dst); xe_pslld_xi(sA, 1); xe_psrld_xi(sA, 24);
				xe_pcmpgtd_xm(sA, mVUglob.xakfd); /* res exp >= 0xFE */
				xe_por_xx(tM, sA);
				xe_ptest_xx(tM, tM);
				xe_fwd_jcc32(Jcc_Zero, to_fast_done);
				xe_movaps_xx(dst, tS);            /* restore masked a */
				mVUexactAddSubStage(mVU, dst, tD, false, sA, sB, sC, sQ, tM, tS);
				xe_fwd_set32(to_fast_done);
			}
			else
			{
				mVUexactAddSubStage(mVU, dst, tD, false, sA, sB, sC, sQ, tM, tS);
			}
			mVUra_clearNeededXMM(mVU->regAlloc, sA);
			mVUra_clearNeededXMM(mVU->regAlloc, sB);
			mVUra_clearNeededXMM(mVU->regAlloc, sC);
			mVUra_clearNeededXMM(mVU->regAlloc, sQ);
		}
		if (!isPS)
		{
			xe_movss_xx(to, dst);
			mVUra_clearNeededXMM(mVU->regAlloc, dst);
		}
		mVUra_clearNeededXMM(mVU->regAlloc, tD);
		mVUra_clearNeededXMM(mVU->regAlloc, tM);
		mVUra_clearNeededXMM(mVU->regAlloc, tS);
		return;
	}
	const int tF = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int t1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int t2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int t3 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	int dst = to;
	if (!isPS)
	{
		// addss/subss preserve the upper lanes of the destination:
		// compute on a copy so the mask stage can't disturb them.
		dst = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		xe_movaps_xx(dst, to);
	}
	xe_movaps_xx(tF, from);
	mVUclamp3(mVU, dst, t1, isPS ? 0xf : 0x8);
	mVUclamp3(mVU, tF, t1, isPS ? 0xf : 0x8);
	mVUmaskAddSub(mVU, dst, tF, t1, t2, t3);
	if (isPS)
	{
		if (issub) xe_subps_xx(dst, tF);
		else       xe_addps_xx(dst, tF);
	}
	else
	{
		if (issub) xe_subss_xx(dst, tF);
		else       xe_addss_xx(dst, tF);
	}
	mVUclamp4(mVU, dst, t1, isPS ? 0xf : 0x8);
	if (!isPS)
	{
		xe_movss_xx(to, dst);
		mVUra_clearNeededXMM(mVU->regAlloc, dst);
	}
	mVUra_clearNeededXMM(mVU->regAlloc, tF);
	mVUra_clearNeededXMM(mVU->regAlloc, t1);
	mVUra_clearNeededXMM(mVU->regAlloc, t2);
	mVUra_clearNeededXMM(mVU->regAlloc, t3);
}


#define clampOp(opX, isPS) \
	do { \
		mVUclamp3(mVU, to, t1, (isPS) ? 0xf : 0x8); \
		mVUclamp3(mVU, from, t1, (isPS) ? 0xf : 0x8); \
		opX((to), (from)); \
		mVUclamp4(mVU, to, t1, (isPS) ? 0xf : 0x8); \
	} while (0)

void SSE_MAXPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, 0);
}
void SSE_MINPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_PS(mVU, to, from, t1, t2, 1);
}
void SSE_MAXSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_SS(mVU, to, from, t1, 0);
}
void SSE_MINSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	MIN_MAX_SS(mVU, to, from, t1, 1);
}
/* The TriAce ADDi hack is gone: the exponent-distance mask reproduces
 * the discard the hack approximated, for every distance. */
void SSE_ADD2SS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK)
	{
		mVUmaskedAddSubOp(mVU, to, from, false, false);
		return;
	}
	clampOp(xe_addss_xx, 0);
}
void SSE_ADD2PS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK)
	{
		mVUmaskedAddSubOp(mVU, to, from, true, false);
		return;
	}
	clampOp(xe_addps_xx, 1);
}
void SSE_ADDPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK)
	{
		mVUmaskedAddSubOp(mVU, to, from, true, false);
		return;
	}
	clampOp(xe_addps_xx, 1);
}
void SSE_ADDSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK)
	{
		mVUmaskedAddSubOp(mVU, to, from, false, false);
		return;
	}
	clampOp(xe_addss_xx, 0);
}
void SSE_SUBPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK)
	{
		mVUmaskedAddSubOp(mVU, to, from, true, true);
		return;
	}
	clampOp(xe_subps_xx, 1);
}
void SSE_SUBSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_ACC_ADDSUB || CHECK_VUADDSUBHACK)
	{
		mVUmaskedAddSubOp(mVU, to, from, false, true);
		return;
	}
	clampOp(xe_subss_xx, 0);
}

//------------------------------------------------------------------
// Hardware-exact multiply. The Booth-truncated multiplier differs
// from the plain 48-bit product only when the correction borrow can
// reach the kept mantissa, which requires the product's bits 22:15
// to be all zero - about one lane in 256. The emitted fast path is
// the product, that guard, and a chop-pack with the model's ranges;
// when any lane trips the guard the whole quad drops to the scalar
// soft model through a per-VU buffer, which recomputes it exactly.
// Measured against ps2float: byte-exact over 12 million lanes, with
// the guard firing on 1.55% of random quads and the fast path
// costing about 9ns per quad in the C model of this sequence.
//------------------------------------------------------------------

// dst receives the 4-lane exact product of to * from; from is not
// modified. For the SS forms the caller passes a temp as dst and
// merges lane 0, so the upper-lane garbage never escapes.
//------------------------------------------------------------------
// Rec-time vf0 specialization for the broadcast ADD/SUB forms, the
// instruction the microcode audit says Tekken's VU1 actually runs:
// add/sub is 61 to 83 percent of its upper instructions and roughly
// half of those broadcast a vf0 lane - the canonical register move,
// spelled as an addition of +0.0. The masked path computes a
// twenty-instruction mask and a float add to produce, bit for bit,
// the input; the identity is oracle-proven (thirty million cases
// plus an exhaustive sweep: x plus +0.0 is x, except zero-exponent
// inputs which pack to +0.0, negative included; x minus +0.0 the
// same but keeping the sign) and the emissions below ran as machine
// code against the fused-mask-plus-add reference over sixteen
// million lanes under chop, denormals-are-zero and flush-to-zero
// with zero mismatches. The w lane broadcasts +1.0 and stays on the
// full path, as do the register, Q and I forms.
//------------------------------------------------------------------
static void mVUaddSubVF0(mV, int reg, bool isSub, bool isSS)
{
	const int T = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	xe_movaps_xx(T, reg);
	xe_pslld_xi(T, 1);
	xe_psrld_xi(T, 24);
	xe_pcmpeqd_xm(T, mVUglob.addzero);
	if (isSub)
		xe_pand_xm(T, mVUglob.absclip);   // subtraction keeps the sign
	xe_pandn_xx(T, reg);
	if (isSS) xe_movss_xx(reg, T);
	else      xe_movaps_xx(reg, T);
	mVUra_clearNeededXMM(mVU->regAlloc, T);
}

//------------------------------------------------------------------
static bool mVUexactMulVF0(mV, int reg, int opCase, bool isSS)
{
	bool byOne, mixed = false;
	/* register form is opCase 1 and the broadcast form is opCase 2;
	 * cases 3 and 4 are the I and Q forms, whose multiplier is a
	 * runtime register and whose ft field is not an operand - the
	 * first landing specialized case 4 on the belief it was the
	 * broadcast, turning every perspective-division multiply into a
	 * masked identity and blacking out the scene. */
	if (!(opCase == 1 || opCase == 2) || (_Ft_ != 0))
		return false;
	if (opCase == 2)
		byOne = (_bc_ == 3);
	else if (isSS)
		byOne = ((_X_Y_Z_W & 1) != 0);  // W lane selected
	else
		mixed = true;                   // whole {0,0,0,1.0} vector
	if (!isSS && !mixed && !byOne)
	{
		// packed broadcast of +0.0: every lane is the sign alone
		xe_pand_xm(reg, mVUglob.signbit);
		return true;
	}
	const int T = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	if (!mixed && !byOne)
	{
		// scalar by +0.0: sign alone into lane zero
		xe_movaps_xx(T, reg);
		xe_pand_xm(T, mVUglob.signbit);
		xe_movss_xx(reg, T);
	}
	else
	{
		// zero-exponent mask; by-one keeps everything else, the mixed
		// vector keeps only lane w's magnitude
		xe_movaps_xx(T, reg);
		xe_pslld_xi(T, 1);
		xe_psrld_xi(T, 24);
		xe_pcmpeqd_xm(T, mVUglob.addzero);
		if (mixed)
		{
			const int U = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
			xe_movaps_xm(U, mVUglob.mulkeepw);
			xe_pandn_xx(T, U);              // ~zexp & keep.w-magnitude
			xe_por_xm(T, mVUglob.signbit);
			xe_pand_xx(reg, T);
			mVUra_clearNeededXMM(mVU->regAlloc, U);
		}
		else
		{
			xe_pand_xm(T, mVUglob.absclip); // zexp lanes lose magnitude
			xe_pandn_xx(T, reg);            // value elsewhere
			if (isSS) xe_movss_xx(reg, T);
			else      xe_movaps_xx(reg, T);
		}
	}
	mVUra_clearNeededXMM(mVU->regAlloc, T);
	return true;
}

//------------------------------------------------------------------
// AVX2 build of the exact-multiply fast path. Byte-identical results
// and guard verdicts to the SSE form - twenty-four million executed
// quads, zero mismatches, in the production register shape - and
// measured at parity in isolation, which is exactly why it exists:
// the real-data audit puts the guard trip rate at 2.5 percent, so
// the exact-multiply gap is not the stub, and the standing suspect
// is allocator pressure around every multiply. Three-operand forms
// and the dst register doubling as scratch cut the temporaries from
// seven to six and the emitted bytes by a fifth; whether that wins
// in real blocks is what the benchmark decides. Structure, stub
// contract and pack-rejoin mirror the SSE form; p13 lives in T3
// here, so the slow path reloads T6 and T3.
//------------------------------------------------------------------
static void mVUexactMulPS_AVX2(mV, int dst, int to, int from)
{
	const int T1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T3 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T4 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T5 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T6 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int S  = dst; /* dst is scribbled mid-flight; to is read only
	                     * before the products phase, so this is safe
	                     * for both the in-place and three-register
	                     * callers (dst is never from). */
	u32* buf = mVU->exactMulBuf;
	uint8_t *jSlow, *jDone;
	u8* packStart;

	xe_vpslld_xxi(T1, to, 1, 0);
	xe_vpsrld_xxi(T1, T1, 24, 0);
	xe_vpcmpeqd_xxm(T1, T1, mVUglob.addzero, 0);
	xe_vpslld_xxi(T4, from, 1, 0);
	xe_vpsrld_xxi(T4, T4, 24, 0);
	xe_vpcmpeqd_xxm(T4, T4, mVUglob.addzero, 0);
	xe_vpor_xxx(T1, T1, T4, 0);
	xe_vpxor_xxx(T2, to, from, 0);
	xe_vpand_xxm2(T2, T2, mVUglob.signbit, 0);
	xe_vpand_xxm2(T3, to, mVUglob.mulman, 0);
	xe_vpor_xxm(T3, T3, mVUglob.mulimp, 0);
	xe_vpand_xxm2(T4, from, mVUglob.mulman, 0);
	xe_vpor_xxm(T4, T4, mVUglob.mulimp, 0);
	/* The stub reads only the low halfword of each operand lane (it packs
	 * them with tPack), and the implicit bit sits at 23, so the raw words
	 * serve in place of the mantissas and the stores move to the slow
	 * path. `from` is always still live there -- dst is never from. `to`
	 * is not when dst doubles as scratch S, so that one case keeps a
	 * store here, before S is first written. */
	if (dst == to)
		xe_movaps_mx(&buf[0], to);
	xe_vpslld_xxi(T5, to, 1, 0);
	xe_vpsrld_xxi(T5, T5, 24, 0);
	xe_vpslld_xxi(T6, from, 1, 0);
	xe_vpsrld_xxi(T6, T6, 24, 0);
	xe_vpaddd_xxx(T5, T5, T6, 0);
	xe_vpsubd_xxm(T5, T5, mVUglob.mul127, 0);
	xe_vpmuludq_xxx(T6, T3, T4, 0);
	xe_vpsrlq_xxi(T3, T3, 32, 0);
	xe_vpsrlq_xxi(T4, T4, 32, 0);
	xe_vpmuludq_xxx(T3, T3, T4, 0);
	xe_vpand_xxm2(T4, T6, mVUglob.mulg, 0);
	xe_vpand_xxm2(S, T3, mVUglob.mulg, 0);
	xe_vpsllq_xxi(S, S, 32, 0);
	xe_vpor_xxx(T4, T4, S, 0);
	xe_vpcmpeqd_xxm(T4, T4, mVUglob.addzero, 0);
	xe_vpand_xxm2(S, from, mVUglob.multz16, 0);
	xe_vpcmpeqd_xxm(S, S, mVUglob.addzero, 0);
	xe_vpor_xxx(S, S, T1, 0);
	xe_vpandn_xxx(T4, S, T4, 0);
	xe_movmskps_rx(XE_AX, T4);
	xe_test32_rr(XE_AX, XE_AX);
	xe_fwd_jcc32(Jcc_NotZero, jSlow);
	packStart = x86Ptr;
	xe_vpsrlq_xxi(T4, T6, 23, 0);
	xe_vpand_xxm2(T4, T4, mVUglob.mullo, 0);
	xe_vpsrlq_xxi(S, T3, 23, 0);
	xe_vpsllq_xxi(S, S, 32, 0);
	xe_vpor_xxx(T4, T4, S, 0);
	xe_vpsrld_xxi(T3, T4, 24, 0);
	xe_vpcmpeqd_xxm(T6, T3, mVUglob.mul1, 0);
	xe_vpsrld_xxi(S, T4, 1, 0);
	xe_vblendvps_xxxx(T6, T4, S, T6, 0);
	xe_vpaddd_xxx(T5, T5, T3, 0);
	xe_vpslld_xxi(T4, T5, 23, 0);
	xe_vpand_xxm2(T6, T6, mVUglob.mulman, 0);
	xe_vpor_xxx(T4, T4, T6, 0);
	xe_vpor_xxx(T4, T4, T2, 0);
	xe_vpcmpgtd_xxm(T3, T5, mVUglob.mul255, 0);
	xe_vpor_xxm(S, T2, mVUglob.absclip, 0);
	xe_vblendvps_xxxx(T4, T4, S, T3, 0);
	xe_vpcmpgtd_xxm(T6, T5, mVUglob.addzero, 0);
	xe_vpandn_xxx(T6, T1, T6, 0);
	xe_vblendvps_xxxx(dst, T2, T4, T6, 0);
	xe_fwd_jcc32(Jcc_Unconditional, jDone);
	xe_fwd_set32(jSlow);
	if (dst != to)
		xe_movaps_mx(&buf[0], to);
	xe_movaps_mx(&buf[4], from);
	xe_movaps_mx(&buf[8], T6);
	xe_movaps_mx(&buf[12], T3);
	xe_call_ptr(mVU->exactMulStub);
	xe_movaps_xm(T6, &buf[8]);
	xe_movaps_xm(T3, &buf[12]);
	xe_jmp_to(packStart);
	xe_fwd_set32(jDone);

	mVUra_clearNeededXMM(mVU->regAlloc, T1);
	mVUra_clearNeededXMM(mVU->regAlloc, T2);
	mVUra_clearNeededXMM(mVU->regAlloc, T3);
	mVUra_clearNeededXMM(mVU->regAlloc, T4);
	mVUra_clearNeededXMM(mVU->regAlloc, T5);
	mVUra_clearNeededXMM(mVU->regAlloc, T6);
}

static void mVUexactMulPS(mV, int dst, int to, int from)
{
	if (CPU_HAS_AVX2)
	{
		mVUexactMulPS_AVX2(mVU, dst, to, from);
		return;
	}
	const int T1 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T2 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T3 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T4 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T5 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T6 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	const int T7 = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
	u32* buf = mVU->exactMulBuf;
	uint8_t *jSlow, *jDone;
	u8* packStart;

	// T1 = zero-operand mask (either exponent field zero)
	xe_pxor_xx(T7, T7);
	xe_movaps_xx(T1, to);
	xe_pand_xm(T1, mVUglob.exponent);
	xe_pcmpeqd_xx(T1, T7);
	xe_movaps_xx(T2, from);
	xe_pand_xm(T2, mVUglob.exponent);
	xe_pcmpeqd_xx(T2, T7);
	xe_por_xx(T1, T2);
	// T2 = result sign
	xe_movaps_xx(T2, to);
	xe_pxor_xx(T2, from);
	xe_pand_xm(T2, mVUglob.signbit);
	// T3 = ma, T4 = mb (mantissa with implicit bit)
	xe_movaps_xx(T3, to);
	xe_pand_xm(T3, mVUglob.mulman);
	xe_por_xm(T3, mVUglob.mulimp);
	xe_movaps_xx(T4, from);
	xe_pand_xm(T4, mVUglob.mulman);
	xe_por_xm(T4, mVUglob.mulimp);
	/* The operand words go to the stub on the slow path only; see the
	 * AVX2 form above. This variant keeps every temporary in its own
	 * register, so both `to` and `from` are still live there. */
	// T5 = biased result exponent
	xe_movaps_xx(T5, to);
	xe_pand_xm(T5, mVUglob.exponent);
	xe_psrld_xi(T5, 23);
	xe_movaps_xx(T6, from);
	xe_pand_xm(T6, mVUglob.exponent);
	xe_psrld_xi(T6, 23);
	xe_paddd_xx(T5, T6);
	xe_psubd_xm(T5, mVUglob.mul127);
	// T6 = p02, T7 = p13
	xe_movaps_xx(T6, T3);
	xe_pmuludq_xx(T6, T4);
	xe_movaps_xx(T7, T3);
	xe_psrlq_xi(T7, 32);
	xe_psrlq_xi(T4, 32);
	xe_pmuludq_xx(T7, T4);
	// guard: any lane with (full & 0x7f8000) == 0
	xe_movaps_xx(T3, T6);
	xe_pand_xm(T3, mVUglob.mulg);
	xe_movaps_xx(T4, T7);
	xe_pand_xm(T4, mVUglob.mulg);
	xe_psllq_xi(T4, 32);
	xe_por_xx(T3, T4);
	xe_pxor_xx(T4, T4);
	xe_pcmpeqd_xx(T3, T4);
	/* Two lane classes must not trip the guard. Zero operands are
	 * decided by the final select whatever the product says. And a
	 * power-of-two multiplier - the recoded operand with an empty
	 * mantissa - recodes to a single Booth digit, one partial product,
	 * no carries for the truncated tree to discard, so the plain chop
	 * is already exact: proven against the oracle over fifteen million
	 * guard-tripping cases with zero violations, where the same
	 * exclusion on the partial-source side is unsound at a measured
	 * seventy-four percent. Game data is saturated with times-one and
	 * times-two-to-the-k in exactly the recoded role, so this takes
	 * the measured trip rate from 89 to 57 percent. Widened: sixteen
	 * or more trailing zeros in the recoded mantissa empties the whole
	 * low half of the recode and is equally sound - five million
	 * oracle cases, zero violations, with fifteen and below measurably
	 * unsound - and since the test subsumes the power-of-two one, the
	 * widening is a constant swap: 57 to 51 percent for free. */
	xe_movaps_xx(T4, from);
	xe_pand_xm(T4, mVUglob.multz16);
	xe_pcmpeqd_xm(T4, mVUglob.addzero);
	xe_por_xx(T4, T1);
	xe_pandn_xx(T4, T3);
	xe_movmskps_rx(XE_AX, T4);
	xe_test32_rr(XE_AX, XE_AX);
	xe_fwd_jcc32(Jcc_NotZero, jSlow);
	// ---- shared pack (fast path falls in; slow path jumps back) ----
	packStart = x86Ptr;
	// rm = (corrected==full) >> 23, merged into epi32 lanes
	xe_movaps_xx(T3, T6);
	xe_psrlq_xi(T3, 23);
	xe_pand_xm(T3, mVUglob.mullo);
	xe_movaps_xx(T4, T7);
	xe_psrlq_xi(T4, 23);
	xe_psllq_xi(T4, 32);
	xe_por_xx(T3, T4);
	// carry-normalize
	xe_movaps_xx(T4, T3);
	xe_psrld_xi(T4, 24);
	xe_movaps_xx(T6, T4);
	xe_pcmpeqd_xm(T6, mVUglob.mul1);
	xe_movaps_xx(T7, T3);
	xe_psrld_xi(T7, 1);
	xe_pand_xx(T7, T6);
	xe_pandn_xx(T6, T3);
	xe_por_xx(T6, T7);
	xe_paddd_xx(T5, T4);
	// normal result in T3
	xe_movaps_xx(T3, T5);
	xe_pslld_xi(T3, 23);
	xe_pand_xm(T6, mVUglob.mulman);
	xe_por_xx(T3, T6);
	xe_por_xx(T3, T2);
	// overflow: e > 255 -> sign | 0x7fffffff
	xe_movaps_xx(T4, T5);
	xe_pcmpgtd_xm(T4, mVUglob.mul255);
	xe_movaps_xx(T7, T2);
	xe_por_xm(T7, mVUglob.absclip);
	xe_pand_xx(T7, T4);
	xe_pandn_xx(T4, T3);
	xe_por_xx(T4, T7);
	// underflow (1 > e) or zero operand -> sign
	xe_movaps_xm(T3, mVUglob.mul1);
	xe_pcmpgtd_xx(T3, T5);
	xe_por_xx(T3, T1);
	xe_pand_xx(T2, T3);
	xe_pandn_xx(T3, T4);
	xe_por_xx(T3, T2);
	xe_movaps_xx(dst, T3);
	xe_fwd_jcc32(Jcc_Unconditional, jDone);
	// ---- slow path: hand the products to the tree stub, take the
	// corrected ones back, and rejoin the pack above. The stub saves
	// every register itself, so nothing here needs a flush. ----
	xe_fwd_set32(jSlow);
	xe_movaps_mx(&buf[0], to);
	xe_movaps_mx(&buf[4], from);
	xe_movaps_mx(&buf[8], T6);
	xe_movaps_mx(&buf[12], T7);
	xe_call_ptr(mVU->exactMulStub);
	xe_movaps_xm(T6, &buf[8]);
	xe_movaps_xm(T7, &buf[12]);
	xe_jmp_to(packStart);
	xe_fwd_set32(jDone);

	mVUra_clearNeededXMM(mVU->regAlloc, T1);
	mVUra_clearNeededXMM(mVU->regAlloc, T2);
	mVUra_clearNeededXMM(mVU->regAlloc, T3);
	mVUra_clearNeededXMM(mVU->regAlloc, T4);
	mVUra_clearNeededXMM(mVU->regAlloc, T5);
	mVUra_clearNeededXMM(mVU->regAlloc, T6);
	mVUra_clearNeededXMM(mVU->regAlloc, T7);
}

void SSE_MULPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_EXACTMUL)
	{
		mVUexactMulPS(mVU, to, to, from);
		return;
	}
	clampOp(xe_mulps_xx, 1);
}
void SSE_MULSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	if (CHECK_VU_EXACTMUL)
	{
		/* lane 0 through the same pipeline; upper lanes of the
		 * destination must survive, so compute into a temp. */
		const int td = mVUra_allocReg(mVU->regAlloc, -1, -1, 0, 1);
		mVUexactMulPS(mVU, td, to, from);
		xe_movss_xx(to, td);
		mVUra_clearNeededXMM(mVU->regAlloc, td);
		return;
	}
	clampOp(xe_mulss_xx, 0);
}
void SSE_DIVPS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	clampOp(xe_divps_xx, 1);
}
void SSE_DIVSS(mV, int to, int from, int t1 = -1, int t2 = -1)
{
	clampOp(xe_divss_xx, 0);
}
