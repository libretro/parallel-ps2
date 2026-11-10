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


/*********************************************************
*   cached MMI opcodes                                   *
*                                                        *
*********************************************************/

#include "common/CpuFeatures.h"

#include "../Common.h"
#include "../R5900OpcodeTables.h"

#include "iR5900.h"
#include "common/emitter/c89ops.h"
#include "iMMI.h"

#include "../../common/MathUtils.h"
void recPLZCW()
{
	int x86regs = -1;
	int xmmregs = -1;

	if (!_Rd_)
		return;

	// TODO(Stenzek): Don't flush to memory at the end here. Careful of Rs == Rd.

	if (GPR_IS_CONST1(_Rs_))
	{
		_eeOnWriteReg(_Rd_, 0);
		_deleteEEreg(_Rd_, 0);
		GPR_SET_CONST(_Rd_);

		// Return the leading sign bits, excluding the original bit
		g_cpuConstRegs[_Rd_].UL[0] = count_leading_sign_bits(g_cpuConstRegs[_Rs_].SL[0]) - 1;
		g_cpuConstRegs[_Rd_].UL[1] = count_leading_sign_bits(g_cpuConstRegs[_Rs_].SL[1]) - 1;

		return;
	}

	_eeOnWriteReg(_Rd_, 0);

	if ((xmmregs = _checkXMMreg(XMMTYPE_GPRREG, _Rs_, MODE_READ)) >= 0)
	{
		xe_movd_rx(XE_AX, xmmregs);
	}
	else if ((x86regs = _checkX86reg(X86TYPE_GPR, _Rs_, MODE_READ)) >= 0)
	{
		xe_mov32_rr(XE_AX, x86regs);
	}
	else
	{
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[0]);
	}

	_deleteEEreg(_Rd_, DELETE_REG_FREE_NO_WRITEBACK);

	// Count the number of leading bits (MSB) that match the sign bit, excluding the sign
	// bit itself.

	// Strategy: If the sign bit is set, then negate the value.  And that way the same
	// bitcompare can be used for either bit status.  but be warned!  BSR returns undefined
	// results if the EAX is zero, so we need to have special checks for zeros before
	// using it.

	// --- first word ---

	xe_mov32_ri(XE_CX, 31);
	xe_test32_rr(XE_AX, XE_AX); // TEST sets the sign flag accordingly.
	uint8_t* label_notSigned; xe_fwd_jcc8(Jcc_Unsigned /* JNS: 0x9 */, label_notSigned);
	xe_not32_r(XE_AX);
	xe_fwd_set8(label_notSigned);

	xe_bsr32_rr(XE_AX, XE_AX);
	uint8_t* label_Zeroed; xe_fwd_jcc8(Jcc_Zero, label_Zeroed); // If BSR sets the ZF, eax is "trash"
	xe_sub32_rr(XE_CX, XE_AX);
	xe_dec32_r(XE_CX); // PS2 doesn't count the first bit

	xe_fwd_set8(label_Zeroed);
	xe_mov32_mr(&cpuRegs.GPR.r[_Rd_].UL[0], XE_CX);

	// second word

	if (xmmregs >= 0)
	{
		xe_pextrd_rxi(XE_AX, xmmregs, 1);
	}
	else if (x86regs >= 0)
	{
		xe_mov64_rr(XE_AX, x86regs);
		xe_shr64_ri(XE_AX, 32);
	}
	else
	{
		xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[1]);
	}

	xe_mov32_ri(XE_CX, 31);
	xe_test32_rr(XE_AX, XE_AX); // TEST sets the sign flag accordingly.
	xe_fwd_jcc8(Jcc_Unsigned /* JNS: 0x9 */, label_notSigned);
	xe_not32_r(XE_AX);
	xe_fwd_set8(label_notSigned);

	xe_bsr32_rr(XE_AX, XE_AX);
	xe_fwd_jcc8(Jcc_Zero, label_Zeroed); // If BSR sets the ZF, eax is "trash"
	xe_sub32_rr(XE_CX, XE_AX);
	xe_dec32_r(XE_CX); // PS2 doesn't count the first bit

	xe_fwd_set8(label_Zeroed);
	xe_mov32_mr(&cpuRegs.GPR.r[_Rd_].UL[1], XE_CX);

	GPR_DEL_CONST(_Rd_);
}

void recPMFHL()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READLO | XMMINFO_READHI);

	int t0reg;

	switch (_Sa_)
	{
		case 0x00: // LW

			t0reg = _allocTempXMMreg(XMMT_INT);
			xe_pshufd_xxi(t0reg, EEREC_HI, 0x88);
			xe_pshufd_xxi(EEREC_D, EEREC_LO, 0x88);
			xe_punpckldq_xx(EEREC_D, t0reg);

			_freeXMMreg(t0reg);
			break;

		case 0x01: // UW
			t0reg = _allocTempXMMreg(XMMT_INT);
			xe_pshufd_xxi(t0reg, EEREC_HI, 0xdd);
			xe_pshufd_xxi(EEREC_D, EEREC_LO, 0xdd);
			xe_punpckldq_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
			break;

		case 0x02: // SLW
		{
			// Saturating 64-to-32 narrow, twice: each lane forms a signed
			// 64-bit value from HI:LO and clamps it into a sign-extended
			// s32. Transcribed from MMI.c (PMFHL).
			//
			// Note the upper bound is tested with >=, not >, so a value of
			// exactly 0x7fffffff takes the saturating branch rather than the
			// pass-through one. It reaches the same answer either way, but
			// only because the clamp constant equals the input; writing it
			// as > would be wrong for no visible reason on most inputs.
			int lane;

			_deleteEEreg(_Rd_, 0);
			_deleteEEreg(XMMGPR_LO, 1);
			_deleteEEreg(XMMGPR_HI, 1);

			for (lane = 0; lane < 2; lane++)
			{
				const int w = lane * 2;

				xe_mov32_rm(XE_AX, &cpuRegs.LO.UL[w]); // zero-extends into rax
				xe_mov32_rm(XE_DX, &cpuRegs.HI.UL[w]);
				xe_shl64_ri(XE_DX, 32);
				xe_or64_rr(XE_AX, XE_DX);                       // rax = HI:LO, signed

				xe_mov64_ri(XE_CX, 0x000000007fffffffLL);
				xe_cmp64_rr(XE_AX, XE_CX);
				uint8_t* sat_hi; xe_fwd_jcc8(Jcc_GreaterOrEqual, sat_hi);
				xe_mov64_ri(XE_CX, -0x80000000LL);
				xe_cmp64_rr(XE_AX, XE_CX);
				uint8_t* sat_lo; xe_fwd_jcc8(Jcc_LessOrEqual, sat_lo);

				xe_movsxd_rm(XE_AX, &cpuRegs.LO.UL[w]);
				uint8_t* done; xe_fwd_jmp8(done);

				xe_fwd_set8(sat_hi);
				xe_mov64_ri(XE_AX, 0x000000007fffffffLL);
				uint8_t* done2; xe_fwd_jmp8(done2);

				xe_fwd_set8(sat_lo);
				xe_mov64_ri(XE_AX, (s64)0xffffffff80000000ULL);

				xe_fwd_set8(done);
				xe_fwd_set8(done2);
				xe_mov64_mr(&cpuRegs.GPR.r[_Rd_].UD[lane], XE_AX);
			}
			break;
		}

		case 0x03: // LH
			t0reg = _allocTempXMMreg(XMMT_INT);
			xe_pshuflw_xxi(t0reg, EEREC_HI, 0x88);
			xe_pshuflw_xxi(EEREC_D, EEREC_LO, 0x88);
			xe_pshufhw_xxi(t0reg, t0reg, 0x88);
			xe_pshufhw_xxi(EEREC_D, EEREC_D, 0x88);
			xe_psrldq_xi(t0reg, 4);
			xe_psrldq_xi(EEREC_D, 4);
			xe_punpckldq_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
			break;

		case 0x04: // SH
			if (EEREC_D == EEREC_HI)
			{
				xe_packssdw_xx(EEREC_D, EEREC_LO);
				xe_pshufd_xxi(EEREC_D, EEREC_D, 0x72);
			}
			else
			{
				xe_movdqa_xx(EEREC_D, EEREC_LO);
				xe_packssdw_xx(EEREC_D, EEREC_HI);

				// shuffle so a1a0b1b0->a1b1a0b0
				xe_pshufd_xxi(EEREC_D, EEREC_D, 0xd8);
			}
			break;
		default:
			break;
	}

	_clearNeededXMMregs();
}

