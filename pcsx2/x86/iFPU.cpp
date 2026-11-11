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

#include "../../common/VectorIntrin.h"

#include "../Common.h"
#include "../Config.h"
#include "../R5900OpcodeTables.h"
#include "iR5900.h"
#include "common/emitter/c89ops.h"
#include "iFPU.h"
alignas(16) const u32 g_minvals[4] = {0xff7fffff, 0xff7fffff, 0xff7fffff, 0xff7fffff};
alignas(16) const u32 g_maxvals[4] = {0x7f7fffff, 0x7f7fffff, 0x7f7fffff, 0x7f7fffff};

//------------------------------------------------------------------

/* Forward declarations of the full-mode (DOUBLE) FPU emitters; the
 * namespace is gone, the prefix carries the distinction. */
void DOUBLE_recABS_S_xmm(int info);
void DOUBLE_recADD_S_xmm(int info);
void DOUBLE_recADDA_S_xmm(int info);
void DOUBLE_recC_EQ_xmm(int info);
void DOUBLE_recC_LE_xmm(int info);
void DOUBLE_recC_LT_xmm(int info);
void DOUBLE_recDIV_S_xmm(int info);
void DOUBLE_recMADD_S_xmm(int info);
void DOUBLE_recMADDA_S_xmm(int info);
void DOUBLE_recMAX_S_xmm(int info);
void DOUBLE_recMIN_S_xmm(int info);
void DOUBLE_recMOV_S_xmm(int info);
void DOUBLE_recMSUB_S_xmm(int info);
void DOUBLE_recMSUBA_S_xmm(int info);
void DOUBLE_recMUL_S_xmm(int info);
void DOUBLE_recMULA_S_xmm(int info);
void DOUBLE_recNEG_S_xmm(int info);
void DOUBLE_recSUB_S_xmm(int info);
void DOUBLE_recSUBA_S_xmm(int info);
void DOUBLE_recSQRT_S_xmm(int info);
void DOUBLE_recRSQRT_S_xmm(int info);

//------------------------------------------------------------------
// Helper Macros
//------------------------------------------------------------------
#define _Ft_ _Rt_
#define _Fs_ _Rd_
#define _Fd_ _Sa_

// FCR31 Flags
#define FPUflagC  0x00800000
#define FPUflagI  0x00020000
#define FPUflagD  0x00010000
#define FPUflagO  0x00008000
#define FPUflagU  0x00004000
#define FPUflagSI 0x00000040
#define FPUflagSD 0x00000020
#define FPUflagSO 0x00000010
#define FPUflagSU 0x00000008

// Add/Sub opcodes produce the same results as the ps2
#define FPU_CORRECT_ADD_SUB 1

alignas(16) static const u32 s_neg[4] = {0x80000000, 0xffffffff, 0xffffffff, 0xffffffff};
alignas(16) static const u32 s_pos[4] = {0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff};

#define REC_FPUBRANCH(f) \
	void f(); \
	void rec##f() \
	{ \
		iFlushCall(FLUSH_INTERPRETER); \
		xe_fastcall0(int##f); \
		g_branch = 2; \
	}

#define REC_FPUFUNC(f) \
	void f(); \
	void rec##f() \
	{ \
		iFlushCall(FLUSH_INTERPRETER); \
		xe_fastcall0(int##f); \
	}
//------------------------------------------------------------------

//------------------------------------------------------------------
// *FPU Opcodes!*
//------------------------------------------------------------------

// Those opcode are marked as special ! But I don't understand why we can't run them in the interpreter

//------------------------------------------------------------------
// CFC1 / CTC1
//------------------------------------------------------------------
void recCFC1(void)
{
	if (!_Rt_)
		return;

	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);
	if (_Fs_ >= 16)
	{
		xe_mov32_rm(regt, &fpuRegs.fprc[31]);
		xe_and32_ri(regt, 0x0083c078); //remove always-zero bits
		xe_or32_ri(regt, 0x01000001); //set always-one bits
		xe_movsxd_rr(regt, regt);
	}
	else
	{
		xe_movsxd_rm(regt, &fpuRegs.fprc[0]);
	}
}

void recCTC1(void)
{
	if (_Fs_ != 31)
		return;

	if (GPR_IS_CONST1(_Rt_))
	{
		xe_mov32_mi(&fpuRegs.fprc[_Fs_], g_cpuConstRegs[_Rt_].UL[0]);
	}
	else
	{
		int mmreg = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);

		if (mmreg >= 0)
		{
			xe_movss_mx(&fpuRegs.fprc[_Fs_], mmreg);
		}
		else if ((mmreg = _checkX86reg(X86TYPE_GPR, _Rt_, MODE_READ)) >= 0)
		{
			xe_mov32_mr(&fpuRegs.fprc[_Fs_], mmreg);
		}
		else
		{
			_deleteGPRtoXMMreg(_Rt_, 1);

			xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rt_].UL[0]);
			xe_mov32_mr(&fpuRegs.fprc[_Fs_], XE_AX);
		}
	}
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// MFC1
//------------------------------------------------------------------

void recMFC1(void)
{
	if (!_Rt_)
		return;

	const int xmmregt = _allocIfUsedGPRtoXMM(_Rt_, MODE_READ | MODE_WRITE);
	const int regs = _allocIfUsedFPUtoXMM(_Fs_, MODE_READ);
	if (regs >= 0 && xmmregt >= 0)
	{
		// both in xmm, sign extend and insert lower bits
		const int temp = _allocTempXMMreg(XMMT_FPS);
		xe_movaps_xx(temp, regs);
		xe_psrad_xi(temp, 31);
		xe_movss_xx(xmmregt, regs);
		xe_insertps_xxi(xmmregt, temp, _MM_MK_INSERTPS_NDX(0, 1, 0));
		_freeXMMreg(temp);
		return;
	}

	// storing to a gpr..
	const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_WRITE);

	if (regs >= 0)
	{
		// xmm -> gpr
		xe_movd_rx(regt, regs);
		xe_movsxd_rr(regt, regt);
	}
	else
	{
		// mem -> gpr
		xe_movsxd_rm(regt, &fpuRegs.fpr[_Fs_].UL);
	}
}

//------------------------------------------------------------------


