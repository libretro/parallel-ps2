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

#include "Config.h"
#include "common/CpuFeatures.h"

/* ------------------------------------------------------------------ */
/* Dispatcher Functions */
/* ------------------------------------------------------------------ */
static int mvuNeedsFPCRUpdate(mV)
{
	/* always update on the vu1 thread */
	if (isVU1 && THREAD_VU1)
		return 1;

	/* otherwise only emit when it's different to the EE */
	return EmuConfig.Cpu.FPUFPCR.bitmask != (isVU0 ? EmuConfig.Cpu.VU0FPCR.bitmask : EmuConfig.Cpu.VU1FPCR.bitmask);
}

/* Generates the code for entering/exit recompiled blocks */

/* ------------------------------------------------------------------ */
/* Shared exact-multiply tree stub, emitted once per dispatcher init. */
/* The paired Booth/CSA tree proven in the harness - 48 million lanes */
/* with zero mismatches against the scalar reference, byte-exact end */
/* to end against ps2float - transcribed instruction for instruction. */
/* Memory ABI through mVU->exactMulBuf: in [0..3] fs, [4..7] ft (the raw */
/* operand words, epi32 lanes; tPack keeps only their low halfwords, */
/* which the implicit bit at 23 does not reach, so mantissas and raw */
/* words are interchangeable here); [8..11] p02 and */
/* [12..15] p13 are the 64-bit products, corrected in place. The stub */
/* saves and restores every vector register, so call sites need no */
/* register flush at all - the old slow path's full backup around a C */
/* call, taken on a guard that real game data trips on most */
/* multiplies, was the reported Tekken slowdown. */
/* ------------------------------------------------------------------ */

/* add3: lo = x^y^z, hi = ((x&y)|((x^y)&z)) << 1. */
/* lo, hi, w must be distinct from each other and from x, y, z. */
static void emitAdd3(int x, int y, int z, int lo, int hi, int w)
{
	xe_movaps_xx(w, x);
	xe_pxor_xx(w, y);          /* u */
	xe_movaps_xx(hi, x);
	xe_pand_xx(hi, y);         /* x & y */
	xe_movaps_xx(lo, w);
	xe_pand_xx(lo, z);         /* u & z */
	xe_por_xx(hi, lo);
	xe_psllw_xi(hi, 1);
	xe_movaps_xx(lo, w);
	xe_pxor_xx(lo, z);
}

