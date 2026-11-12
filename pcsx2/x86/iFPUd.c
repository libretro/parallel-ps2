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

#include "../Common.h"
#include "../Config.h"
#include "../R5900OpcodeTables.h"
#include "../../common/emitter/x86emitter.h"

#include "iR5900.h"
#include "common/emitter/c89ops.h"
#include "iFPU.h"

/* This is a version of the FPU that emulates an exponent of 0xff and overflow/underflow flags */

/* Can be made faster by not converting stuff back and forth between instructions. */

/* ---------------------------------------------------------------- */
/* FPU emulation status: */
/* ADD, SUB (incl. accumulation stage of MADD/MSUB) - no known problems. */
/* Mul (incl. multiplication stage of MADD/MSUB) - incorrect. PS2's result mantissa is sometimes */
/* 													smaller by 0x1 than IEEE's result (with round to zero). */
/* DIV, SQRT, RSQRT - incorrect. PS2's result varies between IEEE's result with round to zero */
/* 													and IEEE's result with round to +/-infinity. */
/* other stuff - no known problems. */
/* ---------------------------------------------------------------- */
/* If 1, result is not clamped (Gives correct results as in PS2, */
/* but can cause problems due to insufficient clamping levels in the VUs) */
#define FPU_RESULT 1

#ifdef FPU_RECOMPILE

/* This file provides only the full-mode (DOUBLE_*) emitter bodies. The
 * FPURECOMPILE_CONSTCODE dispatchers used to be expanded here too, but
 * inside namespace DOUBLE, which made them duplicates of the global ones
 * in iFPU.c that the opcode tables actually use -- eighteen functions
 * that were compiled, linked and never called. With the namespace gone
 * they would collide, so they are deleted rather than renamed. */

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Helper Macros */
/* ------------------------------------------------------------------ */
#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

/* FCR31 Flags */
#define FPUflagC  0x00800000
#define FPUflagI  0x00020000
#define FPUflagD  0x00010000
#define FPUflagO  0x00008000
#define FPUflagU  0x00004000
#define FPUflagSI 0x00000040
#define FPUflagSD 0x00000020
#define FPUflagSO 0x00000010
#define FPUflagSU 0x00000008

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* *FPU Opcodes!* */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* PS2 -> DOUBLE */
/* ------------------------------------------------------------------ */

#define SINGLE(sign, exp, mant) (((u32)(sign) << 31) | ((u32)(exp) << 23) | (u32)(mant))
#define DOUBLE(sign, exp, mant) (((sign##ULL) << 63) | ((exp##ULL) << 52) | (mant##ULL))

typedef struct FPUd_Globals
{
	u32 neg[4], pos[4];

	u32 pos_inf[4], neg_inf[4],
	    one_exp[4];

	u64 dbl_one_exp[2];

	u64 dbl_cvt_overflow, /* needs special code if above or equal */
	    dbl_ps2_overflow, /* overflow & clamp if above or equal */
	    dbl_underflow;    /* underflow if below */

	u64 padding;

	u64 dbl_s_pos[2];
	/* u64		dlb_s_neg[2]; */
} FPUd_Globals;

PCSX2_ALIGN(32) static const FPUd_Globals s_const =
{
	{0x80000000, 0xffffffff, 0xffffffff, 0xffffffff},
	{0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff},

	{SINGLE(0, 0xff, 0), 0, 0, 0},
	{SINGLE(1, 0xff, 0), 0, 0, 0},
	{SINGLE(0,    1, 0), 0, 0, 0},

	{DOUBLE(0, 1, 0), 0},

	DOUBLE(0, 1151, 0), /* cvt_overflow */
	DOUBLE(0, 1152, 0), /* ps2_overflow */
	DOUBLE(0,  897, 0), /* underflow */

	0,                  /* Padding!! */

	{0x7fffffffffffffffULL, 0},
	/* {0x8000000000000000ULL, 0}, */
};