//------------------------------------------------------------------
// MTC1
//------------------------------------------------------------------
void recMTC1(void)
{
	if (GPR_IS_CONST1(_Rt_))
	{
		const int xmmreg = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);
		if (xmmreg >= 0)
		{
			// common case: mtc1 zero, fnn
			if (g_cpuConstRegs[_Rt_].UL[0] == 0)
			{
				xe_pxor_xx(xmmreg, xmmreg);
			}
			else
			{
				// may as well flush the constant register, since we're needing it in a gpr anyway
				const int x86reg = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
				xe_movdzx_xr(xmmreg, x86reg);
			}
		}
		else
		{
			xe_mov32_mi(&fpuRegs.fpr[_Fs_].UL, g_cpuConstRegs[_Rt_].UL[0]);
		}
	}
	else
	{
		const int xmmgpr = _checkXMMreg(XMMTYPE_GPRREG, _Rt_, MODE_READ);
		if (xmmgpr >= 0)
		{
			if (g_pCurInstInfo->regs[_Rt_] & EEINST_LASTUSE)
			{
				// transfer the reg directly
				_deleteFPtoXMMreg(_Fs_, DELETE_REG_FREE_NO_WRITEBACK);
				_reallocateXMMreg(xmmgpr, XMMTYPE_FPREG, _Fs_, MODE_WRITE, 1);
			}
			else
			{
				const int xmmreg2 = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);
				if (xmmreg2 >= 0)
					xe_movss_xx(xmmreg2, xmmgpr);
				else
					xe_movss_mx(&fpuRegs.fpr[_Fs_].UL, xmmgpr);
			}
		}
		else
		{
			// may as well cache it..
			const int regt = _allocX86reg(X86TYPE_GPR, _Rt_, MODE_READ);
			const int mmreg2 = _allocIfUsedFPUtoXMM(_Fs_, MODE_WRITE);

			if (mmreg2 >= 0)
			{
				xe_movdzx_xr(mmreg2, regt);
			}
			else
			{
				xe_mov32_mr(&fpuRegs.fpr[_Fs_].UL, regt);
			}
		}
	}
}
//------------------------------------------------------------------


#ifndef FPU_RECOMPILE // If FPU_RECOMPILE is not defined, then use the interpreter opcodes. (CFC1, CTC1, MFC1, and MTC1 are special because they work specifically with the EE rec so they're defined above)

REC_FPUFUNC(ABS_S);
REC_FPUFUNC(ADD_S);
REC_FPUFUNC(ADDA_S);
REC_FPUBRANCH(BC1F);
REC_FPUBRANCH(BC1T);
REC_FPUBRANCH(BC1FL);
REC_FPUBRANCH(BC1TL);
REC_FPUFUNC(C_EQ);
REC_FPUFUNC(C_F);
REC_FPUFUNC(C_LE);
REC_FPUFUNC(C_LT);
REC_FPUFUNC(CVT_S);
REC_FPUFUNC(CVT_W);
REC_FPUFUNC(DIV_S);
REC_FPUFUNC(MAX_S);
REC_FPUFUNC(MIN_S);
REC_FPUFUNC(MADD_S);
REC_FPUFUNC(MADDA_S);
REC_FPUFUNC(MOV_S);
REC_FPUFUNC(MSUB_S);
REC_FPUFUNC(MSUBA_S);
REC_FPUFUNC(MUL_S);
REC_FPUFUNC(MULA_S);
REC_FPUFUNC(NEG_S);
REC_FPUFUNC(SUB_S);
REC_FPUFUNC(SUBA_S);
REC_FPUFUNC(SQRT_S);
REC_FPUFUNC(RSQRT_S);

#else // FPU_RECOMPILE

//------------------------------------------------------------------
// Clamp Functions (Converts NaN's and Infinities to Normal Numbers)
//------------------------------------------------------------------

static int fpuCopyToTempForClamp(int fpureg, int xmmreg)
{
	if (FPUINST_USEDTEST(fpureg))
	{
		const int tempreg = _allocTempXMMreg(XMMT_FPS);
		xe_movss_xx(tempreg, xmmreg);
		return tempreg;
	}

	/* flush back the original value, before we mess with it below */
	_flushXMMreg(xmmreg);

	/* turn it into a temp, so in case the liveness was incorrect, 
	 * we don't reuse it after clamp */
	_reallocateXMMreg(xmmreg, XMMTYPE_TEMP, 0, 0, 1);
	return xmmreg;
}

static void fpuFreeIfTemp(int xmmreg)
{
	if (xmmregs[xmmreg].inuse && xmmregs[xmmreg].type == XMMTYPE_TEMP)
		_freeXMMreg(xmmreg);
}

static __fi void fpuFloat3(int regd) // +NaN -> +fMax, -NaN -> -fMax, +Inf -> +fMax, -Inf -> -fMax
{
	/* SSE4 codepath */
#if _M_SSE >= 0x401
	xe_pminsd_xm(regd, &g_maxvals[0]);
	xe_pminud_xm(regd, &g_minvals[0]);
	/* SSE2 codepath */
#elif _M_SSE >= 0x200
	const int t1reg = _allocTempXMMreg(XMMT_FPS);
	xe_movss_xx(t1reg, regd);
	xe_andps_xm(t1reg, &s_neg[0]);
	xe_minss_xm(regd, &g_maxvals[0]);
	xe_maxss_xm(regd, &g_minvals[0]);
	xe_orps_xx(regd, t1reg);
	_freeXMMreg(t1reg);
#endif
}

static __fi void fpuFloat(int regd) // +/-NaN -> +fMax, +Inf -> +fMax, -Inf -> -fMax
{
	if (CHECK_FPU_OVERFLOW)
	{
		xe_minss_xm(regd, &g_maxvals[0]); // MIN() must be before MAX()! So that NaN's become +Maximum
		xe_maxss_xm(regd, &g_minvals[0]);
	}
}

static __fi void fpuFloat2(int regd) // +NaN -> +fMax, -NaN -> -fMax, +Inf -> +fMax, -Inf -> -fMax
{
	if (CHECK_FPU_OVERFLOW)
	{
		fpuFloat3(regd);
	}
}

//------------------------------------------------------------------