/* AVX-512 variant of the tree stub, dispatched at emission time. The */
/* six carry-save adders collapse to two ternary-logic ops each - 0x96 */
/* is the xor-sum, 0xE8 the majority - the recode masks come from */
/* resident lookup registers through three-operand shuffles, the */
/* per-iteration shifted constants are precomputed, and the booth */
/* x-update folds its and-xor into one ternary op. A third smaller as */
/* emitted bytes and proven against the SSE stub the same way every */
/* generation is proven: both cores executed on the memory ABI over */
/* thirty-two million corrected-product comparisons, zero mismatches. */
static void mVUemitExactMulStubAVX512(mV)
{
	u32* buf = mVU->exactMulBuf;
	int i, k;

	mVU->exactMulStub = x86Ptr;
	for (i = 0; i < 16; i++) xe_movaps_mx(&mVU->exactMulSave[i][0], i);
	/* pack + duplicate + aP */
	xe_movaps_xm(0, &buf[0]); xe_pshufb_xm(0, mVUglob.tPack); xe_punpcklqdq_xx(0, 0);
	xe_movaps_xm(1, &buf[4]); xe_pshufb_xm(1, mVUglob.tPack); xe_punpcklqdq_xx(1, 1);
	xe_vpsllw_xxi(14, 0, 8, 0);
	xe_vpblendw_xxxi(0, 0, 14, 0xf0, 0);
	/* LUT residents */
	xe_movaps_xm(9, mVUglob.tLUTdbl); xe_movaps_xm(10, mVUglob.tLUTneg); xe_movaps_xm(11, mVUglob.tLUTany);
	{
		const u32* onesk[4]={&mVUglob.tOnesP[0],&mVUglob.tOnesP1[0],&mVUglob.tOnesP2[0],&mVUglob.tOnesP3[0]};
		const u32* onek[4] ={0,&mVUglob.tOneP1[0],&mVUglob.tOneP2[0],&mVUglob.tOneP3[0]};
		for (k = 0; k < 4; k++)
		{
			if (k) xe_vpsrlw_xxi(14, 1, k*2-1, 0);
			else   xe_vpsllw_xxi(14, 1, 1, 0);
			xe_vpsrlw_xxi(15, 1, k*2+7, 0);
			xe_vpblendw_xxxi(14, 14, 15, 0xf0, 0);
			xe_vpand_xxm2(14, 14, mVUglob.t7, 0);
			xe_vpsllw_xxi(15, 14, 8, 0);
			xe_vpor_xxx(14, 14, 15, 0);          /* t both bytes */
			if (k) xe_vpsllw_xxi(12, 0, k*2, 0);
			else   xe_movaps_xx(12, 0);          /* x */
			xe_vpshufb_xxx(15, 9, 14, 0);        /* dbl */
			xe_vpand_xxx(15, 15, 12, 0);
			xe_vpaddw_xxx(12, 12, 15, 0);
			xe_vpshufb_xxx(15, 10, 14, 0);       /* neg */
			if (k >= 1) xe_vpand_xxm2(5+k, 15, onek[k], 0);  /* n[k] */
			xe_vpternlogd_xxmi(12, 15, onesk[k], 0x78);      /* x ^= neg & onesP<<2k */
			xe_vpshufb_xxx(15, 11, 14, 0);       /* any */
			xe_vpand_xxx(2+k, 12, 15, 0);        /* d[k] */
		}
	}
	/* unpair */
	xe_vpsrldq_xxi(9, 2, 8, 0);   /* d4 */
	xe_vpsrldq_xxi(10, 3, 8, 0);  /* d5 */
	xe_vpsrldq_xxi(11, 4, 8, 0);  /* d6 */
	xe_vpsrldq_xxi(12, 5, 8, 0);  /* d7 */
	xe_vpsrldq_xxi(13, 6, 8, 0);  /* n5 */
	/* b7 -> 12 ; d5&0x800 -> 13 ; mask d4/d5 */
	xe_vpand_xxm2(14, 10, mVUglob.t0400, 0);
	xe_vpaddw_xxx(14, 14, 13, 0);
	xe_vpor_xxx(12, 12, 14, 0);
	xe_vpand_xxm2(13, 10, mVUglob.t0800, 0);
	xe_vpand_xxm2(9, 9, mVUglob.tF800, 0);
	xe_vpand_xxm2(10, 10, mVUglob.tF000, 0);
	/* t0 = a3(3,4,5): hi->14, lo in place -> 3 */
	xe_movaps_xx(14, 3); xe_vpternlogd_xxxi(14, 4, 5, 0xE8); xe_vpsllw_xxi(14, 14, 1, 0);
	xe_vpternlogd_xxxi(3, 4, 5, 0x96);
	/* t1 = a3(9,10,11): hi->15, lo->9 ; then |= n6 | (d5&0x800) */
	xe_movaps_xx(15, 9); xe_vpternlogd_xxxi(15, 10, 11, 0xE8); xe_vpsllw_xxi(15, 15, 1, 0);
	xe_vpternlogd_xxxi(9, 10, 11, 0x96);
	xe_vpsrldq_xxi(4, 7, 8, 0);   /* n6 (d1's old reg is free) */
	xe_vpor_xxx(15, 15, 4, 0);
	xe_vpor_xxx(15, 15, 13, 0);
	/* t2 = a3(2,3,14): hi->5, lo->2 */
	xe_movaps_xx(5, 2); xe_vpternlogd_xxxi(5, 3, 14, 0xE8); xe_vpsllw_xxi(5, 5, 1, 0);
	xe_vpternlogd_xxxi(2, 3, 14, 0x96);
	/* t3 = a3(12,9,15): hi->4, lo->12 */
	xe_movaps_xx(4, 12); xe_vpternlogd_xxxi(4, 9, 15, 0xE8); xe_vpsllw_xxi(4, 4, 1, 0);
	xe_vpternlogd_xxxi(12, 9, 15, 0x96);
	/* t4 = a3(5,12,4): hi->3, lo->5 */
	xe_movaps_xx(3, 5); xe_vpternlogd_xxxi(3, 12, 4, 0xE8); xe_vpsllw_xxi(3, 3, 1, 0);
	xe_vpternlogd_xxxi(5, 12, 4, 0x96);
	/* t5 = a3(2,5,3): hi->9, lo->2 */
	xe_movaps_xx(9, 2); xe_vpternlogd_xxxi(9, 5, 3, 0xE8); xe_vpsllw_xxi(9, 9, 1, 0);
	xe_vpternlogd_xxxi(2, 5, 3, 0x96);
	/* t5h += n7 ; sum ; tb */
	xe_vpsrldq_xxi(10, 8, 8, 0);
	xe_vpaddw_xxx(9, 9, 10, 0);
	xe_vpand_xxm2(2, 2, mVUglob.t8000, 0);
	xe_vpand_xxm2(9, 9, mVUglob.t8000, 0);
	xe_vpaddw_xxx(2, 2, 9, 0);
	xe_vpsrlw_xxi(2, 2, 15, 0);
	/* correction */
	xe_vpxor_xxx(0, 0, 0, 0);
	xe_vpunpcklwd_xxx(2, 2, 0, 0);
	xe_vpand_xxm2(6, 2, mVUglob.mullo, 0);
	xe_vpsrlq_xxi(2, 2, 32, 0);
	xe_movaps_xm(1, &buf[8]);
	xe_movaps_xm(4, &buf[12]);
	xe_vpsrlq_xxi(3, 1, 15, 0);
	xe_vpand_xxm2(3, 3, mVUglob.mul64one, 0);
	xe_vpsrlq_xxi(5, 4, 15, 0);
	xe_vpand_xxm2(5, 5, mVUglob.mul64one, 0);
	xe_vpxor_xxx(6, 6, 3, 0);
	xe_vpxor_xxx(2, 2, 5, 0);
	xe_vpsllq_xxi(6, 6, 15, 0);
	xe_vpsllq_xxi(2, 2, 15, 0);
	xe_vpsubq_xxx(1, 1, 6, 0);
	xe_vpsubq_xxx(4, 4, 2, 0);
	xe_movaps_mx(&buf[8], 1);
	xe_movaps_mx(&buf[12], 4);
	for (i = 0; i < 16; i++) xe_movaps_xm(i, &mVU->exactMulSave[i][0]);
	xe_ret();
}

