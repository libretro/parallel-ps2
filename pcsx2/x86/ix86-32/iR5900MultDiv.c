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

#include "Common.h"
#include "R5900OpcodeTables.h"
#include "x86/iR5900.h"
#include "common/emitter/c89ops.h"
/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/

static void recWritebackHILO(int info, int writed, int upper)
{
	/* writeback low 32 bits, sign extended to 64 bits */
	int eax_sign_extended = 0;

	/* case 1: LO is already in an XMM - use the xmm */
	/* case 2: LO is used as an XMM later in the block - use or allocate the XMM */
	/* case 3: LO is used as a GPR later in the block - use XMM if upper, otherwise use GPR, so it can be renamed */
	/* case 4: LO is already in a GPR - write to the GPR, or write to memory if upper */
	/* case 4: LO is not used - writeback to memory */

	{
		const int loused = !!(EEINST_USEDTEST(XMMGPR_LO));
		const int lousedxmm = loused && (upper || EEINST_XMMUSEDTEST(XMMGPR_LO));
		const int xmmlo = lousedxmm ? _allocGPRtoXMMreg(XMMGPR_LO, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_LO, MODE_WRITE);
		if (xmmlo >= 0)
		{
			/* we use CDQE over MOVSX because it's shorter. */
			xe_cdqe();
			xe_pinsrq(xmmlo, XE_AX, (u8)(upper));
		}
		else
		{
			const int gprlo = upper ? -1 : (loused ? _allocX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE));
			if (gprlo >= 0)
			{
				xe_movsxd_rr(gprlo, XE_AX);
			}
			else
			{
				xe_cdqe();
				eax_sign_extended = 1;
				xe_mov64_mr(&cpuRegs.LO.UD[upper], XE_AX);
			}
		}
	}
	{
		const int hiused = !!(EEINST_USEDTEST(XMMGPR_HI));
		const int hiusedxmm = hiused && (upper || EEINST_XMMUSEDTEST(XMMGPR_HI));
		const int xmmhi = hiusedxmm ? _allocGPRtoXMMreg(XMMGPR_HI, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_HI, MODE_WRITE);
		if (xmmhi >= 0)
		{
			xe_movsxd_rr(XE_DX, XE_DX);
			xe_pinsrq(xmmhi, XE_DX, (u8)(upper));
		}
		else
		{
			const int gprhi = upper ? -1 : (hiused ? _allocX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE));
			if (gprhi >= 0)
			{
				xe_movsxd_rr(gprhi, XE_DX);
			}
			else
			{
				xe_movsxd_rr(XE_DX, XE_DX);
				xe_mov64_mr(&cpuRegs.HI.UD[upper], XE_DX);
			}
		}
	}

	/* writeback lo to Rd if present */
	if (writed && _Rd_)
	{
		/* TODO: This can be made optimal by keeping it in an xmm. */
		/* But currently the templates aren't hooked up for that - we'd need a "allow xmm" flag. */
		if (info & PROCESS_EE_D)
		{
			if (eax_sign_extended)
				xe_mov64_rr(EEREC_D, XE_AX);
			else
				xe_movsxd_rr(EEREC_D, XE_AX);
		}
		else
		{
			if (!eax_sign_extended)
				xe_cdqe();
			xe_mov64_mr(&cpuRegs.GPR.r[_Rd_].UD[0], XE_AX);
		}
	}
}

static void recWritebackConstHILO(u64 res, int writed, int upper)
{
	/* It's not often that MULT/DIV are entirely constant. So while the MOV64s here are not optimal */
	/* by any means, it's not something that's going to be hit often enough to worry about a cache. */
	/* Except for apparently when it's getting set to all-zeros, but that'll be fine with immediates. */
	const s64 loval = (s64)((s32)((u32)(res)));
	const s64 hival = (s64)((s32)((u32)(res >> 32)));

	{
		const int lolive = !!(EEINST_USEDTEST(XMMGPR_LO));
		const int lolivexmm = lolive && (upper || EEINST_XMMUSEDTEST(XMMGPR_LO));
		const int xmmlo = lolivexmm ? _allocGPRtoXMMreg(XMMGPR_LO, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_LO, MODE_WRITE);
		if (xmmlo >= 0)
		{
			xe_mov64_ri(XE_AX, loval);
			xe_pinsrq(xmmlo, XE_AX, (u8)(upper));
		}
		else
		{
			const int gprlo = upper ? -1 : (lolive ? _allocX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_LO, MODE_WRITE));
			if (gprlo >= 0)
				xe_imm64op_mov_rr(gprlo, XE_AX, loval);
			else
				xe_mov64_mi(&cpuRegs.LO.UD[upper], XE_AX, loval);
		}
	}

	{
		const int hilive = !!(EEINST_USEDTEST(XMMGPR_HI));
		const int hilivexmm = hilive && (upper || EEINST_XMMUSEDTEST(XMMGPR_HI));
		const int xmmhi = hilivexmm ? _allocGPRtoXMMreg(XMMGPR_HI, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, XMMGPR_HI, MODE_WRITE);
		if (xmmhi >= 0)
		{
			xe_mov64_ri(XE_AX, hival);
			xe_pinsrq(xmmhi, XE_AX, (u8)(upper));
		}
		else
		{
			const int gprhi = upper ? -1 : (hilive ? _allocX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE) : _checkX86reg(X86TYPE_GPR, XMMGPR_HI, MODE_WRITE));
			if (gprhi >= 0)
				xe_imm64op_mov_rr(gprhi, XE_AX, hival);
			else
				xe_mov64_mi(&cpuRegs.HI.UD[upper], XE_AX, hival);
		}
	}

	/* writeback lo to Rd if present */
	if (writed && _Rd_)
	{
		int regd;
		_eeOnWriteReg(_Rd_, 0);

		regd = _checkX86reg(X86TYPE_GPR, _Rd_, MODE_WRITE);
		if (regd >= 0)
			xe_imm64op_mov_rr(regd, XE_AX, loval);
		else
			xe_mov64_mi(&cpuRegs.GPR.r[_Rd_].UD[0], XE_AX, loval);
	}
}