//------------------------------------------------------------------
// ABS XMM
//------------------------------------------------------------------
void recABS_S_xmm(int info)
{
	if (info & PROCESS_EE_S)
		xe_movss_xx(EEREC_D, EEREC_S);
	else
		xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);

	xe_andps_xm(EEREC_D, &s_pos[0]);
	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags

	/* Hardware ABS is a pure sign-bit operation; this clamp exists
	 * only to keep exp-255 patterns out of downstream HOST adds,
	 * which are the one consumer class that does not input-clamp by
	 * default (op >= 2 always clamps its operands). When an accuracy
	 * option covers the add path -- soft float or accurate
	 * arithmetic -- every consumer handles raw operands and the
	 * clamp only destroys round-trippable bit patterns (EE census:
	 * ABS/NEG were the orphan divergence no option closed). */
	if (CHECK_FPU_OVERFLOW && !CHECK_FPU_SOFT_REC && !CHECK_FPU_ACC_ARITH)
		xe_minss_xm(EEREC_D, &g_maxvals[0]);
}

FPURECOMPILE_CONSTCODE(ABS_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// FPU_ADD_SUB (Used to mimic PS2's FPU add/sub behavior)
//------------------------------------------------------------------
// Compliant IEEE FPU uses, in computations, uses additional "guard" bits to the right of the mantissa
// but EE-FPU doesn't. Substraction (and addition of positive and negative) may shift the mantissa left,
// causing those bits to appear in the result; this function masks out the bits of the mantissa that will
// get shifted right to the guard bits to ensure that the guard bits are empty.
// The difference of the exponents = the amount that the smaller operand will be shifted right by.
// Modification - the PS2 uses a single guard bit? (Coded by Nneeve)
//------------------------------------------------------------------
static void FPU_ADD_SUB(int regd, int regt, int issub)
{
	u8 *j8Ptr0, *j8Ptr1, *j8Ptr2, *j8Ptr3;
	u8 *j8Ptr4, *j8Ptr5, *j8Ptr6, *j8Ptr7;
	const int xmmtemp = _allocTempXMMreg(XMMT_FPS); //temporary for anding with regd/regt
	xe_movd_rx(XE_CX, regd); // ecx receives regd
	xe_movd_rx(XE_AX, regt); // eax receives regt

	//mask the exponents
	xe_shr32_ri(XE_CX, 23);
	xe_shr32_ri(XE_AX, 23);
	xe_and32_ri(XE_CX, 0xff);
	xe_and32_ri(XE_AX, 0xff);

	xe_sub32_rr(XE_CX, XE_AX); //tempecx = exponent difference
	xe_cmp32_ri(XE_CX, 25);
	xe_fwd_jcc8(Jcc_GreaterOrEqual, j8Ptr0);
	xe_cmp32_ri(XE_CX, 0);
	xe_fwd_jcc8(Jcc_Greater, j8Ptr1);
	xe_fwd_jcc8(Jcc_Equal, j8Ptr2);
	xe_cmp32_ri(XE_CX, -25);
	xe_fwd_jcc8(Jcc_LessOrEqual, j8Ptr3);

	//diff = -24 .. -1 , expd < expt
	xe_neg32_r(XE_CX);
	xe_dec32_r(XE_CX);
	xe_mov32_ri(XE_AX, 0xffffffff);
	xe_shl32_rcl(XE_AX); //temp2 = 0xffffffff << tempecx
	xe_movdzx_xr(xmmtemp, XE_AX);
	xe_andps_xx(regd, xmmtemp);
	if (issub)
		xe_subss_xx(regd, regt);
	else
		xe_addss_xx(regd, regt);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr4);

	xe_fwd_set8(j8Ptr0);
	//diff = 25 .. 255 , expt < expd
	xe_movaps_xx(xmmtemp, regt);
	xe_andps_xm(xmmtemp, s_neg);
	if (issub)
		xe_subss_xx(regd, xmmtemp);
	else
		xe_addss_xx(regd, xmmtemp);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr5);

	xe_fwd_set8(j8Ptr1);
	//diff = 1 .. 24, expt < expd
	xe_dec32_r(XE_CX);
	xe_mov32_ri(XE_AX, 0xffffffff);
	xe_shl32_rcl(XE_AX); //temp2 = 0xffffffff << tempecx
	xe_movdzx_xr(xmmtemp, XE_AX);
	xe_andps_xx(xmmtemp, regt);
	if (issub)
		xe_subss_xx(regd, xmmtemp);
	else
		xe_addss_xx(regd, xmmtemp);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr6);

	xe_fwd_set8(j8Ptr3);
	//diff = -255 .. -25, expd < expt
	xe_andps_xm(regd, s_neg);
	if (issub)
		xe_subss_xx(regd, regt);
	else
		xe_addss_xx(regd, regt);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr7);

	xe_fwd_set8(j8Ptr2);
	//diff == 0
	if (issub)
		xe_subss_xx(regd, regt);
	else
		xe_addss_xx(regd, regt);

	xe_fwd_set8(j8Ptr4);
	xe_fwd_set8(j8Ptr5);
	xe_fwd_set8(j8Ptr6);
	xe_fwd_set8(j8Ptr7);

	_freeXMMreg(xmmtemp);
}

#ifdef FPU_CORRECT_ADD_SUB
static void FPU_ADD_WRAP(int regd, int regt) { FPU_ADD_SUB(regd, regt, 0); }
#define FPU_ADD(a, b) FPU_ADD_SUB(a, b, 0)
#define FPU_SUB(a, b) FPU_ADD_SUB(a, b, 1)
#else
static void FPU_ADD(int regd, int regt)
{
	xe_addss_xx(regd, regt);
}

static void FPU_SUB(int regd, int regt)
{
	xe_subss_xx(regd, regt);
}
#endif

