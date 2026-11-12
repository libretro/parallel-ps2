/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include "iR5900Analysis.h"
#include "Memory.h"
#include "Config.h"

/* This should be moved to analysis... */
extern int cop2flags(u32 code);

/* C89 shape: the analysis passes are free functions; the per-run state the
 * old classes carried lives on the stack. Iteration is the explicit loop the
 * old ForEachInstruction template unrolled to: fetch the opcode into
 * cpuRegs.code (the _Opcode_/_Rs_/... macros read it), then visit.
 */

void COP2FlagHackPass_Run(u32 start, u32 end, EEINST* inst_cache)
{
	int status_denormalized = 0;
	EEINST* last_status_write = NULL;
	EEINST* last_mac_write    = NULL;
	EEINST* last_clip_write   = NULL;
	u32 cfc2_pc               = start;

	EEINST* inst = inst_cache;
	u32 apc;
	for (apc = start; apc < end; apc += 4, inst++)
	{
		cpuRegs.code = memRead32(apc);

		/* Catch SB/SH/SW to potential DMA->VIF0->VU0 exec.
		 * this is very unlikely in a cop2 chain. */
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053)
		{
			if (last_status_write)
			{
				last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
				status_denormalized = 0;
			}
			if (last_mac_write)
				last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
			if (last_clip_write)
				last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
		}
		else if (_Opcode_ == 022) /* COP2 ? */
		{
			/* Detect CTC2 Status, zero, ..., CFC2 v0, Status pattern where we need accurate sticky bits.
			 * Test case: Tekken Tag Tournament. */
			if (_Rs_ == 6 && _Rd_ == REG_STATUS_FLAG)
			{
				u32 capc;
				/* Read ahead, looking for CFC2. The scan loads each
				 * candidate into cpuRegs.code, which the _Opcode_/_Rs_/_Rd_
				 * macros below read, so restore this instruction's word
				 * before the tests that follow. */
				cfc2_pc = apc;
				for (capc = apc; capc < end; capc += 4)
				{
					cpuRegs.code = memRead32(capc);
					if (_Opcode_ == 022 && _Rs_ == 2 && _Rd_ == REG_STATUS_FLAG)
					{
						cfc2_pc = capc;
						break;
					}
				}
				cpuRegs.code = memRead32(apc);
			}

			/* CFC2/CTC2 */
			if (_Rs_ == 6 || _Rs_ == 2)
			{
				switch (_Rd_)
				{
					case REG_STATUS_FLAG:
						if (last_status_write)
						{
							last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
							status_denormalized = 0;
						}
						break;
					case REG_MAC_FLAG:
						if (last_mac_write)
							last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
						break;
					case REG_CLIP_FLAG:
						if (last_clip_write)
							last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
						break;
					case REG_FBRST:
						{
							/* only apply to CTC2, is FBRST readable? */
							if (_Rs_ == 2)
							{
								if (last_status_write)
								{
									last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
									status_denormalized = 0;
								}
								if (last_mac_write)
									last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
								if (last_clip_write)
									last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
							}
						}
						break;
				}
			}

			/* VCALLMS, everything needs to be up to date */
			if (((cpuRegs.code >> 25 & 1) == 1) && ((cpuRegs.code >> 2 & 15) == 14))
			{
				if (last_status_write)
				{
					last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
					status_denormalized = 0;
				}
				if (last_mac_write)
					last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
				if (last_clip_write)
					last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
			}

			/* 1 - status, 2 - mac, 3 - clip */
			{
				const int flags = cop2flags(cpuRegs.code);
				if (flags != 0)
				{
					/* STATUS */
					if (flags & 1)
					{
						if (!status_denormalized)
						{
							inst->info |= EEINST_COP2_DENORMALIZE_STATUS_FLAG;
							status_denormalized = 1;
						}

						/* If we're still behind the next CFC2 after the sticky bits got cleared,
						 * we need to update flags.
						 * Also do this if we're a vsqrt/vrsqrt/vdiv,
						 * these update status unconditionally. */
						{
							const u32 sub_opcode = (cpuRegs.code & 3) | ((cpuRegs.code >> 4) & 0x7c);
							if (apc < cfc2_pc || (_Rs_ >= 020 && _Funct_ >= 074
										&& sub_opcode >= 070 && sub_opcode <= 072))
								inst->info |= EEINST_COP2_STATUS_FLAG;
						}

						last_status_write = inst;
					}

					/* MAC */
					if (flags & 2)
						last_mac_write = inst;

					/* CLIP */
					if (flags & 4)
					{
						/* We don't track the clip flag yet..
						 * but it's unlikely that we'll have
						 * more than 4 clip flags in a row,
						 * because that would be pointless? */
						inst->info |= EEINST_COP2_CLIP_FLAG;
						last_clip_write = inst;
					}
				}
			}
		}
	}

	if (last_status_write)
	{
		last_status_write->info |= EEINST_COP2_STATUS_FLAG | EEINST_COP2_NORMALIZE_STATUS_FLAG;
		status_denormalized      = 0;
	}
	if (last_mac_write)
		last_mac_write->info  |= EEINST_COP2_MAC_FLAG;
	if (last_clip_write)
		last_clip_write->info |= EEINST_COP2_CLIP_FLAG;
}