/* // MULT */
static void recMULT_const(void)
{
	s64 res = (s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0];

	recWritebackConstHILO(res, 1, 0);
}

static void recMULTsuper(int info, int sign, int upper, int process)
{
	/* TODO(Stenzek): Use MULX where available. */
	if (process & PROCESS_CONSTS)
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rs_].UL[0]);
		if (info & PROCESS_EE_T)
			if (sign) xe_imul32_r(EEREC_T); else xe_mul32_r(EEREC_T);
		else
			if (sign) xe_imul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]); else xe_mul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else if (process & PROCESS_CONSTT)
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rt_].UL[0]);
		if (info & PROCESS_EE_S)
			if (sign) xe_imul32_r(EEREC_S); else xe_mul32_r(EEREC_S);
		else
			if (sign) xe_imul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]); else xe_mul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]);
	}
	else
	{
		/* S is more likely to be in a register than T (so put T in eax). */
		if (info & PROCESS_EE_T)
			xe_mov32_rr(XE_AX, EEREC_T);
		else
			xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rt_].UL[0]);

		if (info & PROCESS_EE_S)
			if (sign) xe_imul32_r(EEREC_S); else xe_mul32_r(EEREC_S);
		else
			if (sign) xe_imul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]); else xe_mul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]);
	}

	recWritebackHILO(info, 1, upper);
}

static void recMULT_(int info)
{
	recMULTsuper(info, 1, 0, 0);
}

static void recMULT_consts(int info)
{
	recMULTsuper(info, 1, 0, PROCESS_CONSTS);
}

static void recMULT_constt(int info)
{
	recMULTsuper(info, 1, 0, PROCESS_CONSTT);
}

