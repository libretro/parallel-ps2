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

#include "SPR.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "Config.h"

static bool spr0finished = false;
static bool spr1finished = false;
static u32 mfifotransferred = 0;

static void TestClearVUs(u32 madr, u32 qwc, bool isWrite)
{
	if (madr >= 0x11000000 && (madr < 0x11010000))
	{
		// Access to VU memory is only allowed when the VU is stopped
		// Use Psychonauts for testing

		if ((madr < 0x11008000) && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x1))
		{
			_vu0FinishMicro();
			//Catch up VU1 too
			vucpu_execute_block(CpuVU1, 0);
		}
		if ((madr >= 0x11008000) && (vuRegs[0].VI[REG_VPU_STAT].UL & 0x100) && (!THREAD_VU1 || !isWrite))
		{
			if (THREAD_VU1)
				mtvu_wait_vu();
			else
				CpuVU1->Execute(vu1RunCycles);
			cpuRegs.cycle = vuRegs[1].cycle;
			//Catch up VU0 too
			vucpu_execute_block(CpuVU0, 0);
		}

		if (isWrite)
		{
			if (madr < 0x11004000)
				CpuVU0->Clear(madr&0xfff, qwc * 16);
			else if (madr >= 0x11008000 && madr < 0x1100c000)
				CpuVU1->Clear(madr&0x3fff, qwc * 16);
		}
	}
}

static void memcpy_to_spr(u32 dst, u8* src, size_t size)
{
	dst &= _16kb - 1;

	if (dst + size >= _16kb) {
		size_t end = _16kb - dst;
		memcpy(&psSu128(dst), src, end);

		src += end;
		memcpy(&psSu128(0)  , src, size - end);
	} else {
		memcpy(&psSu128(dst), src, size);
	}
}

static void memcpy_from_spr(u8* dst, u32 src, size_t size)
{
	src &= _16kb - 1;

	if (src + size >= _16kb) {
		size_t end = _16kb - src;
		memcpy(dst, &psSu128(src), end);

		dst += end;
		memcpy(dst, &psSu128(0)  , size - end);
	} else {
		memcpy(dst, &psSu128(src), size);
	}
}

// Note: Dma addresses are guaranteed to be aligned to 16 bytes (128 bits)
static __fi tDMA_TAG* SPRdmaGetAddr(u32 addr, bool write)
{
	//For some reason Getaway references SPR Memory from itself using SPR0, oh well, let it i guess...
	if((addr & 0x70000000) == 0x70000000)
		return (tDMA_TAG*)&eeMem->Scratch[addr & 0x3ff0];

	// FIXME: Why??? DMA uses physical addresses
	addr &= 0x1ffffff0;

	if (addr < Ps2MemSize::MainRam)
		return (tDMA_TAG*)&eeMem->Main[addr];
	else if (addr < 0x10000000)
		return (tDMA_TAG*)(write ? eeMem->ZeroWrite : eeMem->ZeroRead);
	else if ((addr >= 0x11000000) && (addr < 0x11010000))
	{
		if (addr >= 0x11008000 && THREAD_VU1)
			mtvu_wait_vu();

		//Access for VU Memory

		if((addr >= 0x1100c000) && (addr < 0x11010000))
			return (tDMA_TAG*)(vuRegs[1].Mem + (addr & 0x3ff0));

		if((addr >= 0x11004000) && (addr < 0x11008000))
			return (tDMA_TAG*)(vuRegs[0].Mem + (addr & 0xff0));

		//Possibly not needed but the manual doesn't say SPR cannot access it.
		if((addr >= 0x11000000) && (addr < 0x11004000))
			return (tDMA_TAG*)(vuRegs[0].Micro + (addr & 0xff0));

		if((addr >= 0x11008000) && (addr < 0x1100c000))
			return (tDMA_TAG*)(vuRegs[1].Micro + (addr & 0x3ff0));
	}
	return NULL;
}