//------------------------------------------------------------------
// Note: PS2's multiplication uses some variant of booth multiplication with wallace trees:
// It cuts off some bits, resulting in inaccurate and non-commutative results.
// The PS2's result mantissa is either equal to x86's rounding to zero result mantissa
// or SMALLER (by 0x1). (this means that x86's other rounding modes are only less similar to PS2's mul)
//------------------------------------------------------------------
static void FPU_MUL(int regd, int regt, int reverseOperands)
{
	u8 *endMul = NULL;

	if (CHECK_FPUMULHACK)
	{
		// 	if ((s == 0x3e800000) && (t == 0x40490fdb))
		// 		return 0x3f490fda; // needed for Tales of Destiny Remake (only in a very specific room late-game)
		// 	else
		// 		return 0;

		alignas(16) static const u32 result[4] = { 0x3f490fda };

		xe_movd_rx(XE_CX, reverseOperands ? regt : regd);
		xe_movd_rx(XE_DX, reverseOperands ? regd : regt);

		// if (((s ^ 0x3e800000) | (t ^ 0x40490fdb)) != 0) { hack; }
		xe_xor32_ri(XE_CX, 0x3e800000);
		xe_xor32_ri(XE_DX, 0x40490fdb);
		xe_or32_rr(XE_DX, XE_CX);

		uint8_t* noHack; xe_fwd_jcc8(Jcc_NotZero, noHack);
			xe_movaps_xm(regd, result);
			xe_fwd_jcc8(Jcc_Unconditional, endMul);
		xe_fwd_set8(noHack);
	}

	xe_mulss_xx(regd, regt);

	if (CHECK_FPUMULHACK)
		xe_fwd_set8(endMul);
}

static void FPU_MUL_WRAP(int regd, int regt) { FPU_MUL(regd, regt, 0); }
static void FPU_MUL_WRAP_REV(int regd, int regt) { FPU_MUL(regd, regt, 1); } //reversed operands

//------------------------------------------------------------------
// CommutativeOp XMM (used for ADD, MUL, MAX, and MIN opcodes)
//------------------------------------------------------------------
/* The commutative ops used to be two function-pointer tables indexed by `op`.
 * Two of the four entries -- MAXSS and MINSS -- are emitter macros with no
 * address to take, and the indirect call bought nothing: `op` is a small
 * constant at every call site, so a switch lets the compiler inline the
 * emission and drop the call entirely. */
enum
{
	FPUCOM_ADD = 0,
	FPUCOM_MUL = 1,
	FPUCOM_MAX = 2,
	FPUCOM_MIN = 3
};

static __fi void recComOp(int op, int regd, int regt)
{
	switch (op)
	{
#ifdef FPU_CORRECT_ADD_SUB
		case FPUCOM_ADD: FPU_ADD_WRAP(regd, regt); break;
#else
		case FPUCOM_ADD: FPU_ADD(regd, regt);      break;
#endif
		case FPUCOM_MUL: FPU_MUL_WRAP(regd, regt); break;
		case FPUCOM_MAX: xe_maxss_xx(regd, regt);  break;
		default:         xe_minss_xx(regd, regt);  break;
	}
}

static __fi void recComOpRev(int op, int regd, int regt)
{
	switch (op)
	{
#ifdef FPU_CORRECT_ADD_SUB
		case FPUCOM_ADD: FPU_ADD_WRAP(regd, regt);     break;
#else
		case FPUCOM_ADD: FPU_ADD(regd, regt);          break;
#endif
		case FPUCOM_MUL: FPU_MUL_WRAP_REV(regd, regt); break;
		case FPUCOM_MAX: xe_maxss_xx(regd, regt);      break;
		default:         xe_minss_xx(regd, regt);      break;
	}
}


static int recCommutativeOp(int info, int regd, int op)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW /*&& !CHECK_FPUCLAMPHACK */ || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(t0reg);
				}
				recComOp(op, regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_S);
				}
				recComOpRev(op, regd, EEREC_S);
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(t0reg);
				}
				recComOpRev(op, regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_T);
				}
				recComOp(op, regd, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_S);
				}
				recComOpRev(op, regd, EEREC_S);
			}
			else
			{
				xe_movss_xx(regd, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
				{
					fpuFloat2(regd);
					fpuFloat2(EEREC_T);
				}
				recComOp(op, regd, EEREC_T);
			}
			break;
		default:
			xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			if (CHECK_FPU_EXTRA_OVERFLOW || (op >= 2))
			{
				fpuFloat2(regd);
				fpuFloat2(t0reg);
			}
			recComOp(op, regd, t0reg);
			break;
	}

	_freeXMMreg(t0reg);
	return regd;
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// ADD XMM
//------------------------------------------------------------------
void recADD_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_D, 0));
}

FPURECOMPILE_CONSTCODE_EXACT_DBL(ADD_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recADDA_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_ACC, 0));
}

FPURECOMPILE_CONSTCODE_EXACT_DBL(ADDA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------

//------------------------------------------------------------------
// BC1x XMM
//------------------------------------------------------------------

// COP1 branch conditionals are based on the following equation:
// (fpuRegs.fprc[31] & 0x00800000)
// BC2F checks if the statement is false, BC2T checks if the statement is true.
#define _setupBranchTest() \
	_eeFlushAllDirty(); \
	xe_mov32_rm(XE_AX, &fpuRegs.fprc[31]); \
	xe_test32_ri(XE_AX, FPUflagC)

void recBC1F(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const int swap = !!(TrySwapDelaySlot(0, 0, 0, 1));
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_NotZero, bslot_); recDoBranchImm(branchTo, bslot_, 0, swap); }
}

void recBC1T(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	const int swap = !!(TrySwapDelaySlot(0, 0, 0, 1));
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_Zero, bslot_); recDoBranchImm(branchTo, bslot_, 0, swap); }
}

void recBC1FL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_NotZero, bslot_); recDoBranchImm(branchTo, bslot_, 1, 0); }
}