/* ToDouble : converts single-precision PS2 float to double-precision IEEE float */
static void ToDouble(int reg)
{
	uint8_t* to_complex;
	uint8_t* to_complex2;
	uint8_t* end;
	xe_ucomiss_xm(reg, s_const.pos_inf); /* Sets ZF if reg is equal or incomparable to pos_inf */
	 xe_fwd_jcc8(Jcc_Equal, to_complex); /* Complex conversion if positive infinity or NaN */
	xe_ucomiss_xm(reg, s_const.neg_inf);
	 xe_fwd_jcc8(Jcc_Equal, to_complex2); /* Complex conversion if negative infinity */

	xe_cvtss2sd_xx(reg, reg); /* Simply convert */
	 xe_fwd_jcc8(Jcc_Unconditional, end);

	xe_fwd_set8(to_complex);
	xe_fwd_set8(to_complex2);

	/* Special conversion for when IEEE sees the value in reg as an INF/NaN */
	xe_psubd_xm(reg, s_const.one_exp); /* Lower exponent by one */
	xe_cvtss2sd_xx(reg, reg);
	xe_paddq_xm(reg, s_const.dbl_one_exp); /* Raise exponent by one */

	xe_fwd_set8(end);
}

/* ------------------------------------------------------------------ */
/* DOUBLE -> PS2 */
/* ------------------------------------------------------------------ */

/* If FPU_RESULT is defined, results are more like the real PS2's FPU. */
/* But new issues may happen if the VU isn't clamping all operands since games may transfer FPU results into the VU. */
/* Ar tonelico 1 does this with the result from DIV/RSQRT (when a division by zero occurs). */
/* Otherwise, results are still usually better than iFPU.c. */

/* ToPS2FPU_Full - converts double-precision IEEE float to single-precision PS2 float */

/* converts small normal numbers to PS2 equivalent */
/* converts large normal numbers to PS2 equivalent (which represent NaN/inf in IEEE) */
/* converts really large normal numbers to PS2 signed max */
/* converts really small normal numbers to zero (flush) */
/* doesn't handle inf/nan/denormal */
static void ToPS2FPU_Full(int reg, int flags, int absreg, int acc, int addsub)
{
	uint8_t* to_complex;
	uint8_t* to_underflow;
	uint8_t* end;
	uint8_t* to_overflow;
	uint8_t* end2;
	uint8_t* end3;
	u8* end4;
	if (flags)
	{
		xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagO | FPUflagU));
		if (acc)
			xe_and32_mi(&fpuRegs.ACCflag, ~1);
	}

	xe_movaps_xx(absreg, reg);
	xe_andpd_xm(absreg, &s_const.dbl_s_pos);

	xe_ucomisd_xm(absreg, &s_const.dbl_cvt_overflow);
	 xe_fwd_jcc8(Jcc_AboveOrEqual, to_complex);

	xe_ucomisd_xm(absreg, &s_const.dbl_underflow);
	 xe_fwd_jcc8(Jcc_Below, to_underflow);

	xe_cvtsd2ss_xx(reg, reg); /* simply convert */

	 xe_fwd_jcc32(Jcc_Unconditional, end);

	xe_fwd_set8(to_complex);
	xe_ucomisd_xm(absreg, &s_const.dbl_ps2_overflow);
	 xe_fwd_jcc8(Jcc_AboveOrEqual, to_overflow);

	xe_psubq_xm(reg, &s_const.dbl_one_exp); /* lower exponent */
	xe_cvtsd2ss_xx(reg, reg); /* convert */
	xe_paddd_xm(reg, s_const.one_exp); /* raise exponent */

	 xe_fwd_jcc32(Jcc_Unconditional, end2);

	xe_fwd_set8(to_overflow);
	xe_cvtsd2ss_xx(reg, reg);
	xe_orps_xm(reg, &s_const.pos); /* clamp */
	if (flags)
	{
		xe_or32_mi(&fpuRegs.fprc[31], (FPUflagO | FPUflagSO));
		if (acc)
			xe_or32_mi(&fpuRegs.ACCflag, 1);
	}
	 xe_fwd_jcc8(Jcc_Unconditional, end3);

	xe_fwd_set8(to_underflow);
	end4 = NULL;
	if (flags) /* set underflow flags if not zero */
	{
		uint8_t* is_zero;
		xe_xorpd_xx(absreg, absreg);
		xe_ucomisd_xx(reg, absreg);
		 xe_fwd_jcc8(Jcc_Equal, is_zero);

		xe_or32_mi(&fpuRegs.fprc[31], (FPUflagU | FPUflagSU));
		if (addsub)
		{
			/* On ADD/SUB, the PS2 simply leaves the mantissa bits as they are (after normalization) */
			/* IEEE either clears them (FtZ) or returns the denormalized result. */
			/* not thoroughly tested : other operations such as MUL and DIV seem to clear all mantissa bits? */
			xe_movaps_xx(absreg, reg);
			xe_psllq_xi(reg, 12); /* mantissa bits */
			xe_psrlq_xi(reg, 41);
			xe_psrlq_xi(absreg, 63); /* sign bit */
			xe_psllq_xi(absreg, 31);
			xe_por_xx(reg, absreg);
			xe_fwd_jcc8(Jcc_Unconditional, end4);
		}

		xe_fwd_set8(is_zero);
	}
	xe_cvtsd2ss_xx(reg, reg);
	xe_andps_xm(reg, s_const.neg); /* flush to zero */

	xe_fwd_set32(end);
	xe_fwd_set32(end2);

	xe_fwd_set8(end3);
	if (flags && addsub)
		xe_fwd_set8(end4);
}

