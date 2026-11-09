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

#include "../Common.h"
#include "IPU_hw.h"

static IPUDMAStatus IPU1Status;

void ipuDmaReset(void)
{
	IPU1Status.InProgress	= false;
	IPU1Status.DMAFinished	= true;
}

bool ipuDmaFreeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s,  "IPUdma" )))
		return false;

	SaveState_Freeze(s, IPU1Status);

	return SaveState_IsOkay(s);
}

static __fi int IPU1chain(void)
{
	u32 *pMem = (u32*)dmaGetAddr(ipu1ch.madr, false);
	int qwc;

	if (pMem)
	{
		/* Write our data to the FIFO, no more than is mapped behind
		 * MADR in one run. */
		qwc          = ipu_fifo_in_write(pMem, pcsx2_min_u(ipu1ch.qwc, dmaContiguousQwc(ipu1ch.madr)));
		ipu1ch.madr += qwc << 4;
		ipu1ch.qwc  -= qwc;

		/* Update TADR etc */
		hwDmacSrcTadrInc(&ipu1ch);

		if (!ipu1ch.qwc)
			IPU1Status.InProgress = false;
		return qwc;
	}

	return 0;
}

void IPU1dma(void)
{
	int tagcycles = 0;
	int totalqwc = 0;
	tDMA_TAG* ptag;

	if(!ipu1ch.chcr.STR || ipu1ch.chcr.MOD == 2)
	{
		/* We MUST stop the IPU from trying to fill the FIFO with more data if the DMA has been suspended */
		/* if we don't, we risk causing the data to go out of sync with the fifo and we end up losing some! */
		/* This is true for Dragons Quest 8 and probably others which suspend the DMA. */
		cpuRegs.dmastall |= 1 << DMAC_TO_IPU;
		return;
	}

	if (!IPUCoreStatus.DataRequested)
	{
		/* IPU isn't expecting any data, so put it in to wait mode. */
		cpuRegs.eCycle[4] = 0x9999;
		cpuRegs.dmastall |= 1 << DMAC_TO_IPU;

		/* Shouldn't Happen. */
		if (IPUCoreStatus.WaitingOnIPUTo)
		{
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(4 * BIAS);
		}
		return;
	}

	if (!IPU1Status.InProgress)
	{
		ptag = dmaGetAddr(ipu1ch.tadr, false);  /* Set memory pointer to TADR */

		if (!dmach_transfer(&ipu1ch, ptag))
			return;
		ipu1ch.madr = ptag[1]._u32;

		tagcycles += 1; /* Add 1 cycles from the QW read for the tag */

		IPU1Status.DMAFinished = hwDmacSrcChain(&ipu1ch, ptag->ID);

		if (ipu1ch.chcr.TIE && ptag->IRQ) /* Tag Interrupt is set, so schedule the end/interrupt */
			IPU1Status.DMAFinished = true;

		if (ipu1ch.qwc)
			IPU1Status.InProgress = true;
	}

	if (IPU1Status.InProgress)
		totalqwc += IPU1chain();

	/* Nothing has been processed except maybe a tag, or the DMA is ending */
	if(totalqwc == 0 || (IPU1Status.DMAFinished && !IPU1Status.InProgress))
	{
		totalqwc = pcsx2_max_i(4, totalqwc) + tagcycles;
		IPU_INT_TO(totalqwc * BIAS);
	}
	else
	{
		cpuRegs.eCycle[4] = 0x9999;
		cpuRegs.dmastall |= 1 << DMAC_TO_IPU;
	}

	if (IPUCoreStatus.WaitingOnIPUTo && g_BP.IFC >= 1)
	{
		IPUCoreStatus.WaitingOnIPUTo = false;
		IPU_INT_PROCESS(totalqwc * BIAS);
	}
}

