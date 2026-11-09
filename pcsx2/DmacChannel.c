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

/* The EE DMAC's per-channel helpers: tag fetch, address resolution and the
 * source-chain tag walk.  Every EE DMA channel uses these, from C and from
 * C++ (the DMACh members in Dmac.h forward here). */

#include "Common.h"

bool dmach_transfer(DMACh *ch, tDMA_TAG *ptag)
{
	if (ptag == NULL)
	{
		dmacRegs.stat.BEIS = true;
		return false;
	}
	ch->chcr.TAG = ptag[0]._u32 >> 16;
	ch->qwc      = ptag[0].QWC;
	return true;
}

void dmach_unsafe_transfer(DMACh *ch, const tDMA_TAG *ptag)
{
	ch->chcr.TAG = ptag[0]._u32 >> 16;
	ch->qwc      = ptag[0].QWC;
}

tDMA_TAG *dmach_get_addr(DMACh *ch, u32 addr, u32 num, bool write)
{
	tDMA_TAG *ptr = dmaGetAddr(addr, write);
	if (ptr == NULL)
	{
		dmacRegs.stat.BEIS  = true;
		dmacRegs.stat._u32 |= (1 << num);
		ch->chcr.STR = false;
	}

	return ptr;
}

tDMA_TAG *dmach_dma_transfer(DMACh *ch, u32 addr, u32 num)
{
	tDMA_TAG *tag = dmach_get_addr(ch, addr, num, false);

	if (tag == NULL)
		return NULL;

	ch->chcr.TAG = tag[0]._u32 >> 16;
	ch->qwc      = tag[0].QWC;
	return tag;
}

/* Note: Dma addresses are guaranteed to be aligned to 16 bytes (128 bits) */
tDMA_TAG *dmaGetAddr(u32 addr, bool write)
{
	tDMA_TAG tmp;
	tmp._u32 = addr;
	if (tmp.SPR)
		return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];

	/* FIXME: Why??? DMA uses physical addresses */
	addr &= 0x1ffffff0;

	if (addr < PS2MEM_MAIN_RAM)
		return (tDMA_TAG*)&eeMem->Main[addr];
	else if (addr < 0x10000000)
		return (tDMA_TAG*)(write ? eeMem->ZeroWrite : eeMem->ZeroRead);
	else if (addr < 0x10004000)
	{
		/* Secret scratchpad address for DMA = end of maximum main memory? */
		return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];
	}
	return NULL;
}

u32 dmaContiguousQwc(u32 addr)
{
	tDMA_TAG tmp;
	tmp._u32 = addr;
	if (tmp.SPR)
		return (PS2MEM_SCRATCH - (addr & 0x3ff0)) >> 4;

	addr &= 0x1ffffff0;

	if (addr < PS2MEM_MAIN_RAM)
		return (PS2MEM_MAIN_RAM - addr) >> 4;
	else if (addr < 0x10000000)
	{
		/* Every address here lands on the same 1 MiB sink. */
		const u32 left = (0x10000000 - addr) >> 4;
		return left < (_1mb >> 4) ? left : (_1mb >> 4);
	}
	else if (addr < 0x10004000)
		return (0x10004000 - addr) >> 4;
	return 0;
}