/* lo/hi allocation are taken care of in recWritebackHILO(). */
EERECOMPILE_CODERC0(MULT, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

/* // MULTU */
static void recMULTU_const(void)
{
	const u64 res = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

	recWritebackConstHILO(res, 1, 0);
}

static void recMULTU_(int info)
{
	recMULTsuper(info, 0, 0, 0);
}

static void recMULTU_consts(int info)
{
	recMULTsuper(info, 0, 0, PROCESS_CONSTS);
}

static void recMULTU_constt(int info)
{
	recMULTsuper(info, 0, 0, PROCESS_CONSTT);
}

/* don't specify XMMINFO_WRITELO or XMMINFO_WRITEHI, that is taken care of */
EERECOMPILE_CODERC0(MULTU, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

static void recMULT1_const(void)
{
	s64 res = (s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0];

	recWritebackConstHILO((u64)res, 1, 1);
}

static void recMULT1_(int info)
{
	recMULTsuper(info, 1, 1, 0);
}

static void recMULT1_consts(int info)
{
	recMULTsuper(info, 1, 1, PROCESS_CONSTS);
}

static void recMULT1_constt(int info)
{
	recMULTsuper(info, 1, 1, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(MULT1, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

static void recMULTU1_const(void)
{
	u64 res = (u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0];

	recWritebackConstHILO(res, 1, 1);
}

static void recMULTU1_(int info)
{
	recMULTsuper(info, 0, 1, 0);
}

static void recMULTU1_consts(int info)
{
	recMULTsuper(info, 0, 1, PROCESS_CONSTS);
}

static void recMULTU1_constt(int info)
{
	recMULTsuper(info, 0, 1, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(MULTU1, XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0));

/* // DIV */

static void recDIVconst(int upper)
{
	s32 quot, rem = 0;
	if (g_cpuConstRegs[_Rs_].UL[0] == 0x80000000 && g_cpuConstRegs[_Rt_].SL[0] == -1)
		quot = (s32)0x80000000;
	else if (g_cpuConstRegs[_Rt_].SL[0] != 0)
	{
		quot = g_cpuConstRegs[_Rs_].SL[0] / g_cpuConstRegs[_Rt_].SL[0];
		rem  = g_cpuConstRegs[_Rs_].SL[0] % g_cpuConstRegs[_Rt_].SL[0];
	}
	else
	{
		quot = (g_cpuConstRegs[_Rs_].SL[0] < 0) ? 1 : -1;
		rem  = g_cpuConstRegs[_Rs_].SL[0];
	}
	recWritebackConstHILO((u64)quot | ((u64)rem << 32), 0, upper);
}

static void recDIV_const(void)
{
	recDIVconst(0);
}

static void recDIVsuper(int info, int sign, int upper, int process)
{
	const int divisor = (info & PROCESS_EE_T) ? EEREC_T : XE_CX;
	u8* end1;
	uint8_t* cont3;
	uint8_t* end2;
	if (!(info & PROCESS_EE_T))
	{
		if (process & PROCESS_CONSTT)
			xe_mov32_ri(divisor, g_cpuConstRegs[_Rt_].UL[0]);
		else
			xe_mov32_rm(divisor, &cpuRegs.GPR.r[_Rt_].UL[0]);
	}

	if (process & PROCESS_CONSTS)
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rs_].UL[0]);
	else
		_eeMoveGPRtoR64(XE_AX, _Rs_, 1);

	if (sign) /* test for overflow (x86 will just throw an exception) */
	{
		uint8_t* cont1;
		uint8_t* cont2;
		xe_cmp32_ri(XE_AX, 0x80000000);
		 xe_fwd_jcc8(Jcc_NotEqual, cont1);
		xe_cmp32_ri(divisor, 0xffffffff);
		 xe_fwd_jcc8(Jcc_NotEqual, cont2);
		/* overflow case: */
		xe_xor32_rr(XE_DX, XE_DX); /* EAX remains 0x80000000 */
		xe_fwd_jcc8(Jcc_Unconditional, end1);

		xe_fwd_set8(cont1);
		xe_fwd_set8(cont2);
	}

	xe_cmp32_ri(divisor, 0);
	 xe_fwd_jcc8(Jcc_NotEqual, cont3);
	/* divide by zero */
	xe_mov32_rr(XE_DX, XE_AX);
	if (sign) /* set EAX to (EAX < 0)?1:-1 */
	{
		xe_sar32_ri(XE_AX, 31); /* (EAX < 0)?-1:0 */
		xe_shl32_ri(XE_AX, 1); /* (EAX < 0)?-2:0 */
		xe_not32_r(XE_AX); /* (EAX < 0)?1:-1 */
	}
	else
		xe_mov32_ri(XE_AX, 0xffffffff);
	 xe_fwd_jcc8(Jcc_Unconditional, end2);

	xe_fwd_set8(cont3);
	if (sign)
	{
		xe_cdq();
		xe_idiv32_r(divisor);
	}
	else
	{
		xe_xor32_rr(XE_DX, XE_DX);
		xe_div32_r(divisor);
	}

	if (sign)
		xe_fwd_set8(end1);
	xe_fwd_set8(end2);

	/* need to execute regardless of bad divide */
	recWritebackHILO(info, 0, upper);
}

static void recDIV_(int info)
{
	recDIVsuper(info, 1, 0, 0);
}

static void recDIV_consts(int info)
{
	recDIVsuper(info, 1, 0, PROCESS_CONSTS);
}

static void recDIV_constt(int info)
{
	recDIVsuper(info, 1, 0, PROCESS_CONSTT);
}

/* We handle S reading in the routine itself, since it needs to go into eax. */
EERECOMPILE_CODERC0(DIV, /*XMMINFO_READS |*/ XMMINFO_READT);

/* // DIVU */
static void recDIVUconst(int upper)
{
	u32 quot, rem;
	if (g_cpuConstRegs[_Rt_].UL[0] != 0)
	{
		quot = g_cpuConstRegs[_Rs_].UL[0] / g_cpuConstRegs[_Rt_].UL[0];
		rem = g_cpuConstRegs[_Rs_].UL[0] % g_cpuConstRegs[_Rt_].UL[0];
	}
	else
	{
		quot = 0xffffffff;
		rem = g_cpuConstRegs[_Rs_].UL[0];
	}

	recWritebackConstHILO((u64)quot | ((u64)rem << 32), 0, upper);
}

static void recDIVU_const(void)
{
	recDIVUconst(0);
}

static void recDIVU_(int info)
{
	recDIVsuper(info, 0, 0, 0);
}

static void recDIVU_consts(int info)
{
	recDIVsuper(info, 0, 0, PROCESS_CONSTS);
}

static void recDIVU_constt(int info)
{
	recDIVsuper(info, 0, 0, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(DIVU, /*XMMINFO_READS |*/ XMMINFO_READT);

static void recDIV1_const(void)
{
	recDIVconst(1);
}

static void recDIV1_(int info)
{
	recDIVsuper(info, 1, 1, 0);
}

static void recDIV1_consts(int info)
{
	recDIVsuper(info, 1, 1, PROCESS_CONSTS);
}

static void recDIV1_constt(int info)
{
	recDIVsuper(info, 1, 1, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(DIV1, /*XMMINFO_READS |*/ XMMINFO_READT);

static void recDIVU1_const(void)
{
	recDIVUconst(1);
}

static void recDIVU1_(int info)
{
	recDIVsuper(info, 0, 1, 0);
}

static void recDIVU1_consts(int info)
{
	recDIVsuper(info, 0, 1, PROCESS_CONSTS);
}

static void recDIVU1_constt(int info)
{
	recDIVsuper(info, 0, 1, PROCESS_CONSTT);
}

EERECOMPILE_CODERC0(DIVU1, /*XMMINFO_READS |*/ XMMINFO_READT);

/* TODO(Stenzek): All of these :( */

static void writeBackMAddToHiLoRd(int hiloID)
{
	/* eax -> LO, edx -> HI */
	xe_cdqe();
	if (_Rd_)
	{
		_eeOnWriteReg(_Rd_, 1);
		_deleteEEreg(_Rd_, 0);
		xe_mov64_mr(&cpuRegs.GPR.r[_Rd_].UD[0], XE_AX);
	}
	xe_mov64_mr(&cpuRegs.LO.UD[hiloID], XE_AX);

	xe_movsxd_rr(XE_AX, XE_DX);
	xe_mov64_mr(&cpuRegs.HI.UD[hiloID], XE_AX);
}

static void addConstantAndWriteBackToHiLoRd(int hiloID, u64 constant)
{
	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);

	xe_mov32_rm(XE_AX, &cpuRegs.LO.UL[hiloID * 2]);
	xe_mov32_rm(XE_DX, &cpuRegs.HI.UL[hiloID * 2]);
	xe_add32_ri(XE_AX, (u32)(constant & 0xffffffff));
	xe_adc32_ri(XE_DX, (u32)(constant >> 32));
	writeBackMAddToHiLoRd(hiloID);
}

static void addEaxEdxAndWriteBackToHiLoRd(int hiloID)
{
	xe_add32_rm(XE_AX, &cpuRegs.LO.UL[hiloID * 2]);
	xe_adc32_rm(XE_DX, &cpuRegs.HI.UL[hiloID * 2]);

	writeBackMAddToHiLoRd(hiloID);
}

void recMADD()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0]);
		addConstantAndWriteBackToHiLoRd(0, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rs_].UL[0]);
		xe_imul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rt_].UL[0]);
		xe_imul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]);
	}
	else
	{
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
		xe_imul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}

	addEaxEdxAndWriteBackToHiLoRd(0);
}