/* sets the maximum (positive or negative) value into regd. */
#define SetMaxValue(regd) xe_orps_xm((regd), &s_const.pos[0])

#define GET_S(sreg) \
	do { \
		if (info & PROCESS_EE_S) \
			xe_movss_xx(sreg, EEREC_S); \
		else \
			xe_movss_xm(sreg, &fpuRegs.fpr[_Fs_]); \
	} while (0)

#define ALLOC_S(sreg) \
	do { \
		(sreg) = _allocTempXMMreg(XMMT_FPS); \
		GET_S(sreg); \
	} while (0)

#define GET_T(treg) \
	do { \
		if (info & PROCESS_EE_T) \
			xe_movss_xx(treg, EEREC_T); \
		else \
			xe_movss_xm(treg, &fpuRegs.fpr[_Ft_]); \
	} while (0)

#define ALLOC_T(treg) \
	do { \
		(treg) = _allocTempXMMreg(XMMT_FPS); \
		GET_T(treg); \
	} while (0)

#define GET_ACC(areg) \
	do { \
		if (info & PROCESS_EE_ACC) \
			xe_movss_xx(areg, EEREC_ACC); \
		else \
			xe_movss_xm(areg, &fpuRegs.ACC); \
	} while (0)

#define ALLOC_ACC(areg) \
	do { \
		(areg) = _allocTempXMMreg(XMMT_FPS); \
		GET_ACC(areg); \
	} while (0)

#define CLEAR_OU_FLAGS \
	do { \
		xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagO | FPUflagU)); \
	} while (0)

/* ------------------------------------------------------------------ */
/* ABS XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recABS_S_xmm(int info)
{
	GET_S(EEREC_D);

	CLEAR_OU_FLAGS;

	xe_andps_xm(EEREC_D, s_const.pos);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* FPU_ADD_SUB (Used to mimic PS2's FPU add/sub behavior) */
