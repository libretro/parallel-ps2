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
#include "ps2/HwInternal.h"

/* The VIFcode interpreter for VIF0 and VIF1: hands the packet to the
 * command handlers (Vif_Codes.c) and does the DMA bookkeeping on what
 * they took. */

static __fi void vifTransferLoop(int idx, u32* data)
{
	vifStruct* vifX     = VIF_X(idx);
	VIFregisters* regs  = VIF_XREGS(idx);
	const Fnptr_VifCmdHandler* handlers = vifCmdHandler[idx];

	regs->stat.VPS |= VPS_TRANSFERRING;
	regs->stat.ER1  = false;
	while (vifX->vifpacketsize > 0 && !vifX->vifstalled.enabled)
	{
		int ret;

		if (!vifX->cmd)
		{ /* Get a new VIFcode */
			if (!regs->err.MII)
			{
				if (vifX->irq && !emu_config_vif1stall_hack())
					break;

				vifX->irq |= data[0] >> 31;
			}

			regs->code = data[0];
			vifX->cmd  = data[0] >> 24;
		}

		ret = handlers[vifX->cmd & 0x7f](vifX->pass, data);
		data                += ret;
		vifX->vifpacketsize -= ret;
	}
}

static __fi bool vifTransfer(int idx, u32* data, int size, bool TTE)
{
	vifStruct* vifX = VIF_X(idx);
	DMACh* ch       = VIF_XCH(idx);
	/* irqoffset necessary to add up the right qws, or else will spin (spiderman) */
	int transferred = vifX->irqoffset.enabled ? vifX->irqoffset.value : 0;

	vifX->vifpacketsize = size;
	vifTransferLoop(idx, data);

	transferred += size - vifX->vifpacketsize;

	/* At least one cycle, so the end of a packet does not just fall
	 * through (Metal Saga). */
	if (!idx) g_vif0Cycles += pcsx2_max_i(1, (int)((transferred * BIAS) >> 2));
	else      g_vif1Cycles += pcsx2_max_i(1, (int)((transferred * BIAS) >> 2));

	vifX->irqoffset.value = transferred % 4; /* cannot lose the offset */

	if (vifX->irq && vifX->cmd == 0)
	{
		/* Always needs to be set to return to the correct offset if there
		 * is data left. */
		vifX->vifstalled.enabled = ch->chcr.STR;
		vifX->vifstalled.value   = VIF_IRQ_STALL;
	}

	if (!TTE) /* Tags can have interrupts, so the DMA is left alone for them (GT4) */
	{
		transferred = transferred >> 2;
		transferred = pcsx2_min_i((int)ch->qwc, transferred);
		ch->madr += (transferred << 4);
		ch->qwc  -= transferred;

		hwDmacSrcTadrInc(ch);

		vifX->irqoffset.enabled = false;

		if (!ch->qwc)
			vifX->inprogress &= ~0x1;
		else if (vifX->irqoffset.value != 0)
			vifX->irqoffset.enabled = true;
	}
	else
		vifX->irqoffset.enabled = vifX->irqoffset.value != 0;

	vifExecQueue(idx);

	return !vifX->vifstalled.enabled;
}

/* When TTE is set, MADR and QWC are not updated as part of the transfer. */
bool VIF0transfer(u32* data, int size, bool TTE)
{
	return vifTransfer(0, data, size, TTE);
}

bool VIF1transfer(u32* data, int size, bool TTE)
{
	return vifTransfer(1, data, size, TTE);
}