void COP2MicroFinishPass_Run(u32 start, u32 end, EEINST* inst_cache)
{
	int needs_vu0_sync = 1;
	int needs_vu0_finish = 1;
	int block_interlocked = !!(CHECK_FULLVU0SYNCHACK);
	EEINST* inst;
	u32 apc;

	/* First pass through the block to find out if it's interlocked or not. If it is, we need to use tighter */
	/* synchronization on all COP2 instructions, otherwise Crash Twinsanity breaks. */
	{
		for (apc = start; apc < end; apc += 4)
		{
			cpuRegs.code = memRead32(apc);
			if (_Opcode_ == 022 && (_Rs_ == 001 || _Rs_ == 002 || _Rs_ == 005 || _Rs_ == 006) && cpuRegs.code & 1)
			{
				block_interlocked = 1;
				break;
			}
		}
	}

	inst = inst_cache;
	for (apc = start; apc < end; apc += 4, inst++)
	{
		int is_lqc_sqc;
		int is_non_interlocked_move;
		int likely_clear;
		cpuRegs.code = memRead32(apc);
		/* Catch SQ/SB/SH/SW/SD to potential DMA->VIF0->VU0 exec. */
		/* Also VCALLMS/VCALLMSR, that can start a micro, so the next instruction needs to finish it. */
		/* This is very unlikely in a cop2 chain. */
		if (_Opcode_ == 050 || _Opcode_ == 051 || _Opcode_ == 053 || _Opcode_ == 077 || (_Opcode_ == 022 && _Rs_ >= 020 && (_Funct_ == 070 || _Funct_ == 071)))
		{
			/* If we started a micro, we'll need to finish it before the first COP2 instruction. */
			needs_vu0_sync = 1;
			needs_vu0_finish = 1;
			inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS;
			continue;
		}

		/* LQC2/SQC2 - these don't interlock with VU0, but still sync, so we can persist the cached registers */
		/* for a LQC2..COP2 sequence. If there's no COP2 instructions following, don't bother, just yolo it. */
		/* We do either a sync or a finish here depending on which COP2 instruction follows - we don't want */
		/* to run the program until end if there's nothing which would actually trigger that. */

		/* In essence, what we're doing is moving the finish from the COP2 instruction to the LQC2 in a LQC2..COP2 */
		/* chain, so that we can preserve the cached registers and not need to reload them. */

		is_lqc_sqc = (_Opcode_ == 066 || _Opcode_ == 076);
		is_non_interlocked_move = (_Opcode_ == 022 && _Rs_ < 020 && ((cpuRegs.code & 1) == 0));
		/* Moving zero to the VU registers, so likely removing a loop/lock. */
		likely_clear = _Opcode_ == 022 && _Rs_ < 020 && _Rs_ > 004 && _Rt_ == 000;
		if ((needs_vu0_sync && (is_lqc_sqc || is_non_interlocked_move)) || likely_clear)
		{
			int following_needs_finish = 0;
			u32 apc2;
			for (apc2 = apc + 4; apc2 < end; apc2 += 4)
			{
				cpuRegs.code = memRead32(apc2);
				if (_Opcode_ == 022)
				{
					/* For VCALLMS/VCALLMSR, we only sync, */
					/* because the VCALLMS in itself will finish. */
					/* Since we're paying the cost of syncing anyway, */
					/* better to be less risky. */
					if (_Rs_ >= 020 && (_Funct_ == 070 || _Funct_ == 071))
						break;

					/* Allow the finish from COP2 to be moved to the */
					/* first LQC2 of LQC2..QMTC2..COP2. */
					/* Otherwise, keep searching for a finishing COP2. */
					following_needs_finish = _Rs_ >= 020;
					if (following_needs_finish)
						break;
				}
			}
			if (following_needs_finish && !block_interlocked)
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_FINISH_VU0;
				needs_vu0_sync   = 0;
				needs_vu0_finish = 0;
			}
			else
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_SYNC_VU0;
				needs_vu0_sync   = block_interlocked || (is_non_interlocked_move && likely_clear);
				needs_vu0_finish = 1;
			}
		}
		/* Look for COP2 instructions. */
		else if (_Opcode_ == 022)
		{
			/* Set the flag on the current instruction, and clear it for the next. */
			if (_Rs_ >= 020 && needs_vu0_finish)
			{
				inst->info |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_FINISH_VU0;
				needs_vu0_finish = 0;
				needs_vu0_sync   = 0;
			}
			else if (needs_vu0_sync)
			{
				/* Starting a sync-free block! */
				inst->info     |= EEINST_COP2_FLUSH_VU0_REGISTERS | EEINST_COP2_SYNC_VU0;
				needs_vu0_sync  = block_interlocked;
			}
		}

	}
}