/* ------------------------------------------------------------------ */
/* Compliant IEEE FPU uses, in computations, uses additional "guard" bits to the right of the mantissa */
/* but EE-FPU doesn't. Substraction (and addition of positive and negative) may shift the mantissa left, */
/* causing those bits to appear in the result; this function masks out the bits of the mantissa that will */
/* get shifted right to the guard bits to ensure that the guard bits are empty. */
/* The difference of the exponents = the amount that the smaller operand will be shifted right by. */
/* Modification - the PS2 uses a single guard bit? (Coded by Nneeve) */
/* ------------------------------------------------------------------ */
static void FPU_ADD_SUB(int tempd, int tempt) /* tempd and tempt are overwritten, they are floats */
{
	u8 *j8Ptr0, *j8Ptr1, *j8Ptr2, *j8Ptr3, *j8Ptr4, *j8Ptr5, *j8Ptr6;
	const int xmmtemp = _allocTempXMMreg(XMMT_FPS); /* temporary for anding with regd/regt */
	xe_movd_rx(XE_CX, tempd); /* receives regd */
	xe_movd_rx(XE_AX, tempt); /* receives regt */

	/* mask the exponents */
	xe_shr32_ri(XE_CX, 23);
	xe_shr32_ri(XE_AX, 23);
	xe_and32_ri(XE_CX, 0xff);
	xe_and32_ri(XE_AX, 0xff);

	xe_sub32_rr(XE_CX, XE_AX); /* tempecx = exponent difference */
	xe_cmp32_ri(XE_CX, 25);
	xe_fwd_jcc8(Jcc_GreaterOrEqual, j8Ptr0);
	xe_cmp32_ri(XE_CX, 0);
	xe_fwd_jcc8(Jcc_Greater, j8Ptr1);
	xe_fwd_jcc8(Jcc_Equal, j8Ptr2);
	xe_cmp32_ri(XE_CX, -25);
	xe_fwd_jcc8(Jcc_LessOrEqual, j8Ptr3);

	/* diff = -24 .. -1 , expd < expt */
	xe_neg32_r(XE_CX);
	xe_dec32_r(XE_CX);
	xe_mov32_ri(XE_AX, 0xffffffff);
	xe_shl32_rcl(XE_AX); /* temp2 = 0xffffffff << tempecx */
	xe_movdzx_xr(xmmtemp, XE_AX);
	xe_andps_xx(tempd, xmmtemp);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr4);

	xe_fwd_set8(j8Ptr0);
	/* diff = 25 .. 255 , expt < expd */
	xe_andps_xm(tempt, s_const.neg);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr5);

	xe_fwd_set8(j8Ptr1);
	/* diff = 1 .. 24, expt < expd */
	xe_dec32_r(XE_CX);
	xe_mov32_ri(XE_AX, 0xffffffff);
	xe_shl32_rcl(XE_AX); /* temp2 = 0xffffffff << tempecx */
	xe_movdzx_xr(xmmtemp, XE_AX);
	xe_andps_xx(tempt, xmmtemp);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr6);

	xe_fwd_set8(j8Ptr3);
	/* diff = -255 .. -25, expd < expt */
	xe_andps_xm(tempd, s_const.neg);

	xe_fwd_set8(j8Ptr2);
	/* diff == 0 */

	xe_fwd_set8(j8Ptr4);
	xe_fwd_set8(j8Ptr5);
	xe_fwd_set8(j8Ptr6);

	_freeXMMreg(xmmtemp);
}

static void FPU_MUL(int info, int regd, int sreg, int treg, int acc)
{
	uint8_t* endMul = NULL;

	if (CHECK_FPUMULHACK)
	{
		/* 	if ((s == 0x3e800000) && (t == 0x40490fdb)) */
		/* 		return 0x3f490fda; // needed for Tales of Destiny Remake (only in a very specific room late-game) */
		/* 	else */
		/* 		return 0; */

		PCSX2_ALIGN(16) static const u32 result[4] = { 0x3f490fda };
		uint8_t* noHack;

		xe_movd_rx(XE_CX, sreg);
		xe_movd_rx(XE_DX, treg);

		/* if (((s ^ 0x3e800000) | (t ^ 0x40490fdb)) != 0) { hack; } */
		xe_xor32_ri(XE_CX, 0x3e800000);
		xe_xor32_ri(XE_DX, 0x40490fdb);
		xe_or32_rr(XE_DX, XE_CX);

		 xe_fwd_jcc8(Jcc_NotZero, noHack);
			xe_movaps_xm(regd, result);
			xe_fwd_jcc32(Jcc_Unconditional, endMul);
		xe_fwd_set8(noHack);
	}

	ToDouble(sreg);
	ToDouble(treg);
	xe_mulsd_xx(sreg, treg);
	ToPS2FPU_Full(sreg, 1, treg, acc, 0);
	xe_movss_xx(regd, sreg);

	if (CHECK_FPUMULHACK)
		xe_fwd_set32(endMul);
}