int  _SPR0chain(void)
{
	tDMA_TAG *pMem;
	int partialqwc = 0;
	if (spr0ch.qwc == 0) return 0;
	pMem = SPRdmaGetAddr(spr0ch.madr, true);
	if (pMem == NULL) return -1;

	if(spr0ch.madr >= dmacRegs.rbor.ADDR && spr0ch.madr < (dmacRegs.rbor.ADDR + dmacRegs.rbsr.RMSK + 16u))
	{
		if (dmacRegs.rbsr.RMSK == 0) // Shortcut when MFIFO isn't set up with a size (Hitman series)
		{
			spr0ch.madr += spr0ch.qwc << 4;
			spr0ch.sadr += spr0ch.qwc << 4;
			spr0ch.sadr &= 0x3FFF; // Limited to 16K
			spr0ch.qwc = 0;
		}
		else
		{
			partialqwc = pcsx2_min_i(spr0ch.qwc, 0x400 - ((spr0ch.sadr & 0x3fff) >> 4));

			if ((spr0ch.madr & ~dmacRegs.rbsr.RMSK) != dmacRegs.rbor.ADDR) { }
			else
				mfifotransferred += partialqwc;

			hwMFIFOWrite(spr0ch.madr, &psSu128(spr0ch.sadr), partialqwc);
			spr0ch.madr += partialqwc << 4;
			spr0ch.madr = dmacRegs.rbor.ADDR + (spr0ch.madr & dmacRegs.rbsr.RMSK);
			spr0ch.sadr += partialqwc << 4;
			spr0ch.sadr &= 0x3FFF; // Limited to 16K
			spr0ch.qwc -= partialqwc;
		}
		spr0finished = true;
	}
	else
	{

		// Taking an arbitary small value for games which like to check the QWC/MADR instead of STR, so get most of
		// the cycle delay out of the way before the end.
		partialqwc = pcsx2_min_i(spr0ch.qwc, 0x400 - ((spr0ch.sadr & 0x3fff) >> 4));
		memcpy_from_spr((u8*)pMem, spr0ch.sadr, partialqwc*16);

		// Clear VU mem also!
		TestClearVUs(spr0ch.madr, partialqwc, true);

		spr0ch.madr += partialqwc << 4;
		spr0ch.sadr += partialqwc << 4;
		spr0ch.sadr &= 0x3FFF; // Limited to 16K
		spr0ch.qwc -= partialqwc;

	}

	if (spr0ch.qwc == 0 && dmacRegs.ctrl.STS == STS_fromSPR)
	{
		if (spr0ch.chcr.MOD == NORMAL_MODE || ((spr0ch.chcr.TAG >> 28) & 0x7) == TAG_CNTS)
		{
			dmacRegs.stadr.ADDR = spr0ch.madr; // Copy MADR to DMAC_STADR stall addr register
		}
	}

	return (partialqwc); // Bus is 1/2 the ee speed
}

static __fi void SPR0chain(void)
{
	int cycles = _SPR0chain() * BIAS;
	CPU_INT(DMAC_FROM_SPR, cycles);
}

void _SPR0interleave(void)
{
	int qwc = spr0ch.qwc;
	int sqwc = dmacRegs.sqwc.SQWC;
	int tqwc = dmacRegs.sqwc.TQWC;
	tDMA_TAG *pMem;

	if (tqwc == 0) tqwc = qwc;

	CPU_INT(DMAC_FROM_SPR, qwc * BIAS);

	while (qwc > 0)
	{
		spr0ch.qwc = pcsx2_min_i(tqwc, qwc);
		qwc -= spr0ch.qwc;
		pMem = SPRdmaGetAddr(spr0ch.madr, true);

		switch (dmacRegs.ctrl.MFD)
 		{
			case MFD_VIF1:
			case MFD_GIF:
				hwMFIFOWrite(spr0ch.madr, &psSu128(spr0ch.sadr), spr0ch.qwc);
				mfifotransferred += spr0ch.qwc;
				break;

			case NO_MFD:
			case MFD_RESERVED:
				// Clear VU mem also!
				TestClearVUs(spr0ch.madr, spr0ch.qwc, true);
				memcpy_from_spr((u8*)pMem, spr0ch.sadr, spr0ch.qwc*16);
				break;
 		}
		spr0ch.sadr += spr0ch.qwc * 16;
		spr0ch.sadr &= 0x3FFF; // Limited to 16K
		spr0ch.madr += (sqwc + spr0ch.qwc) * 16;
	}
	if (dmacRegs.ctrl.STS == STS_fromSPR)
		dmacRegs.stadr.ADDR = spr0ch.madr; // Copy MADR to DMAC_STADR stall addr register
	spr0ch.qwc = 0;
}

