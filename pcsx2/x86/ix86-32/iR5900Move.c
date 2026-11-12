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
* Shift arithmetic with constant shift                   *
* Format:  OP rd, rt, sa                                 *
*********************************************************/

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/

/* // LUI */
void recLUI(void)
{
	if (!_Rt_)
		return;

	/* need to flush the upper 64 bits for xmm */
	GPR_DEL_CONST(_Rt_);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FREE_NO_WRITEBACK);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH_AND_FREE);

	GPR_SET_CONST(_Rt_);
	g_cpuConstRegs[_Rt_].UD[0] = (s32)(cpuRegs.code << 16);
}

static void recMFHILO(int hi, int upper)
{
	int reg;
	int xmmd;
	int xmmhilo;
	if (!_Rd_)
		return;

	/* kill any constants on rd, lower 64 bits get written regardless of upper */
	_eeOnWriteReg(_Rd_, 0);

	reg = hi ? XMMGPR_HI : XMMGPR_LO;
	xmmd = EEINST_XMMUSEDTEST(_Rd_) ? _allocGPRtoXMMreg(_Rd_, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, _Rd_, MODE_READ | MODE_WRITE);
	xmmhilo = EEINST_XMMUSEDTEST(reg) ? _allocGPRtoXMMreg(reg, MODE_READ) : _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ);
	if (xmmd >= 0)
	{
		if (xmmhilo >= 0)
		{
			if (upper)
				xe_movhlps(xmmd, xmmhilo);
			else
				xe_movsd_xx(xmmd, xmmhilo);
		}
		else
		{
			const int gprhilo = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_READ);
			if (gprhilo >= 0)
				xe_pinsrq(xmmd, gprhilo, 0);
			else
				xe_pinsrq_xm(xmmd, hi ? &cpuRegs.HI.UD[(u8)(upper)] : &cpuRegs.LO.UD[(u8)(upper)], 0);
		}
	}
	else
	{
		/* try rename {hi,lo} -> rd */
		const int gprreg = upper ? -1 : _checkX86reg(X86TYPE_GPR, reg, MODE_READ);
		int gprd;
		if (gprreg >= 0 && _eeTryRenameReg(_Rd_, reg, gprreg, -1, 0) >= 0)
			return;

		gprd = _allocIfUsedGPRtoX86(_Rd_, MODE_WRITE);
		if (gprd >= 0 && xmmhilo >= 0)
		{
			if (upper)
				xe_pextrq_rx(gprd, xmmhilo, 1);
			else
				xe_movq_rx(gprd, xmmhilo);
		}
		else if (gprd < 0 && xmmhilo >= 0)
		{
			if (upper)
				xe_pextrq_mx(&cpuRegs.GPR.r[_Rd_].UD[0], xmmhilo, 1);
			else
				xe_movq_mx(&cpuRegs.GPR.r[_Rd_].UD[0], xmmhilo);
		}
		else if (gprd >= 0)
		{
			if (gprreg >= 0)
				xe_mov64_rr(gprd, gprreg);
			else
				xe_mov64_rm(gprd, hi ? &cpuRegs.HI.UD[(u8)(upper)] : &cpuRegs.LO.UD[(u8)(upper)]);
		}
		else if (gprreg >= 0)
		{
			xe_mov64_mr(&cpuRegs.GPR.r[_Rd_].UD[0], gprreg);
		}
		else
		{
			xe_mov64_rm(XE_AX, hi ? &cpuRegs.HI.UD[(u8)(upper)] : &cpuRegs.LO.UD[(u8)(upper)]);
			xe_mov64_mr(&cpuRegs.GPR.r[_Rd_].UD[0], XE_AX);
		}
	}
}

static void recMTHILO(int hi, int upper)
{
	const int reg = hi ? XMMGPR_HI : XMMGPR_LO;
	int xmms;
	int xmmhilo;
	_eeOnWriteReg(reg, 0);

	xmms = EEINST_XMMUSEDTEST(_Rs_) ? _allocGPRtoXMMreg(_Rs_, MODE_READ) : _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ);
	xmmhilo = EEINST_XMMUSEDTEST(reg) ? _allocGPRtoXMMreg(reg, MODE_READ | MODE_WRITE) : _checkXMMreg(XMMTYPE_GPRREG, reg, MODE_READ | MODE_WRITE);
	if (xmms >= 0)
	{
		if (xmmhilo >= 0)
		{
			if (upper)
				xe_movlhps(xmmhilo, xmms);
			else
				xe_movsd_xx(xmmhilo, xmms);
		}
		else
		{
			const int gprhilo = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_WRITE);
			if (gprhilo >= 0)
				xe_movq_rx(gprhilo, xmms); /* movq: 66 REX.W 0f 7e */
			else
				xe_movq_mx(hi ? &cpuRegs.HI.UD[(u8)(upper)] : &cpuRegs.LO.UD[(u8)(upper)], xmms);
		}
	}
	else
	{
		int gprs = _allocIfUsedGPRtoX86(_Rs_, MODE_READ);

		if (xmmhilo >= 0)
		{
			if (gprs >= 0)
			{
				xe_pinsrq(xmmhilo, gprs, (u8)(upper));
			}
			else if (GPR_IS_CONST1(_Rs_))
			{
				/* force it into a register, since we need to load the constant anyway */
				gprs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
				xe_pinsrq(xmmhilo, gprs, (u8)(upper));
			}
			else
			{
				xe_pinsrq_xm(xmmhilo, &cpuRegs.GPR.r[_Rs_].UD[0], (u8)(upper));
			}
		}
		else
		{
			int gprreg;
			/* try rename rs -> {hi,lo} */
			if (gprs >= 0 && !upper && _eeTryRenameReg(reg, _Rs_, gprs, -1, 0) >= 0)
				return;

			gprreg = upper ? -1 : _allocIfUsedGPRtoX86(reg, MODE_WRITE);
			if (gprreg >= 0)
			{
				_eeMoveGPRtoR64(gprreg, _Rs_, 1);
			}
			else
			{
				/* force into a register, since we need to load it to write anyway */
				gprs = _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
				xe_mov64_mr(hi ? &cpuRegs.HI.UD[(u8)(upper)] : &cpuRegs.LO.UD[(u8)(upper)], gprs);
			}
		}
	}
}

