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
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "Gif_hw.h"
#include "vtlb.h"
#include "ps2/HwInternal.h"

static u32 qwctag(u32 mask)
{
	return (dmacRegs.rbor.ADDR + (mask & dmacRegs.rbsr.RMSK));
}

static u32 QWCinVIFMFIFO(u32 DrainADDR, u32 qwc)
{
	u32 ret;

	/* Calculate what we have in the fifo. */
	if (DrainADDR <= spr0ch.madr)
	{
		/* Drain is below the tadr, calculate the difference between them */
		ret = (spr0ch.madr - DrainADDR) >> 4;
	}
	else
	{
		u32 limit = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
		/* Drain is higher than SPR so it has looped round, */
		/* calculate from base to the SPR tag addr and what is left in the top of the ring */
		ret = ((spr0ch.madr - dmacRegs.rbor.ADDR) + (limit - DrainADDR)) >> 4;
	}

	return ret;
}
static __fi bool mfifoVIF1rbTransfer()
{
	u32 msize = dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16;
	u32 mfifoqwc = pcsx2_min_i(QWCinVIFMFIFO(vif1ch.madr, vif1ch.qwc), vif1ch.qwc);
	u32* src;
	bool ret;

	if (mfifoqwc == 0)
		return true; /* Cant do anything, lets forget it */

	/* Check if the transfer should wrap around the ring buffer */
	if ((vif1ch.madr + (mfifoqwc << 4)) > (msize))
	{
		int s1 = ((msize)-vif1ch.madr) >> 2;

		/* it does, so first copy 's1' bytes from 'addr' to 'data' */
		vif1ch.madr = qwctag(vif1ch.madr);

		src = (u32*)PSM(vif1ch.madr);
		if (src == NULL)
			return false;

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer(src + vif1.irqoffset.value, s1 - vif1.irqoffset.value, false);
		else
			ret = VIF1transfer(src, s1, false);

		if (ret)
		{
			/* and second copy 's2' bytes from 'maddr' to '&data[s1]' */
			vif1ch.tadr = qwctag(vif1ch.tadr);
			vif1ch.madr = qwctag(vif1ch.madr);

			src = (u32*)PSM(vif1ch.madr);
			if (src == NULL)
				return false;
			VIF1transfer(src, ((mfifoqwc << 2) - s1), false);
		}
	}
	else
	{
		/* it doesn't, so just transfer 'qwc*4' words */
		src = (u32*)PSM(vif1ch.madr);
		if (src == NULL)
			return false;

		if (vif1.irqoffset.enabled)
			ret = VIF1transfer(src + vif1.irqoffset.value, mfifoqwc * 4 - vif1.irqoffset.value, false);
		else
			ret = VIF1transfer(src, mfifoqwc << 2, false);
	}
	return ret;
}

static __fi void mfifo_VIF1chain()
{
	/* Is QWC = 0? if so there is nothing to transfer */
	if (vif1ch.qwc == 0)
	{
		vif1.inprogress &= ~1;
		return;
	}

	if (vif1ch.madr >= dmacRegs.rbor.ADDR &&
		vif1ch.madr < (dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16u))
	{
		if (QWCinVIFMFIFO(vif1ch.madr, vif1ch.qwc) == 0)
		{
			vif1.inprogress |= 0x10;
			g_vif1Cycles += 4;
			return;
		}

		mfifoVIF1rbTransfer();
		vif1ch.madr = qwctag(vif1ch.madr);
		/* When transferring direct from the MFIFO, the TADR needs to be after the data last read */
		/* FF7 DoC Expects the transfer to end with an Empty interrupt, so the TADR has to match SPR0_MADR */
		/* It does an END tag (which normally doesn't increment TADR because it breaks Soul Calibur 2) */
		/* with a QWC of 1 (rare) so we need to increment the TADR in the case of MFIFO. */
		vif1ch.tadr = vif1ch.madr;
	}
	else
	{
		tDMA_TAG* pMem = dmaGetAddr(vif1ch.madr, !vif1ch.chcr.DIR);
		u32 qwc;

		/* No need to exit on non-mfifo as it is indirect anyway, so it can be transferring this while spr refills the mfifo */

		if (pMem == NULL)
			return;

		/* No more than is mapped behind MADR in one run. */
		qwc = pcsx2_min_u(vif1ch.qwc, dmaContiguousQwc(vif1ch.madr));
		if (vif1.irqoffset.enabled)
			VIF1transfer((u32*)pMem + vif1.irqoffset.value, qwc * 4 - vif1.irqoffset.value, false);
		else
			VIF1transfer((u32*)pMem, qwc << 2, false);
	}
}