void recBC1TL(void)
{
	const u32 branchTo = ((s32)_Imm_ * 4) + pc;
	_setupBranchTest();
	{ uint8_t* bslot_; xe_fwd_jcc32(Jcc_Zero, bslot_); recDoBranchImm(branchTo, bslot_, 1, 0); }
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// C.x.S XMM
//------------------------------------------------------------------
// Integer ordering emissions: the FPU's MAX/MIN and compares are total
// orders on the raw sign-magnitude bits (fp_max and fpuCompareFull in
// the interpreter). Folding the non-sign bits of negatives maps that
// order onto two's-complement, so one signed compare decides it: no
// operand clamps, no host FP, and exponent-255 values order correctly,
// which the clamped comiss/maxss sequences got wrong.
//------------------------------------------------------------------

// gpr := sign-fold(gpr): negatives get their non-sign bits inverted.
static void fpuEmitSignFold(int gpr, int scratch)
{
	xe_mov32_rr(scratch, gpr);
	xe_sar32_ri(scratch, 31);
	xe_shr32_ri(scratch, 1);
	xe_xor32_rr(gpr, scratch);
}

// gpr := 0 when the exponent field is zero (fpuCompareFull's flush).
static void fpuEmitDenormFlush(int gpr, int scratch)
{
	uint8_t* keep;
	xe_mov32_rr(scratch, gpr);
	xe_and32_ri(scratch, 0x7f800000);
	xe_fwd_jcc8(Jcc_NotZero, keep);
	xe_xor32_rr(gpr, gpr);
	xe_fwd_set8(keep);
}

static void fpuLoadRaw(int gpr, int have_reg, int xmmreg, int fpureg)
{
	if (have_reg)
		xe_movd_rx(gpr, xmmreg);
	else
		xe_mov32_rm(gpr, &fpuRegs.fpr[fpureg].UL);
}

static void recMaxMin_int(int info, int ismin)
{
	fpuLoadRaw(XE_AX, info & PROCESS_EE_S, EEREC_S, _Fs_);
	fpuLoadRaw(XE_DX, info & PROCESS_EE_T, EEREC_T, _Ft_);
	fpuEmitSignFold(XE_AX, XE_CX);
	fpuEmitSignFold(XE_DX, XE_CX);
	xe_cmp32_rr(XE_AX, XE_DX);
	xe_cmovcc64_rr(ismin ? Jcc_Greater : Jcc_Less, XE_AX, XE_DX);
	fpuEmitSignFold(XE_AX, XE_CX);
	xe_movd_xr(EEREC_D, XE_AX);
	/* The interpreter's MAX_S/MIN_S clear O and U. */
	xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagO | FPUflagU));
}

static void recCcond_int(int info, int cond)
{
	uint8_t *j8Ptr0, *j8Ptr1;
	fpuLoadRaw(XE_AX, info & PROCESS_EE_S, EEREC_S, _Fs_);
	fpuLoadRaw(XE_DX, info & PROCESS_EE_T, EEREC_T, _Ft_);
	fpuEmitDenormFlush(XE_AX, XE_CX);
	fpuEmitDenormFlush(XE_DX, XE_CX);
	fpuEmitSignFold(XE_AX, XE_CX);
	fpuEmitSignFold(XE_DX, XE_CX);
	xe_cmp32_rr(XE_AX, XE_DX);
	xe_fwd_jcc8(cond, j8Ptr0);
	xe_and32_mi(&fpuRegs.fprc[31], ~FPUflagC);
	xe_fwd_jcc8(Jcc_Unconditional, j8Ptr1);
	xe_fwd_set8(j8Ptr0);
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagC);
	xe_fwd_set8(j8Ptr1);
}

//------------------------------------------------------------------
void recC_EQ_xmm(int info)
{
	recCcond_int(info, Jcc_Equal);
}

FPURECOMPILE_CONSTCODE_EXACT_SS(C_EQ, XMMINFO_READS | XMMINFO_READT);

void recC_F()
{
	xe_and32_mi(&fpuRegs.fprc[31], ~FPUflagC);
}

void recC_LE_xmm(int info)
{
	recCcond_int(info, Jcc_LessOrEqual);
}

FPURECOMPILE_CONSTCODE_EXACT_SS(C_LE, XMMINFO_READS | XMMINFO_READT);

void recC_LT_xmm(int info)
{
	recCcond_int(info, Jcc_Less);
}

FPURECOMPILE_CONSTCODE_EXACT_SS(C_LT, XMMINFO_READS | XMMINFO_READT);
//REC_FPUFUNC(C_LT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// CVT.x XMM
//------------------------------------------------------------------
/* MXCSR window for the accurate convert: hardware CVT.S.W rounds to
 * nearest, but the EE JIT's global MXCSR is chop, leaving the
 * recompiled convert one ulp low on integers wider than 24 bits of
 * mantissa (census: 162/400, untouched by either accuracy option).
 * Under either option, flip rounding to nearest for the convert
 * only. The nearest-mode value is computed at emission time from
 * the configured FPCR with the RC field cleared. */
alignas(16) static u32 s_cvt_mxcsr_saved;
alignas(16) static u32 s_cvt_mxcsr_nearest;

static __fi int cvt_accurate(void) { return CHECK_FPU_SOFT_REC || CHECK_FPU_ACC_ARITH; }

static __fi void cvt_round_open(void)
{
	if (!cvt_accurate())
		return;
	xe_mov32_mi(&s_cvt_mxcsr_nearest, EmuConfig.Cpu.FPUFPCR.bitmask & ~0x6000u);
	xe_stmxcsr_m(&s_cvt_mxcsr_saved);
	xe_ldmxcsr_m(&s_cvt_mxcsr_nearest);
}

static __fi void cvt_round_close(void)
{
	if (!cvt_accurate())
		return;
	xe_ldmxcsr_m(&s_cvt_mxcsr_saved);
}

void recCVT_S_xmm(int info)
{
	cvt_round_open();
	if (info & PROCESS_EE_D)
	{
		if (info & PROCESS_EE_S)
			xe_cvtdq2ps_xx(EEREC_D, EEREC_S);
		else
			xe_cvtsi2ss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
	}
	else
	{
		const int temp = _allocTempXMMreg(XMMT_FPS);
		xe_cvtsi2ss_xm(temp, &fpuRegs.fpr[_Fs_]);
		xe_movss_mx(&fpuRegs.fpr[_Fd_], temp);
		_freeXMMreg(temp);
	}
	cvt_round_close();
}

void recCVT_S(void)
{
	/* Float version is fully accurate, no double version */
	eeFPURecompileCode(recCVT_S_xmm, intCVT_S, XMMINFO_WRITED | XMMINFO_READS);
}