void IPU0dma(void)
{
	u32 readsize;
	tDMA_TAG* pMem;

	if(!ipuRegs.ctrl.OFC)
	{
		/* This shouldn't happen. */
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUProcessInterrupt();
		}
		cpuRegs.dmastall |= 1 << DMAC_FROM_IPU;
		return;
	}

	if ((!(ipu0ch.chcr.STR) || (cpuRegs.interrupt & (1 << DMAC_FROM_IPU))) || (ipu0ch.qwc == 0))
	{
		/* This shouldn't happen. */
		if (IPUCoreStatus.WaitingOnIPUFrom)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPU_INT_PROCESS(ipuRegs.ctrl.OFC * BIAS);
		}
		return;
	}

	/* An address with nothing behind it is a bus error, which stops the
	 * channel; otherwise no more than is mapped behind MADR in one run. */
	pMem = dmach_get_addr(&ipu0ch, ipu0ch.madr, DMAC_FROM_IPU, true);
	if (!pMem)
		return;

	readsize = pcsx2_min_u(ipu0ch.qwc, (u32)ipuRegs.ctrl.OFC);
	readsize = pcsx2_min_u(readsize, dmaContiguousQwc(ipu0ch.madr));
	ipu_fifo_out_read(pMem, readsize);

	ipu0ch.madr += readsize << 4;
	ipu0ch.qwc -= readsize;

	if (dmacRegs.ctrl.STS == STS_fromIPU)   /* STS == fromIPU */
		dmacRegs.stadr.ADDR = ipu0ch.madr;

	if (!ipu0ch.qwc)
		IPU_INT_FROM(readsize * BIAS);

	cpuRegs.dmastall |= 1 << DMAC_FROM_IPU;

	if (ipuRegs.ctrl.BUSY && IPUCoreStatus.WaitingOnIPUFrom)
	{
		IPUCoreStatus.WaitingOnIPUFrom = false;
		IPU_INT_PROCESS(readsize * BIAS);
	}
}

void dmaIPU0(void) /* fromIPU */
{
	if (dmacRegs.ctrl.STS == STS_fromIPU)   /* STS == fromIPU - Initial settings */
		dmacRegs.stadr.ADDR = ipu0ch.madr;

	cpuRegs.dmastall &= ~(1 << DMAC_FROM_IPU);
	/* Note: This should probably be a very small value, however anything lower than this will break Mana Khemia */
	/* This is because the game sends bad DMA information, starts an IDEC, then sets it to the correct values */
	/* but because our IPU is too quick, it messes up the sync between the DMA and IPU. */
	/* So this will do until (if) we sort the timing out of IPU, shouldn't cause any problems for games for now. */
	/* IPU_INT_FROM( 160 ); */
	/* Update 22/12/2021 - Doesn't seem to need this now after fixing some FIFO/DMA behaviour */
	IPU0dma();

	/* Explanation of this: */
	/* The DMA logic on a NORMAL transfer is generally a "transfer first, ask questions later" so when it's sent */
	/* QWC == 0 (which we change to 0x10000) it transfers, causing an underflow, then asks if it's reached 0 */
	/* since IPU_FROM is beholden to the OUT FIFO, if there's nothing to transfer, it will stay at 0 and won't underflow */
	/* so the DMA will end. */
	if (ipu0ch.qwc == 0x10000)
	{
		ipu0ch.qwc = 0;
		ipu0ch.chcr.STR = false;
		hwDmacIrq(DMAC_FROM_IPU);
	}
}

void dmaIPU1(void) /* toIPU */
{
	cpuRegs.dmastall &= ~(1 << DMAC_TO_IPU);

	if (ipu1ch.chcr.MOD == CHAIN_MODE)  /* Chain Mode */
	{
		if(ipu1ch.qwc == 0)
		{
			IPU1Status.InProgress = false;
			IPU1Status.DMAFinished = false;
		}
		else /* Attempting to continue a previous chain */
		{
			tDMA_TAG tmp;
			tmp._u32 = ipu1ch.chcr._u32;
			IPU1Status.InProgress = true;
			if ((tmp.ID == TAG_REFE) || (tmp.ID == TAG_END) || (tmp.IRQ && ipu1ch.chcr.TIE))
				IPU1Status.DMAFinished = true;
			else
				IPU1Status.DMAFinished = false;
		}
	}
	else /* Normal Mode */
	{
		IPU1Status.InProgress = true;
		IPU1Status.DMAFinished = true;
	}

	IPU1dma();
}

void ipu0Interrupt(void)
{
	if(ipu0ch.qwc > 0)
	{
		IPU0dma();
		return;
	}

	ipu0ch.chcr.STR = false;
	hwDmacIrq(DMAC_FROM_IPU);
	cpuRegs.dmastall &= ~(1 << DMAC_FROM_IPU);
}

void ipu1Interrupt(void)
{
	if(!IPU1Status.DMAFinished || IPU1Status.InProgress)  /* Sanity Check */
	{
		IPU1dma();
		return;
	}

	ipu1ch.chcr.STR = false;
	hwDmacIrq(DMAC_TO_IPU);
	cpuRegs.dmastall &= ~(1 << DMAC_TO_IPU);
}