/* Unlike the exact-divide thunk, the multiply stubs contain no C call:
 * both variants are pure emitted SIMD trees that save and restore all
 * sixteen xmm registers, touch no GPR and never move rsp, so they are
 * ABI-inert by construction and need none of the divide thunk's
 * caller-saved/shadow-space treatment. Recorded here because a
 * previous commit wrongly warned that they shared the vulnerable
 * mechanism; an audit shows the divide thunk was the only mid-block C
 * call in the recompiler -- every other emitted fastcall sits in the
 * block-exit paths of microVU_Branch.inl, where register state is
 * being torn down under the long-established upstream discipline. */
static void mVUemitExactMulStub(mV)
{
	u32* buf = mVU->exactMulBuf;
	int i, k;

	mVU->exactMulStub = x86Ptr;
	for (i = 0; i < 16; i++)
		xe_movaps_mx(&mVU->exactMulSave[i][0], i);

	/* regs: 0=aP 1=b16 2..5=d0..d3 6..8=n1..n3 9..12=iter/tree 13..15=working */
	xe_movaps_xm(0, &buf[0]);
	xe_pshufb_xm(0, mVUglob.tPack);
	xe_punpcklqdq_xx(0, 0);
	xe_movaps_xm(1, &buf[4]);
	xe_pshufb_xm(1, mVUglob.tPack);
	xe_punpcklqdq_xx(1, 1);
	xe_movaps_xx(9, 0);
	xe_psllw_xi(9, 8);
	xe_pblendw_xxi(0, 9, 0xf0);        /* aP */

	for (k = 0; k < 4; k++)
	{
		/* 9 = t: blend of the two half shifts, &7, doubled into both bytes */
		xe_movaps_xx(9, 1);
		if (k == 0) xe_psllw_xi(9, 1);
		else        xe_psrlw_xi(9, k * 2 - 1);
		xe_movaps_xx(10, 1);
		xe_psrlw_xi(10, k * 2 + 7);
		xe_pblendw_xxi(9, 10, 0xf0);
		xe_pand_xm(9, mVUglob.t7);
		xe_movaps_xx(10, 9);
		xe_psllw_xi(10, 8);
		xe_por_xx(9, 10);
		/* 11 = x = aP << 2k */
		xe_movaps_xx(11, 0);
		if (k) xe_psllw_xi(11, k * 2);
		/* x += x & dbl */
		xe_movaps_xm(10, mVUglob.tLUTdbl);
		xe_pshufb_xx(10, 9);
		xe_pand_xx(10, 11);
		xe_paddw_xx(11, 10);
		/* 10 = neg; n[k] for k>=1 into 5+k; x ^= neg & (onesP<<2k) */
		xe_movaps_xm(10, mVUglob.tLUTneg);
		xe_pshufb_xx(10, 9);
		if (k >= 1)
		{
			xe_movaps_xx(5 + k, 10);
			xe_movaps_xm(12, mVUglob.tOneP);
			xe_psllw_xi(12, k * 2);
			xe_pand_xx(5 + k, 12);
		}
		xe_movaps_xm(12, mVUglob.tOnesP);
		if (k) xe_psllw_xi(12, k * 2);
		xe_pand_xx(10, 12);
		xe_pxor_xx(11, 10);
		/* x &= any (t still live in 9) */
		xe_movaps_xm(10, mVUglob.tLUTany);
		xe_pshufb_xx(10, 9);
		xe_pand_xx(11, 10);
		xe_movaps_xx(2 + k, 11);       /* d[k] */
	}

	/* unpair the high halves; 0,1,9..15 free */
	xe_movaps_xx(9, 2);  xe_psrldq_xi(9, 8);   /* d4 */
	xe_movaps_xx(10, 3); xe_psrldq_xi(10, 8);  /* d5 */
	xe_movaps_xx(11, 4); xe_psrldq_xi(11, 8);  /* d6 */
	xe_movaps_xx(12, 5); xe_psrldq_xi(12, 8);  /* d7 */
	xe_movaps_xx(13, 6); xe_psrldq_xi(13, 8);  /* n5 */

	/* b7 = d7 | ((d5 & 0x400) + n5)  -> 12 ; frees 13 */
	xe_movaps_xx(14, 10);
	xe_pand_xm(14, mVUglob.t0400);
	xe_paddw_xx(14, 13);
	xe_por_xx(12, 14);
	/* 13 = d5 & 0x800 (needed after d5 is masked), then mask d4/d5 */
	xe_movaps_xx(13, 10);
	xe_pand_xm(13, mVUglob.t0800);
	xe_pand_xm(9, mVUglob.tF800);
	xe_pand_xm(10, mVUglob.tF000);

	/* t0 = add3(d1, d2, d3) -> lo 14, hi 15 ; d1..d3 die */
	emitAdd3(3, 4, 5, 14, 15, 1);
	/* t1 = add3(d4m, d5m, d6) -> lo 3, hi 4 ; then t1h |= n6 | (d5&0x800) */
	emitAdd3(9, 10, 11, 3, 4, 5);
	xe_movaps_xx(5, 7);  xe_psrldq_xi(5, 8);   /* n6 */
	xe_por_xx(4, 5);
	xe_por_xx(4, 13);
	/* t2 = add3(d0, t0l, t0h) -> lo 9, hi 10 ; d0 dies */
	emitAdd3(2, 14, 15, 9, 10, 11);
	/* t3 = add3(b7, t1l, t1h) -> lo 13, hi 14 */
	emitAdd3(12, 3, 4, 13, 14, 15);
	/* t4 = add3(t2h, t3l, t3h) -> lo 2, hi 3 */
	emitAdd3(10, 13, 14, 2, 3, 4);
	/* t5 = add3(t2l, t4l, t4h) -> lo 5, hi 10 */
	emitAdd3(9, 2, 3, 5, 10, 11);
	/* t5h += n7 ; sum = (t5l & 0x8000) + (t5h & 0x8000) ; tb = sum >> 15 */
	xe_movaps_xx(11, 8); xe_psrldq_xi(11, 8);  /* n7 */
	xe_paddw_xx(10, 11);
	xe_pand_xm(5, mVUglob.t8000);
	xe_pand_xm(10, mVUglob.t8000);
	xe_paddw_xx(5, 10);
	xe_psrlw_xi(5, 15);                        /* tb, epi16 lanes 0-3 */

	/* correction: p -= ((tb ^ ((p >> 15) & 1)) << 15), 64-bit lanes */
	xe_pxor_xx(0, 0);
	xe_punpcklwd_xx(5, 0);                     /* tb32 lanes {b0,b1,b2,b3} */
	xe_movaps_xx(6, 5);
	xe_pand_xm(6, mVUglob.mullo);              /* tb02 {b0,0,b2,0} */
	xe_psrlq_xi(5, 32);                        /* tb13 {b1,0,b3,0} */
	xe_movaps_xm(1, &buf[8]);                  /* p02 */
	xe_movaps_xm(2, &buf[12]);                 /* p13 */
	xe_movaps_xx(3, 1);
	xe_psrlq_xi(3, 15);
	xe_pand_xm(3, mVUglob.mul64one);           /* f02 */
	xe_movaps_xx(4, 2);
	xe_psrlq_xi(4, 15);
	xe_pand_xm(4, mVUglob.mul64one);           /* f13 */
	xe_pxor_xx(6, 3);
	xe_pxor_xx(5, 4);
	xe_psllq_xi(6, 15);
	xe_psllq_xi(5, 15);
	xe_psubq_xx(1, 6);
	xe_psubq_xx(2, 5);
	xe_movaps_mx(&buf[8], 1);
	xe_movaps_mx(&buf[12], 2);

	for (i = 0; i < 16; i++)
		xe_movaps_xm(i, &mVU->exactMulSave[i][0]);
	xe_ret();
}

