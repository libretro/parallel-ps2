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
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "Vif_Dma.h"
#include "x86/newVif.h"

u32 g_vif1Cycles = 0;

__fi void vif1FLUSH(void)
{
	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x500) // T bit stop or Busy
	{
		vif1.waitforvu = true;
		vif1.vifstalled.enabled = VifStallEnable(vif1ch);
		vif1.vifstalled.value = VIF_TIMING_BREAK;
		vif1Regs.stat.VEW = true;
	}
}

void vif1TransferToMemory(void)
{
	// Reverse VIF1 is a download: the GS writes into this buffer, so the
	// address has to be resolved for writing. dmaGetAddr hands back a
	// different placeholder for the two cases when madr falls outside main
	// RAM -- eeMem->ZeroWrite for writers, the read-only eeMem->ZeroRead for
	// readers -- so asking for the read one here yields a page that faults
	// the moment the readback copies into it. SPR0, the other DMA that
	// writes into EE memory, already passes true.
	u128* pMem = (u128*)dmaGetAddr(vif1ch.madr, true);

	// VIF from gsMemory
	if (pMem == NULL)
	{ // Is vif0ptag empty?
		dmacRegs.stat.BEIS = true; // Bus Error
		vif1Regs.stat.FQC = 0;

		vif1ch.qwc = 0;
		vif1.done = true;
		CPU_INT(DMAC_VIF1, 0);
		return; // An error has occurred.
	}

	// MTGS concerns:  The MTGS is inherently disagreeable with the idea of downloading
	// stuff from the GS.  The *only* way to handle this case safely is to flush the GS
	// completely and execute the transfer there-after.
	const u32 size = pcsx2_min_u(vif1.GSLastDownloadSize, (u32)vif1ch.qwc);

	// The GS writes into EE RAM here, from the MTGS consumer thread, with a
	// plain memcpy. If the recompiler has marked those pages read-only to
	// catch self-modifying code, that store faults -- and unlike a fault from
	// recompiled EE code there is no instruction behind it and no handler on
	// that thread, so it goes unhandled and takes the process down. Seen as an
	// access violation writing a COMMIT/MAPPED page whose allocation
	// protection was READWRITE and whose current protection was READONLY,
	// which is mmap_MarkCountedRamPage's doing and nothing else's.
	//
	// Cpu->Clear is not enough on its own, which cost a round to find out: it
	// invalidates the blocks but never calls MemProtect, so the page stays
	// read-only and the memcpy still faults. The protection has to come off
	// as well, which is what the fault handler's mmap_ClearCpuBlock does and
	// what mmap_UnprotectRamRange applies over a range.
	mmap_UnprotectRamRange(vif1ch.madr & 0x1ffffff0, size * 16);

	MTGS::InitAndReadFIFO(reinterpret_cast<u8*>(pMem), size);

	g_vif1Cycles += size * 2;
	vif1ch.madr += size * 16; // MGS3 scene changes
	if (vif1.GSLastDownloadSize >= vif1ch.qwc)
	{
		vif1.GSLastDownloadSize -= vif1ch.qwc;
		vif1Regs.stat.FQC = pcsx2_min_u((u32)16, vif1.GSLastDownloadSize);
		vif1ch.qwc = 0;
	}
	else
	{
		vif1Regs.stat.FQC = 0;
		vif1ch.qwc -= vif1.GSLastDownloadSize;
		vif1.GSLastDownloadSize = 0;
	}
}

bool _VIF1chain(void)
{
	u32* pMem;

	if (vif1ch.qwc == 0)
	{
		vif1.inprogress &= ~1;
		vif1.irqoffset.value = 0;
		vif1.irqoffset.enabled = false;
		return true;
	}

	// Clarification - this is TO memory mode, for some reason i used the other way round >.<
	if (vif1.dmamode == VIF_NORMAL_TO_MEM_MODE)
	{
		vif1TransferToMemory();
		vif1.inprogress &= ~1;
		return true;
	}

	pMem = (u32*)dmaGetAddr(vif1ch.madr, !vif1ch.chcr.DIR);
	if (pMem == NULL)
	{
		vif1.cmd = 0;
		vif1.tag.size = 0;
		vif1ch.qwc = 0;
		return true;
	}

	if (vif1.irqoffset.enabled)
		return VIF1transfer(pMem + vif1.irqoffset.value, vif1ch.qwc * 4 - vif1.irqoffset.value, false);
	return VIF1transfer(pMem, vif1ch.qwc * 4, false);
}