void recPMTHL()
{
	if (_Sa_ != 0)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITELO | XMMINFO_WRITEHI);

	xe_blendps_xxi(EEREC_LO, EEREC_S, 0x5);
	xe_shufps_xxi(EEREC_HI, EEREC_S, 0xdd);
	xe_shufps_xxi(EEREC_HI, EEREC_HI, 0x72);

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRLH()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if ((_Sa_ & 0xf) == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psrlw_xi(EEREC_D, _Sa_ & 0xf);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRLW()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (_Sa_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psrld_xi(EEREC_D, _Sa_);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRAH()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if ((_Sa_ & 0xf) == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psraw_xi(EEREC_D, _Sa_ & 0xf);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRAW()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (_Sa_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psrad_xi(EEREC_D, _Sa_);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSLLH()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if ((_Sa_ & 0xf) == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psllw_xi(EEREC_D, _Sa_ & 0xf);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSLLW()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	if (_Sa_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_pslld_xi(EEREC_D, _Sa_);
	}
	_clearNeededXMMregs();
}


/*********************************************************
*   MMI0 opcodes                                         *
*                                                        *
*********************************************************/

////////////////////////////////////////////////////
void recPMAXW()
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_S == EEREC_T)
		xe_movdqa_xx(EEREC_D, EEREC_S);
	else if (EEREC_D == EEREC_S)
		xe_pmaxsd_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pmaxsd_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pmaxsd_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPPACW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(((_Rs_ != 0) ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		xe_pshufd_xxi(EEREC_D, EEREC_T, 0x88);
		xe_psrldq_xi(EEREC_D, 8);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		if (EEREC_D == EEREC_T)
		{
			xe_pshufd_xxi(t0reg, EEREC_S, 0x88);
			xe_pshufd_xxi(EEREC_D, EEREC_T, 0x88);
			xe_punpcklqdq_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_pshufd_xxi(t0reg, EEREC_T, 0x88);
			xe_pshufd_xxi(EEREC_D, EEREC_S, 0x88);
			xe_punpcklqdq_xx(t0reg, EEREC_D);

			// swap mmx regs.. don't ask
			xmmregs[t0reg] = xmmregs[EEREC_D];
			xmmregs[EEREC_D].inuse = 0;
		}
	}

	_clearNeededXMMregs();
}

void recPPACH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xe_pshuflw_xxi(EEREC_D, EEREC_T, 0x88);
		xe_pshufhw_xxi(EEREC_D, EEREC_D, 0x88);
		xe_pslldq_xi(EEREC_D, 4);
		xe_psrldq_xi(EEREC_D, 8);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_pshuflw_xxi(t0reg, EEREC_S, 0x88);
		xe_pshuflw_xxi(EEREC_D, EEREC_T, 0x88);
		xe_pshufhw_xxi(t0reg, t0reg, 0x88);
		xe_pshufhw_xxi(EEREC_D, EEREC_D, 0x88);

		xe_psrldq_xi(t0reg, 4);
		xe_psrldq_xi(EEREC_D, 4);
		xe_punpcklqdq_xx(EEREC_D, t0reg);

		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPPACB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		const int t0reg = _allocTempXMMreg(XMMT_INT);

		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psllw_xi(EEREC_D, 8);
		xe_pxor_xx(t0reg, t0reg);
		xe_psrlw_xi(EEREC_D, 8);
		xe_packuswb_xx(EEREC_D, t0reg);

		_freeXMMreg(t0reg);
	}
	else
	{
		const int t0reg = _allocTempXMMreg(XMMT_INT);

		xe_movdqa_xx(t0reg, EEREC_S);
		xe_movdqa_xx(EEREC_D, EEREC_T);
		xe_psllw_xi(t0reg, 8);
		xe_psllw_xi(EEREC_D, 8);
		xe_psrlw_xi(t0reg, 8);
		xe_psrlw_xi(EEREC_D, 8);

		xe_packuswb_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXT5(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	xe_movdqa_xx(t0reg, EEREC_T); // for bit 5..9
	xe_movdqa_xx(t1reg, EEREC_T); // for bit 15

	xe_pslld_xi(t0reg, 22);
	xe_psrlw_xi(t1reg, 15);
	xe_psrld_xi(t0reg, 27);
	xe_pslld_xi(t1reg, 20);
	xe_por_xx(t0reg, t1reg);

	xe_movdqa_xx(t1reg, EEREC_T); // for bit 10..14
	xe_movdqa_xx(EEREC_D, EEREC_T); // for bit 0..4

	xe_pslld_xi(EEREC_D, 27);
	xe_pslld_xi(t1reg, 17);
	xe_psrld_xi(EEREC_D, 27);
	xe_psrlw_xi(t1reg, 11);
	xe_por_xx(EEREC_D, t1reg);

	xe_psllw_xi(EEREC_D, 3);
	xe_psllw_xi(t0reg, 11);
	xe_por_xx(EEREC_D, t0reg);

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPPAC5(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	xe_movdqa_xx(t0reg, EEREC_T); // for bit 10..14
	xe_movdqa_xx(t1reg, EEREC_T); // for bit 15

	xe_pslld_xi(t0reg, 8);
	xe_psrld_xi(t1reg, 31);
	xe_psrld_xi(t0reg, 17);
	xe_pslld_xi(t1reg, 15);
	xe_por_xx(t0reg, t1reg);

	xe_movdqa_xx(t1reg, EEREC_T); // for bit 5..9
	xe_movdqa_xx(EEREC_D, EEREC_T); // for bit 0..4

	xe_pslld_xi(EEREC_D, 24);
	xe_psrld_xi(t1reg, 11);
	xe_psrld_xi(EEREC_D, 27);
	xe_pslld_xi(t1reg, 5);
	xe_por_xx(EEREC_D, t1reg);

	xe_pcmpeqd_xx(t1reg, t1reg);
	xe_psrld_xi(t1reg, 22);
	xe_pand_xx(EEREC_D, t1reg);
	xe_pandn_xx(t1reg, t0reg);
	xe_por_xx(EEREC_D, t1reg);

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMAXH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_pmaxsw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pmaxsw_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pmaxsw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCGTB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D != EEREC_T)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpgtb_xx(EEREC_D, EEREC_T);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpgtb_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCGTH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D != EEREC_T)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpgtw_xx(EEREC_D, EEREC_T);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpgtw_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCGTW(void)
{
	//TODO:optimize RS | RT== 0
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D != EEREC_T)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpgtd_xx(EEREC_D, EEREC_T);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpgtd_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDSB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_paddsb_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_paddsb_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_paddsb_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDSH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_paddsw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_paddsw_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_paddsw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
//NOTE: check kh2 movies if changing this
void recPADDSW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);
	int t2reg = _allocTempXMMreg(XMMT_INT);

	// The idea is:
	//  s = x + y; (wrap-arounded)
	//  if Sign(x) == Sign(y) && Sign(s) != Sign(x) && Sign(x) == 0 then positive overflow (clamp with 0x7fffffff)
	//  if Sign(x) == Sign(y) && Sign(s) != Sign(x) && Sign(x) == 1 then negative overflow (clamp with 0x80000000)

	// get sign bit
	xe_movdqa_xx(t0reg, EEREC_S);
	xe_movdqa_xx(t1reg, EEREC_T);

	// normal addition
	if (EEREC_D == EEREC_S)
		xe_paddd_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_paddd_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_paddd_xx(EEREC_D, EEREC_T);
	}

	xe_pxor_xx(t0reg, t1reg); // Sign(Rs) != Sign(Rt)
	xe_pxor_xx(t1reg, EEREC_D); // Sign(Rs) != Sign(Rd)
	xe_pandn_xx(t0reg, t1reg); // (Sign(Rs) == Sign(Rt)) & (Sign(Rs) != Sign(Rd))
	xe_psrad_xi(t0reg, 31);

	xe_pcmpeqd_xx(t1reg, t1reg);
	xe_pxor_xx(t0reg, t1reg); // could've been avoided if Intel wasn't too prudish for a PORN instruction
	xe_pslld_xi(t1reg, 31); // 0x80000000

	xe_movdqa_xx(t2reg, EEREC_D);
	xe_psrad_xi(t2reg, 31);
	xe_pxor_xx(t1reg, t2reg); // t2reg = (Rd < 0) ? 0x7fffffff : 0x80000000

	xe_pand_xx(EEREC_D, t0reg);
	xe_pandn_xx(t0reg, t1reg);
	xe_por_xx(EEREC_D, t0reg);

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_freeXMMreg(t2reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBSB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubsb_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubsb_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubsb_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBSH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubsw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubsw_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubsw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
//NOTE: check kh2 movies if changing this
void recPSUBSW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);
	int t2reg = _allocTempXMMreg(XMMT_INT);

	// The idea is:
	//  s = x - y; (wrap-arounded)
	//  if Sign(x) != Sign(y) && Sign(s) != Sign(x) && Sign(x) == 0 then positive overflow (clamp with 0x7fffffff)
	//  if Sign(x) != Sign(y) && Sign(s) != Sign(x) && Sign(x) == 1 then negative overflow (clamp with 0x80000000)

	// get sign bit
	xe_movdqa_xx(t0reg, EEREC_S);
	xe_movdqa_xx(t1reg, EEREC_T);
	xe_psrld_xi(t0reg, 31);
	xe_psrld_xi(t1reg, 31);

	// normal subtraction
	if (EEREC_D == EEREC_S)
		xe_psubd_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		xe_movdqa_xx(t2reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubd_xx(EEREC_D, t2reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubd_xx(EEREC_D, EEREC_T);
	}

	// overflow check
	// t2reg = 0xffffffff if NOT overflow, else 0
	xe_movdqa_xx(t2reg, EEREC_D);
	xe_psrld_xi(t2reg, 31);
	xe_pcmpeqd_xx(t1reg, t0reg); // Sign(Rs) == Sign(Rt)
	xe_pcmpeqd_xx(t2reg, t0reg); // Sign(Rs) == Sign(Rd)
	xe_por_xx(t2reg, t1reg); // (Sign(Rs) == Sign(Rt)) | (Sign(Rs) == Sign(Rd))
	xe_pcmpeqd_xx(t1reg, t1reg);
	xe_psrld_xi(t1reg, 1); // 0x7fffffff
	xe_paddd_xx(t1reg, t0reg); // t1reg = (Rs < 0) ? 0x80000000 : 0x7fffffff

	// saturation
	xe_pand_xx(EEREC_D, t2reg);
	xe_pandn_xx(t2reg, t1reg);
	xe_por_xx(EEREC_D, t2reg);

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_freeXMMreg(t2reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_paddb_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_paddb_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_paddb_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
			xe_pxor_xx(EEREC_D, EEREC_D);
		else
			xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else if (_Rt_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
	}
	else
	{
		if (EEREC_D == EEREC_S)
			xe_paddw_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_paddw_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_paddw_xx(EEREC_D, EEREC_T);
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
			xe_pxor_xx(EEREC_D, EEREC_D);
		else
			xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else if (_Rt_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
	}
	else
	{
		if (EEREC_D == EEREC_S)
			xe_paddd_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_paddd_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_paddd_xx(EEREC_D, EEREC_T);
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubb_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubb_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubb_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubw_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubd_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubd_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubd_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXTLW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xe_punpckldq_xx(EEREC_D, EEREC_T);
		xe_psrlq_xi(EEREC_D, 32);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpckldq_xx(EEREC_D, EEREC_S);
		else if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckldq_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckldq_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}

void recPEXTLB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xe_punpcklbw_xx(EEREC_D, EEREC_T);
		xe_psrlw_xi(EEREC_D, 8);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpcklbw_xx(EEREC_D, EEREC_S);
		else if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpcklbw_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpcklbw_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}

void recPEXTLH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xe_punpcklwd_xx(EEREC_D, EEREC_T);
		xe_psrld_xi(EEREC_D, 16);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpcklwd_xx(EEREC_D, EEREC_S);
		else if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpcklwd_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpcklwd_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}


/*********************************************************
*   MMI1 opcodes                                         *
*                                                        *
*********************************************************/

////////////////////////////////////////////////////

void recPABSW(void) //needs clamping
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	xe_pcmpeqd_xx(t0reg, t0reg);
	xe_pslld_xi(t0reg, 31);
	xe_pcmpeqd_xx(t0reg, EEREC_T); //0xffffffff if equal to 0x80000000
	xe_pabsd_xx(EEREC_D, EEREC_T); //0x80000000 -> 0x80000000
	xe_pxor_xx(EEREC_D, t0reg); //0x80000000 -> 0x7fffffff
	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}


////////////////////////////////////////////////////
void recPABSH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	xe_pcmpeqw_xx(t0reg, t0reg);
	xe_psllw_xi(t0reg, 15);
	xe_pcmpeqw_xx(t0reg, EEREC_T); //0xffff if equal to 0x8000
	xe_pabsw_xx(EEREC_D, EEREC_T); //0x8000 -> 0x8000
	xe_pxor_xx(EEREC_D, t0reg); //0x8000 -> 0x7fff
	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMINW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_S == EEREC_T)
		xe_movdqa_xx(EEREC_D, EEREC_S);
	else if (EEREC_D == EEREC_S)
		xe_pminsd_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pminsd_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pminsd_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADSBH(void)
{
	if (!_Rd_)
		return;

	const int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

	if (EEREC_S == EEREC_T)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_paddw_xx(EEREC_D, EEREC_D);
		// reset lower bits to 0s
		xe_psrldq_xi(EEREC_D, 8);
		xe_pslldq_xi(EEREC_D, 8);
	}
	else
	{
		const int t0reg = _allocTempXMMreg(XMMT_INT);

		xe_movdqa_xx(t0reg, EEREC_T);

		if (EEREC_D == EEREC_S)
		{
			xe_paddw_xx(t0reg, EEREC_S);
			xe_psubw_xx(EEREC_D, EEREC_T);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_psubw_xx(EEREC_D, EEREC_T);
			xe_paddw_xx(t0reg, EEREC_S);
		}

		// t0reg - adds, EEREC_D - subs
		xe_psrldq_xi(t0reg, 8);
		xe_movlhps_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDUW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	if (_Rt_ == 0)
	{
		if (_Rs_ == 0)
		{
			xe_pxor_xx(EEREC_D, EEREC_D);
		}
		else
			xe_movdqa_xx(EEREC_D, EEREC_S);
	}
	else if (_Rs_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		xe_pcmpeqb_xx(t0reg, t0reg);
		xe_pslld_xi(t0reg, 31); // 0x80000000
		xe_movdqa_xx(t1reg, t0reg);
		xe_pxor_xx(t0reg, EEREC_S); // invert MSB of Rs (for unsigned comparison)

		// normal 32-bit addition
		if (EEREC_D == EEREC_S)
			xe_paddd_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_paddd_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_paddd_xx(EEREC_D, EEREC_T);
		}

		// unsigned 32-bit comparison
		xe_pxor_xx(t1reg, EEREC_D); // invert MSB of Rd (for unsigned comparison)
		xe_pcmpgtd_xx(t0reg, t1reg);

		// saturate
		xe_por_xx(EEREC_D, t0reg); // clear word with 0xFFFFFFFF if (Rd < Rs)

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBUB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubusb_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubusb_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubusb_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBUH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_psubusw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movdqa_xx(t0reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubusw_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubusw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSUBUW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	xe_pcmpeqb_xx(t0reg, t0reg);
	xe_pslld_xi(t0reg, 31); // 0x80000000

	// normal 32-bit subtraction
	// and invert MSB of Rs and Rt (for unsigned comparison)
	if (EEREC_D == EEREC_S)
	{
		xe_movdqa_xx(t1reg, t0reg);
		xe_pxor_xx(t0reg, EEREC_S);
		xe_pxor_xx(t1reg, EEREC_T);
		xe_psubd_xx(EEREC_D, EEREC_T);
	}
	else if (EEREC_D == EEREC_T)
	{
		xe_movdqa_xx(t1reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubd_xx(EEREC_D, t1reg);
		xe_pxor_xx(t1reg, t0reg);
		xe_pxor_xx(t0reg, EEREC_S);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_psubd_xx(EEREC_D, EEREC_T);
		xe_movdqa_xx(t1reg, t0reg);
		xe_pxor_xx(t0reg, EEREC_S);
		xe_pxor_xx(t1reg, EEREC_T);
	}

	// unsigned 32-bit comparison
	xe_pcmpgtd_xx(t0reg, t1reg);

	// saturate
	xe_pand_xx(EEREC_D, t0reg); // clear word with zero if (Rs <= Rt)

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXTUH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xe_punpckhwd_xx(EEREC_D, EEREC_T);
		xe_psrld_xi(EEREC_D, 16);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpckhwd_xx(EEREC_D, EEREC_S);
		else if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckhwd_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckhwd_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}

alignas(16) static u32 tempqw[8];

void recQFSRV(void)
{
	if (!_Rd_)
		return;

	if (_Rs_ == _Rt_ + 1)
	{
		_flushEEreg(_Rs_, 0);
		_flushEEreg(_Rt_, 0);
		int info = eeRecompileCodeXMM(XMMINFO_WRITED);

		xe_mov32_rm(XE_AX, &cpuRegs.sa);
		xe_lea64_m(XE_CX, &cpuRegs.GPR.r[_Rt_]);
		{ struct e_mem xm; E_MEM(xm, XE_CX, XE_AX, 1, 0); xe_movdqu_xmem(EEREC_D, xm); }
		return;
	}

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);

	xe_mov32_rm(XE_AX, &cpuRegs.sa);
	xe_lea64_m(XE_CX, tempqw);
	{ struct e_mem xm; E_MEM(xm, XE_CX, E_NOREG, 0, 0); xe_movdqa_memx(xm, EEREC_T); }
	{ struct e_mem xm; E_MEM(xm, XE_CX, E_NOREG, 0, 16); xe_movdqa_memx(xm, EEREC_S); }
	{ struct e_mem xm; E_MEM(xm, XE_CX, XE_AX, 1, 0); xe_movdqu_xmem(EEREC_D, xm); }

	_clearNeededXMMregs();
}


void recPEXTUB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		xe_punpckhbw_xx(EEREC_D, EEREC_T);
		xe_psrlw_xi(EEREC_D, 8);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpckhbw_xx(EEREC_D, EEREC_S);
		else if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckhbw_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckhbw_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXTUW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | XMMINFO_READT | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		xe_punpckhdq_xx(EEREC_D, EEREC_T);
		xe_psrlq_xi(EEREC_D, 32);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpckhdq_xx(EEREC_D, EEREC_S);
		else if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckhdq_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_punpckhdq_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMINH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_pminsw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pminsw_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pminsw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCEQB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_pcmpeqb_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pcmpeqb_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpeqb_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCEQH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_pcmpeqw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pcmpeqw_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpeqw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCEQW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_pcmpeqd_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_pcmpeqd_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pcmpeqd_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDUB(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rt_)
	{
		if (EEREC_D == EEREC_S)
			xe_paddusb_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_paddusb_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_paddusb_xx(EEREC_D, EEREC_T);
		}
	}
	else
		xe_movdqa_xx(EEREC_D, EEREC_S);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPADDUH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
		xe_paddusw_xx(EEREC_D, EEREC_T);
	else if (EEREC_D == EEREC_T)
		xe_paddusw_xx(EEREC_D, EEREC_S);
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_paddusw_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

