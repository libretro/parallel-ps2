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

#include <string.h> /* memset */

#include "../Common.h"
#include "IPU_hw.h"
#include "../ps2/HwInternal.h"

PCSX2_ALIGN(16) IPU_Fifo ipu_fifo;

void ipu_fifo_init(void)
{
	ipu_fifo.out.readpos = 0;
	ipu_fifo.out.writepos = 0;
	ipu_fifo.in.readpos = 0;
	ipu_fifo.in.writepos = 0;
	memset(ipu_fifo.in.data, 0, sizeof(ipu_fifo.in.data));
	memset(ipu_fifo.out.data, 0, sizeof(ipu_fifo.out.data));
}

void ipu_fifo_in_clear(void)
{
	IPU_Fifo_Input *in = &ipu_fifo.in;
	memset(in->data, 0, sizeof(in->data));
	g_BP.IFC = 0;
	ipuRegs.ctrl.IFC = 0;
	in->readpos = 0;
	in->writepos = 0;

	/* Because the FIFO is drained it will request more data immediately */
	IPUCoreStatus.DataRequested = true;

	if (ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
	{
		CPU_INT(DMAC_TO_IPU, 4);
	}
}

void ipu_fifo_out_clear(void)
{
	IPU_Fifo_Output *out = &ipu_fifo.out;
	memset(out->data, 0, sizeof(out->data));
	ipuRegs.ctrl.OFC = 0;
	out->readpos = 0;
	out->writepos = 0;
}

void ipu_fifo_clear(void)
{
	ipu_fifo_in_clear();
	ipu_fifo_out_clear();
}

/* The positions are kept in range by the masking after each step and by
 * ipuFreeze on a load, so each copy splits at most once, where the ring
 * wraps. */
int ipu_fifo_in_write(const u32* pMem, int size)
{
	IPU_Fifo_Input *in = &ipu_fifo.in;
	const int transfer_size = pcsx2_min_i(size, 8 - (int)g_BP.IFC);
	int first_words, second_words;
	if (transfer_size <= 0) return 0;

	first_words  = pcsx2_min_i((32 - in->writepos), transfer_size << 2);
	second_words = (transfer_size << 2) - first_words;

	memcpy(&in->data[in->writepos], pMem, first_words << 2);
	pMem += first_words;

	if(second_words)
		memcpy(&in->data[0], pMem, second_words << 2);

	in->writepos = (in->writepos + (transfer_size << 2)) & 31;

	g_BP.IFC += transfer_size;

	if (g_BP.IFC == 8)
		IPUCoreStatus.DataRequested = false;

	return transfer_size;
}

int ipu_fifo_in_read(void *value)
{
	IPU_Fifo_Input *in = &ipu_fifo.in;

	/* wait until enough data to ensure proper streaming. */
	if (g_BP.IFC <= 1)
	{
		/* IPU FIFO is empty and DMA is waiting so lets tell the DMA we are ready to put data in the FIFO */
		IPUCoreStatus.DataRequested = true;

		if(ipu1ch.chcr.STR && cpuRegs.eCycle[4] == 0x9999)
		{
			CPU_INT( DMAC_TO_IPU, pcsx2_min_u(8U, ipu1ch.qwc));
		}

		if (g_BP.IFC == 0) return 0;
	}

	memcpy(value, &in->data[in->readpos], 16);

	in->readpos = (in->readpos + 4) & 31;
	g_BP.IFC--;
	return 1;
}

int ipu_fifo_out_write(const u32 *value, uint size)
{
	IPU_Fifo_Output *out = &ipu_fifo.out;
	const int transfer_size = pcsx2_min_i((int)size, 8 - (int)ipuRegs.ctrl.OFC);
	int first_words, second_words;
	if(transfer_size <= 0) return 0;

	first_words  = pcsx2_min_i((32 - out->writepos), transfer_size << 2);
	second_words = (transfer_size << 2) - first_words;

	memcpy(&out->data[out->writepos], value, first_words << 2);
	value += first_words;
	if (second_words)
		memcpy(&out->data[0], value, second_words << 2);

	out->writepos = (out->writepos + (transfer_size << 2)) & 31;

	ipuRegs.ctrl.OFC += transfer_size;

	if(ipu0ch.chcr.STR)
		IPU_INT_FROM(1);
	return transfer_size;
}

void ipu_fifo_out_read(void *value, uint size)
{
	IPU_Fifo_Output *out = &ipu_fifo.out;
	const int first_words  = pcsx2_min_i((32 - out->readpos), (int)(size << 2));
	const int second_words = (int)(size << 2) - first_words;

	ipuRegs.ctrl.OFC -= size;

	memcpy(value, &out->data[out->readpos], first_words << 2);
	value = (u32*)value + first_words;

	if (second_words)
		memcpy(value, &out->data[0], second_words << 2);

	out->readpos = (out->readpos + (int)(size << 2)) & 31;
}

void ReadFIFO_IPUout(mem128_t* out)
{
	if (!( ipuRegs.ctrl.OFC > 0)) return;
	ipu_fifo_out_read(out, 1);

	/* Games should always check the fifo before reading from it -- so if the FIFO has no data
	 * its either some glitchy game or a bug in pcsx2. */
}

void WriteFIFO_IPUin(const mem128_t* value)
{
	/* committing every 16 bytes */
	if(ipu_fifo_in_write(value->_u32, 1) > 0)
	{
		if (ipuRegs.ctrl.BUSY)
		{
			IPUCoreStatus.WaitingOnIPUFrom = false;
			IPUCoreStatus.WaitingOnIPUTo = false;
			IPU_INT_PROCESS(2 * BIAS);
		}
	}
}