void recMADDU()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0]);
		addConstantAndWriteBackToHiLoRd(0, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rs_].UL[0]);
		xe_mul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rt_].UL[0]);
		xe_mul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]);
	}
	else
	{
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
		xe_mul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}

	addEaxEdxAndWriteBackToHiLoRd(0);
}

void recMADD1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((s64)g_cpuConstRegs[_Rs_].SL[0] * (s64)g_cpuConstRegs[_Rt_].SL[0]);
		addConstantAndWriteBackToHiLoRd(1, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rs_].UL[0]);
		xe_imul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rt_].UL[0]);
		xe_imul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]);
	}
	else
	{
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
		xe_imul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}

	addEaxEdxAndWriteBackToHiLoRd(1);
}

void recMADDU1()
{
	if (GPR_IS_CONST2(_Rs_, _Rt_))
	{
		u64 result = ((u64)g_cpuConstRegs[_Rs_].UL[0] * (u64)g_cpuConstRegs[_Rt_].UL[0]);
		addConstantAndWriteBackToHiLoRd(1, result);
		return;
	}

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	if (GPR_IS_CONST1(_Rs_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rs_].UL[0]);
		xe_mul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}
	else if (GPR_IS_CONST1(_Rt_))
	{
		xe_mov32_ri(XE_AX, g_cpuConstRegs[_Rt_].UL[0]);
		xe_mul32_m(&cpuRegs.GPR.r[_Rs_].UL[0]);
	}
	else
	{
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
		xe_mul32_m(&cpuRegs.GPR.r[_Rt_].UL[0]);
	}

	addEaxEdxAndWriteBackToHiLoRd(1);
}