/*********************************************************
*   MMI2 opcodes                                         *
*                                                        *
*********************************************************/

////////////////////////////////////////////////////
void recPMADDW(void)
{
	int info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI | XMMINFO_READLO | XMMINFO_READHI);
	xe_shufps_xxi(EEREC_LO, EEREC_HI, 0x88);
	xe_pshufd_xxi(EEREC_LO, EEREC_LO, 0xd8); // LO = {LO[0], HI[0], LO[2], HI[2]}
	if (_Rd_)
	{
		if (!_Rs_ || !_Rt_)
			xe_pxor_xx(EEREC_D, EEREC_D);
		else if (EEREC_D == EEREC_S)
			xe_pmuldq_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_pmuldq_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_pmuldq_xx(EEREC_D, EEREC_T);
		}
	}
	else
	{
		if (!_Rs_ || !_Rt_)
			xe_pxor_xx(EEREC_HI, EEREC_HI);
		else
		{
			xe_movdqa_xx(EEREC_HI, EEREC_S);
			xe_pmuldq_xx(EEREC_HI, EEREC_T);
		}
	}

	// add from LO/HI
	if (_Rd_)
		xe_paddq_xx(EEREC_D, EEREC_LO);
	else
		xe_paddq_xx(EEREC_HI, EEREC_LO);

	// interleave & sign extend
	if (_Rd_)
	{
		xe_pshufd_xxi(EEREC_LO, EEREC_D, 0x88);
		xe_pshufd_xxi(EEREC_HI, EEREC_D, 0xdd);
	}
	else
	{
		xe_pshufd_xxi(EEREC_LO, EEREC_HI, 0x88);
		xe_pshufd_xxi(EEREC_HI, EEREC_HI, 0xdd);
	}
	xe_pmovsxdq_xx(EEREC_LO, EEREC_LO);
	xe_pmovsxdq_xx(EEREC_HI, EEREC_HI);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSLLVW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xe_pxor_xx(EEREC_D, EEREC_D);
		}
		else
		{
			xe_pshufd_xxi(EEREC_D, EEREC_T, 0x88);
			xe_pmovsxdq_xx(EEREC_D, EEREC_D);
		}
	}
	else if (_Rt_ == 0)
	{
		xe_pxor_xx(EEREC_D, EEREC_D);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		// shamt is 5-bit
		xe_movdqa_xx(t0reg, EEREC_S);
		xe_psllq_xi(t0reg, 27 + 32);
		xe_psrlq_xi(t0reg, 27 + 32);

		// EEREC_D[0] <- Rt[0], t1reg[0] <- Rt[2]
		xe_movhlps_xx(t1reg, EEREC_T);
		if (EEREC_D != EEREC_T)
			xe_movdqa_xx(EEREC_D, EEREC_T);

		// shift (left) Rt[0]
		xe_pslld_xx(EEREC_D, t0reg);

		// shift (left) Rt[2]
		xe_movhlps_xx(t0reg, t0reg);
		xe_pslld_xx(t1reg, t0reg);

		// merge & sign extend
		xe_punpckldq_xx(EEREC_D, t1reg);
		xe_pmovsxdq_xx(EEREC_D, EEREC_D);

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPSRLVW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xe_pxor_xx(EEREC_D, EEREC_D);
		}
		else
		{
			xe_pshufd_xxi(EEREC_D, EEREC_T, 0x88);
			xe_pmovsxdq_xx(EEREC_D, EEREC_D);
		}
	}
	else if (_Rt_ == 0)
	{
		xe_pxor_xx(EEREC_D, EEREC_D);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		// shamt is 5-bit
		xe_movdqa_xx(t0reg, EEREC_S);
		xe_psllq_xi(t0reg, 27 + 32);
		xe_psrlq_xi(t0reg, 27 + 32);

		// EEREC_D[0] <- Rt[0], t1reg[0] <- Rt[2]
		xe_movhlps_xx(t1reg, EEREC_T);
		if (EEREC_D != EEREC_T)
			xe_movdqa_xx(EEREC_D, EEREC_T);

		// shift (right logical) Rt[0]
		xe_psrld_xx(EEREC_D, t0reg);

		// shift (right logical) Rt[2]
		xe_movhlps_xx(t0reg, t0reg);
		xe_psrld_xx(t1reg, t0reg);

		// merge & sign extend
		xe_punpckldq_xx(EEREC_D, t1reg);
		xe_pmovsxdq_xx(EEREC_D, EEREC_D);

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMSUBW(void)
{
	int info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI | XMMINFO_READLO | XMMINFO_READHI);
	xe_shufps_xxi(EEREC_LO, EEREC_HI, 0x88);
	xe_pshufd_xxi(EEREC_LO, EEREC_LO, 0xd8); // LO = {LO[0], HI[0], LO[2], HI[2]}
	if (_Rd_)
	{
		if (!_Rs_ || !_Rt_)
			xe_pxor_xx(EEREC_D, EEREC_D);
		else if (EEREC_D == EEREC_S)
			xe_pmuldq_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_pmuldq_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_pmuldq_xx(EEREC_D, EEREC_T);
		}
	}
	else
	{
		if (!_Rs_ || !_Rt_)
			xe_pxor_xx(EEREC_HI, EEREC_HI);
		else
		{
			xe_movdqa_xx(EEREC_HI, EEREC_S);
			xe_pmuldq_xx(EEREC_HI, EEREC_T);
		}
	}

	// sub from LO/HI
	if (_Rd_)
	{
		xe_psubq_xx(EEREC_LO, EEREC_D);
		xe_movdqa_xx(EEREC_D, EEREC_LO);
	}
	else
	{
		xe_psubq_xx(EEREC_LO, EEREC_HI);
		xe_movdqa_xx(EEREC_HI, EEREC_LO);
	}

	// interleave & sign extend
	if (_Rd_)
	{
		xe_pshufd_xxi(EEREC_LO, EEREC_D, 0x88);
		xe_pshufd_xxi(EEREC_HI, EEREC_D, 0xdd);
	}
	else
	{
		xe_pshufd_xxi(EEREC_LO, EEREC_HI, 0x88);
		xe_pshufd_xxi(EEREC_HI, EEREC_HI, 0xdd);
	}
	xe_pmovsxdq_xx(EEREC_LO, EEREC_LO);
	xe_pmovsxdq_xx(EEREC_HI, EEREC_HI);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMULTW(void)
{
	int info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	if (!_Rs_ || !_Rt_)
	{
		if (_Rd_)
			xe_pxor_xx(EEREC_D, EEREC_D);
		xe_pxor_xx(EEREC_LO, EEREC_LO);
		xe_pxor_xx(EEREC_HI, EEREC_HI);
	}
	else
	{
		if (_Rd_)
		{
			if (EEREC_D == EEREC_S)
				xe_pmuldq_xx(EEREC_D, EEREC_T);
			else if (EEREC_D == EEREC_T)
				xe_pmuldq_xx(EEREC_D, EEREC_S);
			else
			{
				xe_movdqa_xx(EEREC_D, EEREC_S);
				xe_pmuldq_xx(EEREC_D, EEREC_T);
			}
		}
		else
		{
			xe_movdqa_xx(EEREC_HI, EEREC_S);
			xe_pmuldq_xx(EEREC_HI, EEREC_T);
		}

		// interleave & sign extend
		if (_Rd_)
		{
			xe_pshufd_xxi(EEREC_LO, EEREC_D, 0x88);
			xe_pshufd_xxi(EEREC_HI, EEREC_D, 0xdd);
		}
		else
		{
			xe_pshufd_xxi(EEREC_LO, EEREC_HI, 0x88);
			xe_pshufd_xxi(EEREC_HI, EEREC_HI, 0xdd);
		}
		xe_pmovsxdq_xx(EEREC_LO, EEREC_LO);
		xe_pmovsxdq_xx(EEREC_HI, EEREC_HI);
	}
	_clearNeededXMMregs();
}
////////////////////////////////////////////////////
// PDIVW divides the signed words 0 and 2 of Rs by those of Rt, putting the
// quotients in LO.SD[0..1] and the remainders in HI.SD[0..1], sign-extended.
// Rd is not written.
//
// The three cases are exactly the ones scalar DIV already handles (see
// recDIVsuper in iR5900MultDiv.cpp), because _PDIVW is the same algorithm run
// twice: INT_MIN / -1 is defined to give INT_MIN remainder 0 rather than
// trapping, and division by zero gives quotient (dividend < 0) ? 1 : -1 with
// the dividend as remainder.
static void recPDIVW_half(int hilo, int word)
{
	u8* end1 = NULL;
	u8* end2;
	u8* cont1;
	u8* cont2;
	u8* cont3;

	xe_mov32_rm(XE_CX, &cpuRegs.GPR.r[_Rt_].UL[word]);
	xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[word]);

	// INT_MIN / -1: quotient stays 0x80000000, remainder 0. x86 would raise
	// #DE here, so the case has to be branched around rather than divided.
	xe_cmp32_ri(XE_AX, 0x80000000);
	xe_fwd_jcc8(Jcc_NotEqual, cont1);
	xe_cmp32_ri(XE_CX, 0xffffffff);
	xe_fwd_jcc8(Jcc_NotEqual, cont2);
	xe_xor32_rr(XE_DX, XE_DX);
	xe_fwd_jcc8(Jcc_Unconditional, end1);

	xe_fwd_set8(cont1);
	xe_fwd_set8(cont2);

	xe_cmp32_ri(XE_CX, 0);
	xe_fwd_jcc8(Jcc_NotEqual, cont3);
	// Divide by zero: remainder is the dividend, quotient is its sign
	// mapped to 1 / -1. SAR+SHL+NOT turns (x<0) into 1 and (x>=0) into -1
	// without a branch.
	xe_mov32_rr(XE_DX, XE_AX);
	xe_sar32_ri(XE_AX, 31);
	xe_shl32_ri(XE_AX, 1);
	xe_not32_r(XE_AX);
	xe_fwd_jcc8(Jcc_Unconditional, end2);

	xe_fwd_set8(cont3);
	xe_cdq();
	xe_idiv32_r(XE_CX);

	xe_fwd_set8(end1);
	xe_fwd_set8(end2);

	// LO/HI hold 64-bit lanes; both results are sign-extended into them.
	xe_movsxd_rr(XE_AX, XE_AX);
	xe_mov64_mr(&cpuRegs.LO.UD[hilo], XE_AX);
	xe_movsxd_rr(XE_DX, XE_DX);
	xe_mov64_mr(&cpuRegs.HI.UD[hilo], XE_DX);
}

