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

#include "R3000A.h"
#include "Common.h"
#include "SPU2/spu2.h"
#include "IopCounters.h"
#include "IopHw.h"
#include "IopDma.h"
#include "Sio.h"

#include "Sif.h"
#include "DEV9/DEV9.h"
#include "SPU2/defs.h"

// Dma0/1   in Mdec.c
// Dma3     in CdRom.c
// Dma8     in PsxSpd.c
// Dma11/12 in PsxSio2.c

static void psxDmaGeneric(u32 madr, u32 bcr, u32 chcr, u32 spuCore)
{
	const char dmaNum = spuCore ? 7 : 4;
	const int size = (bcr >> 16) * (bcr & 0xFFFF);

	// Update the SPU2 to the current cycle before initiating the DMA
	TimeUpdate(psxRegs.cycle);

	psxCounters[6].startCycle  = psxRegs.cycle;
	psxCounters[6].deltaCycles = size * 4;

	psxNextDeltaCounter -= (psxRegs.cycle - psxNextStartCounter);
	psxNextStartCounter  = psxRegs.cycle;
	if (psxCounters[6].deltaCycles < psxNextDeltaCounter)
		psxNextDeltaCounter = psxCounters[6].deltaCycles;

	if ((psxRegs.iopNextEventCycle - psxNextStartCounter) > (u32)psxNextDeltaCounter)
		psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;

	switch (chcr)
	{
		case 0x01000201: //cpu to spu2 transfer
			if (dmaNum == 7)
			{
				TimeUpdate(psxRegs.cycle);
				V_Core_DoDMAwrite(&Cores[1], (u16*)&iopMem->Main[madr & 0x1fffff], size * 2);
			}
			else if (dmaNum == 4)
			{
				TimeUpdate(psxRegs.cycle);
				V_Core_DoDMAwrite(&Cores[0], (u16*)&iopMem->Main[madr & 0x1fffff], size * 2);
			}
			break;

		case 0x01000200: //spu2 to cpu transfer
			if (dmaNum == 7)
			{
				TimeUpdate(psxRegs.cycle);
				V_Core_DoDMAread(&Cores[1], (u16*)&iopMem->Main[madr & 0x1fffff], size * 2);
			}
			else if (dmaNum == 4)
			{
				TimeUpdate(psxRegs.cycle);
				V_Core_DoDMAread(&Cores[0], (u16*)&iopMem->Main[madr & 0x1fffff], size * 2);
			}
			psxCpu->Clear(spuCore ? HW_DMA7_MADR : HW_DMA4_MADR, size);
			break;

		default:
			break;
	}
}

void psxDma4(u32 madr, u32 bcr, u32 chcr) // SPU2's Core 0
{
	psxDmaGeneric(madr, bcr, chcr, 0);
}

void psxDma7(u32 madr, u32 bcr, u32 chcr) // SPU2's Core 1
{
	psxDmaGeneric(madr, bcr, chcr, 1);
}

#ifndef DISABLE_PSX_GPU_DMAS
void psxDma2(u32 madr, u32 bcr, u32 chcr) // GPU
{
	sif2.iop.busy = true;
	sif2.iop.end = false;
	//SIF2Dma();
	// todo: psxmode: dmaSIF2 appears to interface with PGPU but everything is already handled without it.
	// it slows down psxmode if it's run.
	//dmaSIF2();
}

void psxDma6(u32 madr, u32 bcr, u32 chcr)
{
	u32* mem = (u32*)&iopMem->Main[madr & 0x1fffff];

	if (chcr == 0x11000002)
	{
		while (bcr--)
		{
			*mem-- = (madr - 4) & 0xffffff;
			madr -= 4;
		}
		mem++;
		*mem = 0xffffff;
	}
	HW_DMA6_CHCR &= ~0x01000000;
	psxDmaInterrupt(6);
}
#endif

void psxDma8(u32 madr, u32 bcr, u32 chcr)
{
	const int size = (bcr >> 16) * (bcr & 0xFFFF) * 8;

	switch (chcr & 0x01000201)
	{
		case 0x01000201: //cpu to dev9 transfer
			DEV9writeDMA8Mem((u32*)&iopMem->Main[madr & 0x1fffff], size);
			break;

		case 0x01000200: //dev9 to cpu transfer
			DEV9readDMA8Mem((u32*)&iopMem->Main[madr & 0x1fffff], size);
			break;

		default:
			break;
	}
	HW_DMA8_CHCR &= ~0x01000000;
	psxDmaInterrupt2(1);
}

void psxDma9(u32 madr, u32 bcr, u32 chcr)
{
	sif0.iop.busy = true;
	sif0.iop.end = false;

	SIF0Dma();
}

void psxDma10(u32 madr, u32 bcr, u32 chcr)
{
	sif1.iop.busy = true;
	sif1.iop.end = false;

	SIF1Dma();
}

void psxDma11(u32 madr, u32 bcr, u32 chcr)
{
	unsigned int i, j;
	int size = (bcr >> 16) * (bcr & 0xffff);
	// Set dmaBlockSize, so SIO2 knows to count based on the DMA block rather than SEND3 length.
	// When SEND3 is written, SIO2 will automatically reset this to zero.
	sio2.dmaBlockSize = (bcr & 0xffff) * 4;

	if (chcr != 0x01000201)
	{
		return;
	}

	for (i = 0; i < (bcr >> 16); i++)
	{
		for (j = 0; j < ((bcr & 0xFFFF) * 4); j++)
		{
			const u8 data = iopMemRead8(madr);
			sio2_write(data);
			madr++;
		}
	}

	HW_DMA11_MADR = madr;
	PSX_INT(IopEvt_Dma11, (size >> 2));
}

void psxDMA11Interrupt()
{
	if (HW_DMA11_CHCR & 0x01000000)
	{
		HW_DMA11_CHCR &= ~0x01000000;
		psxDmaInterrupt2(4);
	}
}

void psxDma12(u32 madr, u32 bcr, u32 chcr)
{
	int size = ((bcr >> 16) * (bcr & 0xFFFF)) * 4;

	if (chcr != 0x41000200)
	{
		return;
	}

	bcr = size;

	while (bcr > 0)
	{
		const u8 data = sio2_read();
		iopMemWrite8(madr, data);
		bcr--;
		madr++;
	}

	HW_DMA12_MADR = madr;
	PSX_INT(IopEvt_Dma12, (size >> 2));
}

void psxDMA12Interrupt()
{
	if (HW_DMA12_CHCR & 0x01000000)
	{
		HW_DMA12_CHCR &= ~0x01000000;
		psxDmaInterrupt2(5);
	}
}