/* ------------------------------------------------------------------ */
/* Exact DIV/SQRT/RSQRT: the SRT quotient's final +1 has no closed */
/* form - it depends on the carry-save remainder trajectory, proven */
/* by a four-thousand-bin overlap analysis - so exact Q means running */
/* the model's recurrence. The microcode audit shows Q-producing */
/* instructions are per-batch cold (zero in Tekken Tag's entire */
/* cached program set), so a register-preserving thunk around a C */
/* call costs nothing where it runs: all sixteen xmm saved, block */
/* state lives in callee-saved GPRs already, and call sites need no */
/* flush. Flags are derived from the model's own IV/DZ bits. */
/* ------------------------------------------------------------------ */

/* The old all-sixteen-register exactdiv thunk is gone: the call
 * sites use the field-proven mVUbackupRegs(onlyNeeded) idiom, saving
 * only the allocator's cached caller-saved registers with the same
 * stack and shadow-space discipline the XGKICK C calls rely on. */

void mVUdispatcherAB(mV)
{
	mVU->startFunct = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		/* = The caller has already put the needed parameters in ecx/edx: */
		if (!isVU1) xe_fastcall2_rr(mVUexecuteVU0, XE_ARG1, XE_ARG2);
		else        xe_fastcall2_rr(mVUexecuteVU1, XE_ARG1, XE_ARG2);

		/* Load VU's MXCSR state */
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask);

		/* Load Regs */
		xe_movaps_xm(xmmT1, &vuRegs[mVU->index].VI[REG_P].UL);
		xe_movaps_xm(xmmPQ, &vuRegs[mVU->index].VI[REG_Q].UL);
		{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].pending_q); xe_movdzx_xmemg(xmmT2, xm); }
		xe_shufps_xxi(xmmPQ, xmmT1, 0); /* wzyx = PPQQ */
		/* Load in other Q instance */
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);
		xe_movss_xx(xmmPQ, xmmT2);
		xe_pshufd_xxi(xmmPQ, xmmPQ, 0xe1);

		if (isVU1)
		{
			/* Load in other P instance */
			{ struct e_mem xm; XE_MEM_ABS(xm, &vuRegs[mVU->index].pending_p); xe_movdzx_xmemg(xmmT2, xm); }
			xe_pshufd_xxi(xmmPQ, xmmPQ, 0x1B);
			xe_movss_xx(xmmPQ, xmmT2);
			xe_pshufd_xxi(xmmPQ, xmmPQ, 0x1B);
		}

		xe_movaps_xm(xmmT1, &vuRegs[mVU->index].micro_macflags);
		xe_movaps_mx(mVU->macFlag, xmmT1);


		xe_movaps_xm(xmmT1, &vuRegs[mVU->index].micro_clipflags);
		xe_movaps_mx(mVU->clipFlag, xmmT1);

		xe_mov32_rm(gprF0, &vuRegs[mVU->index].micro_statusflags[0]);
		xe_mov32_rm(gprF1, &vuRegs[mVU->index].micro_statusflags[1]);
		xe_mov32_rm(gprF2, &vuRegs[mVU->index].micro_statusflags[2]);
		xe_mov32_rm(gprF3, &vuRegs[mVU->index].micro_statusflags[3]);

		/* Jump to Recompiled Code Block */
		xe_jmp_r(XE_AX);

		mVU->exitFunct = x86Ptr;

		/* Load EE's MXCSR state */
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);

		/* = The first two DWORD or smaller arguments are passed in ECX and EDX registers; */
		/*              all other arguments are passed right to left. */
		if (!isVU1) xe_fastcall0(mVUcleanUpVU0);
		else        xe_fastcall0(mVUcleanUpVU1);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xe_ret();
}