void recPDIVW(void)
{
	// Flush everything the sequence reads or clobbers: it works out of
	// memory and needs eax, ecx and edx. Same preamble recMADD uses.
	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	// Const-propagated registers do not live in cpuRegs.GPR.r[] until they
	// are flushed there, and none of the deletes above does that -- recMADD
	// checks GPR_IS_CONST1 after the very same sequence for exactly this
	// reason. Materialise them instead, which also covers the four-word case
	// below where the manual check would only cover the low half.
	_flushConstReg(_Rs_);
	_flushConstReg(_Rt_);

	recPDIVW_half(0, 0);
	recPDIVW_half(1, 2);
}

////////////////////////////////////////////////////
// PDIVBW divides all four words of Rs by a single divisor: halfword 0 of Rt,
// sign-extended to 32 bits. Unlike PDIVW the results are 32-bit lanes of
// LO/HI, so nothing is sign-extended on the way out.
//
// Both interpreter tests fall out of the sign-extended divisor in ECX:
// Rt.US[0] == 0xffff is ECX == -1, and Rt.US[0] == 0 is ECX == 0. IDIV does
// not clobber ECX, so the divisor is loaded once for all four lanes.
static void recPDIVBW_lane(int n)
{
	u8* end1;
	u8* end2;
	u8* cont1;
	u8* cont2;
	u8* cont3;

	xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[n]);

	xe_cmp32_ri(XE_AX, 0x80000000);
	xe_fwd_jcc8(Jcc_NotEqual, cont1);
	xe_cmp32_ri(XE_CX, 0xffffffff);
	xe_fwd_jcc8(Jcc_NotEqual, cont2);
	xe_xor32_rr(XE_DX, XE_DX);
	xe_fwd_jcc8(Jcc_Unconditional, end1);

	xe_fwd_set8(cont1);
	xe_fwd_set8(cont2);

	xe_cmp32_ri(XE_CX, 0);
	xe_fwd_jcc8(Jcc_NotEqual, cont3);
	xe_mov32_rr(XE_DX, XE_AX);
	xe_sar32_ri(XE_AX, 31);
	xe_shl32_ri(XE_AX, 1);
	xe_not32_r(XE_AX);
	xe_fwd_jcc8(Jcc_Unconditional, end2);

	xe_fwd_set8(cont3);
	xe_cdq();
	xe_idiv32_r(XE_CX);

	xe_fwd_set8(end1);
	xe_fwd_set8(end2);

	xe_mov32_mr(&cpuRegs.LO.UL[n], XE_AX);
	xe_mov32_mr(&cpuRegs.HI.UL[n], XE_DX);
}