void recCVT_W(void)
{
	/* Float version is fully accurate, no double version */
	int regs = _checkXMMreg(XMMTYPE_FPREG, _Fs_, MODE_READ);

	if (regs >= 0)
	{
		xe_cvttss2si_rx(XE_AX, regs);
		xe_movd_rx(XE_DX, regs);
	}
	else
	{
		xe_cvttss2si_rm(XE_AX, &fpuRegs.fpr[_Fs_]);
		xe_mov32_rm(XE_DX, &fpuRegs.fpr[_Fs_]);
	}

	/* Kill register allocation for dst 
	 * because we write directly to fpuRegs.fpr[_Fd_] */
	_deleteFPtoXMMreg(_Fd_, DELETE_REG_FREE_NO_WRITEBACK);

	/* cvttss2si converts unrepresentable values to 0x80000000, 
	 * so negative values are already handled.
	 * So we just need to handle positive values. */
	xe_cmp32_ri(XE_DX, 0x4f000000); /* If the input is greater than INT_MAX */
	xe_mov32_ri(XE_DX, 0x7fffffff);
	xe_cmovge32_rr(XE_AX, XE_DX);     /* Saturate it */

	/* Write the result */
	xe_mov32_mr(&fpuRegs.fpr[_Fd_], XE_AX);
}
//------------------------------------------------------------------


//------------------------------------------------------------------
// DIV XMM
//------------------------------------------------------------------
static void recDIVhelper1(int regd, int regt) // Sets flags
{
	u8 *pjmp1, *pjmp2;
	uint8_t *ajmp32, *bjmp32;
	const int t1reg = _allocTempXMMreg(XMMT_FPS);

	xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	/*--- Check for divide by zero ---*/
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, regt);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); //Check sign (if regt == zero, sign will be set)
	xe_fwd_jcc32(Jcc_Zero, ajmp32); //Skip if not set

	/*--- Check for 0/0 ---*/
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, regd);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); //Check sign (if regd == zero, sign will be set)
	xe_fwd_jcc8(Jcc_Zero, pjmp1); //Skip if not set
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
	xe_fwd_jcc8(Jcc_Unconditional, pjmp2);
	xe_fwd_set8(pjmp1); //x/0 but not 0/0
	xe_or32_mi(&fpuRegs.fprc[31], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
	xe_fwd_set8(pjmp2);

	/*--- Make regd +/- Maximum ---*/
	xe_xorps_xx(regd, regt); // Make regd Positive or Negative
	xe_andps_xm(regd, &s_neg[0]); // Get the sign bit
	xe_orps_xm(regd, &g_maxvals[0]); // regd = +/- Maximum
	//xe_movss_xm(regd, &g_maxvals[0]);
	xe_fwd_jcc32(Jcc_Unconditional, bjmp32);

	xe_fwd_set32(ajmp32);

	/*--- Normal Divide ---*/
	if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(regt); }
	xe_divss_xx(regd, regt);

	fpuFloat(regd);
	xe_fwd_set32(bjmp32);

	_freeXMMreg(t1reg);
}

void recDIV_S_xmm(int info)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUDivFPCR.bitmask);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			xe_movss_xx(EEREC_D, EEREC_S);
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			// Sets D/I flags on FPU instructions
			recDIVhelper1(EEREC_D, t0reg);
			break;
		case PROCESS_EE_T:
			if (EEREC_D == EEREC_T)
			{
				xe_movss_xx(t0reg, EEREC_T);
				xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, t0reg);
			}
			else
			{
				xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (EEREC_D == EEREC_T)
			{
				xe_movss_xx(t0reg, EEREC_T);
				xe_movss_xx(EEREC_D, EEREC_S);
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, t0reg);
			}
			else
			{
				xe_movss_xx(EEREC_D, EEREC_S);
				// Sets D/I flags on FPU instructions
				recDIVhelper1(EEREC_D, EEREC_T);
			}
			break;
		default:
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
			// Sets D/I flags on FPU instructions
			recDIVhelper1(EEREC_D, t0reg);
			break;
	}

	if (EmuConfig.Cpu.FPUFPCR.bitmask != EmuConfig.Cpu.FPUDivFPCR.bitmask)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);
	_freeXMMreg(t0reg);
}

FPURECOMPILE_CONSTCODE_SOFT(DIV_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------



//------------------------------------------------------------------
// MADD XMM
//------------------------------------------------------------------
void recMADDtemp(int info, int regd)
{
	const int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xe_mulss_xx(regd, t0reg);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_S); fpuFloat2(t0reg); }
				xe_mulss_xx(t0reg, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xe_mulss_xx(regd, EEREC_S);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xe_mulss_xx(regd, t0reg);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_T); fpuFloat2(t0reg); }
				xe_mulss_xx(t0reg, EEREC_T);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xe_mulss_xx(regd, EEREC_T);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(regd); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(EEREC_ACC); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_S)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xe_mulss_xx(regd, EEREC_T);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xe_mulss_xx(regd, EEREC_S);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xe_movss_xx(t0reg, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(EEREC_T); }
				xe_mulss_xx(t0reg, EEREC_T);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
			}
			else
			{
				xe_movss_xx(regd, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xe_mulss_xx(regd, EEREC_T);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
		default:
			if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				const int t1reg = _allocTempXMMreg(XMMT_FPS);
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				xe_movss_xm(t1reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(t1reg); }
				xe_mulss_xx(t0reg, t1reg);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_ADD(regd, t0reg);
				_freeXMMreg(t1reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xe_mulss_xx(regd, t0reg);
				if (info & PROCESS_EE_ACC)
				{
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(EEREC_ACC); }
					FPU_ADD(regd, EEREC_ACC);
				}
				else
				{
					xe_movss_xm(t0reg, &fpuRegs.ACC);
					if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
					FPU_ADD(regd, t0reg);
				}
			}
			break;
	}

	fpuFloat(regd);
	_freeXMMreg(t0reg);
}