/* Back-Prop Function Tables - Gathering Info */
/* Note to anyone changing these: writes must go before reads. */
/* Otherwise the last use flag won't get set. */

#define recBackpropSetGPRRead(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			prev->regs[reg] = (EEINST_LIVE | EEINST_USED); \
			pinst->regs[reg] = (pinst->regs[reg] & ~EEINST_XMM) | EEINST_USED; \
			_recFillRegister(pinst, XMMTYPE_GPRREG, reg, 0); \
		} \
	} while (0)

#define recBackpropSetGPRWrite(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			prev->regs[reg] &= ~(EEINST_XMM | EEINST_LIVE | EEINST_USED); \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_USED; \
			_recFillRegister(pinst, XMMTYPE_GPRREG, reg, 1); \
		} \
	} while (0)

#define recBackpropSetGPRRead128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			prev->regs[reg] |= EEINST_LIVE | EEINST_USED | EEINST_XMM; \
			pinst->regs[reg] |= EEINST_USED | EEINST_XMM; \
			_recFillRegister(pinst, XMMTYPE_GPRREG, reg, 0); \
		} \
	} while (0)

#define recBackpropSetGPRPartialWrite128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_LIVE | EEINST_USED | EEINST_XMM; \
			prev->regs[reg] |= EEINST_USED | EEINST_XMM; \
			_recFillRegister(pinst, XMMTYPE_GPRREG, reg, 1); \
		} \
	} while (0)

#define recBackpropSetGPRWrite128(reg) \
	do \
	{ \
		if ((reg) != 0) \
		{ \
			prev->regs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
			if (!(pinst->regs[reg] & EEINST_USED)) \
				pinst->regs[reg] |= EEINST_LASTUSE; \
			pinst->regs[reg] |= EEINST_USED | EEINST_XMM; \
			_recFillRegister(pinst, XMMTYPE_GPRREG, reg, 1); \
		} \
	} while (0)

#define recBackpropSetFPURead(reg) \
	do \
	{ \
		if (!(pinst->fpuregs[reg] & EEINST_USED)) \
			pinst->fpuregs[reg] |= EEINST_LASTUSE; \
		prev->fpuregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->fpuregs[reg] |= EEINST_USED; \
		_recFillRegister(pinst, XMMTYPE_FPREG, reg, 0); \
	} while (0)

#define recBackpropSetFPUWrite(reg) \
	do \
	{ \
		prev->fpuregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->fpuregs[reg] & EEINST_USED)) \
			pinst->fpuregs[reg] |= EEINST_LASTUSE; \
		pinst->fpuregs[reg] |= EEINST_USED; \
		_recFillRegister(pinst, XMMTYPE_FPREG, reg, 1); \
	} while (0)

#define recBackpropSetVFRead(reg) \
	do \
	{ \
		if (!(pinst->vfregs[reg] & EEINST_USED)) \
			pinst->vfregs[reg] |= EEINST_LASTUSE; \
		prev->vfregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->vfregs[reg] |= EEINST_USED; \
		_recFillRegister(pinst, XMMTYPE_VFREG, reg, 0); \
	} while (0)

#define recBackpropSetVFWrite(reg) \
	do \
	{ \
		prev->vfregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->vfregs[reg] & EEINST_USED)) \
			pinst->vfregs[reg] |= EEINST_LASTUSE; \
		pinst->vfregs[reg] |= EEINST_USED; \
		_recFillRegister(pinst, XMMTYPE_VFREG, reg, 1); \
	} while (0)

#define recBackpropSetVIRead(reg) \
	if ((reg) < 16) \
	{ \
		if (!(pinst->viregs[reg] & EEINST_USED)) \
			pinst->viregs[reg] |= EEINST_LASTUSE; \
		prev->viregs[reg] |= EEINST_LIVE | EEINST_USED; \
		pinst->viregs[reg] |= EEINST_USED; \
		_recFillRegister(pinst, X86TYPE_VIREG, reg, 0); \
	}