/* ------------------------------------------------------------------ */
/* CommutativeOp XMM (used for ADD and SUB opcodes. that's it.) */
/* ------------------------------------------------------------------ */
/* ADD and SUB, selected by op. Was a two-entry function-pointer table; the
 * entries are emitter macros with no address, and op is constant at every
 * call site, so the switch inlines to a direct emission. */
static __fi void recFPUOpEmit(int op, int regd, int regt)
{
	if (op == 0)
		xe_addsd_xx(regd, regt);
	else
		xe_subsd_xx(regd, regt);
}

static void recFPUOp(int info, int regd, int op, int acc)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_ADD_SUB(sreg, treg);

	ToDouble(sreg);
	ToDouble(treg);

	recFPUOpEmit(op, sreg, treg);

	ToPS2FPU_Full(sreg, 1, treg, acc, 1);
	xe_movss_xx(regd, sreg);

	_freeXMMreg(sreg); _freeXMMreg(treg);
}
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* ADD XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recADD_S_xmm(int info)
{
	recFPUOp(info, EEREC_D, 0, 0);
}

void DOUBLE_recADDA_S_xmm(int info)
{
	recFPUOp(info, EEREC_ACC, 0, 1);
}

/* ------------------------------------------------------------------ */

void recCMP(int info)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);
	ToDouble(sreg);
	ToDouble(treg);

	xe_ucomisd_xx(sreg, treg);

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

/* ------------------------------------------------------------------ */
/* C.x.S XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recC_EQ_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	recCMP(info);

	xe_fwd_jcc8(Jcc_Zero, j8Ptr0);
	xe_and32_mi(&fpuRegs.fprc[31], ~FPUflagC);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr1);
	xe_fwd_set8(j8Ptr0);
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagC);
	xe_fwd_set8(j8Ptr1);
}

void DOUBLE_recC_LE_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	recCMP(info);

	xe_fwd_jcc8(Jcc_BelowOrEqual, j8Ptr0);
	xe_and32_mi(&fpuRegs.fprc[31], ~FPUflagC);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr1);
	xe_fwd_set8(j8Ptr0);
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagC);
	xe_fwd_set8(j8Ptr1);
}

void DOUBLE_recC_LT_xmm(int info)
{
	u8 *j8Ptr0, *j8Ptr1;
	recCMP(info);

	xe_fwd_jcc8(Jcc_Below, j8Ptr0);
	xe_and32_mi(&fpuRegs.fprc[31], ~FPUflagC);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr1);
	xe_fwd_set8(j8Ptr0);
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagC);
	xe_fwd_set8(j8Ptr1);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* CVT.x XMM */