__fi void vif1SetupTransfer(void)
{
	tDMA_TAG* ptag = dmaGetAddr(vif1ch.tadr, false); //Set memory pointer to TADR

	if (!(vif1ch.transfer(ptag)))
		return;

	vif1ch.madr = ptag[1]._u32; //MADR = ADDR field + SPR
	g_vif1Cycles += 1; // Add 1 g_vifCycles from the QW read for the tag
	vif1.inprogress &= ~1;

	if (!vif1.done && ((dmacRegs.ctrl.STD == STD_VIF1) && (ptag->ID == TAG_REFS))) // STD == VIF1
	{
		// there are still bugs, need to also check if gif->madr +16*qwc >= stadr, if not, stall
		if ((vif1ch.madr + vif1ch.qwc * 16) > dmacRegs.stadr.ADDR)
		{
			// stalled
			hwDmacIrq(DMAC_STALL_SIS);
			cpuRegs.dmastall |= 1 << DMAC_VIF1;
			return;
		}
	}

	if (vif1ch.chcr.TTE)
	{
		// Transfer dma tag if tte is set

		bool ret;

		alignas(16) static u128 masked_tag;

		masked_tag._u64[0] = 0;
		masked_tag._u64[1] = *((u64*)ptag + 1);

		if (vif1.irqoffset.enabled)
		{
			ret = VIF1transfer((u32*)&masked_tag + vif1.irqoffset.value, 4 - vif1.irqoffset.value, true); //Transfer Tag on stall
		}
		else
		{
			// Some games (like killzone) do Tags mid unpack, the nops will just write blank data
			// to the VU's, which breaks stuff, this is where the 128bit packet will fail, so we ignore the first 2 words
			vif1.irqoffset.value = 2;
			vif1.irqoffset.enabled = true;
			ret = VIF1transfer((u32*)&masked_tag + 2, 2, true); //Transfer Tag
		}

		if (!ret && vif1.irqoffset.enabled)
		{
			vif1.inprogress &= ~1; // Better clear this so it has to do it again (Jak 1)
			vif1ch.qwc = 0; // Gumball 3000 pauses the DMA when the tag stalls so we need to reset the QWC, it'll be gotten again later
			return; // IRQ set by VIFTransfer
		}
	}
	vif1.irqoffset.value     = 0;
	vif1.irqoffset.enabled   = false;

	vif1.done               |= hwDmacSrcChainWithStack(&vif1ch, ptag->ID);

	if (vif1ch.qwc > 0)
		vif1.inprogress |= 1;

	//Check TIE bit of CHCR and IRQ bit of tag
	if (vif1ch.chcr.TIE && ptag->IRQ)
	{
		//End Transfer
		vif1.done = true;
		return;
	}
}

__fi void vif1VUFinish(void)
{
	// Sync up VU1 so we don't errantly wait.
	while (!THREAD_VU1 && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
	{
		const s64 cycle_diff = static_cast<int>(cpuRegs.cycle - vuRegs[1].cycle);

		if ((EmuConfig.Gamefixes.VUSyncHack && cycle_diff < vuRegs[1].nextBlockCycles) || cycle_diff <= 0)
			break;
		vucpu_execute_block(CpuVU1, 0);
	}

	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x500)
	{
		vu1Thread.Get_MTVUChanges();

		if (THREAD_VU1 && !INSTANT_VU1 && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			CPU_INT(VIF_VU1_FINISH, cpuGetCycles());
		else
			CPU_INT(VIF_VU1_FINISH, 128);
		cpuRegs.dmastall |= 1 << VIF_VU1_FINISH;
		return;
	}

	if (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100)
	{
		u64 _cycles = vuRegs[1].cycle;
		vu1Finish(false);
		if (THREAD_VU1 && !INSTANT_VU1 && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100))
			CPU_INT(VIF_VU1_FINISH, cpuGetCycles());
		else
			CPU_INT(VIF_VU1_FINISH, vuRegs[1].cycle - _cycles);
		cpuRegs.dmastall |= 1 << VIF_VU1_FINISH;
		return;
	}

	vif1Regs.stat.VEW = false;

	if (vif1.waitforvu)
	{
		vif1.waitforvu = false;
		//Check if VIF is already scheduled to interrupt, if it's waiting, kick it :P
		if ((cpuRegs.interrupt & ((1 << DMAC_VIF1) | (1 << DMAC_MFIFO_VIF))) == 0 && vif1ch.chcr.STR && !VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			if (dmacRegs.ctrl.MFD == MFD_VIF1)
				vifMFIFOInterrupt();
			else
				vif1Interrupt();
		}
	}
}