#define recBackpropSetVIWrite(reg) \
	if ((reg) < 16) \
	{ \
		prev->viregs[reg] &= ~(EEINST_LIVE | EEINST_USED); \
		if (!(pinst->viregs[reg] & EEINST_USED)) \
			pinst->viregs[reg] |= EEINST_LASTUSE; \
		pinst->viregs[reg] |= EEINST_USED; \
		_recFillRegister(pinst, X86TYPE_VIREG, reg, 1); \
	}

static void recBackpropSPECIAL(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropREGIMM(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP0(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP1(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropCOP2(u32 code, EEINST* prev, EEINST* pinst);
static void recBackpropMMI(u32 code, EEINST* prev, EEINST* pinst);

void recBackpropBSC(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (code >> 26)
	{
		case 0:
			recBackpropSPECIAL(code, prev, pinst);
			break;
		case 1:
			recBackpropREGIMM(code, prev, pinst);
			break;
		case 3: /* jal */
			recBackpropSetGPRWrite(31);
			break;
		case 4: /* beq */
		case 5: /* bne */
		case 20: /* beql */
		case 21: /* bnel */
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 6: /* blez */
		case 7: /* bgtz */
		case 22: /* blezl */
		case 23: /* bgtzl */
			recBackpropSetGPRRead(rs);
			break;

		case 15: /* lui */
			recBackpropSetGPRWrite(rt);
			break;

		case 8: /* addi */
		case 9: /* addiu */
		case 10: /* slti */
		case 11: /* sltiu */
		case 12: /* andi */
		case 13: /* ori */
		case 14: /* xori */
		case 24: /* daddi */
		case 25: /* daddiu */
		case 32: /* lb */
		case 33: /* lh */
		case 35: /* lw */
		case 36: /* lbu */
		case 37: /* lhu */
		case 39: /* lwu */
		case 55: /* ld */
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 30: /* lq */
			recBackpropSetGPRWrite128(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 26: /* ldl */
		case 27: /* ldr */
		case 34: /* lwl */
		case 38: /* lwr */
			recBackpropSetGPRWrite(rt);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 40: /* sb */
		case 41: /* sh */
		case 42: /* swl */
		case 43: /* sw */
		case 44: /* sdl */
		case 45: /* sdr */
		case 46: /* swr */
		case 63: /* sd */
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead(rs);
			break;

		case 31: /* sq */
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead128(rs);
			break;

		case 16:
			recBackpropCOP0(code, prev, pinst);
			break;

		case 17:
			recBackpropCOP1(code, prev, pinst);
			break;

		case 18:
			recBackpropCOP2(code, prev, pinst);
			break;

		case 28:
			recBackpropMMI(code, prev, pinst);
			break;

		case 49: /* lwc1 */
		case 57: /* swc1 */
			recBackpropSetGPRRead(rs);
			recBackpropSetFPURead(rt);
			break;

		case 54: /* lqc2 */
			recBackpropSetVFWrite(rt);
			recBackpropSetGPRRead128(rs);
			break;

		case 62: /* sqc2 */
			recBackpropSetGPRRead128(rs);
			recBackpropSetVFRead(rt);
			break;

		case 47: /* cache */
			recBackpropSetGPRRead(rs);
			break;

		case 51: /* pref */
		case 2: /* j */
		default:
			break;
	}
}

void recBackpropSPECIAL(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 rd = ((code >> 11) & 0x1F);
	const u32 funct = (code & 0x3F);

	switch (funct)
	{
		case 0: /* sll */
		case 2: /* srl */
		case 3: /* sra */
		case 56: /* dsll */
		case 58: /* dsrl */
		case 59: /* dsra */
		case 60: /* dsll32 */
		case 62: /* dsrl32 */
		case 63: /* dsra32 */
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rt);
			break;

		case 4: /* sllv */
		case 6: /* srlv */
		case 7: /* srav */
		case 10: /* movz */
		case 11: /* movn */
		case 20: /* dsllv */
		case 22: /* dsrlv */
		case 23: /* dsrav */
		case 32: /* add */
		case 33: /* addu */
		case 34: /* sub */
		case 35: /* subu */
		case 36: /* and */
		case 37: /* or */
		case 38: /* xor */
		case 39: /* nor */
		case 42: /* slt */
		case 43: /* sltu */
		case 44: /* dadd */
		case 45: /* daddu */
		case 46: /* dsub */
		case 47: /* dsubu */
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 8: /* jr */
			recBackpropSetGPRRead(rs);
			break;

		case 9: /* jalr */
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(rs);
			break;

		case 24: /* mult */
		case 25: /* multu */
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 26: /* div */
		case 27: /* divu */
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 16: /* mfhi */
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(XMMGPR_HI);
			break;

		case 17: /* mthi */
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			break;

		case 18: /* mflo */
			recBackpropSetGPRWrite(rd);
			recBackpropSetGPRRead(XMMGPR_LO);
			break;

		case 19: /* mtlo */
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRRead(rs);
			break;

		case 40: /* mfsa */
			recBackpropSetGPRWrite(rd);
			break;

		case 41: /* mtsa */
		case 48: /* tge */
		case 49: /* tgeu */
		case 50: /* tlt */
		case 51: /* tltu */
		case 52: /* teq */
		case 54: /* tne */
			recBackpropSetGPRRead(rs);
			break;

		case 12: /* syscall */
		case 13: /* break */
			_recClearInst(prev);
			prev->info = 0;
			break;

		case 15: /* sync */
		default:
			break;
	}
}

void recBackpropREGIMM(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (rt)
	{
		case 0: /* bltz */
		case 1: /* bgez */
		case 2: /* bltzl */
		case 3: /* bgezl */
		case 9: /* tgei */
		case 10: /* tgeiu */
		case 11: /* tlti */
		case 12: /* tltiu */
		case 13: /* teqi */
		case 15: /* tnei */
		case 24: /* mtsab */
		case 25: /* mtsah */
		case 16: /* bltzal */
		case 17: /* bgezal */
		case 18: /* bltzall */
		case 19: /* bgezall */
			/* do not write 31 */
			recBackpropSetGPRRead(rs);
			break;

		default:
			break;
	}
}

void recBackpropCOP0(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);

	switch (rs)
	{
		case 0: /* mfc0 */
		case 2: /* cfc0 */
			recBackpropSetGPRWrite(rt);
			break;

		case 4: /* mtc0 */
		case 6: /* ctc0 */
			recBackpropSetGPRRead(rt);
			break;

		case 8: /* bc0f/bc0t/bc0fl/bc0tl */
		case 16: /* tlb/eret/ei/di */
		default:
			break;
	}
}

void recBackpropCOP1(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 fmt = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 fs = ((code >> 11) & 0x1F);
	const u32 ft = ((code >> 16) & 0x1F);
	const u32 fd = ((code >> 6) & 0x1F);
	const u32 funct = (code & 0x3F);

	switch (fmt)
	{
		case 0: /* mfc1 */
			recBackpropSetGPRWrite(rt);
			recBackpropSetFPURead(fs);
			break;

		case 2: /* cfc1 */
			recBackpropSetGPRWrite(rt);
			/* read fprc[31] or fprc[0] */
			break;

		case 4: /* mtc1 */
			recBackpropSetFPUWrite(fs);
			recBackpropSetGPRRead(rt);
			break;

		case 6: /* ctc1 */
			recBackpropSetGPRRead(rt);
			/* write fprc[fs] */
			break;

		case 8: /* bc1f/bc1t/bc1fl/bc1tl */
			/* read fprc[31] */
			break;

		case 16: /* cop1.s */
		{
			switch (funct)
			{
				case 0: /* add.s */
				case 1: /* sub.s */
				case 2: /* mul.s */
				case 3: /* div.s */
				case 40: /* max.s */
				case 41: /* min.s */
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				case 5: /* abs.s */
				case 6: /* mov.s */
				case 7: /* neg.s */
				case 36: /* cvt.w */
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					break;

				case 24: /* adda.s */
				case 25: /* suba.s */
				case 26: /* mula.s */
					recBackpropSetFPUWrite(XMMFPU_ACC);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					break;

				case 28: /* madd.s */
				case 29: /* msub.s */
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					recBackpropSetFPURead(XMMFPU_ACC);
					break;

				case 30: /* madda.s */
				case 31: /* msuba.s */
					recBackpropSetFPUWrite(XMMFPU_ACC);
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					recBackpropSetFPURead(XMMFPU_ACC);
					break;

				case 4: /* sqrt.s */
				case 22: /* rsqrt.s */
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(ft);
					break;

				case 50: /* c.eq */
				case 52: /* c.lt */
				case 54: /* c.le */
					recBackpropSetFPURead(fs);
					recBackpropSetFPURead(ft);
					/* read + write fprc */
					break;
				case 48: /* c.f */
					/* read + write fprc */
				default:
					break;
			}
		}
		break;

		case 20: /* cop1.w */
		{
			switch (funct)
			{
				case 32: /* cvt.s */
					recBackpropSetFPUWrite(fd);
					recBackpropSetFPURead(fs);
					break;

				default:
					break;
			}
		}
		break;

		default:
			break;
	}
}

void recBackpropCOP2(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 fmt = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 fs = ((code >> 11) & 0x1F);
	const u32 ft = ((code >> 16) & 0x1F);
	const u32 fd = ((code >> 6) & 0x1F);
	const u32 funct = (code & 0x3F);

	const u32 VF_ACC = 32;
	const u32 VF_I = 33;

	switch (fmt)
	{
		case 1: /* qmfc2 */
			recBackpropSetGPRWrite128(rt);
			recBackpropSetVFRead(fs);
			break;

		case 2: /* cfc1 */
			recBackpropSetGPRWrite(rt);
			recBackpropSetVIRead(fs);
			break;

		case 5: /* qmtc2 */
			recBackpropSetVFWrite(fs);
			recBackpropSetGPRRead128(rt);
			break;

		case 6: /* ctc2 */
			recBackpropSetVIWrite(fs);
			recBackpropSetGPRRead(rt);
			break;

		case 8: /* bc2f/bc2t/bc2fl/bc2tl */
			/* read vi[29] */
			break;

		case 16: /* SPEC1 */
		case 17:
		case 18:
		case 19:
		case 20:
		case 21:
		case 22:
		case 23:
		case 24:
		case 25:
		case 26:
		case 27:
		case 28:
		case 29:
		case 30:
		case 31: /* SPEC1 */
		{
			switch (funct)
			{
				case 0: /* VADDx */
				case 1: /* VADDy */
				case 2: /* VADDz */
				case 3: /* VADDw */
				case 4: /* VSUBx */
				case 5: /* VSUBy */
				case 6: /* VSUBz */
				case 7: /* VSUBw */
				case 16: /* VMAXx */
				case 17: /* VMAXy */
				case 18: /* VMAXz */
				case 19: /* VMAXw */
				case 20: /* VMINIx */
				case 21: /* VMINIy */
				case 22: /* VMINIz */
				case 23: /* VMINIw */
				case 24: /* VMULx */
				case 25: /* VMULy */
				case 26: /* VMULz */
				case 27: /* VMULw */
				case 40: /* VADD */
				case 42: /* VMUL */
				case 43: /* VMAX */
				case 44: /* VSUB */
				case 47: /* VMINI */
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(ft);
					recBackpropSetVFRead(fd); /* unnecessary if _X_Y_Z_W == 0xF */
					break;

				case 8: /* VMADDx */
				case 9: /* VMADDy */
				case 10: /* VMADDz */
				case 11: /* VMADDw */
				case 12: /* VMSUBx */
				case 13: /* VMSUBy */
				case 14: /* VMSUBz */
				case 15: /* VMSUBw */
				case 41: /* VMADD */
				case 45: /* VMSUB */
				case 46: /* VOPMSUB */
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(ft);
					recBackpropSetVFRead(VF_ACC);
					recBackpropSetVFRead(fd);
					break;

				case 29: /* VMAXi */
				case 30: /* VMULi */
				case 31: /* VMINIi */
				case 34: /* VADDi */
				case 38: /* VSUBi */
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_I);
					break;

				case 35: /* VMADDi */
				case 39: /* VMSUBi */
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_ACC);
					recBackpropSetVFRead(VF_I);
					break;

				case 28: /* VMULq */
				case 32: /* VADDq */
				case 36: /* VSUBq */
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					break;

				case 33: /* VMADDq */
				case 37: /* VMSUBq */
					recBackpropSetVFWrite(fd);
					recBackpropSetVFRead(fs);
					recBackpropSetVFRead(VF_ACC);
					break;

				case 48: /* VIADD */
				case 49: /* VISUB */
				case 50: /* VIADDI */
				case 52: /* VIAND */
				case 53: /* VIOR */
				{
					const u32 is = fs & 0xFu;
					const u32 it = ft & 0xFu;
					const u32 id = fd & 0xFu;
					recBackpropSetVIWrite(id);
					recBackpropSetVIRead(is);
					recBackpropSetVIRead(it);
					recBackpropSetVIRead(id);
				}
				break;

				case 56: /* VCALLMS */
				case 57: /* VCALLMSR */
					break;

				case 60: /* COP2_SPEC2 */
				case 61: /* COP2_SPEC2 */
				case 62: /* COP2_SPEC2 */
				case 63: /* COP2_SPEC2 */
				{
					const u32 idx = (code & 3u) | ((code >> 4) & 0x7cu);
					switch (idx)
					{
						case 0: /* VADDAx */
						case 1: /* VADDAy */
						case 2: /* VADDAz */
						case 3: /* VADDAw */
						case 4: /* VSUBAx */
						case 5: /* VSUBAy */
						case 6: /* VSUBAz */
						case 7: /* VSUBAw */
						case 24: /* VMULAx */
						case 25: /* VMULAy */
						case 26: /* VMULAz */
						case 27: /* VMULAw */
						case 40: /* VADDA */
						case 42: /* VMULA */
						case 44: /* VSUBA */
						case 8: /* VMADDAx */
						case 9: /* VMADDAy */
						case 10: /* VMADDAz */
						case 11: /* VMADDAw */
						case 12: /* VMSUBAx */
						case 13: /* VMSUBAy */
						case 14: /* VMSUBAz */
						case 15: /* VMSUBAw */
						case 41: /* VMADDA */
						case 45: /* VMSUBA */
						case 46: /* VOPMULA */
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 16: /* VITOF0 */
						case 17: /* VITOF4 */
						case 18: /* VITOF12 */
						case 19: /* VITOF15 */
						case 20: /* VFTOI0 */
						case 21: /* VFTOI4 */
						case 22: /* VFTOI12 */
						case 23: /* VFTOI15 */
						case 29: /* VABS */
						case 48: /* VMOVE */
						case 49: /* VMR32 */
							recBackpropSetVFWrite(ft);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							break;

						case 31: /* VCLIP */
							recBackpropSetVFRead(fs);
							/* Write CLIP */
							break;

						case 30: /* VMULAi */
						case 34: /* VADDAi */
						case 38: /* VSUBAi */
						case 35: /* VMADDAi */
						case 39: /* VMSUBAi */
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_I);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 32: /* VADDAq */
						case 36: /* VSUBAq */
						case 28: /* VMULAq */
						case 33: /* VMADDAq */
						case 37: /* VMSUBAq */
							recBackpropSetVFWrite(VF_ACC);
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(VF_ACC);
							break;

						case 52: /* VLQI */
						case 54: /* VLQD */
							recBackpropSetVFWrite(ft);
							recBackpropSetVIWrite(fs & 0xFu);
							recBackpropSetVIRead(fs & 0xFu);
							recBackpropSetVFRead(ft);
							break;

						case 53: /* VSQI */
						case 55: /* VSQD */
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVIRead(ft & 0xFu);
							recBackpropSetVFRead(fs);
							break;

						case 56: /* VDIV */
						case 58: /* VRSQRT */
							/* recBackpropSetVIWrite(REG_Q); */
							recBackpropSetVFRead(fs);
							recBackpropSetVFRead(ft);
							break;

						case 57: /* VSQRT */
							recBackpropSetVFRead(ft);
							break;

						case 60: /* VMTIR */
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVFRead(fs);
							break;

						case 61: /* VMFIR */
							recBackpropSetVFWrite(ft);
							recBackpropSetVIRead(fs & 0xFu);
							break;

						case 62: /* VILWR */
							recBackpropSetVIWrite(ft & 0xFu);
							recBackpropSetVIRead(fs & 0xFu);
							break;

						case 63: /* VISWR */
							recBackpropSetVIRead(fs & 0xFu);
							recBackpropSetVIRead(ft & 0xFu);
							break;

						case 64: /* VRNEXT */
						case 65: /* VRGET */
							recBackpropSetVFWrite(ft);
							break;

						case 66: /* VRINIT */
						case 67: /* VRXOR */
							recBackpropSetVFRead(fs);
							break;

						case 47: /* VNOP */
						case 59: /* VWAITQ */
						default:
							break;
					}
				}
				break;

				default:
					break;
			}
		}
		break;

		default:
			break;
	}
}