static __fi void _dmaSPR0(void)
{
	// Transfer Dn_QWC from SPR to Dn_MADR
	switch(spr0ch.chcr.MOD)
	{
		case NORMAL_MODE:
		{
			if (dmacRegs.ctrl.STS == STS_fromSPR) // STS == fromSPR
			{
				dmacRegs.stadr.ADDR = spr0ch.madr;
			}
			SPR0chain();
			spr0finished = true;
			return;
		}
		case CHAIN_MODE:
		{
			tDMA_TAG *ptag;
			bool done = false;

			if (spr0ch.qwc > 0)
			{
				SPR0chain();
				return;
			}
			// Destination Chain Mode
			ptag = (tDMA_TAG*)&psSu32(spr0ch.sadr);
			spr0ch.sadr += 16;
			spr0ch.sadr &= 0x3FFF; // Limited to 16K

			spr0ch.unsafeTransfer(ptag);

			spr0ch.madr = ptag[1]._u32; // MADR = ADDR field + SPR

			switch (ptag->ID)
			{
				case TAG_CNTS: // CNTS - Transfer QWC following the tag (Stall Control)
					if (dmacRegs.ctrl.STS == STS_fromSPR) // STS == fromSPR - Initial Value
					{
						dmacRegs.stadr.ADDR = spr0ch.madr;
					}
					break;

				case TAG_CNT: // CNT - Transfer QWC following the tag.
					done = false;
					break;

				case TAG_END: // End - Transfer QWC following the tag
					done = true;
					break;
			}

			SPR0chain();

			if (spr0ch.chcr.TIE && ptag->IRQ) // Check TIE bit of CHCR and IRQ bit of tag
				done = true;

			spr0finished = done;
			break;
		}
		//case INTERLEAVE_MODE:
		default:
		{
			_SPR0interleave();
			spr0finished = true;
			break;
		}
	}
}

void SPRFROMinterrupt(void)
{
	if (!spr0finished || spr0ch.qwc > 0)
	{
		_dmaSPR0();

		// The qwc check is simply because having data still to transfer from the packet can freak games out if they do a d.tadr == s.madr check
		// and there is still data to come over (FF12 ingame menu)
		if(mfifotransferred != 0 && spr0ch.qwc == 0)
		{
			switch (dmacRegs.ctrl.MFD)
			{
				case MFD_VIF1: // Most common case.
				case MFD_GIF:
				{
					spr0ch.madr = dmacRegs.rbor.ADDR + (spr0ch.madr & dmacRegs.rbsr.RMSK);
					hwMFIFOResume(mfifotransferred);
					break;
				}
				default:
					break;
			}

			mfifotransferred = 0;
		}

		return;
	}

	spr0ch.chcr.STR = false;
	hwDmacIrq(DMAC_FROM_SPR);
}

void dmaSPR0(void)   // fromSPR
{
	spr0finished = false; //Init

	if(spr0ch.chcr.MOD == CHAIN_MODE && spr0ch.qwc > 0)
	{
		tDMA_TAG tmp;
		tmp._u32 = spr0ch.chcr._u32;
		if (tmp.ID == TAG_END) // But not TAG_REFE?
			spr0finished = true; // correct not REFE, Destination Chain doesnt have REFE!
	}

	SPRFROMinterrupt();
}

__fi static void SPR1transfer(const void* data, int qwc)
{
	if ((spr1ch.madr >= 0x11000000) && (spr1ch.madr < 0x11010000))
		TestClearVUs(spr1ch.madr, spr1ch.qwc, false);

	memcpy_to_spr(spr1ch.sadr, (u8*)data, qwc*16);
	spr1ch.sadr += qwc * 16;
	spr1ch.sadr &= 0x3FFF; // Limited to 16K
}