void recPDIVBW(void)
{
	int n;

	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	// Const-propagated registers do not live in cpuRegs.GPR.r[] until they
	// are flushed there, and none of the deletes above does that -- recMADD
	// checks GPR_IS_CONST1 after the very same sequence for exactly this
	// reason. Materialise them instead, which also covers the four-word case
	// below where the manual check would only cover the low half.
	_flushConstReg(_Rs_);
	_flushConstReg(_Rt_);

	xe_movsx32_rm16(XE_CX, &cpuRegs.GPR.r[_Rt_].US[0]);
	for (n = 0; n < 4; n++)
		recPDIVBW_lane(n);
}

////////////////////////////////////////////////////

//upper word of each doubleword in LO and HI is undocumented/undefined
//contains the upper multiplication result (before the addition with the lower multiplication result)
void recPHMADH(void)
{
	int info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);

	xe_movdqa_xx(t0reg, EEREC_S);
	xe_psrld_xi(t0reg, 16);
	xe_pslld_xi(t0reg, 16);
	xe_pmaddwd_xx(t0reg, EEREC_T);

	if (_Rd_)
	{
		if (EEREC_D == EEREC_S)
		{
			xe_pmaddwd_xx(EEREC_D, EEREC_T);
		}
		else if (EEREC_D == EEREC_T)
		{
			xe_pmaddwd_xx(EEREC_D, EEREC_S);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_pmaddwd_xx(EEREC_D, EEREC_S);
		}
		xe_movdqa_xx(EEREC_LO, EEREC_D);
	}
	else
	{
		xe_movdqa_xx(EEREC_LO, EEREC_T);
		xe_pmaddwd_xx(EEREC_LO, EEREC_S);
	}

	xe_movdqa_xx(EEREC_HI, EEREC_LO);

	xe_shufps_xxi(EEREC_LO, t0reg, 0x88);
	xe_shufps_xxi(EEREC_LO, EEREC_LO, 0xd8);

	xe_shufps_xxi(EEREC_HI, t0reg, 0xdd);
	xe_shufps_xxi(EEREC_HI, EEREC_HI, 0xd8);

	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