void recBackpropMMI(u32 code, EEINST* prev, EEINST* pinst)
{
	const u32 funct = (code & 0x3F);
	const u32 rs = ((code >> 21) & 0x1F);
	const u32 rt = ((code >> 16) & 0x1F);
	const u32 rd = ((code >> 11) & 0x1F);

	switch (funct)
	{
		case 0: /* madd */
		case 1: /* maddu */
			recBackpropSetGPRWrite(XMMGPR_LO);
			recBackpropSetGPRWrite(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead(XMMGPR_LO);
			recBackpropSetGPRRead(XMMGPR_HI);
			break;

		case 32: /* madd1 */
		case 33: /* maddu1 */
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRRead128(XMMGPR_HI);
			break;

		case 24: /* mult1 */
		case 25: /* multu1 */
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			recBackpropSetGPRWrite(rd);
			break;

		case 26: /* div1 */
		case 27: /* divu1 */
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRRead(rt);
			break;

		case 16: /* mfhi1 */
			recBackpropSetGPRRead128(XMMGPR_HI);
			recBackpropSetGPRWrite(rd);
			break;

		case 17: /* mthi1 */
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead(rs);
			break;

		case 18: /* mflo1 */
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRWrite(rd);
			break;

		case 19: /* mtlo1 */
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRRead(rs);
			break;

		case 4: /* plzcw */
			recBackpropSetGPRRead(rs);
			recBackpropSetGPRWrite(rd);
			break;

		case 48: /* pmfhl */
			recBackpropSetGPRPartialWrite128(rd);
			recBackpropSetGPRRead128(XMMGPR_LO);
			recBackpropSetGPRRead128(XMMGPR_HI);
			break;

		case 49: /* pmthl */
			recBackpropSetGPRPartialWrite128(XMMGPR_LO);
			recBackpropSetGPRPartialWrite128(XMMGPR_HI);
			recBackpropSetGPRRead128(rs);
			break;

		case 52: /* psllh */
		case 54: /* psrlh */
		case 55: /* psrah */
		case 60: /* psllw */
		case 62: /* psrlw */
		case 63: /* psraw */
			recBackpropSetGPRWrite128(rd);
			recBackpropSetGPRRead128(rt);
			break;

		case 8: /* mmi0 */
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0: /* PADDW */
				case 1: /* PSUBW */
				case 2: /* PCGTW */
				case 3: /* PMAXW */
				case 4: /* PADDH */
				case 5: /* PSUBH */
				case 6: /* PCGTH */
				case 7: /* PMAXH */
				case 8: /* PADDB */
				case 9: /* PSUBB */
				case 10: /* PCGTB */
				case 16: /* PADDSW */
				case 17: /* PSUBSW */
				case 18: /* PEXTLW */
				case 19: /* PPACW */
				case 20: /* PADDSH */
				case 21: /* PSUBSH */
				case 22: /* PEXTLH */
				case 23: /* PPACH */
				case 24: /* PADDSB */
				case 25: /* PSUBSB */
				case 26: /* PEXTLB */
				case 27: /* PPACB */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 30: /* PEXT5 */
				case 31: /* PPAC5 */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					break;
			}
		}
		break;

		case 40: /* mmi1 */
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 2: /* PCEQW */
				case 3: /* PMINW */
				case 4: /* PADSBH */
				case 6: /* PCEQH */
				case 7: /* PMINH */
				case 10: /* PCEQB */
				case 16: /* PADDUW */
				case 17: /* PSUBUW */
				case 18: /* PEXTUW */
				case 20: /* PADDUH */
				case 21: /* PSUBUH */
				case 22: /* PEXTUH */
				case 24: /* PADDUB */
				case 25: /* PSUBUB */
				case 26: /* PEXTUB */
				case 27: /* QFSRV */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 1: /* PABSW */
				case 5: /* PABSH */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				case 0: /* MMI_Unknown */
				default:
					break;
			}
		}
		break;

		case 9: /* mmi2 */
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0: /* PMADDW */
				case 4: /* PMSUBW */
				case 16: /* PMADDH */
				case 17: /* PHMADH */
				case 20: /* PMSUBH */
				case 21: /* PHMSBH */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					recBackpropSetGPRRead128(XMMGPR_LO);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 12: /* PMULTW */
				case 28: /* PMULTH */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 13: /* PDIVW */
				case 29: /* PDIVBW */
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 2: /* PSLLVW */
				case 3: /* PSRLVW */
				case 10: /* PINTH */
				case 14: /* PCPYLD */
				case 18: /* PAND */
				case 19: /* PXOR */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 8: /* PMFHI */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(XMMGPR_LO);
					break;

				case 9: /* PMFLO */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 26: /* PEXEH */
				case 27: /* PREVH */
				case 30: /* PEXEW */
				case 31: /* PROT3W */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					break;
			}
		}
		break;

		case 41: /* mmi3 */
		{
			const u32 idx = ((code >> 6) & 0x1F);
			switch (idx)
			{
				case 0: /* PMADDUW */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					recBackpropSetGPRRead128(XMMGPR_LO);
					recBackpropSetGPRRead128(XMMGPR_HI);
					break;

				case 3: /* PSRAVW */
				case 10: /* PINTEH */
				case 18: /* POR */
				case 19: /* PNOR */
				case 14: /* PCPYUD */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 26: /* PEXCH */
				case 27: /* PCPYH */
				case 30: /* PEXCW */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRRead128(rt);
					break;

				case 8: /* PMTHI */
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					break;

				case 9: /* PMTLO */
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRRead128(rs);
					break;

				case 12: /* PMULTUW */
					recBackpropSetGPRWrite128(rd);
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				case 13: /* PDIVUW */
					recBackpropSetGPRWrite128(XMMGPR_LO);
					recBackpropSetGPRWrite128(XMMGPR_HI);
					recBackpropSetGPRRead128(rs);
					recBackpropSetGPRRead128(rt);
					break;

				default:
					break;
			}
		}
		break;

		default:
			break;
	}
}
