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

#include <string.h>

#include "Common.h"
#include "common/Pcsx2Defs.h"

#include "VUmicro.h"
#include "ps2/HwInternal.h"

static void _vu1ExecUpper(VURegs* VU, u32* ptr)
{
	VU->code = ptr[1];
	VU1_UPPER_OPCODE[VU->code & 0x3f]();
}

static void _vu1ExecLower(VURegs* VU, u32* ptr)
{
	VU->code = ptr[0];
	VU1_LOWER_OPCODE[VU->code >> 25]();
}

static void _vu1Exec(VURegs* VU)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	u64 cyclesBeforeOp;
	u32* ptr;

	ptr = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL += 8;

	if (ptr[1] & 0x40000000) /* E flag */
	{
		VU->ebit = 2;
	}
	if (ptr[1] & 0x10000000) /* D flag */
	{
		if (vuRegs[0].VI[REG_FBRST].UL & 0x400)
		{
			vuRegs[0].VI[REG_VPU_STAT].UL |= 0x200;
			hwIntcIrq(INTC_VU1);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) /* T flag */
	{
		if (vuRegs[0].VI[REG_FBRST].UL & 0x800)
		{
			vuRegs[0].VI[REG_VPU_STAT].UL |= 0x400;
			hwIntcIrq(INTC_VU1);
			VU->ebit = 1;
		}
	}

	VU->code = ptr[1];
	VU1regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);

	cyclesBeforeOp = vuRegs[1].cycle - 1;

	_vuTestUpperStalls(VU, &uregs);

	/* check upper flags */
	if (ptr[1] & 0x80000000) /* I Flag (Lower op is a float) */
	{
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= pcsx2_min_i((u8)(vuRegs[1].cycle - cyclesBeforeOp), VU->VIBackupCycles);

		_vu1ExecUpper(VU, ptr);

		VU->VI[REG_I].UL = ptr[0];
		/* Lower not used, set to 0 to fill in the FMAC stall gap */
		/* Could probably get away with just running upper stalls, but lets not tempt fate. */
		memset(&lregs, 0, sizeof(lregs));
	}
	else
	{
		VECTOR _VF;
		VECTOR _VFc;
		REG_VI _VI;
		REG_VI _VIc;
		int vfreg = 0;
		int vireg = 0;
		int discard = 0;

		VU->code = ptr[0];
		lregs.cycles = 0;
		VU1regs_LOWER_OPCODE[VU->code >> 25](&lregs);

		_vuTestLowerStalls(VU, &lregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles-= pcsx2_min_i((u8)(vuRegs[1].cycle- cyclesBeforeOp), VU->VIBackupCycles);

		if (uregs.VFwrite)
		{
			if (lregs.VFwrite == uregs.VFwrite)
			{
				/* log_cb(RETRO_LOG_WARN, "*PCSX2*: Warning, VF write to the same reg in both lower/upper cycle pc=%x\n", VU->VI[REG_TPC].UL); */
				discard = 1;
			}
			if (lregs.VFread0 == uregs.VFwrite ||
				lregs.VFread1 == uregs.VFwrite)
			{
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIwrite & (1 << REG_CLIP_FLAG))
		{
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
			{
				/* log_cb(RETRO_LOG_WARN, "*PCSX2*: Warning, VI write to the same reg in both lower/upper cyclepc=%x\n", VU->VI[REG_TPC].UL); */
				discard = 1;
			}
			if (lregs.VIread & (1 << REG_CLIP_FLAG))
			{
				/* log_cb(RETRO_LOG_WARN, "*PCSX2*: Warning, VI read same cycle as write pc=%x\n", VU->VI[REG_TPC].UL); */
				_VI = VU->VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		_vu1ExecUpper(VU, ptr);

		if (discard == 0)
		{
			if (vfreg)
			{
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg)
			{
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			_vu1ExecLower(VU, ptr);

			if (vfreg)
			{
				VU->VF[vfreg] = _VFc;
			}
			if (vireg)
			{
				VU->VI[vireg] = _VIc;
			}
		}
	}
	/* Clear an FMAC read for use */
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		_vuClearFMAC(VU);

	_vuAddUpperStalls(VU, &uregs);
	_vuAddLowerStalls(VU, &lregs);

	if (VU->branch > 0)
	{
		if (VU->branch-- == 1)
		{
			VU->VI[REG_TPC].UL = VU->branchpc;

			if (VU->takedelaybranch)
			{
				VU->branch = 1;
				VU->branchpc = VU->delaybranchpc;
				VU->takedelaybranch = false;
			}
		}
	}

	if (VU->ebit > 0)
	{
		if (VU->ebit-- == 1)
		{
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			vuRegs[0].VI[REG_VPU_STAT].UL &= ~0x100;
			vif1Regs.stat.VEW = false;

			if(vuRegs[1].xgkickenable)
				_vuXGKICKTransfer(0, true);
			/* In instant VU mode, VU1 goes WAY ahead of the CPU, */
			/* making the XGKick fall way behind */
			/* We also have some code to update it in VIF Unpacks too, */
			/* since in some games (Aggressive Inline) overwrite the XGKick data */
			/* VU currently flushes XGKICK on end, so this isn't needed, yet */
			if (emu_config_instant_vu1())
				vuRegs[1].xgkicklastcycle = cpuRegs.cycle;
		}
	}

	/* Progress the write position of the FMAC pipeline by one place */
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;
}

void vu1Exec(VURegs* VU)
{
	VU->cycle++;
	_vu1Exec(VU);
}

static void interp_vu1_reset(void)
{
	vuRegs[1].fmacwritepos = 0;
	vuRegs[1].fmacreadpos = 0;
	vuRegs[1].fmaccount = 0;
	vuRegs[1].ialuwritepos = 0;
	vuRegs[1].ialureadpos = 0;
	vuRegs[1].ialucount = 0;
}

static void interp_vu1_set_start_pc(u32 startPC)
{
	vuRegs[1].start_pc = startPC;
}

static void interp_vu1_execute(u32 cycles)
{
	const u64 fpcr = emu_fpcr_set_vu(1);
	u64 startcycles;

	vuRegs[1].VI[REG_TPC].UL <<= 3;
	startcycles = vuRegs[1].cycle;

	while ((vuRegs[1].cycle - startcycles) < cycles)
	{
		if (!(vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
		{
			if (vuRegs[1].branch == 1)
			{
				vuRegs[1].VI[REG_TPC].UL = vuRegs[1].branchpc;
				vuRegs[1].branch = 0;
			}
			break;
		}
		vuRegs[1].VI[REG_TPC].UL &= VU1_PROGMASK;
		vu1Exec(&vuRegs[1]);
	}
	vuRegs[1].VI[REG_TPC].UL >>= 3;
	vuRegs[1].nextBlockCycles = (vuRegs[1].cycle - cpuRegs.cycle) + 1;
	emu_fpcr_restore(fpcr);
}

static void interp_vu1_shutdown(void) {}
static void interp_vu1_clear(u32 addr, u32 size) { (void)addr; (void)size; }

const struct VUmicroCpu vucpu_interp_vu1 =
{
	1,                          /* idx */
	1,                          /* is_interpreter */
	interp_vu1_shutdown,
	interp_vu1_reset,
	interp_vu1_set_start_pc,
	interp_vu1_execute,
	interp_vu1_clear,
	NULL                        /* ResumeXGkick */
};