/* Generates the code for resuming/exit xgkick */
void mVUdispatcherCD(mV)
{
	mVU->startFunctXG = x86Ptr;

	{
		int m_offset;
		SCOPED_STACK_FRAME_BEGIN(m_offset);

		/* Load VU's MXCSR state */
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(isVU0 ? &EmuConfig.Cpu.VU0FPCR.bitmask : &EmuConfig.Cpu.VU1FPCR.bitmask);

		mVUrestoreRegs(mVU, 0, 0);
		xe_mov32_rm(gprF0, &vuRegs[mVU->index].micro_statusflags[0]);
		xe_mov32_rm(gprF1, &vuRegs[mVU->index].micro_statusflags[1]);
		xe_mov32_rm(gprF2, &vuRegs[mVU->index].micro_statusflags[2]);
		xe_mov32_rm(gprF3, &vuRegs[mVU->index].micro_statusflags[3]);

		/* Jump to Recompiled Code Block */
		xe_jmp_mem_abs(&mVU->resumePtrXG);

		mVU->exitFunctXG = x86Ptr;

		/* Backup Status Flag (other regs were backed up on xgkick) */
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[0], gprF0);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[1], gprF1);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[2], gprF2);
		xe_mov32_mr(&vuRegs[mVU->index].micro_statusflags[3], gprF3);

		/* Load EE's MXCSR state */
		if (mvuNeedsFPCRUpdate(mVU))
			xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);
		SCOPED_STACK_FRAME_END(m_offset);
	}

	xe_ret();
}