/* PATH2 has finished its packet: release the GIF and let any path
 * waiting on it run. Taken at most once per PATH2 transfer. */
static __noinline void vif1_path2_done(void)
{
	gifRegs.stat.APATH = 0;
	gifRegs.stat.OPH   = 0;
	vif1Regs.stat.VGW  = false; //Let vif continue if it's stuck on a flush

	if (gifUnit.checkPaths(1, 0, 1))
		gifUnit.Execute(false, true);
}

void vif1Interrupt(void)
{
	g_vif1Cycles = 0;

	if (gifRegs.stat.APATH == 2 && gifUnit.gifPath[GIF_PATH_2].isDone())
		vif1_path2_done();
	//Some games (Fahrenheit being one) start vif first, let it loop through blankness while it sets MFIFO mode, so we need to check it here.
	if (dmacRegs.ctrl.MFD == MFD_VIF1)
	{
		// Test changed because the Final Fantasy 12 opening somehow has the tag in *Undefined* mode, which is not in the documentation that I saw.
		vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);
		vifMFIFOInterrupt();
		return;
	}

	// We need to check the direction, if it is downloading
	// from the GS then we handle that separately (KH2 for testing)
	if (vif1ch.chcr.DIR)
	{
		bool isDirect   = (vif1.cmd & 0x7f) == 0x50;
		bool isDirectHL = (vif1.cmd & 0x7f) == 0x51;
		if ((isDirect && !gifUnit.CanDoPath2()) || (isDirectHL && !gifUnit.CanDoPath2HL()))
		{
			CPU_INT(DMAC_VIF1, 128);
			if (gifRegs.stat.APATH == 3)
				vif1Regs.stat.VGW = 1; //We're waiting for path 3. Gunslinger II
			cpuRegs.dmastall |= 1 << DMAC_VIF1;
			return;
		}
		vif1Regs.stat.VGW = 0; //Path 3 isn't busy so we don't need to wait for it.
		vif1Regs.stat.FQC = pcsx2_min_u(vif1ch.qwc, (u32)16);
		//Simulated GS transfer time done, clear the flags
	}

	if (vif1.waitforvu)
	{
		CPU_INT(VIF_VU1_FINISH, pcsx2_max_i(16, cpuGetCycles()));
		cpuRegs.dmastall |= 1 << DMAC_VIF1;
		return;
	}

	if (vif1Regs.stat.VGW)
	{
		cpuRegs.dmastall |= 1 << DMAC_VIF1;
		return;
	}

	if (!vif1ch.chcr.STR)
		return;

	if (vif1.irq && vif1.vifstalled.enabled && vif1.vifstalled.value == VIF_IRQ_STALL)
	{
		if (!vif1Regs.stat.ER1)
			vif1Regs.stat.INT = true;

		//Yakuza watches VIF_STAT so lets do this here.
		if (((vif1Regs.code >> 24) & 0x7f) != 0x7)
			vif1Regs.stat.VIS = true;

		hwIntcIrq(VIF1intc);
		--vif1.irq;

		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			//NFSHPS stalls when the whole packet has gone across (it stalls in the last 32bit cmd)
			//In this case VIF will end
			vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);
			if ((vif1ch.qwc > 0 || !vif1.done) && !CHECK_VIF1STALLHACK)
			{
				vif1Regs.stat.VPS = VPS_DECODING; //If there's more data you need to say it's decoding the next VIF CMD (Onimusha - Blade Warriors)
				cpuRegs.dmastall |= 1 << DMAC_VIF1;
				return;
			}
		}
	}

	vif1.vifstalled.enabled = false;

	//Mirroring change to VIF0
	if (vif1.cmd)
	{
		if (vif1.done && (vif1ch.qwc == 0))
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
		vif1Regs.stat.VPS = VPS_IDLE;

	if (vif1.inprogress & 0x1)
	{
		_VIF1chain();
		// VIF_NORMAL_FROM_MEM_MODE is a very slow operation.
		// Timesplitters 2 depends on this beeing a bit higher than 128.
		if (vif1ch.chcr.DIR)
			vif1Regs.stat.FQC = pcsx2_min_u(vif1ch.qwc, (u32)16);

		if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE)) //If we're waiting on GIF, stop looping, (can be over 1000 loops!)
		{
			if (vif1.waitforvu)
			{
				CPU_INT(DMAC_VIF1, pcsx2_max_i(static_cast<int>(g_vif1Cycles), cpuGetCycles()));
			}
			else
				CPU_INT(DMAC_VIF1, g_vif1Cycles);
		}
		return;
	}

	if (!vif1.done)
	{

		if (!(dmacRegs.ctrl.DMAE) || vif1Regs.stat.VSS) //Stopped or DMA Disabled
			return;

		if ((vif1.inprogress & 0x1) == 0)
			vif1SetupTransfer();
		if (vif1ch.chcr.DIR)
			vif1Regs.stat.FQC = pcsx2_min_u(vif1ch.qwc, (u32)16);

		if (!(vif1Regs.stat.VGW && gifUnit.gifPath[GIF_PATH_3].state != GIF_PATH_IDLE)) //If we're waiting on GIF, stop looping, (can be over 1000 loops!)
		{
			if (vif1.waitforvu)
			{
				CPU_INT(DMAC_VIF1, pcsx2_max_i(static_cast<int>(g_vif1Cycles), cpuGetCycles()));
			}
			else
				CPU_INT(DMAC_VIF1, g_vif1Cycles);
		}
		return;
	}

	if (vif1.vifstalled.enabled && vif1.done)
	{
		CPU_INT(DMAC_VIF1, 0);
		cpuRegs.dmastall |= 1 << DMAC_VIF1;
		return; //Dont want to end if vif is stalled.
	}

	if (vif1ch.chcr.DIR)
	{
		//Reverse fifo has finished and nothing is left, so lets clear the outputting flag
		if ((vif1ch.chcr.DIR == VIF_NORMAL_TO_MEM_MODE) && vif1.GSLastDownloadSize <= 16)
			gifRegs.stat.OPH = false;
		vif1Regs.stat.FQC = pcsx2_min_u(vif1ch.qwc, (u32)16);
	}

	vif1ch.chcr.STR          = false;
	vif1.vifstalled.enabled  = false;
	vif1.irqoffset.enabled   = false;
	if (vif1.queued_program)
		vifExecQueue(1);
	g_vif1Cycles             = 0;
	if (THREAD_VU1)
		vu1Thread.KickPending(); // C.80: DMA done -- flush any deferred unpack notify
	hwDmacIrq(DMAC_VIF1);
	cpuRegs.dmastall        &= ~(1 << DMAC_VIF1);
}