void mfifoVifMaskMem(int id)
{
	switch (id)
	{
		/* These five transfer data following the tag, need to check its within the buffer (Front Mission 4) */
		case TAG_CNT:
		case TAG_NEXT:
		case TAG_CALL:
		case TAG_RET:
		case TAG_END:
			if (vif1ch.madr < dmacRegs.rbor.ADDR) /* probably not needed but we will check anyway. */
				vif1ch.madr = qwctag(vif1ch.madr);
			if (vif1ch.madr > (dmacRegs.rbor.ADDR + (u32)dmacRegs.rbsr.RMSK)) /* Usual scenario is the tag is near the end (Front Mission 4) */
				vif1ch.madr = qwctag(vif1ch.madr);
			break;
		default:
			/* Do nothing as the MADR could be outside */
			break;
	}
}

void mfifoVIF1transfer(void)
{
	tDMA_TAG* ptag;

	g_vif1Cycles = 0;

	if (vif1ch.qwc == 0)
	{
		if (QWCinVIFMFIFO(vif1ch.tadr, 1) == 0)
		{
			vif1.inprogress |= 0x10;
			g_vif1Cycles += 4;
			return;
		}

		vif1ch.tadr = qwctag(vif1ch.tadr);
		/* A tag address with nothing behind it is a bus error, which
		 * stops the channel, rather than a tag read through NULL. */
		ptag = dmach_get_addr(&vif1ch, vif1ch.tadr, DMAC_VIF1, false);
		if (!ptag)
		{
			vif1.done = true;
			return;
		}

		if (vif1ch.chcr.TTE)
		{
			bool ret;

			static PCSX2_ALIGN(16) u128 masked_tag;

			masked_tag._u64[0] = 0;
			masked_tag._u64[1] = *((u64*)ptag + 1);

			if (vif1.irqoffset.enabled)
			{
				ret = VIF1transfer((u32*)&masked_tag + vif1.irqoffset.value, 4 - vif1.irqoffset.value, true); /* Transfer Tag on stall */
				/* ret = VIF1transfer((u32*)ptag + (2 + vif1.irqoffset), 2 - vif1.irqoffset, false);  //Transfer Tag on stall */
			}
			else
			{
				vif1.irqoffset.value = 2;
				vif1.irqoffset.enabled = true;
				ret = VIF1transfer((u32*)&masked_tag + 2, 2, true); /* Transfer Tag */
			}

			if (!ret && vif1.irqoffset.enabled)
			{
				vif1.inprogress &= ~1;
				return; /* IRQ set by VIFTransfer */
			}
			g_vif1Cycles += 2;
		}

		vif1.irqoffset.value = 0;
		vif1.irqoffset.enabled = false;

		dmach_unsafe_transfer(&vif1ch, ptag);

		vif1ch.madr = ptag[1]._u32;

		vif1.done |= hwDmacSrcChainWithStack(&vif1ch, ptag->ID);

		mfifoVifMaskMem(ptag->ID);

		if (vif1ch.chcr.TIE && ptag->IRQ)
		{
			vif1.done = true;
		}

		vif1ch.tadr = qwctag(vif1ch.tadr);

		if (vif1ch.qwc > 0)
			vif1.inprogress |= 1;
	}
}