/* ------------------------------------------------------------------ */
/* CVT.S: Identical to non-double variant, omitted */
/* CVT.W: Identical to non-double variant, omitted */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* DIV XMM */
/* ------------------------------------------------------------------ */
static void recDIVhelper1(int regd, int regt) /* Sets flags */
{
	u8 *pjmp1, *pjmp2;
	uint8_t *ajmp32, *bjmp32;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagI | FPUflagD)); /* Clear I and D flags */

	/* --- Check for divide by zero --- */
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, regt);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); /* Check sign (if regt == zero, sign will be set) */
	xe_fwd_jcc32(Jcc_Zero, ajmp32); /* Skip if not set */

	/* --- Check for 0/0 --- */
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, regd);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); /* Check sign (if regd == zero, sign will be set) */
	xe_fwd_jcc8(Jcc_Zero, pjmp1); /* Skip if not set */
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); /* Set I and SI flags ( 0/0 ) */
	xe_fwd_jcc8(Jcc_Unconditional, pjmp2);
	xe_fwd_set8(pjmp1); /* x/0 but not 0/0 */
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagD | FPUflagSD); /* Set D and SD flags ( x/0 ) */
	xe_fwd_set8(pjmp2);

	/* --- Make regd +/- Maximum --- */
	xe_xorps_xx(regd, regt); /* Make regd Positive or Negative */
	SetMaxValue(regd); /* clamp to max */
	xe_fwd_jcc32(Jcc_Unconditional, bjmp32);

	xe_fwd_set32(ajmp32);

	/* --- Normal Divide --- */
	ToDouble(regd);
	ToDouble(regt);

	xe_divsd_xx(regd, regt);

	ToPS2FPU_Full(regd, 0, regt, 0, 0);

	xe_fwd_set32(bjmp32);

	_freeXMMreg(t1reg);
}

PCSX2_ALIGN(16) static FPControlRegister roundmode_nearest;

void DOUBLE_recDIV_S_xmm(int info)
{
	int sreg, treg;

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUDivFPCR.bitmask);

	ALLOC_S(sreg);
	ALLOC_T(treg);

	recDIVhelper1(sreg, treg);

	xe_movss_xx(EEREC_D, sreg);

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* MADD/MSUB XMM */
/* ------------------------------------------------------------------ */

/* Unlike what the documentation implies, it seems that MADD/MSUB support all numbers just like other operations */
/* The complex overflow conditions the document describes apparently test whether the multiplication's result */
/* has overflowed and whether the last operation that used ACC as a destination has overflowed. */
/* For example,   { adda.s -MAX, 0.0 ; madd.s fd, MAX, 1.0 } -> fd = 0 */
/* while          { adda.s -MAX, -MAX ; madd.s fd, MAX, 1.0 } -> fd = -MAX */
/* (where MAX is 0x7fffffff and -MAX is 0xffffffff) */
static void recMaddsub(int info, int regd, int op, int acc)
{
	int sreg, treg;
	uint8_t* mulovf;
	uint8_t* accovf;
	uint8_t* operation;
	uint8_t* skipall;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_MUL(info, sreg, sreg, treg, 0);

	GET_ACC(treg);

	FPU_ADD_SUB(treg, sreg); /* might be problematic for something!!!! */

	/*          TEST FOR ACC/MUL OVERFLOWS, PROPOGATE THEM IF THEY OCCUR */

	xe_test32_mi(&fpuRegs.fprc[31], FPUflagO);
	 xe_fwd_jcc8(Jcc_NotZero, mulovf);
	ToDouble(sreg); /* else, convert */

	xe_test32_mi(&fpuRegs.ACCflag, 1);
	 xe_fwd_jcc8(Jcc_NotZero, accovf);
	ToDouble(treg); /* else, convert */
	 xe_fwd_jcc8(Jcc_Unconditional, operation);

	xe_fwd_set8(mulovf);
	if (op == 1) /* sub */
		xe_xorps_xm(sreg, s_const.neg);
	xe_movaps_xx(treg, sreg); /* fall through below */

	xe_fwd_set8(accovf);
	SetMaxValue(treg); /* just in case... I think it has to be a MaxValue already here */
	CLEAR_OU_FLAGS; /* clear U flag */
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagO | FPUflagSO);
	if (acc)
		xe_or32_mi(&fpuRegs.ACCflag, 1);
	 xe_fwd_jcc32(Jcc_Unconditional, skipall);

	/* 			PERFORM THE ACCUMULATION AND TEST RESULT. CONVERT TO SINGLE */

	xe_fwd_set8(operation);
	if (op == 1)
		xe_subsd_xx(treg, sreg);
	else
		xe_addsd_xx(treg, sreg);

	ToPS2FPU_Full(treg, 1, sreg, acc, 1);
	xe_fwd_set32(skipall);

	xe_movss_xx(regd, treg);

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