int  _SPR1chain(void)
{
	tDMA_TAG *pMem;

	if (spr1ch.qwc == 0) return 0;

	pMem = SPRdmaGetAddr(spr1ch.madr, false);
	if (pMem == NULL) return -1;
	int partialqwc = 0;
	// Taking an arbitary small value for games which like to check the QWC/MADR instead of STR, so get most of
	// the cycle delay out of the way before the end.
	partialqwc = pcsx2_min_i(spr1ch.qwc, 0x400u);

	SPR1transfer(pMem, partialqwc);
	spr1ch.madr += partialqwc * 16;
	spr1ch.qwc -= partialqwc;

	hwDmacSrcTadrInc(&spr1ch);

	return (partialqwc);
}

static __fi void SPR1chain(void)
{
	int cycles =  _SPR1chain() * BIAS;
	CPU_INT(DMAC_TO_SPR, cycles);
}

void _SPR1interleave(void)
{
	int qwc  = spr1ch.qwc;
	int sqwc = dmacRegs.sqwc.SQWC;
	int tqwc = dmacRegs.sqwc.TQWC;
	tDMA_TAG *pMem;

	if (tqwc == 0) tqwc = qwc;
	CPU_INT(DMAC_TO_SPR, qwc * BIAS);
	while (qwc > 0)
	{
		spr1ch.qwc = pcsx2_min_i(tqwc, qwc);
		qwc -= spr1ch.qwc;
		pMem = SPRdmaGetAddr(spr1ch.madr, false);
		memcpy_to_spr(spr1ch.sadr, (u8*)pMem, spr1ch.qwc*16);
		spr1ch.sadr += spr1ch.qwc * 16;
		spr1ch.sadr &= 0x3FFF; // Limited to 16K
		spr1ch.madr += (sqwc + spr1ch.qwc) * 16;
	}

	spr1ch.qwc = 0;
}

void _dmaSPR1(void)   // toSPR work function
{
	switch(spr1ch.chcr.MOD)
	{
		case NORMAL_MODE:
			// Transfer Dn_QWC from Dn_MADR to SPR1
			SPR1chain();
			spr1finished = true;
			return;
		case CHAIN_MODE:
		{
			tDMA_TAG *ptag;
			bool done = false;

			if (spr1ch.qwc > 0)
			{
				// Transfer Dn_QWC from Dn_MADR to SPR1
				SPR1chain();
				return;
			}
			// Chain Mode

			ptag = SPRdmaGetAddr(spr1ch.tadr, false); // Set memory pointer to TADR

			if (!spr1ch.transfer(ptag))
			{
				done = true;
				spr1finished = done;
			}

			spr1ch.madr = ptag[1]._u32;	// MADR = ADDR field + SPR

			// Transfer dma tag if tte is set
			if (spr1ch.chcr.TTE)
				SPR1transfer(ptag, 1); // Transfer Tag

			done = hwDmacSrcChain(&spr1ch, ptag->ID);
			SPR1chain(); // Transfers the data set by the switch

			if (spr1ch.chcr.TIE && ptag->IRQ) // Check TIE bit of CHCR and IRQ bit of tag
				done = true;

			spr1finished = done;
			break;
		}
		//case INTERLEAVE_MODE:
		default:
		{
			_SPR1interleave();
			spr1finished = true;
			break;
		}
	}
}

void dmaSPR1(void)   // toSPR
{
	spr1finished = false; // Init

	if(spr1ch.chcr.MOD == CHAIN_MODE && spr1ch.qwc > 0)
	{
		tDMA_TAG tmp;
		tmp._u32 = spr1ch.chcr._u32;
		if ((tmp.ID == TAG_END) || (tmp.ID == TAG_REFE) || (tmp.IRQ && spr1ch.chcr.TIE))
			spr1finished = true;
	}

	SPRTOinterrupt();
}

void SPRTOinterrupt(void)
{
	if (!spr1finished || spr1ch.qwc > 0)
	{
		_dmaSPR1();
		return;
	}

	spr1ch.chcr.STR = false;
	hwDmacIrq(DMAC_TO_SPR);
}

bool sprFreeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "SPRdma")))
		return false;

	SaveState_Freeze(s, spr0finished);
	SaveState_Freeze(s, spr1finished);
	SaveState_Freeze(s, mfifotransferred);

	return SaveState_IsOkay(s);
}