void vifMFIFOInterrupt(void)
{
	g_vif1Cycles = 0;

	if (dmacRegs.ctrl.MFD != MFD_VIF1)
	{
		vif1Interrupt();
		return;
	}

	if (gifRegs.stat.APATH == 2 && gif_path_is_done(GIF_PATH_2))
	{
		gifRegs.stat.APATH = 0;
		gifRegs.stat.OPH = 0;

		if (gif_unit_check_paths(1, 0, 1))
			gif_unit_execute(false, true);
	}

	if (vif1ch.chcr.DIR)
	{
		bool isDirect = (vif1.cmd & 0x7f) == 0x50;
		bool isDirectHL = (vif1.cmd & 0x7f) == 0x51;
		if ((isDirect && !gif_unit_can_do_path2()) || (isDirectHL && !gif_unit_can_do_path2hl()))
		{
			CPU_INT(DMAC_MFIFO_VIF, 128);
			cpuRegs.dmastall |= 1 << DMAC_MFIFO_VIF;
			return;
		}
	}
	if (vif1.waitforvu)
	{
		CPU_INT(VIF_VU1_FINISH, pcsx2_max_i(16, cpuGetCycles()));
		cpuRegs.dmastall |= 1 << DMAC_MFIFO_VIF;
		return;
	}

	/* We need to check the direction, if it is downloading from the GS, */
	/* we handle that separately (KH2 for testing) */

	/* Simulated GS transfer time done, clear the flags */

	if (vif1.irq && vif1.vifstalled.enabled && vif1.vifstalled.value == VIF_IRQ_STALL)
	{
		vif1Regs.stat.INT = true;

		if (((vif1Regs.code >> 24) & 0x7f) != 0x7)
			vif1Regs.stat.VIS = true;

		hwIntcIrq(INTC_VIF1);
		--vif1.irq;

		if (VIF_TEST(vif1Regs.stat, VIF1_STAT_VSS | VIF1_STAT_VIS | VIF1_STAT_VFS))
		{
			/* vif1Regs.stat.FQC = 0; // FQC=0 */
			/* vif1ch.chcr.STR = false; */
			vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);
			/* Used to check if the MFIFO was empty, there's really no need if it's finished what it needed. */
			if ((vif1ch.qwc > 0 || !vif1.done))
			{
				vif1Regs.stat.VPS = VPS_DECODING; /* If there's more data you need to say it's decoding the next VIF CMD (Onimusha - Blade Warriors) */
				cpuRegs.dmastall |= 1 << DMAC_MFIFO_VIF;
				return;
			}
		}
	}

	/* Mirroring change to VIF0 */
	if (vif1.cmd)
	{
		if (vif1.done && vif1ch.qwc == 0)
			vif1Regs.stat.VPS = VPS_WAITING;
	}
	else
	{
		vif1Regs.stat.VPS = VPS_IDLE;
	}

	if (vif1.inprogress & 0x10)
	{
		FireMFIFOEmpty();
		cpuRegs.dmastall |= 1 << DMAC_MFIFO_VIF;
		return;
	}

	vif1.vifstalled.enabled = false;

	if (!vif1.done || vif1ch.qwc)
	{
		switch (vif1.inprogress & 1)
		{
			case 0: /* Set up transfer */
				mfifoVIF1transfer();
				vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);
				/* fallthrough */

			case 1: /* Transfer data */
				if (vif1.inprogress & 0x1) /* Just in case the tag breaks early (or something wierd happens)! */
					mfifo_VIF1chain();
				/* Sanity check! making sure we always have non-zero values */
				if (!(vif1Regs.stat.VGW && !gif_path_is_idle(GIF_PATH_3))) /* If we're waiting on GIF, stop looping, (can be over 1000 loops!) */
				{
					if (vif1.waitforvu)
					{
						CPU_INT(DMAC_MFIFO_VIF, pcsx2_max_i((int)((g_vif1Cycles == 0 ? 4 : g_vif1Cycles)), cpuGetCycles()));
					}
					else
						CPU_INT(DMAC_MFIFO_VIF, (g_vif1Cycles == 0 ? 4 : g_vif1Cycles));
				}

				vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);
				return;
		}
		return;
	}

	vif1.vifstalled.enabled = false;
	vif1.irqoffset.enabled = false;
	vif1.done = 1;

	if (spr0ch.madr == vif1ch.tadr)
	{
		FireMFIFOEmpty();
	}

	g_vif1Cycles = 0;
	vif1Regs.stat.FQC = pcsx2_min_u((u32)0x10, vif1ch.qwc);
	vif1ch.chcr.STR   = false;
	if (emu_config_thread_vu1())
		mtvu_kick_pending(); /* C.80: MFIFO transfer done -- flush deferred unpack notify */
	hwDmacIrq(DMAC_VIF1);
	cpuRegs.dmastall &= ~(1 << DMAC_VIF1);
	vif1Regs.stat.FQC = 0;
}