bool hwDmacSrcChainWithStack(DMACh *dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE:
			/* Refe - Transfer Packet According to ADDR field */
			dma->tadr += 16;
			/* End Transfer */
			return true;

		case TAG_CNT:
			/* CNT - Transfer QWC following the tag.
			 * Set MADR to QW afer tag, and set TADR to QW following the data. */
			dma->tadr += 16;
			dma->madr = dma->tadr;
			break;

		case TAG_NEXT: /* Next - Transfer QWC following tag. TADR = ADDR */
			/* Set MADR to QW following the tag, and set TADR to the address formerly in MADR. */
			temp = dma->madr;
			dma->madr = dma->tadr + 16;
			dma->tadr = temp;
			break;

		case TAG_REF:  /* Ref - Transfer QWC from ADDR field */
		case TAG_REFS: /* Refs - Transfer QWC from ADDR field (Stall Control) */
			/* Set TADR to next tag */
			dma->tadr += 16;
			break;

		case TAG_CALL: /* Call - Transfer QWC following the tag, save succeeding tag */
			/* Store the address in MADR in temp, and set MADR to the data following the tag. */
			temp = dma->madr;
			dma->madr = dma->tadr + 16;

			/* Stash an address on the address stack pointer. */
			switch (dma->chcr.ASP)
			{
				case 0:
					/* Store the succeeding tag in asr0, and mark chcr as having 1 address. */
					dma->asr0 = dma->madr + (dma->qwc << 4);
					dma->chcr.ASP++;
					break;

				case 1:
					/* Store the succeeding tag in asr1, and mark chcr as having 2 addresses. */
					dma->asr1 = dma->madr + (dma->qwc << 4);
					dma->chcr.ASP++;
					break;

				default:
					return true;
			}

			/* Set TADR to the address from MADR we stored in temp. */
			dma->tadr = temp;

			return false;

		case TAG_RET: /* Ret - Transfer QWC following the tag, load next tag */
			/* Set MADR to data following the tag. */
			dma->madr = dma->tadr + 16;

			/* Snag an address from the address stack pointer. */
			switch (dma->chcr.ASP)
			{
				case 2:
					/* Pull asr1 from the stack, give it to TADR, and decrease the # of addresses. */
					dma->tadr = dma->asr1;
					dma->asr1 = 0;
					dma->chcr.ASP--;
					break;

				case 1:
					/* Pull asr0 from the stack, give it to TADR, and decrease the # of addresses. */
					dma->tadr = dma->asr0;
					dma->asr0 = 0;
					dma->chcr.ASP--;
					break;

				case 0:
					/* There aren't any addresses to pull, so end the transfer. */
				default:
					/* If ASR1 and ASR0 are messed up, end the transfer. */
					return true;
			}
			break;

		case TAG_END: /* End - Transfer QWC following the tag */
			/* Set MADR to data following the tag, and end the transfer. */
			dma->madr = dma->tadr + 16;
			/* Don't Increment tadr; breaks Soul Calibur II and III */
			return true;
	}

	return false;
}

/********TADR NOTES***********
From what i've gathered from testing tadr increment stuff (with CNT) is that we might not be 100% accurate in what
increments it and what doesnt. Previously we presumed REFE and END didn't increment the tag, but SIF and IPU never
liked this.

From what i've deduced, REFE does in fact increment, but END doesn't, after much testing, i've concluded this is how
we can standardize DMA chains, so i've modified the code to work like this.   The below function controls the increment
of the TADR along with the MADR on VIF, GIF and SPR1 when using the CNT tag, the others don't use it yet, but they
can probably be modified to do so now.

Reason for this:- Many games  (such as clock tower 3 and FFX Videos) watched the TADR to see when a transfer has finished,
so we need to simulate this wherever we can!  Even the FFX video gets corruption and tries to fire multiple DMA Kicks
if this doesnt happen, which was the reasoning for the hacked up SPR timing we had, that is no longer required.

-Refraction
******************************/

void hwDmacSrcTadrInc(DMACh *dma)
{
	u16 tagid;

	/* Don't touch it if in normal/interleave mode. */
	if (dma->chcr.STR == 0) return;
	if (dma->chcr.MOD != 1) return;

	tagid = (dma->chcr.TAG >> 12) & 0x7;

	if (tagid == TAG_CNT)
		dma->tadr = dma->madr;
}

bool hwDmacSrcChain(DMACh *dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE: /* Refe - Transfer Packet According to ADDR field */
			dma->tadr += 16;
			/* End the transfer. */
			return true;
		case TAG_CNT: /* CNT - Transfer QWC following the tag.
			       * Set MADR to QW after the tag, and TADR to QW following the data. */
			dma->madr = dma->tadr + 16;
			dma->tadr = dma->madr;
			break;
		case TAG_NEXT: /* Next - Transfer QWC following tag. TADR = ADDR
				* Set MADR to QW following the tag, and set TADR to the address formerly in MADR. */
			temp = dma->madr;
			dma->madr = dma->tadr + 16;
			dma->tadr = temp;
			break;
		case TAG_REF:  /* Ref - Transfer QWC from ADDR field */
		case TAG_REFS: /* Refs - Transfer QWC from ADDR field (Stall Control)
				* Set TADR to next tag */
			dma->tadr += 16;
			break;
		case TAG_END: /* End - Transfer QWC following the tag */
			/* Set MADR to data following the tag, and end the transfer. */
			dma->madr = dma->tadr + 16;
			/* Don't Increment tadr; breaks Soul Calibur II and III
			 * Undefined Tag handling ends the DMA, maintaining the bad TADR and Tag in upper CHCR
			 * Some games such as DT racer try to use RET tags on IPU, which it doesn't support */
			PCSX2_FALLTHROUGH;
		default:
			return true;
	}

	return false;
}
