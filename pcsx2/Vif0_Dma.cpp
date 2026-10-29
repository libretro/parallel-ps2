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
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "x86/newVif.h"
#include "Config.h"

u32 g_vif0Cycles = 0;

// Run VU0 until finish, don't add cycles to EE
// because its vif stalling not the EE core...
void vif0FLUSH(void)
{
	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x5) // T bit stop or Busy
	{
		vif0.waitforvu = true;
		vif0.vifstalled.enabled = VifStallEnable(vif0ch);
		vif0.vifstalled.value = VIF_TIMING_BREAK;
		vif0Regs.stat.VEW = true;
	}
	return;
}

bool _VIF0chain(void)
{
	u32 *pMem;

	if (vif0ch.qwc == 0)
	{
		vif0.inprogress = 0;
		return true;
	}

	pMem = (u32*)dmaGetAddr(vif0ch.madr, false);
	if (pMem == NULL)
	{
		vif0.cmd = 0;
		vif0.tag.size = 0;
		vif0ch.qwc = 0;
		return true;
	}

	if (vif0.irqoffset.enabled)
		return VIF0transfer(pMem + vif0.irqoffset.value, vif0ch.qwc * 4 - vif0.irqoffset.value, false);
	return VIF0transfer(pMem, vif0ch.qwc * 4, false);
}

static __fi void vif0SetupTransfer(void)
{
	tDMA_TAG *ptag = dmaGetAddr(vif0ch.tadr, false); //Set memory pointer to TADR

	if (!(vif0ch.transfer(ptag))) return;

	vif0ch.madr = ptag[1]._u32;            //MADR = ADDR field + SPR
	g_vif0Cycles += 1; // Add 1 g_vifCycles from the QW read for the tag

	// Transfer dma tag if tte is set
	vif0.inprogress = 0;

	if (vif0ch.chcr.TTE)
	{
		// Transfer dma tag if tte is set

		bool ret;

		alignas(16) static u128 masked_tag;

		masked_tag._u64[0] = 0;
		masked_tag._u64[1] = *((u64*)ptag + 1);

		if (vif0.irqoffset.enabled)
			ret = VIF0transfer((u32*)&masked_tag + vif0.irqoffset.value, 4 - vif0.irqoffset.value, true);  //Transfer Tag on stall
		else
		{
			// Some games (like killzone) do Tags mid unpack, the nops will just write blank data
			// to the VU's, which breaks stuff, this is where the 128bit packet will fail, so we ignore the first 2 words
			vif0.irqoffset.value = 2;
			vif0.irqoffset.enabled = true;
			ret = VIF0transfer((u32*)&masked_tag + 2, 2, true);  //Transfer Tag
		}

		if (!ret && vif0.irqoffset.enabled)
		{
			vif0.inprogress = 0; // Better clear this so it has to do it again (Jak 1)
			vif0ch.qwc = 0; // Gumball 3000 pauses the DMA when the tag stalls so we need to reset the QWC, it'll be gotten again later
			return;        // IRQ set by VIFTransfer

		}
	}

	vif0.irqoffset.value = 0;
	vif0.irqoffset.enabled = false;
	vif0.done |= hwDmacSrcChainWithStack(&vif0ch, ptag->ID);

	if(vif0ch.qwc > 0) vif0.inprogress = 1;
	//Check TIE bit of CHCR and IRQ bit of tag
	if (vif0ch.chcr.TIE && ptag->IRQ)
	{
		//End Transfer
		vif0.done = true;
		return;
	}
}

static __fi void vif0Interrupt_inl(void);

void vif0VUFinish(void)
{
	// Sync up VU0 so we don't errantly wait.
	while (vuRegs[0].VI[REG_VPU_STAT].UL & 0x1)
	{
		const s64 cycle_diff = static_cast<int>(cpuRegs.cycle - vuRegs[0].cycle);

		if ((EmuConfig.Gamefixes.VUSyncHack && cycle_diff < vuRegs[0].nextBlockCycles) || cycle_diff <= 0)
			break;
		vucpu_execute_block(CpuVU0, 0);
	}

	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x5)
	{
		CPU_INT(VIF_VU0_FINISH, 128);
		cpuRegs.dmastall |= 1 << VIF_VU0_FINISH;
		return;
	}

	if ((vuRegs[0].VI[REG_VPU_STAT].UL & 1))
	{
		s64 _cycles = vuRegs[0].cycle;
		vu0Finish();
		_cycles = vuRegs[0].cycle - _cycles;
		CPU_INT(VIF_VU0_FINISH, _cycles * BIAS);
		cpuRegs.dmastall |= 1 << VIF_VU0_FINISH;
		return;
	}
	vif0Regs.stat.VEW = false;
	if(vif0.waitforvu)
	{
		vif0.waitforvu = false;
		//Make sure VIF0 isnt already scheduled to spin.
		if(!(cpuRegs.interrupt & 0x1) && vif0ch.chcr.STR && !VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
			vif0Interrupt_inl();
	}
}

