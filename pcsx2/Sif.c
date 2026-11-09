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

#define _PC_	/* disables MIPS opcode macros. */

#include "R3000A.h"
#include "Common.h"
#include "IopMem.h"
#include "SaveState.h"
#include "Sif.h"

void sifReset(void)
{
	memset(&sif0, 0, sizeof(sif0));
	memset(&sif1, 0, sizeof(sif1));
}

static void sif_fifo_sanitize(sifFifo *f)
{
	f->readPos  &= FIFO_SIF_W - 1;
	f->writePos &= FIFO_SIF_W - 1;
	if (f->size < 0 || f->size > FIFO_SIF_W)
		f->size = (f->writePos - f->readPos) & (FIFO_SIF_W - 1);
}

bool sifFreeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "SIFdma")))
		return false;

	SaveState_Freeze(s, sif0);
	SaveState_Freeze(s, sif1);

	/* The fifo's positions index its ring and its size bounds every copy
	 * out of it; a state from elsewhere gets them put back in range. */
	if (SaveState_IsLoading(s))
	{
		sif_fifo_sanitize(&sif0.fifo);
		sif_fifo_sanitize(&sif1.fifo);
	}

	return SaveState_IsOkay(s);
}

int sif_fifo_to_ee(sifFifo *f, DMACh *ch, u32 num, int qwc)
{
	u32 addr     = ch->madr;
	int done     = 0;
	tDMA_TAG *to = dmach_get_addr(ch, addr, num, true);
	if (to == NULL)
		return -1;

	while (done < qwc)
	{
		int run = (int)dmaContiguousQwc(addr);
		if (run > qwc - done)
			run = qwc - done;
		sif_fifo_read(f, to, run << 2);
		done += run;
		addr += (u32)run << 4;
		if (done == qwc)
			break;
		if ((to = dmach_get_addr(ch, addr, num, true)) == NULL)
			break;
	}
	return done;
}

int sif_ee_to_fifo(sifFifo *f, DMACh *ch, u32 num, int qwc)
{
	u32 addr       = ch->madr;
	int done       = 0;
	tDMA_TAG *from = dmach_get_addr(ch, addr, num, false);
	if (from == NULL)
		return -1;

	while (done < qwc)
	{
		int run = (int)dmaContiguousQwc(addr);
		if (run > qwc - done)
			run = qwc - done;
		sif_fifo_write(f, from, run << 2);
		done += run;
		addr += (u32)run << 4;
		if (done == qwc)
			break;
		if ((from = dmach_get_addr(ch, addr, num, false)) == NULL)
			break;
	}
	return done;
}

void sif_iop_read(void *dst, u32 addr, u32 bytes)
{
	const u32 off   = addr & (PS2MEM_IOP_RAM - 1);
	const u32 first = (bytes < PS2MEM_IOP_RAM - off) ? bytes : PS2MEM_IOP_RAM - off;
	memcpy(dst, &iopMem->Main[off], first);
	memcpy((u8*)dst + first, &iopMem->Main[0], bytes - first);
}

void sif_iop_to_fifo(sifFifo *f, u32 addr, int words)
{
	u32 off = addr & (PS2MEM_IOP_RAM - 1);
	u32 tmp[FIFO_SIF_W];

	if (words <= 0)
		return;
	if (off + ((u32)words << 2) <= PS2MEM_IOP_RAM)
	{
		sif_fifo_write(f, &iopMem->Main[off], words);
		return;
	}
	/* Only a run that crosses the top of RAM takes the bounce. */
	while (words > 0)
	{
		const int n = words < FIFO_SIF_W ? words : FIFO_SIF_W;
		sif_iop_read(tmp, off, (u32)n << 2);
		sif_fifo_write(f, tmp, n);
		off    = (off + ((u32)n << 2)) & (PS2MEM_IOP_RAM - 1);
		words -= n;
	}
}

void sif_fifo_to_iop(sifFifo *f, u32 addr, int words)
{
	u32 off = addr & (PS2MEM_IOP_RAM - 1);
	u32 tmp[FIFO_SIF_W];

	if (words <= 0)
		return;
	if (off + ((u32)words << 2) <= PS2MEM_IOP_RAM)
	{
		sif_fifo_read(f, &iopMem->Main[off], words);
		return;
	}
	while (words > 0)
	{
		const int n     = words < FIFO_SIF_W ? words : FIFO_SIF_W;
		const u32 bytes = (u32)n << 2;
		const u32 first = (bytes < PS2MEM_IOP_RAM - off) ? bytes : PS2MEM_IOP_RAM - off;
		sif_fifo_read(f, tmp, n);
		memcpy(&iopMem->Main[off], tmp, first);
		memcpy(&iopMem->Main[0], (u8*)tmp + first, bytes - first);
		off    = (off + bytes) & (PS2MEM_IOP_RAM - 1);
		words -= n;
	}
}