void recMADD_S_xmm(int info)
{
	recMADDtemp(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE_SOFT_DBL(MADD_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMADDA_S_xmm(int info)
{
	recMADDtemp(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE_SOFT_DBL(MADDA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------




//------------------------------------------------------------------
// MAX / MIN XMM
//------------------------------------------------------------------
void recMAX_S_xmm(int info)
{
	recMaxMin_int(info, 0);
}

FPURECOMPILE_CONSTCODE_EXACT_SS(MAX_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMIN_S_xmm(int info)
{
	recMaxMin_int(info, 1);
}

FPURECOMPILE_CONSTCODE_EXACT_SS(MIN_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MOV XMM
//------------------------------------------------------------------
void recMOV_S_xmm(int info)
{
	if (info & PROCESS_EE_S)
		xe_movss_xx(EEREC_D, EEREC_S);
	else
		xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
}

FPURECOMPILE_CONSTCODE(MOV_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MSUB XMM
//------------------------------------------------------------------
void recMSUBtemp(int info, int regd)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd == EEREC_S)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xe_mulss_xx(regd, t0reg);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_S); fpuFloat2(t0reg); }
				xe_mulss_xx(t0reg, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xe_mulss_xx(regd, EEREC_S);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xe_mulss_xx(regd, t0reg);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(EEREC_T); fpuFloat2(t0reg); }
				xe_mulss_xx(t0reg, EEREC_T);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xe_mulss_xx(regd, EEREC_T);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_S)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xe_mulss_xx(regd, EEREC_T);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			else if (regd == EEREC_T)
			{
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_S); }
				xe_mulss_xx(regd, EEREC_S);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			else if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				xe_movss_xx(t0reg, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(EEREC_T); }
				xe_mulss_xx(t0reg, EEREC_T);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
			}
			else
			{
				xe_movss_xx(regd, EEREC_S);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(EEREC_T); }
				xe_mulss_xx(regd, EEREC_T);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			break;
		default:
			if ((info & PROCESS_EE_ACC) && regd == EEREC_ACC)
			{
				const int t1reg = _allocTempXMMreg(XMMT_FPS);
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Fs_]);
				xe_movss_xm(t1reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(t0reg); fpuFloat2(t1reg); }
				xe_mulss_xx(t0reg, t1reg);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(regd, t0reg);
				_freeXMMreg(t1reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat2(regd); fpuFloat2(t0reg); }
				xe_mulss_xx(regd, t0reg);
				if (info & PROCESS_EE_ACC)
					xe_movss_xx(t0reg, EEREC_ACC);
				else
					xe_movss_xm(t0reg, &fpuRegs.ACC);
				if (CHECK_FPU_EXTRA_OVERFLOW) { fpuFloat(regd); fpuFloat(t0reg); }
				FPU_SUB(t0reg, regd);
				xe_movss_xx(regd, t0reg);
			}
			break;
	}

	fpuFloat(regd);
	_freeXMMreg(t0reg);
}

void recMSUB_S_xmm(int info)
{
	recMSUBtemp(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE_SOFT_DBL(MSUB_S, XMMINFO_WRITED | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);

void recMSUBA_S_xmm(int info)
{
	recMSUBtemp(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE_SOFT_DBL(MSUBA_S, XMMINFO_WRITEACC | XMMINFO_READACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// MUL XMM
//------------------------------------------------------------------
void recMUL_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_D, 1));
}

FPURECOMPILE_CONSTCODE_SOFT_DBL(MUL_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

void recMULA_S_xmm(int info)
{
	fpuFloat(recCommutativeOp(info, EEREC_ACC, 1));
}

FPURECOMPILE_CONSTCODE_SOFT_DBL(MULA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// NEG XMM
//------------------------------------------------------------------
void recNEG_S_xmm(int info)
{
	if (info & PROCESS_EE_S)
		xe_movss_xx(EEREC_D, EEREC_S);
	else
		xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);

	xe_xorps_xm(EEREC_D, &s_neg[0]);

	// Always preserve sign. Using float clamping here would result in
	// +inf to become +fMax instead of -fMax, which is definitely wrong.
	/* Same reasoning as ABS: hardware NEG is a pure sign-bit flip and
	 * the clamp only guards downstream host adds; under either
	 * accuracy option every consumer handles raw operands. */
	if (!CHECK_FPU_SOFT_REC && !CHECK_FPU_ACC_ARITH)
		fpuFloat3(EEREC_D);
}

FPURECOMPILE_CONSTCODE(NEG_S, XMMINFO_WRITED | XMMINFO_READS);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SUB XMM
//------------------------------------------------------------------
static void recSUBhelper(int regd, int regt)
{
	if (CHECK_FPU_EXTRA_OVERFLOW /*&& !CHECK_FPUCLAMPHACK*/) { fpuFloat2(regd); fpuFloat2(regt); }
	FPU_SUB(regd, regt);
}

static void recSUBop(int info, int regd)
{
	int t0reg = _allocTempXMMreg(XMMT_FPS);

	//xAND(ptr32[&fpuRegs.fprc[31]], ~(FPUflagO|FPUflagU)); // Clear O and U flags

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			if (regd != EEREC_S)
				xe_movss_xx(regd, EEREC_S);
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			recSUBhelper(regd, t0reg);
			break;
		case PROCESS_EE_T:
			if (regd == EEREC_T)
			{
				xe_movss_xx(t0reg, EEREC_T);
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				recSUBhelper(regd, t0reg);
			}
			else
			{
				xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
				recSUBhelper(regd, EEREC_T);
			}
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			if (regd == EEREC_T)
			{
				xe_movss_xx(t0reg, EEREC_T);
				xe_movss_xx(regd, EEREC_S);
				recSUBhelper(regd, t0reg);
			}
			else
			{
				xe_movss_xx(regd, EEREC_S);
				recSUBhelper(regd, EEREC_T);
			}
			break;
		default:
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			xe_movss_xm(regd, &fpuRegs.fpr[_Fs_]);
			recSUBhelper(regd, t0reg);
			break;
	}

	fpuFloat(regd);
	_freeXMMreg(t0reg);
}

void recSUB_S_xmm(int info)
{
	recSUBop(info, EEREC_D);
}

FPURECOMPILE_CONSTCODE_EXACT_DBL(SUB_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);


void recSUBA_S_xmm(int info)
{
	recSUBop(info, EEREC_ACC);
}

FPURECOMPILE_CONSTCODE_EXACT_DBL(SUBA_S, XMMINFO_WRITEACC | XMMINFO_READS | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// SQRT XMM
//------------------------------------------------------------------
void recSQRT_S_xmm(int info)
{
	int roundmodeFlag = 0;
	alignas(16) static FPControlRegister roundmode_nearest;

	if (fpcr_round_mode(EmuConfig.Cpu.FPUFPCR) != FPROUND_NEAREST)
	{
		// Set roundmode to nearest if it isn't already
		roundmode_nearest = EmuConfig.Cpu.FPUFPCR;
		roundmode_nearest = fpcr_set_round_mode(roundmode_nearest, FPROUND_NEAREST);
		xe_ldmxcsr_m(&roundmode_nearest.bitmask);
		roundmodeFlag = 1;
	}

	if (info & PROCESS_EE_T)
		xe_movss_xx(EEREC_D, EEREC_T);
	else
		xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Ft_]);

	// Sets D/I flags on FPU instructions
	{
		xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagI | FPUflagD)); // Clear I and D flags

		/*--- Check for negative SQRT ---*/
		xe_movmskps_rx(XE_AX, EEREC_D);
		xe_and32_ri(XE_AX, 1); //Check sign
		uint8_t* pjmp; xe_fwd_jcc8(Jcc_Zero, pjmp); //Skip if none are
			xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); // Set I and SI flags
			xe_andps_xm(EEREC_D, &s_pos[0]); // Make EEREC_D Positive
		xe_fwd_set8(pjmp);
	}

	if (CHECK_FPU_OVERFLOW) // Only need to do positive clamp, since EEREC_D is positive
		xe_minss_xm(EEREC_D, &g_maxvals[0]);
	xe_sqrtss_xx(EEREC_D, EEREC_D);
	if (CHECK_FPU_EXTRA_OVERFLOW) // Shouldn't need to clamp again since SQRT of a number will always be smaller than the original number, doing it just incase :/
		fpuFloat(EEREC_D);

	if (roundmodeFlag)
		xe_ldmxcsr_m(&EmuConfig.Cpu.FPUFPCR.bitmask);
}