static void mVUGenerateWaitMTVU(mV)
{
	int num_xmms;
	int num_gprs;
	int i;
	int stack_size;
	int stack_offset;
	mVU->waitMTVU = x86Ptr;

	num_xmms = 0;
	num_gprs = 0;

	for (i = 0; i < (int)(iREGCNT_GPR); i++)
	{
		if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
			continue;

		/* T1 often contains the address we're loading when waiting for VU1. */
		/* T2 isn't used until afterwards, so don't bother saving it. */
		if (i == gprT2)
			continue;

		xe_push64_r(i);
		num_gprs++;
	}

	for (i = 0; i < (int)(iREGCNT_XMM); i++)
	{
		if (!XE_XMM_CALLER_SAVED(i))
			continue;

		num_xmms++;
	}

	/* We need 16 byte alignment on the stack. */
	/* Since the stack is unaligned at entry to this function, we add 8 when it's even, not odd. */
	stack_size = (num_xmms * sizeof(u128)) + ((~num_gprs & 1) * sizeof(u64)) + SHADOW_STACK_SIZE;
	stack_offset = SHADOW_STACK_SIZE;

	if (stack_size > 0)
	{
		int i;
		xe_sub64_ri(XE_SP, stack_size);
		for (i = 0; i < (int)(iREGCNT_XMM); i++)
		{
			if (!XE_XMM_CALLER_SAVED(i))
				continue;

			{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_memxg(xm, i); }
			stack_offset += sizeof(u128);
		}
	}

	xe_fastcall0(mVUwaitMTVU);

	stack_offset = (num_xmms - 1) * sizeof(u128) + SHADOW_STACK_SIZE;
	for (i = (int)(iREGCNT_XMM - 1); i >= 0; i--)
	{
		if (!XE_XMM_CALLER_SAVED(i))
			continue;

		{ struct e_mem xm; E_MEM(xm, XE_SP, E_NOREG, 0, stack_offset); xe_movaps_xmemg(i, xm); }
		stack_offset -= sizeof(u128);
	}
	xe_add64_ri(XE_SP, stack_size);

	for (i = (int)(iREGCNT_GPR - 1); i >= 0; i--)
	{
		if (!XE_GPR_CALLER_SAVED(i) || i == XE_SP)
			continue;

		if (i == gprT2)
			continue;

		xe_pop64_r(i);
	}

	xe_ret();
}