void recMFHI(void)
{
	recMFHILO(1, 0);
}

void recMFLO(void)
{
	recMFHILO(0, 0);
}

void recMTHI(void)
{
	recMTHILO(1, 0);
}

void recMTLO(void)
{
	recMTHILO(0, 0);
}

void recMFHI1(void)
{
	recMFHILO(1, 1);
}

void recMFLO1(void)
{
	recMFHILO(0, 1);
}

void recMTHI1(void)
{
	recMTHILO(1, 1);
}

void recMTLO1(void)
{
	recMTHILO(0, 1);
}

/* // MOVZ */
/* if (rt == 0) then rd <- rs */
static void recMOVZtemp_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0];
}

static void recMOVZtemp_consts(int info)
{
	/* we need the constant anyway, so just force it into a register */
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	if (info & PROCESS_EE_T)
		xe_test64_rr(EEREC_T, EEREC_T);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rt_].UD[0], 0);

	xe_cmovcc64_rr(Jcc_Equal, EEREC_D, regs);
}

static void recMOVZtemp_constt(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov64_rr(EEREC_D, EEREC_S);
	else
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
}

static void recMOVZtemp_(int info)
{
	if (info & PROCESS_EE_T)
		xe_test64_rr(EEREC_T, EEREC_T);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rt_].UD[0], 0);

	if (info & PROCESS_EE_S)
		xe_cmovcc64_rr(Jcc_Equal, EEREC_D, EEREC_S);
	else
		xe_cmovcc64_rm(Jcc_Equal, EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
}

/* Specify READD here, because we might not write to it, and want to preserve the value. */
static EERECOMPILE_CODERC0(MOVZtemp, XMMINFO_READS | XMMINFO_READT | XMMINFO_READD | XMMINFO_WRITED | XMMINFO_NORENAME);

void recMOVZ(void)
{
	if (_Rs_ == _Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_) && g_cpuConstRegs[_Rt_].UD[0] != 0)
		return;

	recMOVZtemp();
}

/* // MOVN */
static void recMOVNtemp_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = g_cpuConstRegs[_Rs_].UD[0];
}

static void recMOVNtemp_consts(int info)
{
	/* we need the constant anyway, so just force it into a register */
	const int regs = (info & PROCESS_EE_S) ? EEREC_S : _allocX86reg(X86TYPE_GPR, _Rs_, MODE_READ);
	if (info & PROCESS_EE_T)
		xe_test64_rr(EEREC_T, EEREC_T);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rt_].UD[0], 0);

	xe_cmovcc64_rr(Jcc_NotEqual, EEREC_D, regs);
}

static void recMOVNtemp_constt(int info)
{
	if (info & PROCESS_EE_S)
		xe_mov64_rr(EEREC_D, EEREC_S);
	else
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
}

static void recMOVNtemp_(int info)
{
	if (info & PROCESS_EE_T)
		xe_test64_rr(EEREC_T, EEREC_T);
	else
		xe_cmp64_mi(&cpuRegs.GPR.r[_Rt_].UD[0], 0);

	if (info & PROCESS_EE_S)
		xe_cmovcc64_rr(Jcc_NotEqual, EEREC_D, EEREC_S);
	else
		xe_cmovcc64_rm(Jcc_NotEqual, EEREC_D, &cpuRegs.GPR.r[_Rs_].UD[0]);
}

static EERECOMPILE_CODERC0(MOVNtemp, XMMINFO_READS | XMMINFO_READT | XMMINFO_READD | XMMINFO_WRITED | XMMINFO_NORENAME);

void recMOVN(void)
{
	if (_Rs_ == _Rd_)
		return;

	if (GPR_IS_CONST1(_Rt_) && g_cpuConstRegs[_Rt_].UD[0] == 0)
		return;

	recMOVNtemp();
}

