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

static void recMoveTtoD(int info)
{
	if (info & PROCESS_EE_T)
		xe_mov32_rr(EEREC_D, EEREC_T);
	else
		xe_mov32_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UL[0]);
}

static void recMoveTtoD64(int info)
{
	if (info & PROCESS_EE_T)
		xe_mov64_rr(EEREC_D, EEREC_T);
	else
		xe_mov64_rm(EEREC_D, &cpuRegs.GPR.r[_Rt_].UD[0]);
}

static void recMoveSToRCX(int info)
{
	/* load full 64-bits for store->load forwarding, since we always store >=64. */
	if (info & PROCESS_EE_S)
		xe_mov64_rr(XE_CX, EEREC_S);
	else
		xe_mov64_rm(XE_CX, &cpuRegs.GPR.r[_Rs_].UL[0]);
}

/* // SLL */
static void recSLL_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << _Sa_);
}

static void recSLLs_(int info, int sa)
{
	recMoveTtoD(info);
	if (sa != 0)
		xe_shl32_ri(EEREC_D, sa);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

static void recSLL_(int info)
{
	recSLLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SLL, XMMINFO_WRITED | XMMINFO_READT);

/* // SRL */
static void recSRL_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> _Sa_);
}

static void recSRLs_(int info, int sa)
{
	recMoveTtoD(info);
	if (sa != 0)
		xe_shr32_ri(EEREC_D, sa);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

static void recSRL_(int info)
{
	recSRLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SRL, XMMINFO_WRITED | XMMINFO_READT);

/* // SRA */
static void recSRA_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> _Sa_);
}

static void recSRAs_(int info, int sa)
{
	recMoveTtoD(info);
	if (sa != 0)
		xe_sar32_ri(EEREC_D, sa);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

static void recSRA_(int info)
{
	recSRAs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, SRA, XMMINFO_WRITED | XMMINFO_READT);

static void recDSLL_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << _Sa_);
}

static void recDSLLs_(int info, int sa)
{
	recMoveTtoD64(info);
	if (sa != 0)
		xe_shl64_ri(EEREC_D, sa);
}

static void recDSLL_(int info)
{
	recDSLLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSLL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

static void recDSRL_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> _Sa_);
}

static void recDSRLs_(int info, int sa)
{
	recMoveTtoD64(info);
	if (sa != 0)
		xe_shr64_ri(EEREC_D, sa);
}

static void recDSRL_(int info)
{
	recDSRLs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRL, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/* // DSRA */
static void recDSRA_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (u64)(g_cpuConstRegs[_Rt_].SD[0] >> _Sa_);
}

static void recDSRAs_(int info, int sa)
{
	recMoveTtoD64(info);
	if (sa != 0)
		xe_sar64_ri(EEREC_D, sa);
}

static void recDSRA_(int info)
{
	recDSRAs_(info, _Sa_);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRA, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/* DSLL32 */
static void recDSLL32_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << (_Sa_ + 32));
}

static void recDSLL32_(int info)
{
	recDSLLs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSLL32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/* // DSRL32 */
static void recDSRL32_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> (_Sa_ + 32));
}

static void recDSRL32_(int info)
{
	recDSRLs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRL32, XMMINFO_WRITED | XMMINFO_READT);

/* // DSRA32 */
static void recDSRA32_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (u64)(g_cpuConstRegs[_Rt_].SD[0] >> (_Sa_ + 32));
}

static void recDSRA32_(int info)
{
	recDSRAs_(info, _Sa_ + 32);
}

EERECOMPILE_CODEX(eeRecompileCodeRC2, DSRA32, XMMINFO_WRITED | XMMINFO_READT | XMMINFO_64BITOP);

/*********************************************************
* Shift arithmetic with variant register shift           *
* Format:  OP rd, rt, rs                                 *
*********************************************************/

static void recShiftV_constt(int info, int g2op)
{
	recMoveSToRCX(info);
	xe_mov32_ri(EEREC_D, g_cpuConstRegs[_Rt_].UL[0]);
	xe_g2op32_rcl(g2op, EEREC_D);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

static void recShiftV(int info, int g2op)
{
	recMoveSToRCX(info);
	recMoveTtoD(info);
	xe_g2op32_rcl(g2op, EEREC_D);
	xe_movsxd_rr(EEREC_D, EEREC_D);
}

static void recDShiftV_constt(int info, int g2op)
{
	recMoveSToRCX(info);
	xe_mov64_ri(EEREC_D, g_cpuConstRegs[_Rt_].SD[0]);
	xe_g2op64_rcl(g2op, EEREC_D);
}

static void recDShiftV(int info, int g2op)
{
	recMoveSToRCX(info);
	recMoveTtoD64(info);
	xe_g2op64_rcl(g2op, EEREC_D);
}

/* // SLLV */
static void recSLLV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSLLV_consts(int info)
{
	recSLLs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSLLV_constt(int info)
{
	recShiftV_constt(info, 4);
}

static void recSLLV_(int info)
{
	recShiftV(info, 4);
}

EERECOMPILE_CODERC0(SLLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

/* // SRLV */
static void recSRLV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].UL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRLV_consts(int info)
{
	recSRLs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSRLV_constt(int info)
{
	recShiftV_constt(info, 5);
}

static void recSRLV_(int info)
{
	recShiftV(info, 5);
}

EERECOMPILE_CODERC0(SRLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

/* // SRAV */
static void recSRAV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s32)(g_cpuConstRegs[_Rt_].SL[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x1f));
}

static void recSRAV_consts(int info)
{
	recSRAs_(info, g_cpuConstRegs[_Rs_].UL[0] & 0x1f);
}

static void recSRAV_constt(int info)
{
	recShiftV_constt(info, 7);
}

static void recSRAV_(int info)
{
	recShiftV(info, 7);
}

EERECOMPILE_CODERC0(SRAV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

/* // DSLLV */
static void recDSLLV_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] << (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSLLV_consts(int info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSLLs_(info, sa);
}

static void recDSLLV_constt(int info)
{
	recDShiftV_constt(info, 4);
}

static void recDSLLV_(int info)
{
	recDShiftV(info, 4);
}

EERECOMPILE_CODERC0(DSLLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // DSRLV */
static void recDSRLV_const(void)
{
	g_cpuConstRegs[_Rd_].UD[0] = (u64)(g_cpuConstRegs[_Rt_].UD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSRLV_consts(int info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSRLs_(info, sa);
}

static void recDSRLV_constt(int info)
{
	recDShiftV_constt(info, 5);
}

static void recDSRLV_(int info)
{
	recDShiftV(info, 5);
}

EERECOMPILE_CODERC0(DSRLV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

/* // DSRAV */
static void recDSRAV_const(void)
{
	g_cpuConstRegs[_Rd_].SD[0] = (s64)(g_cpuConstRegs[_Rt_].SD[0] >> (g_cpuConstRegs[_Rs_].UL[0] & 0x3f));
}

static void recDSRAV_consts(int info)
{
	int sa = g_cpuConstRegs[_Rs_].UL[0] & 0x3f;
	recDSRAs_(info, sa);
}

static void recDSRAV_constt(int info)
{
	recDShiftV_constt(info, 7);
}

static void recDSRAV_(int info)
{
	recDShiftV(info, 7);
}

EERECOMPILE_CODERC0(DSRAV, XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED | XMMINFO_64BITOP);