FPURECOMPILE_CONSTCODE_SOFT(SQRT_S, XMMINFO_WRITED | XMMINFO_READT);
//------------------------------------------------------------------


//------------------------------------------------------------------
// RSQRT XMM
//------------------------------------------------------------------
static void recRSQRThelper1(int regd, int t0reg) // Preforms the RSQRT function when regd <- Fs and t0reg <- Ft (Sets correct flags)
{
	u8 *pjmp1, *pjmp2;
	uint8_t* pjmp32;
	u8 *qjmp1, *qjmp2;
	int t1reg = _allocTempXMMreg(XMMT_FPS);

	xe_and32_mi(&fpuRegs.fprc[31], ~(FPUflagI | FPUflagD)); // Clear I and D flags

	/*--- (first) Check for negative SQRT ---*/
	xe_movmskps_rx(XE_AX, t0reg);
	xe_and32_ri(XE_AX, 1); //Check sign
	xe_fwd_jcc8(Jcc_Zero, pjmp2); //Skip if not set
		xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); // Set I and SI flags
		xe_andps_xm(t0reg, &s_pos[0]); // Make t0reg Positive
	xe_fwd_set8(pjmp2);

	/*--- Check for zero ---*/
	xe_xorps_xx(t1reg, t1reg);
	xe_cmpeqss_xx(t1reg, t0reg);
	xe_movmskps_rx(XE_AX, t1reg);
	xe_and32_ri(XE_AX, 1); //Check sign (if t0reg == zero, sign will be set)
	xe_fwd_jcc8(Jcc_Zero, pjmp1); //Skip if not set
		/*--- Check for 0/0 ---*/
		xe_xorps_xx(t1reg, t1reg);
		xe_cmpeqss_xx(t1reg, regd);
		xe_movmskps_rx(XE_AX, t1reg);
		xe_and32_ri(XE_AX, 1); //Check sign (if regd == zero, sign will be set)
		xe_fwd_jcc8(Jcc_Zero, qjmp1); //Skip if not set
			xe_or32_mi(&fpuRegs.fprc[31], FPUflagI | FPUflagSI); // Set I and SI flags ( 0/0 )
			xe_fwd_jcc8(Jcc_Unconditional, qjmp2);
		xe_fwd_set8(qjmp1); //x/0 but not 0/0
			xe_or32_mi(&fpuRegs.fprc[31], FPUflagD | FPUflagSD); // Set D and SD flags ( x/0 )
		xe_fwd_set8(qjmp2);

		/*--- Make regd +/- Maximum ---*/
		xe_andps_xm(regd, &s_neg[0]); // Get the sign bit
		xe_orps_xm(regd, &g_maxvals[0]); // regd = +/- Maximum
		xe_fwd_jcc32(Jcc_Unconditional, pjmp32);
	xe_fwd_set8(pjmp1);

	if (CHECK_FPU_EXTRA_OVERFLOW)
	{
		xe_minss_xm(t0reg, &g_maxvals[0]); // Only need to do positive clamp, since t0reg is positive
		fpuFloat2(regd);
	}

	xe_sqrtss_xx(t0reg, t0reg);
	xe_divss_xx(regd, t0reg);

	fpuFloat(regd);
	xe_fwd_set32(pjmp32);

	_freeXMMreg(t1reg);
}

void recRSQRT_S_xmm(int info)
{
	// RSQRT doesn't change the round mode, because RSQRTSS ignores the rounding mode in MXCSR.
	const int t0reg = _allocTempXMMreg(XMMT_FPS);

	switch (info & (PROCESS_EE_S | PROCESS_EE_T))
	{
		case PROCESS_EE_S:
			xe_movss_xx(EEREC_D, EEREC_S);
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			break;
		case PROCESS_EE_T:
			xe_movss_xx(t0reg, EEREC_T);
			xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
			break;
		case (PROCESS_EE_S | PROCESS_EE_T):
			xe_movss_xx(t0reg, EEREC_T);
			xe_movss_xx(EEREC_D, EEREC_S);
			break;
		default:
			xe_movss_xm(t0reg, &fpuRegs.fpr[_Ft_]);
			xe_movss_xm(EEREC_D, &fpuRegs.fpr[_Fs_]);
			break;
	}
	// Sets D/I flags on FPU instructions
	recRSQRThelper1(EEREC_D, t0reg);
	_freeXMMreg(t0reg);
}

FPURECOMPILE_CONSTCODE_SOFT(RSQRT_S, XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);

#endif // FPU_RECOMPILE