static void mVUGenerateCopyPipelineState(mV)
{
	mVU->copyPLState = x86Ptr;

	if (CPU_HAS_AVX2)
	{
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 0); xe_vmovaps_xmemg(0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 32); xe_vmovaps_xmemg(1, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 64); xe_vmovaps_xmemg(2, xm, 1); }

		{ struct e_mem xm; XE_MEM_ABS(xm, &mVU->prog.lpState); xe_vmovups_memxg(xm, 0, 1); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 32); xe_vmovups_memxg(xm, 1, 1); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 64); xe_vmovups_memxg(xm, 2, 1); }

		xe_vzeroupper();
	}
	else
	{
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 0); xe_movaps_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 16); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 32); xe_movaps_xmemg(2, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 48); xe_movaps_xmemg(3, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 64); xe_movaps_xmemg(4, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_AX, E_NOREG, 0, 80); xe_movaps_xmemg(5, xm); }

		{ struct e_mem xm; XE_MEM_ABS(xm, &mVU->prog.lpState); xe_movups_memxg(xm, 0); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 16); xe_movups_memxg(xm, 1); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 32); xe_movups_memxg(xm, 2); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 48); xe_movups_memxg(xm, 3); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 64); xe_movups_memxg(xm, 4); }
		{ struct e_mem xm; E_MEM(xm, E_NOREG, E_NOREG, 0, (intptr_t)&mVU->prog.lpState + 80); xe_movups_memxg(xm, 5); }
	}

	xe_ret();
}

/* ------------------------------------------------------------------ */
/* Micro VU - Custom Quick Search */
/* ------------------------------------------------------------------ */