void recPMSUBH(void)
{
	int info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	if (!_Rd_)
	{
		xe_pxor_xx(t0reg, t0reg);
		xe_pshufd_xxi(t1reg, EEREC_S, 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xe_punpcklwd_xx(t1reg, t0reg); //S0, 0, S1, 0, S4, 0, S5, 0
		xe_pshufd_xxi(t0reg, EEREC_T, 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xe_punpcklwd_xx(t0reg, t0reg); //T0, T0, T1, T1, T4, T4, T5, T5
		xe_pmaddwd_xx(t0reg, t1reg); //S0*T0+0*T0, S1*T1+0*T1, S4*T4+0*T4, S5*T5+0*T5

		xe_psubd_xx(EEREC_LO, t0reg);

		xe_pxor_xx(t0reg, t0reg);
		xe_pshufd_xxi(t1reg, EEREC_S, 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xe_punpckhwd_xx(t1reg, t0reg); //S2, 0, S3, 0, S6, 0, S7, 0
		xe_pshufd_xxi(t0reg, EEREC_T, 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xe_punpckhwd_xx(t0reg, t0reg); //T2, T2, T3, T3, T6, T6, T7, T7
		xe_pmaddwd_xx(t0reg, t1reg); //S2*T2+0*T2, S3*T3+0*T3, S6*T6+0*T6, S7*T7+0*T7

		xe_psubd_xx(EEREC_HI, t0reg);
	}
	else
	{
		xe_movdqa_xx(t0reg, EEREC_S);
		xe_movdqa_xx(t1reg, EEREC_S);

		xe_pmullw_xx(t0reg, EEREC_T);
		xe_pmulhw_xx(t1reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, t0reg);

		// 0-3
		xe_punpcklwd_xx(t0reg, t1reg);
		// 4-7
		xe_punpckhwd_xx(EEREC_D, t1reg);
		xe_movdqa_xx(t1reg, t0reg);

		// 0,1,4,5, L->H
		xe_punpcklqdq_xx(t0reg, EEREC_D);
		// 2,3,6,7, L->H
		xe_punpckhqdq_xx(t1reg, EEREC_D);

		xe_psubd_xx(EEREC_LO, t0reg);
		xe_psubd_xx(EEREC_HI, t1reg);

		// 0,2,4,6, L->H
		xe_pshufd_xxi(EEREC_D, EEREC_LO, 0x88);
		xe_pshufd_xxi(t0reg, EEREC_HI, 0x88);
		xe_punpckldq_xx(EEREC_D, t0reg);
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);

	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
//upper word of each doubleword in LO and HI is undocumented/undefined
//it contains the NOT of the upper multiplication result (before the substraction of the lower multiplication result)
void recPHMSBH(void)
{
	int info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);

	xe_pcmpeqd_xx(EEREC_LO, EEREC_LO);
	xe_psrld_xi(EEREC_LO, 16);
	xe_movdqa_xx(EEREC_HI, EEREC_S);
	xe_pand_xx(EEREC_HI, EEREC_LO);
	xe_pmaddwd_xx(EEREC_HI, EEREC_T);
	xe_pslld_xi(EEREC_LO, 16);
	xe_pand_xx(EEREC_LO, EEREC_S);
	xe_pmaddwd_xx(EEREC_LO, EEREC_T);
	xe_movdqa_xx(t0reg, EEREC_LO);
	xe_psubd_xx(EEREC_LO, EEREC_HI);
	if (_Rd_)
		xe_movdqa_xx(EEREC_D, EEREC_LO);

	xe_pcmpeqd_xx(EEREC_HI, EEREC_HI);
	xe_pxor_xx(t0reg, EEREC_HI);

	xe_movdqa_xx(EEREC_HI, EEREC_LO);

	xe_shufps_xxi(EEREC_LO, t0reg, 0x88);
	xe_shufps_xxi(EEREC_LO, EEREC_LO, 0xd8);

	xe_shufps_xxi(EEREC_HI, t0reg, 0xdd);
	xe_shufps_xxi(EEREC_HI, EEREC_HI, 0xd8);

	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXEH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshuflw_xxi(EEREC_D, EEREC_T, 0xc6);
	xe_pshufhw_xxi(EEREC_D, EEREC_D, 0xc6);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPREVH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshuflw_xxi(EEREC_D, EEREC_T, 0x1B);
	xe_pshufhw_xxi(EEREC_D, EEREC_D, 0x1B);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPINTH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | XMMINFO_WRITED);
	if (EEREC_D == EEREC_S)
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_movhlps_xx(t0reg, EEREC_S);
		if (EEREC_D != EEREC_T)
			xe_movqzx_xx(EEREC_D, EEREC_T);
		xe_punpcklwd_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	else
	{
		xe_movlhps_xx(EEREC_D, EEREC_T);
		xe_punpckhwd_xx(EEREC_D, EEREC_S);
	}
	_clearNeededXMMregs();
}

void recPEXEW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshufd_xxi(EEREC_D, EEREC_T, 0xc6);
	_clearNeededXMMregs();
}

void recPROT3W(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshufd_xxi(EEREC_D, EEREC_T, 0xc9);
	_clearNeededXMMregs();
}

void recPMULTH(void)
{
	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_READT | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);

	xe_movdqa_xx(EEREC_LO, EEREC_S);
	xe_movdqa_xx(EEREC_HI, EEREC_S);

	xe_pmullw_xx(EEREC_LO, EEREC_T);
	xe_pmulhw_xx(EEREC_HI, EEREC_T);
	xe_movdqa_xx(t0reg, EEREC_LO);

	// 0-3
	xe_punpcklwd_xx(EEREC_LO, EEREC_HI);
	// 4-7
	xe_punpckhwd_xx(t0reg, EEREC_HI);

	if (_Rd_)
	{
		// 0,2,4,6, L->H
		xe_pshufd_xxi(EEREC_D, EEREC_LO, 0x88);
		xe_pshufd_xxi(EEREC_HI, t0reg, 0x88);
		xe_punpcklqdq_xx(EEREC_D, EEREC_HI);
	}

	xe_movdqa_xx(EEREC_HI, EEREC_LO);

	// 0,1,4,5, L->H
	xe_punpcklqdq_xx(EEREC_LO, t0reg);
	// 2,3,6,7, L->H
	xe_punpckhqdq_xx(EEREC_HI, t0reg);

	_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

void recPMFHI(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READHI);
	xe_movdqa_xx(EEREC_D, EEREC_HI);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMFLO(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READLO);
	xe_movdqa_xx(EEREC_D, EEREC_LO);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPAND(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
	if (EEREC_D == EEREC_T)
	{
		xe_pand_xx(EEREC_D, EEREC_S);
	}
	else if (EEREC_D == EEREC_S)
	{
		xe_pand_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pand_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPXOR(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_WRITED | XMMINFO_READS | XMMINFO_READT);
	if (EEREC_D == EEREC_T)
	{
		xe_pxor_xx(EEREC_D, EEREC_S);
	}
	else if (EEREC_D == EEREC_S)
	{
		xe_pxor_xx(EEREC_D, EEREC_T);
	}
	else
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pxor_xx(EEREC_D, EEREC_T);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCPYLD(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_WRITED | ((_Rs_ == 0) ? 0 : XMMINFO_READS) | XMMINFO_READT);
	if (_Rs_ == 0)
	{
		xe_movqzx_xx(EEREC_D, EEREC_T);
	}
	else
	{
		if (EEREC_D == EEREC_T)
			xe_punpcklqdq_xx(EEREC_D, EEREC_S);
		else if (EEREC_S == EEREC_T)
			xe_pshufd_xxi(EEREC_D, EEREC_S, 0x44);
		else if (EEREC_D == EEREC_S)
		{
			xe_punpcklqdq_xx(EEREC_D, EEREC_T);
			xe_pshufd_xxi(EEREC_D, EEREC_D, 0x4e);
		}
		else
		{
			xe_movqzx_xx(EEREC_D, EEREC_T);
			xe_punpcklqdq_xx(EEREC_D, EEREC_S);
		}
	}
	_clearNeededXMMregs();
}

void recPMADDH(void)
{
	int info = eeRecompileCodeXMM((_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_READS | XMMINFO_READT | XMMINFO_READLO | XMMINFO_READHI | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	int t0reg = _allocTempXMMreg(XMMT_INT);
	int t1reg = _allocTempXMMreg(XMMT_INT);

	if (!_Rd_)
	{
		xe_pxor_xx(t0reg, t0reg);
		xe_pshufd_xxi(t1reg, EEREC_S, 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xe_punpcklwd_xx(t1reg, t0reg); //S0, 0, S1, 0, S4, 0, S5, 0
		xe_pshufd_xxi(t0reg, EEREC_T, 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xe_punpcklwd_xx(t0reg, t0reg); //T0, T0, T1, T1, T4, T4, T5, T5
		xe_pmaddwd_xx(t0reg, t1reg); //S0*T0+0*T0, S1*T1+0*T1, S4*T4+0*T4, S5*T5+0*T5

		xe_paddd_xx(EEREC_LO, t0reg);

		xe_pxor_xx(t0reg, t0reg);
		xe_pshufd_xxi(t1reg, EEREC_S, 0xd8); //S0, S1, S4, S5, S2, S3, S6, S7
		xe_punpckhwd_xx(t1reg, t0reg); //S2, 0, S3, 0, S6, 0, S7, 0
		xe_pshufd_xxi(t0reg, EEREC_T, 0xd8); //T0, T1, T4, T5, T2, T3, T6, T7
		xe_punpckhwd_xx(t0reg, t0reg); //T2, T2, T3, T3, T6, T6, T7, T7
		xe_pmaddwd_xx(t0reg, t1reg); //S2*T2+0*T2, S3*T3+0*T3, S6*T6+0*T6, S7*T7+0*T7

		xe_paddd_xx(EEREC_HI, t0reg);
	}
	else
	{
		xe_movdqa_xx(t0reg, EEREC_S);
		xe_movdqa_xx(t1reg, EEREC_S);

		xe_pmullw_xx(t0reg, EEREC_T);
		xe_pmulhw_xx(t1reg, EEREC_T);
		xe_movdqa_xx(EEREC_D, t0reg);

		// 0-3
		xe_punpcklwd_xx(t0reg, t1reg);
		// 4-7
		xe_punpckhwd_xx(EEREC_D, t1reg);
		xe_movdqa_xx(t1reg, t0reg);

		// 0,1,4,5, L->H
		xe_punpcklqdq_xx(t0reg, EEREC_D);
		// 2,3,6,7, L->H
		xe_punpckhqdq_xx(t1reg, EEREC_D);

		xe_paddd_xx(EEREC_LO, t0reg);
		xe_paddd_xx(EEREC_HI, t1reg);

		// 0,2,4,6, L->H
		xe_pshufd_xxi(EEREC_D, EEREC_LO, 0x88);
		xe_pshufd_xxi(t0reg, EEREC_HI, 0x88);
		xe_punpckldq_xx(EEREC_D, t0reg);
	}

	_freeXMMreg(t0reg);
	_freeXMMreg(t1reg);

	_clearNeededXMMregs();
}

/*********************************************************
*   MMI3 opcodes                                         *
*                                                        *
*********************************************************/

////////////////////////////////////////////////////
//REC_FUNC( PSRAVW, _Rd_ );

void recPSRAVW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);
	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xe_pxor_xx(EEREC_D, EEREC_D);
		}
		else
		{
			xe_pshufd_xxi(EEREC_D, EEREC_T, 0x88);
			xe_pmovsxdq_xx(EEREC_D, EEREC_D);
		}
	}
	else if (_Rt_ == 0)
	{
		xe_pxor_xx(EEREC_D, EEREC_D);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		int t1reg = _allocTempXMMreg(XMMT_INT);

		// shamt is 5-bit
		xe_movdqa_xx(t0reg, EEREC_S);
		xe_psllq_xi(t0reg, 27 + 32);
		xe_psrlq_xi(t0reg, 27 + 32);

		// EEREC_D[0] <- Rt[0], t1reg[0] <- Rt[2]
		xe_movhlps_xx(t1reg, EEREC_T);
		if (EEREC_D != EEREC_T)
			xe_movdqa_xx(EEREC_D, EEREC_T);

		// shift (right arithmetic) Rt[0]
		xe_psrad_xx(EEREC_D, t0reg);

		// shift (right arithmetic) Rt[2]
		xe_movhlps_xx(t0reg, t0reg);
		xe_psrad_xx(t1reg, t0reg);

		// merge & sign extend
		xe_punpckldq_xx(EEREC_D, t1reg);
		if (CPU_HAS_SSE41)
		{
			xe_pmovsxdq_xx(EEREC_D, EEREC_D);
		}
		else
		{
			xe_movdqa_xx(t0reg, EEREC_D);
			xe_psrad_xi(t0reg, 31); // get the signs
			xe_punpckldq_xx(EEREC_D, t0reg);
		}

		_freeXMMreg(t0reg);
		_freeXMMreg(t1reg);
	}

	_clearNeededXMMregs();
}


////////////////////////////////////////////////////
alignas(16) static const u32 s_tempPINTEH[4] = {0x0000ffff, 0x0000ffff, 0x0000ffff, 0x0000ffff};

void recPINTEH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ ? XMMINFO_READS : 0) | (_Rt_ ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	int t0reg = -1;

	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xe_pxor_xx(EEREC_D, EEREC_D);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			xe_pand_xm(EEREC_D, s_tempPINTEH);
		}
	}
	else if (_Rt_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
		xe_pslld_xi(EEREC_D, 16);
	}
	else
	{
		if (EEREC_S == EEREC_T)
		{
			xe_pshuflw_xxi(EEREC_D, EEREC_S, 0xa0);
			xe_pshufhw_xxi(EEREC_D, EEREC_D, 0xa0);
		}
		else if (EEREC_D == EEREC_T)
		{
			t0reg = _allocTempXMMreg(XMMT_INT);
			xe_pslld_xi(EEREC_D, 16);
			xe_movdqa_xx(t0reg, EEREC_S);
			xe_psrld_xi(EEREC_D, 16);
			xe_pslld_xi(t0reg, 16);
			xe_por_xx(EEREC_D, t0reg);
		}
		else
		{
			t0reg = _allocTempXMMreg(XMMT_INT);
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_movdqa_xx(t0reg, EEREC_T);
			xe_pslld_xi(t0reg, 16);
			xe_pslld_xi(EEREC_D, 16);
			xe_psrld_xi(t0reg, 16);
			xe_por_xx(EEREC_D, t0reg);
		}
	}

	if (t0reg >= 0)
		_freeXMMreg(t0reg);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMULTUW(void)
{
	int info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI);
	if (!_Rs_ || !_Rt_)
	{
		if (_Rd_)
			xe_pxor_xx(EEREC_D, EEREC_D);
		xe_pxor_xx(EEREC_LO, EEREC_LO);
		xe_pxor_xx(EEREC_HI, EEREC_HI);
	}
	else
	{
		if (_Rd_)
		{
			if (EEREC_D == EEREC_S)
				xe_pmuludq_xx(EEREC_D, EEREC_T);
			else if (EEREC_D == EEREC_T)
				xe_pmuludq_xx(EEREC_D, EEREC_S);
			else
			{
				xe_movdqa_xx(EEREC_D, EEREC_S);
				xe_pmuludq_xx(EEREC_D, EEREC_T);
			}
			xe_movdqa_xx(EEREC_HI, EEREC_D);
		}
		else
		{
			xe_movdqa_xx(EEREC_HI, EEREC_S);
			xe_pmuludq_xx(EEREC_HI, EEREC_T);
		}

		// interleave & sign extend
		if (CPU_HAS_SSE41)
		{
			xe_pshufd_xxi(EEREC_LO, EEREC_HI, 0x88);
			xe_pshufd_xxi(EEREC_HI, EEREC_HI, 0xdd);
			xe_pmovsxdq_xx(EEREC_LO, EEREC_LO);
			xe_pmovsxdq_xx(EEREC_HI, EEREC_HI);
		}
		else
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_pshufd_xxi(t0reg, EEREC_HI, 0xd8);
			xe_movdqa_xx(EEREC_LO, t0reg);
			xe_movdqa_xx(EEREC_HI, t0reg);
			xe_psrad_xi(t0reg, 31); // get the signs

			xe_punpckldq_xx(EEREC_LO, t0reg);
			xe_punpckhdq_xx(EEREC_HI, t0reg);
			_freeXMMreg(t0reg);
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMADDUW(void)
{
	int info = eeRecompileCodeXMM((((_Rs_) && (_Rt_)) ? XMMINFO_READS : 0) | (((_Rs_) && (_Rt_)) ? XMMINFO_READT : 0) | (_Rd_ ? XMMINFO_WRITED : 0) | XMMINFO_WRITELO | XMMINFO_WRITEHI | XMMINFO_READLO | XMMINFO_READHI);
	xe_shufps_xxi(EEREC_LO, EEREC_HI, 0x88);
	xe_pshufd_xxi(EEREC_LO, EEREC_LO, 0xd8); // LO = {LO[0], HI[0], LO[2], HI[2]}
	if (_Rd_)
	{
		if (!_Rs_ || !_Rt_)
			xe_pxor_xx(EEREC_D, EEREC_D);
		else if (EEREC_D == EEREC_S)
			xe_pmuludq_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_pmuludq_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			xe_pmuludq_xx(EEREC_D, EEREC_T);
		}
	}
	else
	{
		if (!_Rs_ || !_Rt_)
			xe_pxor_xx(EEREC_HI, EEREC_HI);
		else
		{
			xe_movdqa_xx(EEREC_HI, EEREC_S);
			xe_pmuludq_xx(EEREC_HI, EEREC_T);
		}
	}

	// add from LO/HI
	if (_Rd_)
	{
		xe_paddq_xx(EEREC_D, EEREC_LO);
		xe_movdqa_xx(EEREC_HI, EEREC_D);
	}
	else
		xe_paddq_xx(EEREC_HI, EEREC_LO);

	// interleave & sign extend
	if (CPU_HAS_SSE41)
	{
		xe_pshufd_xxi(EEREC_LO, EEREC_HI, 0x88);
		xe_pshufd_xxi(EEREC_HI, EEREC_HI, 0xdd);
		xe_pmovsxdq_xx(EEREC_LO, EEREC_LO);
		xe_pmovsxdq_xx(EEREC_HI, EEREC_HI);
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);
		xe_pshufd_xxi(t0reg, EEREC_HI, 0xd8);
		xe_movdqa_xx(EEREC_LO, t0reg);
		xe_movdqa_xx(EEREC_HI, t0reg);
		xe_psrad_xi(t0reg, 31); // get the signs

		xe_punpckldq_xx(EEREC_LO, t0reg);
		xe_punpckhdq_xx(EEREC_HI, t0reg);
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
// PDIVUW: unsigned divide of words 0 and 2. No INT_MIN/-1 case exists --
// with EDX zeroed a 32-bit unsigned divide can only fault on a zero divisor
// -- so only that one branch is needed. The interpreter casts each result to
// s32 before storing into the 64-bit LO/HI lane, so both sign-extend.
static void recPDIVUW_half(int hilo, int word)
{
	u8* cont;
	u8* end;

	xe_mov32_rm(XE_CX, &cpuRegs.GPR.r[_Rt_].UL[word]);
	xe_mov32_rm(XE_AX, &cpuRegs.GPR.r[_Rs_].UL[word]);

	xe_cmp32_ri(XE_CX, 0);
	xe_fwd_jcc8(Jcc_NotEqual, cont);
	// Divide by zero: quotient -1, remainder the dividend.
	xe_mov32_rr(XE_DX, XE_AX);
	xe_mov32_ri(XE_AX, 0xffffffff);
	xe_fwd_jcc8(Jcc_Unconditional, end);

	xe_fwd_set8(cont);
	xe_xor32_rr(XE_DX, XE_DX);
	xe_udiv32_r(XE_CX);

	xe_fwd_set8(end);

	xe_movsxd_rr(XE_AX, XE_AX);
	xe_mov64_mr(&cpuRegs.LO.UD[hilo], XE_AX);
	xe_movsxd_rr(XE_DX, XE_DX);
	xe_mov64_mr(&cpuRegs.HI.UD[hilo], XE_DX);
}

void recPDIVUW(void)
{
	_deleteEEreg(XMMGPR_LO, 1);
	_deleteEEreg(XMMGPR_HI, 1);
	_deleteGPRtoX86reg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoX86reg(_Rt_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rs_, DELETE_REG_FLUSH);
	_deleteGPRtoXMMreg(_Rt_, DELETE_REG_FLUSH);

	// Const-propagated registers do not live in cpuRegs.GPR.r[] until they
	// are flushed there, and none of the deletes above does that -- recMADD
	// checks GPR_IS_CONST1 after the very same sequence for exactly this
	// reason. Materialise them instead, which also covers the four-word case
	// below where the manual check would only cover the low half.
	_flushConstReg(_Rs_);
	_flushConstReg(_Rt_);

	recPDIVUW_half(0, 0);
	recPDIVUW_half(1, 2);
}

////////////////////////////////////////////////////
void recPEXCW(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshufd_xxi(EEREC_D, EEREC_T, 0xd8);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPEXCH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshuflw_xxi(EEREC_D, EEREC_T, 0xd8);
	xe_pshufhw_xxi(EEREC_D, EEREC_D, 0xd8);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPNOR(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xe_pcmpeqd_xx(EEREC_D, EEREC_D);
		}
		else
		{
			if (EEREC_D == EEREC_T)
			{
				int t0reg = _allocTempXMMreg(XMMT_INT);
				xe_pcmpeqd_xx(t0reg, t0reg);
				xe_pxor_xx(EEREC_D, t0reg);
				_freeXMMreg(t0reg);
			}
			else
			{
				xe_pcmpeqd_xx(EEREC_D, EEREC_D);
				if (_Rt_ != 0)
					xe_pxor_xx(EEREC_D, EEREC_T);
			}
		}
	}
	else if (_Rt_ == 0)
	{
		if (EEREC_D == EEREC_S)
		{
			int t0reg = _allocTempXMMreg(XMMT_INT);
			xe_pcmpeqd_xx(t0reg, t0reg);
			xe_pxor_xx(EEREC_D, t0reg);
			_freeXMMreg(t0reg);
		}
		else
		{
			xe_pcmpeqd_xx(EEREC_D, EEREC_D);
			xe_pxor_xx(EEREC_D, EEREC_S);
		}
	}
	else
	{
		int t0reg = _allocTempXMMreg(XMMT_INT);

		if (EEREC_D == EEREC_S)
			xe_por_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
			xe_por_xx(EEREC_D, EEREC_S);
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_S);
			if (EEREC_S != EEREC_T)
				xe_por_xx(EEREC_D, EEREC_T);
		}

		xe_pcmpeqd_xx(t0reg, t0reg);
		xe_pxor_xx(EEREC_D, t0reg);
		_freeXMMreg(t0reg);
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMTHI(void)
{
	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_WRITEHI);
	xe_movdqa_xx(EEREC_HI, EEREC_S);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPMTLO(void)
{
	int info = eeRecompileCodeXMM(XMMINFO_READS | XMMINFO_WRITELO);
	xe_movdqa_xx(EEREC_LO, EEREC_S);
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCPYUD(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READS | ((_Rt_ == 0) ? 0 : XMMINFO_READT) | XMMINFO_WRITED);

	if (_Rt_ == 0)
	{
		if (EEREC_D == EEREC_S)
		{
			xe_punpckhqdq_xx(EEREC_D, EEREC_S);
			xe_movqzx_xx(EEREC_D, EEREC_D);
		}
		else
		{
			xe_movhlps_xx(EEREC_D, EEREC_S);
			xe_movqzx_xx(EEREC_D, EEREC_D);
		}
	}
	else
	{
		if (EEREC_D == EEREC_S)
			xe_punpckhqdq_xx(EEREC_D, EEREC_T);
		else if (EEREC_D == EEREC_T)
		{
			//TODO
			xe_punpckhqdq_xx(EEREC_D, EEREC_S);
			xe_pshufd_xxi(EEREC_D, EEREC_D, 0x4e);
		}
		else
		{
			if (EEREC_S == EEREC_T)
			{
				xe_pshufd_xxi(EEREC_D, EEREC_S, 0xee);
			}
			else
			{
				xe_movdqa_xx(EEREC_D, EEREC_S);
				xe_punpckhqdq_xx(EEREC_D, EEREC_T);
			}
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPOR(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM((_Rs_ != 0 ? XMMINFO_READS : 0) | (_Rt_ != 0 ? XMMINFO_READT : 0) | XMMINFO_WRITED);

	if (_Rs_ == 0)
	{
		if (_Rt_ == 0)
		{
			xe_pxor_xx(EEREC_D, EEREC_D);
		}
		else
			xe_movdqa_xx(EEREC_D, EEREC_T);
	}
	else if (_Rt_ == 0)
	{
		xe_movdqa_xx(EEREC_D, EEREC_S);
	}
	else
	{
		if (EEREC_D == EEREC_S)
		{
			xe_por_xx(EEREC_D, EEREC_T);
		}
		else if (EEREC_D == EEREC_T)
		{
			xe_por_xx(EEREC_D, EEREC_S);
		}
		else
		{
			xe_movdqa_xx(EEREC_D, EEREC_T);
			if (EEREC_S != EEREC_T)
			{
				xe_por_xx(EEREC_D, EEREC_S);
			}
		}
	}
	_clearNeededXMMregs();
}

////////////////////////////////////////////////////
void recPCPYH(void)
{
	if (!_Rd_)
		return;

	int info = eeRecompileCodeXMM(XMMINFO_READT | XMMINFO_WRITED);
	xe_pshuflw_xxi(EEREC_D, EEREC_T, 0);
	xe_pshufhw_xxi(EEREC_D, EEREC_D, 0);
	_clearNeededXMMregs();
}