static __fi void vif0Interrupt_inl(void)
{
	g_vif0Cycles = 0;

	vif0Regs.stat.FQC = pcsx2_min_u(vif0ch.qwc, (u32)8);

	if(vif0.waitforvu)
	{
		CPU_INT(VIF_VU0_FINISH, 16);
		cpuRegs.dmastall |= 1 << DMAC_VIF0;
		return;
	}

	if (vif0.irq && vif0.vifstalled.enabled && vif0.vifstalled.value == VIF_IRQ_STALL)
	{
		if (!vif0Regs.stat.ER1)
			vif0Regs.stat.INT = true;

		//Yakuza watches VIF_STAT so lets do this here.
		if (((vif0Regs.code >> 24) & 0x7f) != 0x7) {
			vif0Regs.stat.VIS = true;
		}

		hwIntcIrq(VIF0intc);
		--vif0.irq;

		if (VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
		{
			//vif0Regs.stat.FQC = 0;

			// One game doesn't like vif stalling at end, can't remember what. Spiderman isn't keen on it tho
			//vif0ch.chcr.STR = false;
			vif0Regs.stat.FQC = pcsx2_min_u((u32)0x8, vif0ch.qwc);
			if (vif0ch.qwc > 0 || !vif0.done)
			{
				vif0Regs.stat.VPS = VPS_DECODING; //If there's more data you need to say it's decoding the next VIF CMD (Onimusha - Blade Warriors)
				cpuRegs.dmastall |= 1 << DMAC_VIF0;
				return;
			}
		}
	}

	vif0.vifstalled.enabled = false;

	//Must go after the Stall, incase it's still in progress, GTC africa likes to see it still transferring.
	if (vif0.cmd)
	{
		if(vif0.done && vif0ch.qwc == 0)	vif0Regs.stat.VPS = VPS_WAITING;
	}
	else
	{
		vif0Regs.stat.VPS = VPS_IDLE;
	}

	if (vif0.inprogress & 0x1)
	{
		_VIF0chain();
		vif0Regs.stat.FQC = pcsx2_min_u(vif0ch.qwc, (u32)8);
		CPU_INT(DMAC_VIF0, g_vif0Cycles);
		return;
	}

	if (!vif0.done)
	{

		if (!(dmacRegs.ctrl.DMAE) || vif0Regs.stat.VSS) //Stopped or DMA Disabled
			return;

		if ((vif0.inprogress & 0x1) == 0) vif0SetupTransfer();
		vif0Regs.stat.FQC = pcsx2_min_u(vif0ch.qwc, (u32)8);
		CPU_INT(DMAC_VIF0, g_vif0Cycles);
		return;
	}

	if (vif0.vifstalled.enabled && vif0.done)
	{
		CPU_INT(DMAC_VIF0, 0);
		return; //Dont want to end if vif is stalled.
	}

	vif0ch.chcr.STR = false;
	vif0Regs.stat.FQC = pcsx2_min_u((u32)0x8, vif0ch.qwc);
	vif0.vifstalled.enabled = false;
	vif0.irqoffset.enabled = false;
	if(vif0.queued_program) vifExecQueue(0);
	g_vif0Cycles = 0;
	hwDmacIrq(DMAC_VIF0);
	cpuRegs.dmastall &= ~(1 << DMAC_VIF0);
	vif0Regs.stat.FQC = 0;
}

void vif0Interrupt(void)
{
	vif0Interrupt_inl();
}

void dmaVIF0(void)
{
	g_vif0Cycles = 0;
	cpuRegs.dmastall &= ~(1 << DMAC_VIF0);

	if (vif0ch.qwc > 0)   // Normal Mode
	{
		if (vif0ch.chcr.MOD == CHAIN_MODE)
		{
			tDMA_TAG tmp;
			tmp._u32     = vif0ch.chcr._u32;
			vif0.dmamode = VIF_CHAIN_MODE;

			if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && vif0ch.chcr.TIE))
				vif0.done = true;
			else
				vif0.done = false;
		}
		else //Assume Normal mode.
		{
			vif0.dmamode = VIF_NORMAL_FROM_MEM_MODE;

			vif0.done = true;
		}

		vif0.inprogress |= 1;
	}
	else
	{
		vif0.dmamode = VIF_CHAIN_MODE;
		vif0.done = false;
		vif0.inprogress &= ~0x1;
	}

	vif0Regs.stat.FQC = pcsx2_min_u((u32)0x8, vif0ch.qwc);

	//Using a delay as Beyond Good and Evil does the DMA twice with 2 different TADR's (no checks in the middle, all one block of code),
	//the first bit it sends isnt required for it to work.
	//Also being an end chain it ignores the second lot, this causes infinite loops ;p
	if (!VIF_TEST(vif0Regs.stat, VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
		CPU_INT(DMAC_VIF0, 4);
}