void DOUBLE_recMADD_S_xmm(int info)
{
	recMaddsub(info, EEREC_D, 0, 0);
}

void DOUBLE_recMADDA_S_xmm(int info)
{
	recMaddsub(info, EEREC_ACC, 0, 1);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* MAX / MIN XMM */
/* ------------------------------------------------------------------ */

/* FPU's MAX/MIN work with all numbers (including "denormals"). Check VU's logical min max for more info. */
static void recMINMAX(int info, int ismin)
{
	PCSX2_ALIGN(16) static const u32 minmax_mask[8] =
	{
		0xffffffff, 0x80000000, 0, 0,
		0,          0x40000000, 0, 0,
	};
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	CLEAR_OU_FLAGS;

	xe_pshufd_xxi(sreg, sreg, 0x00);
	xe_pand_xm(sreg, minmax_mask);
	xe_por_xm(sreg, &minmax_mask[4]);
	xe_pshufd_xxi(treg, treg, 0x00);
	xe_pand_xm(treg, minmax_mask);
	xe_por_xm(treg, &minmax_mask[4]);
	if (ismin)
		xe_minsd_xx(sreg, treg);
	else
		xe_maxsd_xx(sreg, treg);

	xe_movss_xx(EEREC_D, sreg);

	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

void DOUBLE_recMAX_S_xmm(int info)
{
	recMINMAX(info, 0);
}

void DOUBLE_recMIN_S_xmm(int info)
{
	recMINMAX(info, 1);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* MOV XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recMOV_S_xmm(int info)
{
	GET_S(EEREC_D);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* MSUB XMM */
/* ------------------------------------------------------------------ */

void DOUBLE_recMSUB_S_xmm(int info)
{
	recMaddsub(info, EEREC_D, 1, 0);
}

void DOUBLE_recMSUBA_S_xmm(int info)
{
	recMaddsub(info, EEREC_ACC, 1, 1);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* MUL XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recMUL_S_xmm(int info)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_MUL(info, EEREC_D, sreg, treg, 0);
	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

void DOUBLE_recMULA_S_xmm(int info)
{
	int sreg, treg;
	ALLOC_S(sreg);
	ALLOC_T(treg);

	FPU_MUL(info, EEREC_ACC, sreg, treg, 1);
	_freeXMMreg(sreg);
	_freeXMMreg(treg);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* NEG XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recNEG_S_xmm(int info)
{
	GET_S(EEREC_D);

	CLEAR_OU_FLAGS;

	xe_xorps_xm(EEREC_D, &s_const.neg[0]);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* SUB XMM */
/* ------------------------------------------------------------------ */

void DOUBLE_recSUB_S_xmm(int info)
{
	recFPUOp(info, EEREC_D, 1, 0);
}

void DOUBLE_recSUBA_S_xmm(int info)
{
	recFPUOp(info, EEREC_ACC, 1, 1);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* SQRT XMM */
/* ------------------------------------------------------------------ */
void DOUBLE_recSQRT_S_xmm(int info)
{
	int roundmodeFlag = 0;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	if (fpcr_round_mode(EmuConfig.Cpu.FPUFPCR) != FPROUND_NEAREST)
	{
		/* Set roundmode to nearest if it isn't already */
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest = fpcr_set_round_mode(roundmode_nearest, FPROUND_NEAREST);
		xe_ldmxcsr_m(&roundmode_nearest.bitmask);
		roundmodeFlag = 1;
	}

	GET_T(EEREC_D);

	{
		uint8_t* pjmp;
		xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagI | FPUflagD)); /* Clear I and D flags */

		/* --- Check for negative SQRT --- (sqrt(-0) = 0, unlike what the docs say) */
		xe_movmskps_rx(XE_AX, EEREC_D);
		xe_and32_ri(XE_AX, 1); /* Check sign */
		 xe_fwd_jcc8(Jcc_Zero, pjmp); /* Skip if none are */
			xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); /* Set I and SI flags */
			xe_andps_xm(EEREC_D, &s_const.pos[0]); /* Make EEREC_D Positive */
		xe_fwd_set8(pjmp);
	}

	ToDouble(EEREC_D);

	xe_sqrtsd_xx(EEREC_D, EEREC_D);

	ToPS2FPU_Full(EEREC_D, 0, t1reg, 0, 0);

	if (roundmodeFlag == 1)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);

	_freeXMMreg(t1reg);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* RSQRT XMM */
/* ------------------------------------------------------------------ */
static void recRSQRThelper1(int regd, int regt) /* Preforms the RSQRT function when regd <- Fs and regt <- Ft (Sets correct flags) */
{
	u8 *pjmp1, *pjmp2;
	u8 *qjmp1, *qjmp2;
	uint8_t* pjmp32;
	int t1reg = _allocTempXMMreg(XMMT_FPS);

	xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagI | FPUflagD)); /* Clear I and D flags */

	/* --- (first) Check for negative SQRT --- */
	xe_movmskps_rx(XE_AX, regt);
	xe_and32_ri(XE_AX, 1); /* Check sign */
	xe_fwd_jcc8(Jcc_Zero, pjmp2); /* Skip if not set */
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); /* Set I and SI flags */
	xe_andps_xm(regt, &s_const.pos[0]); /* Make regt Positive */
	xe_fwd_set8(pjmp2);

	/* --- Check for zero --- */
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, regt);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); /* Check sign (if regt == zero, sign will be set) */
	xe_fwd_jcc8(Jcc_Zero, pjmp1); /* Skip if not set */

	/* --- Check for 0/0 --- */
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, regd);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); /* Check sign (if regd == zero, sign will be set) */
	xe_fwd_jcc8(Jcc_Zero, qjmp1); /* Skip if not set */
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); /* Set I and SI flags ( 0/0 ) */
	xe_fwd_jcc8(Jcc_Unconditional, qjmp2);
	xe_fwd_set8(qjmp1); /* x/0 but not 0/0 */
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagD | FPUflagSD); /* Set D and SD flags ( x/0 ) */
	xe_fwd_set8(qjmp2);

	SetMaxValue(regd); /* clamp to max */
	xe_fwd_jcc32(Jcc_Unconditional, pjmp32);
	xe_fwd_set8(pjmp1);

	ToDouble(regt);
	ToDouble(regd);

	xe_sqrtsd_xx(regt, regt);
	xe_divsd_xx(regd, regt);

	ToPS2FPU_Full(regd, 0, regt, 0, 0);
	xe_fwd_set32(pjmp32);

	_freeXMMreg(t1reg);
}

void DOUBLE_recRSQRT_S_xmm(int info)
{
	int sreg, treg;

	/* iFPU (regular FPU) doesn't touch roundmode for rSQRT. */
	/* Should this do the same?  or is changing the roundmode to nearest the better */
	/* behavior for both recs? --air */

	int roundmodeFlag = 0;
	if (fpcr_round_mode(EmuConfig.Cpu.FPUFPCR) != FPROUND_NEAREST)
	{
		/* Set roundmode to nearest if it isn't already */
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest = fpcr_set_round_mode(roundmode_nearest, FPROUND_NEAREST);
		xe_ldmxcsr_m(&roundmode_nearest.bitmask);
		roundmodeFlag = 1;
	}

	ALLOC_S(sreg);
	ALLOC_T(treg);

	recRSQRThelper1(sreg, treg);

	xe_movss_xx(EEREC_D, sreg);

	_freeXMMreg(treg);
	_freeXMMreg(sreg);

	if (roundmodeFlag)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);
}

#endif