/* Generates a custom optimized block-search function */
/* Note: Structs must be 16-byte aligned! (GCC doesn't guarantee this) */
static void mVUGenerateCompareState(mV)
{
	mVU->compareStateF = x86Ptr;

	if (CPU_HAS_AVX2)
	{
		uint8_t* exitPoint;
		/* We have to use unaligned loads here, because the blocks are only 16 byte aligned. */
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_vmovups_xmemg(0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0); xe_vpcmpeqd_xxmemg(0, 0, xm, 1); }
		xe_vpmovmskb_rx(XE_AX, 0, 1);
		xe_xor32_ri(XE_AX, 0xffffffff);
		 xe_fwd_jcc8(Jcc_NotZero, exitPoint);

		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x20); xe_vmovups_xmemg(0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x40); xe_vmovups_xmemg(1, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x20); xe_vpcmpeqd_xxmemg(0, 0, xm, 1); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x40); xe_vpcmpeqd_xxmemg(1, 1, xm, 1); }
		xe_vpand_xxx(0, 0, 1, 1);

		xe_vpmovmskb_rx(XE_AX, 0, 1);
		xe_not32_r(XE_AX);

		xe_fwd_set8(exitPoint);
		xe_vzeroupper();
	}
	else
	{
		uint8_t* exitPoint;
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0); xe_movaps_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0); xe_pcmpeqd_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x10); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x10); xe_pcmpeqd_xmemg(1, xm); }
		xe_pand_xx(0, 1);

		xe_movmskps_rx(XE_AX, 0);
		xe_xor32_ri(XE_AX, 0xf);
		 xe_fwd_jcc8(Jcc_NotZero, exitPoint);

		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x20); xe_movaps_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x20); xe_pcmpeqd_xmemg(0, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x30); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x30); xe_pcmpeqd_xmemg(1, xm); }
		xe_pand_xx(0, 1);

		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x40); xe_movaps_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x40); xe_pcmpeqd_xmemg(1, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG1, E_NOREG, 0, 0x50); xe_movaps_xmemg(2, xm); }
		{ struct e_mem xm; E_MEM(xm, XE_ARG2, E_NOREG, 0, 0x50); xe_pcmpeqd_xmemg(2, xm); }
		xe_pand_xx(1, 2);
		xe_pand_xx(0, 1);

		xe_movmskps_rx(XE_AX, 0);
		xe_xor32_ri(XE_AX, 0xf);

		xe_fwd_set8(exitPoint);
	}

	xe_ret();
}

/* ------------------------------------------------------------------ */
/* Execution Functions */
/* ------------------------------------------------------------------ */

/* Executes for number of cycles */
static __fi void* mVUexecute(int vuIndex, u32 startPC, u32 cycles)
{
	microVU* mVU    = mVUx;
	u32 vuLimit     = vuIndex ? 0x3ff8 : 0xff8;
	mVU->cycles      = cycles;
	mVU->totalCycles = cycles;
	x86Ptr = (u8*)(mVU->prog.codePtr); /* Set codePtr to where last program left off */
	return mVUsearchProg(vuIndex, startPC & vuLimit, (uptr)&mVU->prog.lpState); /* Find and set correct program */
}

/* ------------------------------------------------------------------ */
/* Cleanup Functions */
/* ------------------------------------------------------------------ */

static __fi void mVUcleanUp(int vuIndex)
{
	microVU* mVU = mVUx;

	mVU->prog.codePtr = x86Ptr;

	if ((x86Ptr < mVU->prog.codeStart) || (x86Ptr >= mVU->prog.codeEnd))
		mVUreset(mVU, 0);

	mVU->cycles = mVU->totalCycles - C89_MAX(0, mVU->cycles);
	vuRegs[mVU->index].cycle += mVU->cycles;

	if (!vuIndex || !THREAD_VU1)
	{
		u32 cycles_passed = C89_MIN(mVU->cycles, 3000) * EmuConfig.Speedhacks.EECycleSkip;
		if (cycles_passed > 0)
		{
			s64 vu0_offset = vuRegs[0].cycle - cpuRegs.cycle;
			cpuRegs.cycle += cycles_passed;

			/* VU0 needs to stay in sync with the CPU otherwise things get messy */
			/* So we need to adjust when VU1 skips cycles also */
			if (!vuIndex)
				vuRegs[0].cycle  = cpuRegs.cycle + vu0_offset;
			else
				vuRegs[0].cycle += cycles_passed;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Caller Functions */
/* ------------------------------------------------------------------ */

void* mVUexecuteVU0(u32 startPC, u32 cycles) { return mVUexecute(0, startPC, cycles); }
void* mVUexecuteVU1(u32 startPC, u32 cycles) { return mVUexecute(1, startPC, cycles); }
void mVUcleanUpVU0(void) { mVUcleanUp(0); }
void mVUcleanUpVU1(void) { mVUcleanUp(1); }