void dmaVIF1(void)
{
	g_vif1Cycles      = 0;
	vif1.inprogress   = 0;
	cpuRegs.dmastall &= ~(1 << DMAC_VIF1);

	if (vif1ch.qwc > 0) // Normal Mode
	{

		// ignore tag if it's a GS download (Def Jam Fight for NY)
		if (vif1ch.chcr.MOD == CHAIN_MODE && vif1ch.chcr.DIR)
		{
			tDMA_TAG tmp;
			tmp._u32     = vif1ch.chcr._u32;
			vif1.dmamode = VIF_CHAIN_MODE;

			if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && vif1ch.chcr.TIE))
				vif1.done = true;
			else
				vif1.done = false;
		}
		else //Assume normal mode for reverse FIFO and Normal.
		{
			if (vif1ch.chcr.DIR) // from Memory
				vif1.dmamode = VIF_NORMAL_FROM_MEM_MODE;
			else
				vif1.dmamode = VIF_NORMAL_TO_MEM_MODE;

			vif1.done = true;
		}

		vif1.inprogress |= 1;
	}
	else
	{
		vif1.inprogress &= ~0x1;
		vif1.dmamode     = VIF_CHAIN_MODE;
		vif1.done        = false;
	}

	if (vif1ch.chcr.DIR)
		vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);

	// Check VIF isn't stalled before starting the loop.
	// Batman Vengence does something stupid and instead of cancelling a stall it tries to restart VIF, THEN check the stall
	// However if VIF FIFO is reversed, it can continue
	if (!vif1ch.chcr.DIR || !VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		CPU_INT(DMAC_VIF1, 4);
}
