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

#include <cstring> /* memset */

#include "Common.h"
#include "Gif_Unit.h"
#include "Hardware.h"
#include "IopHw.h"
#include "IopMem.h"
#include "R3000A.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"

#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

#include "ps2/HwInternal.h"
#include "ps2/pgif.h"

static const int rdram_devices = 2;	// put 8 for TOOL and 2 for PS2 and PSX
static int rdram_sdevid = 0;

void hwReset(void)
{
	memset(eeHw, 0, sizeof(eeHw));

	psHu32(SBUS_F260) = 0x1D000060;

	// i guess this is kinda a version, it's used by some bioses
	psHu32(DMAC_ENABLEW) = 0x1201;
	psHu32(DMAC_ENABLER) = 0x1201;

	// Sets SPU2 sample rate to PS2 standard (48KHz) whenever emulator is reset.
	// For PSX mode sample rate setting, see HwWrite.cpp
	SPU2_Reset(false);

	sifReset();

	gsReset();
	gifUnit.Reset();
	ipuReset();
	vif0Reset();
	vif1Reset();
	gif_fifo.init();
	rcntInit();
	USBreset();
}

uint intcInterrupt(void)
{
	u32 intc_stat = psHu32(INTC_STAT);
	if (intc_stat == 0)
		return 0;
	if ((intc_stat & psHu32(INTC_MASK)) == 0)
		return 0;

	if (intc_stat & 0x2)
	{
		counters[0].hold = rcntRcount(0);
		counters[1].hold = rcntRcount(1);
	}

	return 0x400;
}

uint dmacInterrupt(void)
{
	if( ((psHu16(DMAC_STAT + 2) & psHu16(DMAC_STAT)) == 0 ) &&
		( psHu16(DMAC_STAT) & 0x8000) == 0 )
		return 0;

	if (!dmacRegs.ctrl.DMAE || psHu8(DMAC_ENABLER+2) == 1)
		return 0;

	return 0x800;
}

void hwIntcIrq(int n)
{
	psHu32(INTC_STAT) |= 1<<n;
	if(psHu32(INTC_MASK) & (1<<n))cpuTestINTCInts();
}

void hwDmacIrq(int n)
{
	psHu32(DMAC_STAT) |= 1<<n;
	if(psHu16(DMAC_STAT+2) & (1<<n))cpuTestDMACInts();
}

void FireMFIFOEmpty(void)
{
	hwDmacIrq(DMAC_MFIFO_EMPTY);

	if (dmacRegs.ctrl.MFD == MFD_VIF1) vif1Regs.stat.FQC = 0;
	if (dmacRegs.ctrl.MFD == MFD_GIF)  gifRegs.stat.FQC  = 0;
}

// Write 'size' bytes to memory address 'addr' from 'data'.
bool hwMFIFOWrite(u32 addr, const u128* data, uint qwc)
{
	// DMAC Address resolution:  FIFO can be placed anywhere in the *physical* memory map
	// for the PS2.  Its probably a serious error for a PS2 app to have the buffer cross
	// valid/invalid page areas of ram, so realistically we only need to test the base address
	// of the FIFO for address validity.

	if (u128* dst = (u128*)PSM(dmacRegs.rbor.ADDR))
	{
		const u32 ringsize = (dmacRegs.rbsr.RMSK / 16) + 1;
		uint startpos      = (addr & dmacRegs.rbsr.RMSK)/16;
		MemCopy_WrappedDest( data, dst, startpos, ringsize, qwc );
		return true;
	}
	return false;
}

void hwMFIFOResume(u32 transferred)
{
	if (transferred == 0)
		return; //Nothing got put in the MFIFO, we don't care

	switch (dmacRegs.ctrl.MFD)
	{
		case MFD_VIF1: // Most common case.
			if (vif1.inprogress & 0x10)
			{
				vif1.inprogress &= ~0x10;
				//Don't resume if stalled or already looping
				if (vif1ch.chcr.STR && !(cpuRegs.interrupt & (1 << DMAC_MFIFO_VIF)) && !vif1Regs.stat.INT)
				{
					//Need to simulate the time it takes to copy here, if the VIF resumes before the SPR has finished, it isn't happy.
					CPU_INT(DMAC_MFIFO_VIF, transferred * BIAS);
				}

			}
			break;
		case MFD_GIF:
			if ((gif.gifstate & GIF_STATE_EMPTY)) {
				CPU_INT(DMAC_MFIFO_GIF, transferred * BIAS);
				gif.gifstate = GIF_STATE_READY;
			}
			break;
		default:
			break;
	}
}

bool hwDmacSrcChainWithStack(DMACh *dma, int id) {
	switch (id) {
		case TAG_REFE: 
			// Refe - Transfer Packet According to ADDR field
			dma->tadr += 16;
			//End Transfer
			return true;

		case TAG_CNT: 
			// CNT - Transfer QWC following the tag.
			// Set MADR to QW afer tag, and set TADR to QW following the data.
			dma->tadr += 16;
			dma->madr = dma->tadr;
			break;

		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
			{
				// Set MADR to QW following the tag, and set TADR to the address formerly in MADR.
				u32 temp = dma->madr;
				dma->madr = dma->tadr + 16;
				dma->tadr = temp;
			}
			break;
		case TAG_REF: // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
			//Set TADR to next tag
			dma->tadr += 16;
			break;

		case TAG_CALL: // Call - Transfer QWC following the tag, save succeeding tag
			{
				// Store the address in MADR in temp, and set MADR to the data following the tag.
				u32 temp = dma->madr;
				dma->madr = dma->tadr + 16;

				// Stash an address on the address stack pointer.
				switch(dma->chcr.ASP)
				{
					case 0: //Check if ASR0 is empty
						// Store the succeeding tag in asr0, and mark chcr as having 1 address.
						dma->asr0 = dma->madr + (dma->qwc << 4);
						dma->chcr.ASP++;
						break;

					case 1:
						// Store the succeeding tag in asr1, and mark chcr as having 2 addresses.
						dma->asr1 = dma->madr + (dma->qwc << 4);
						dma->chcr.ASP++;
						break;

					default:
						return true;
				}

				// Set TADR to the address from MADR we stored in temp.
				dma->tadr = temp;

				return false;
			}

		case TAG_RET: // Ret - Transfer QWC following the tag, load next tag
			//Set MADR to data following the tag.
			dma->madr = dma->tadr + 16;

			// Snag an address from the address stack pointer.
			switch(dma->chcr.ASP)
			{
				case 2:
					// Pull asr1 from the stack, give it to TADR, and decrease the # of addresses.
					dma->tadr = dma->asr1;
					dma->asr1 = 0;
					dma->chcr.ASP--;
					break;

				case 1:
					// Pull asr0 from the stack, give it to TADR, and decrease the # of addresses.
					dma->tadr = dma->asr0;
					dma->asr0 = 0;
					dma->chcr.ASP--;
					break;

				case 0:
					// There aren't any addresses to pull, so end the transfer.
				default:
					// If ASR1 and ASR0 are messed up, end the transfer.
					return true;
			}
			break;

		case TAG_END: // End - Transfer QWC following the tag
            //Set MADR to data following the tag, and end the transfer.
			dma->madr = dma->tadr + 16;
			//Don't Increment tadr; breaks Soul Calibur II and III
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
	//Don't touch it if in normal/interleave mode.
	if (dma->chcr.STR == 0) return;
	if (dma->chcr.MOD != 1) return;

	u16 tagid = (dma->chcr.TAG >> 12) & 0x7;

	if (tagid == TAG_CNT)
		dma->tadr = dma->madr;
}

bool hwDmacSrcChain(DMACh *dma, int id)
{
	u32 temp;

	switch (id)
	{
		case TAG_REFE: // Refe - Transfer Packet According to ADDR field
			dma->tadr += 16;
			// End the transfer.
			return true;
		case TAG_CNT: // CNT - Transfer QWC following the tag.
			      // Set MADR to QW after the tag, and TADR to QW following the data.
			dma->madr = dma->tadr + 16;
			dma->tadr = dma->madr;
			break;
		case TAG_NEXT: // Next - Transfer QWC following tag. TADR = ADDR
			       // Set MADR to QW following the tag, and set TADR to the address formerly in MADR.
			temp = dma->madr;
			dma->madr = dma->tadr + 16;
			dma->tadr = temp;
			break;
		case TAG_REF:  // Ref - Transfer QWC from ADDR field
		case TAG_REFS: // Refs - Transfer QWC from ADDR field (Stall Control)
			       //Set TADR to next tag
			dma->tadr += 16;
			break;
		case TAG_END: // End - Transfer QWC following the tag
			      //Set MADR to data following the tag, and end the transfer.
			dma->madr = dma->tadr + 16;
			//Don't Increment tadr; breaks Soul Calibur II and III
			// Undefined Tag handling ends the DMA, maintaining the bad TADR and Tag in upper CHCR
			// Some games such as DT racer try to use RET tags on IPU, which it doesn't support
			PCSX2_FALLTHROUGH;
		default:
			return true;
	}

	return false;
}

static __fi void IntCHackCheck(void)
{
	// Sanity check: To protect from accidentally "rewinding" the cyclecount
	// on the few times nextBranchCycle can be behind our current cycle.
	s64 diff = cpuRegs.nextEventCycle - cpuRegs.cycle;
	if (diff > 0 && (cpuRegs.cycle - cpuRegs.lastEventCycle) > 8) cpuRegs.cycle = cpuRegs.nextEventCycle;
}

template< uint page >
RETURNS_R128 hwRead128(u32 mem)
{
	// Zero-init: several page/address combinations -- a 128-bit read of a
	// page-0x0F register that is neither in the SBUS_PS1 range nor at
	// 0x1000f300 -- break out of the switch below without ever writing
	// `result`, then fall through to the r128_load at the end. Left
	// uninitialized that returns whatever was on the stack, which is
	// nondeterministic and depends on the caller's frame layout. The paths
	// that do fill it overwrite this.
	alignas(16) mem128_t result = {};

	// FIFOs are the only "legal" 128 bit registers, so we Handle them first.
	// All other registers fall back on the 64-bit handler (and from there
	// all non-IPU reads fall back to the 32-bit handler).

	switch (page)
	{
		case 0x05:
			ReadFIFO_VIF1(&result);
			break;

		case 0x07:
			if (mem & 0x10)
				return r128_zero(); // IPUin is write-only
			ReadFIFO_IPUout(&result);
			break;

		case 0x04:
		case 0x06:
			// VIF0 and GIF are write-only.
			// [Ps2Confirm] Reads from these FIFOs (and IPUin) do one of the following:
			// return zero, leave contents of the dest register unchanged, or in some
			// indeterminate state.  The actual behavior probably isn't important.
			return r128_zero();
		case 0x0F:
			// TODO/FIXME: PSX mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				PGIFrQword((mem & 0x1FFFFFFF), &result);
				break;
			}

			// WARNING: this code is never executed anymore due to previous condition.
			// It requires investigation of what to do.
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 part0 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part1 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part2 = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 part3 = psHu32(0x1000f3E0);
					return r128_from_u32x4(part0, part1, part2, part3);
				}
			}
			break;

		default:
			return r128_from_u64_dup(hwRead64<page>(mem));
	}
	return r128_load(&result);
}


template< uint page, bool intcstathack >
mem32_t _hwRead32(u32 mem)
{
	switch( page )
	{
		case 0x00:
		case 0x01:	return rcntRead32( mem );

		case 0x02:	return ipuRead32( mem );

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
					return vifRead32<1>(mem);
				return vifRead32<0>(mem);
			}
			return dmacRead32<0x03>( mem );

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			// No game is known to attempt such a thing (yay!), so probably nothing for us to
			// worry about.  Chances are, though, doing so is "legal" and yields some sort
			// of reproducible behavior.  Candidate for real hardware testing.
			// Current assumption: Reads 128 bits and discards the unused portion.

			r128 out128 = hwRead128<page>(mem & ~0x0f);
			return reinterpret_cast<u32*>(&out128)[(mem >> 2) & 0x3];
		}
		break;

		case 0x0f:
		{
			// INTC_STAT shortcut for heavy spinning.
			// Performance Note: Visual Studio handles this best if we just manually check for it here,
			// outside the context of the switch statement below.  This is likely fixed by PGO also,
			// but it's an easy enough conditional to account for anyways.

			if (mem == INTC_STAT)
			{
				// Disable INTC hack when in PS1 mode as it seems to break games.
				if (intcstathack && !(psxHu32(HW_ICFG) & (1 << 3)))
					IntCHackCheck();
				return psHu32(INTC_STAT);
			}

			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End))
				return PGIFr((mem & 0x1FFFFFFF));

			// WARNING: this code is never executed anymore due to previous condition.
			// It requires investigation of what to do.
			if ((mem & 0x1000ff00) == 0x1000f300)
			{
				int ret = 0;
				u32 sif2fifosize = pcsx2_min_i(sif2.fifo.size, 7);

				switch (mem & 0xf0)
				{
					case 0x00:
						return psxHu32(0x1f801814);
					case 0x80:
						ret = psHu32(mem) | (sif2fifosize << 16);
						if (sif2.fifo.size > 0)
							ret |= 0x80000000;
						return ret;
					case 0xc0:
						ReadFifoSingleWord();
						return psHu32(mem);
					case 0xe0:
						//ret = 0xa000e1ec;
						if (sif2.fifo.size > 0)
						{
							ReadFifoSingleWord();
							return psHu32(mem);
						}
						break;
				}
				return 0;


			}
			switch( mem )
			{
				case SIO_ISR:

				case 0x1000f410:
				case MCH_RICM:
					return 0;

				case SBUS_F240:
					return psHu32(SBUS_F240) | 0xF0000102;
				case SBUS_F260:
					return psHu32(SBUS_F260);
				case MCH_DRD:
					if( !((psHu32(MCH_RICM) >> 6) & 0xF) )
					{
						switch ((psHu32(MCH_RICM)>>16) & 0xFFF)
						{
							//MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5

							case 0x21: /* INIT */
								if (rdram_sdevid < rdram_devices)
								{
									rdram_sdevid++;
									return 0x1F;
								}
								return 0;

							case 0x23: /* CNFGA */
								return 0x0D0D;	//PVER=3 | MVER=16 | DBL=1 | REFBIT=5

							case 0x24: /* CNFGB */
								//0x0110 for PSX  SVER=0 | CORG=8(5x9x7) | SPT=1 | DEVTYP=0 | BYTE=0
								return 0x0090;	//SVER=0 | CORG=4(5x9x6) | SPT=1 | DEVTYP=0 | BYTE=0

							case 0x40://DEVID
								return psHu32(MCH_RICM) & 0x1F;	// =SDEV
						}
					}
					return 0;
			}
		}
		break;
		default: break;
	}
	//Hack for Transformers and Test Drive Unlimited to simulate filling the VIF FIFO
	//It actually stalls VIF a few QW before the end of the transfer, so we need to pretend its all gone
	//else itll take aaaaaaaaages to boot.
	if(mem == (D1_CHCR + 0x10) && CHECK_VIFFIFOHACK)
		return psHu32(mem) + (vif1ch.qwc * 16);

	return psHu32(mem);
}

template< uint page >
mem32_t hwRead32(u32 mem)
{
	return _hwRead32<page,false>(mem);
}

mem32_t hwRead32_page_0F_INTC_HACK(u32 mem)
{
	return _hwRead32<0x0f,true>(mem);
}

// --------------------------------------------------------------------------------------
//  hwRead8 / hwRead16 / hwRead64 / hwRead128
// --------------------------------------------------------------------------------------

template< uint page >
mem8_t hwRead8(u32 mem)
{
	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u8*)&ret32)[mem & 0x03];
}

template< uint page >
mem16_t hwRead16(u32 mem)
{
	u32 ret32 = _hwRead32<page, false>(mem & ~0x03);
	return ((u16*)&ret32)[(mem>>1) & 0x01];
}

mem16_t hwRead16_page_0F_INTC_HACK(u32 mem)
{
	u32 ret32 = _hwRead32<0x0f, true>(mem & ~0x03);
	return ((u16*)&ret32)[(mem>>1) & 0x01];
}

template< uint page >
mem64_t hwRead64(u32 mem)
{
	switch (page)
	{
		case 0x02:
			return ipuRead64(mem);

		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Reading from FIFOs using non-128 bit reads is a complete mystery.
			// No game is known to attempt such a thing (yay!), so probably nothing for us to
			// worry about.  Chances are, though, doing so is "legal" and yields some sort
			// of reproducible behavior.  Candidate for real hardware testing.

			// Current assumption: Reads 128 bits and discards the unused portion.

			uint wordpart = (mem >> 3) & 0x1;
			r128 full = hwRead128<page>(mem & ~0x0f);
			return *(reinterpret_cast<u64*>(&full) + wordpart);
		}
		case 0x0F:
			if ((mem & 0xffffff00) == 0x1000f300)
			{
				if (mem == 0x1000f3E0)
				{

					ReadFifoSingleWord();
					u32 lo = psHu32(0x1000f3E0);
					ReadFifoSingleWord();
					u32 hi = psHu32(0x1000f3E0);
					return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
				}
			}
		default: break;
	}

	return static_cast<u64>(_hwRead32<page, false>(mem));
}

#define InstantizeHwRead(pageidx) \
	template mem8_t hwRead8<pageidx>(u32 mem); \
	template mem16_t hwRead16<pageidx>(u32 mem); \
	template mem32_t hwRead32<pageidx>(u32 mem); \
	template mem64_t hwRead64<pageidx>(u32 mem); \
	template RETURNS_R128 hwRead128<pageidx>(u32 mem); \
	template mem32_t _hwRead32<pageidx, false>(u32 mem);

InstantizeHwRead(0x00);
InstantizeHwRead(0x01);
InstantizeHwRead(0x02);
InstantizeHwRead(0x03);
InstantizeHwRead(0x04);
InstantizeHwRead(0x05);
InstantizeHwRead(0x06);
InstantizeHwRead(0x07);
InstantizeHwRead(0x08);
InstantizeHwRead(0x09);
InstantizeHwRead(0x0a);
InstantizeHwRead(0x0b);
InstantizeHwRead(0x0c);
InstantizeHwRead(0x0d);
InstantizeHwRead(0x0e);
InstantizeHwRead(0x0f);

// Shift the middle 8 bits (bits 4-12) into the lower 8 bits.
// This helps the compiler optimize the switch statement into a lookup table. :)

#define HELPSWITCH(m) (((m)>>4) & 0xff)

template< uint page >
#if PCSX2_MINGW_R128_BY_PTR
void hwWrite128(u32 mem, const r128* srcval_ptr)
{
	const r128 srcval = r128_load(srcval_ptr);
#else
void TAKES_R128 hwWrite128(u32 mem, r128 srcval)
{
#endif
	// FIFOs are the only "legal" 128 bit registers.  Handle them first.
	// all other registers fall back on the 64-bit handler (and from there
	// most of them fall back to the 32-bit handler).

	switch (page)
	{
		case 0x04:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF0(&usrcval);
			}
			break;

		case 0x05:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_VIF1(&usrcval);
			}
			break;

		case 0x06:
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_GIF(&usrcval);
			}
			break;

		case 0x07:
			if (mem & 0x10)
			{
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				WriteFIFO_IPUin(&usrcval);
			}
			break;

		case 0x0F:
			// todo: psx mode: this is new
			if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
				alignas(16) const u128 usrcval = r128_to_u128(srcval);
				PGIFwQword((mem & 0x1FFFFFFF), (void*)&usrcval);
				return;
			}
			// fallthrough
		default:
			hwWrite64<page>(mem, r128_to_u64(srcval));
			break;
	}
}


// --------------------------------------------------------------------------------------
//  hwWrite8 / hwWrite16 / hwWrite64 / hwWrite128
// --------------------------------------------------------------------------------------

template< uint page >
void hwWrite8(u32 mem, u8 value)
{
	if (mem == SIO_TXFIFO)
	{
		static bool included_newline = false;
		static char sio_buffer[1024];
		static int sio_count;

		if (value == '\r')
		{
			included_newline        = true;
			sio_buffer[sio_count++] = '\n';
		}
		else if (!included_newline || (value != '\n'))
		{
			included_newline        = false;
			sio_buffer[sio_count++] = value;
		}

		if ((sio_count == C89_ARRAY_SIZE(sio_buffer)-1) || (sio_count != 0 && sio_buffer[sio_count-1] == '\n'))
		{
			sio_buffer[sio_count]   = 0;
			sio_count               = 0;
		}
		return;
	}

	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			break;
		default:
			{
				u32 merged = _hwRead32<page,false>(mem & ~0x03);
				((u8*)&merged)[mem & 0x3] = value;

				hwWrite32<page>(mem & ~0x03, merged);
			}
			break;
	}

}


template<uint page>
void hwWrite32( u32 mem, u32 value )
{
	// Notes:
	// All unknown registers on the EE are "reserved" as discarded writes and indeterminate
	// reads.  Bus error is only generated for registers outside the first 16k of mapped
	// register space (which is handled by the VTLB mapping, so no need for checks here).

	switch (page)
	{
		case 0x00:
		case 0x01:
			if (!rcntWrite32(mem, &value))
				return;
			break;
		case 0x02:
			if (!ipuWrite32(mem, value))
				return;
			break;
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
		{
			// [Ps2Confirm] Direct FIFO read/write behavior.  We need to create a test that writes
			// data to one of the FIFOs and determine the result.  I'm not quite sure offhand a good
			// way to do that --air
			// Current assumption is that 32-bit and 64-bit writes likely do 128-bit zero-filled
			// writes (upper 96 bits are 0, lower 32 bits are effective).
			alignas(16) u128 zerofill;
			zerofill._u32[0]                 = 0;
			zerofill._u32[1]                 = 0;
			zerofill.hi                      = 0;
			zerofill._u32[(mem >> 2) & 0x03] = value;

#if PCSX2_MINGW_R128_BY_PTR
			hwWrite128<page>(mem & ~0x0f, reinterpret_cast<const r128*>(&zerofill));
#else
			hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
#endif
		}
		return;

		case 0x03:
			if (mem >= EEMemoryMap::VIF0_Start)
			{
				if(mem >= EEMemoryMap::VIF1_Start)
				{
					if (!vifWrite32<1>(mem, value)) return;
				}
				else
				{
					if (!vifWrite32<0>(mem, value)) return;
				}
			}
			else switch(mem)
			{
				case (GIF_CTRL):
				{
					// Not exactly sure what RST needs to do
					gifRegs.ctrl._u32 = value & 9;
					if (gifRegs.ctrl.RST)
						gifUnit.Reset(true); // Should it reset gsSIGNAL?
					gifRegs.stat.PSE = gifRegs.ctrl.PSE;
					return;
				}

				case (GIF_MODE):
				{
					gifRegs.mode._u32 = value;
					//Need to kickstart the GIF if the M3R mask comes off
					if (               gifRegs.stat.M3R == 1 
							&& gifRegs.mode.M3R == 0 
							&& (gifch.chcr.STR || gif_fifo.fifoSize))
					{
						CPU_INT(DMAC_GIF, 8);
					}


					gifRegs.stat.M3R = gifRegs.mode.M3R;
					gifRegs.stat.IMT = gifRegs.mode.IMT;
					return;
				}
			}
			break;

		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0b:
		case 0x0c:
		case 0x0d:
		case 0x0e:
			if (!dmacWrite32<page>(mem, value))
				return;
			break;

		case 0x0f:
		{
			switch( HELPSWITCH(mem) )
			{
				case HELPSWITCH(INTC_STAT):
					psHu32(INTC_STAT) &= ~value;
					return;

				case HELPSWITCH(INTC_MASK):
					psHu32(INTC_MASK) ^= (u16)value;
					cpuTestINTCInts();
					return;

				case HELPSWITCH(SIO_TXFIFO):
				{
					u8* woot = (u8*)&value;
					// [Ps2Confirm] What happens when we write 32bit 
					// values to SIO_TXFIFO?
					// If it works like the IOP, then all 32bits are 
					// written to the FIFO in  order.  
					// PCSX2 up to this point simply ignored 
					// non-8bit writes to this port.
					hwWrite8<0x0f>(SIO_TXFIFO, woot[0]);
					hwWrite8<0x0f>(SIO_TXFIFO, woot[1]);
					hwWrite8<0x0f>(SIO_TXFIFO, woot[2]);
					hwWrite8<0x0f>(SIO_TXFIFO, woot[3]);
				}
				return;

				case HELPSWITCH(SBUS_F220):
					psHu32(mem) |= value;
					return;

				case HELPSWITCH(SBUS_F230):
					psHu32(mem) &= ~value;
					return;

				case HELPSWITCH(SBUS_F240) :
					if (value & (1 << 19))
					{
						u32 cycle = psxRegs.cycle;
						//pgifInit();
						psxReset();
						PSXCLK =  33868800;
						SPU2_Reset(true);
						setPs1CDVDSpeed(cdvd.Speed);
						psxHu32(0x1f801450) = 0x8;
						psxHu32(0x1f801078) = 1;
						psxRegs.cycle = cycle;
					}
					if(!(value & 0x100))
						psHu32(mem) &= ~0x100;
					else
						psHu32(mem) |= 0x100;
					return;

				case HELPSWITCH(SBUS_F260):
					psHu32(mem) = value;
					return;

				case HELPSWITCH(MCH_RICM)://MCH_RICM: x:4|SA:12|x:5|SDEV:1|SOP:4|SBC:1|SDEV:5
					if ((((value >> 16) & 0xFFF) == 0x21) && (((value >> 6) & 0xF) == 1) && (((psHu32(0xf440) >> 7) & 1) == 0))//INIT & SRP=0
						rdram_sdevid = 0;	// if SIO repeater is cleared, reset sdevid
					psHu32(mem) = value & ~0x80000000;	//kill the busy bit
				return;

				case HELPSWITCH(SBUS_F200):
				case HELPSWITCH(MCH_DRD):
					break;

				case HELPSWITCH(DMAC_ENABLEW):
					if (!dmacWrite32<0x0f>(DMAC_ENABLEW, value)) return;
					break;

				default:
					// TODO: psx add the real address in a sbus HELPSWITCH
					if (((mem & 0x1FFFFFFF) >= EEMemoryMap::SBUS_PS1_Start) && ((mem & 0x1FFFFFFF) < EEMemoryMap::SBUS_PS1_End)) {
						// Tharr be console spam here! Need to figure out how to print what mode
						PGIFw((mem & 0x1FFFFFFF), value);
						return;
					}

			}
		}
		break;
	}

	psHu32(mem) = value;
}

template< uint page >
void hwWrite16(u32 mem, u16 value)
{
	switch(mem & ~3)
	{
		case DMAC_STAT:
		case INTC_STAT:
		case INTC_MASK:
		case DMAC_FAKESTAT:
			hwWrite32<page>(mem & ~3, (u32)value << (mem & 3) * 8);
			break;
		default:
			{
				u32 merged = _hwRead32<page,false>(mem & ~0x03);
				((u16*)&merged)[(mem>>1) & 0x1] = value;

				hwWrite32<page>(mem & ~0x03, merged);
			}
			break;
	}

}

template<uint page>
void hwWrite64( u32 mem, u64 value )
{
	// * Only the IPU has true 64 bit registers.
	// * FIFOs have 128 bit registers that are probably zero-fill.
	// * All other registers likely disregard the upper 32-bits and simply act as normal
	//   32-bit writes.
	switch (page)
	{
		case 0x02:
			if (!ipuWrite64(mem, value))
				return;
			memcpy(&eeHw[(mem) & 0xffff], &value, sizeof(value));
			break;
		case 0x04:
		case 0x05:
		case 0x06:
		case 0x07:
			{
				alignas(16) u128 zerofill;
				zerofill._u32[0] = 0;
				zerofill._u32[1] = 0;
				zerofill.hi      = 0;
				zerofill._u64[(mem >> 3) & 0x01] = value;
#if PCSX2_MINGW_R128_BY_PTR
				hwWrite128<page>(mem & ~0x0f, reinterpret_cast<const r128*>(&zerofill));
#else
				hwWrite128<page>(mem & ~0x0f, r128_from_u128(zerofill));
#endif
			}
			break;

		default:
			// disregard everything except the lower 32 bits.
			// ... and skip the 64 bit writeback since the 32-bit one will suffice.
			hwWrite32<page>( mem, value );
			break;
	}

}

#if PCSX2_MINGW_R128_BY_PTR
#define InstantizeHwWrite(pageidx) \
	template void hwWrite8<pageidx>(u32 mem, mem8_t value); \
	template void hwWrite16<pageidx>(u32 mem, mem16_t value); \
	template void hwWrite32<pageidx>(u32 mem, mem32_t value); \
	template void hwWrite64<pageidx>(u32 mem, mem64_t value); \
	template void hwWrite128<pageidx>(u32 mem, const r128* srcval);
#else
#define InstantizeHwWrite(pageidx) \
	template void hwWrite8<pageidx>(u32 mem, mem8_t value); \
	template void hwWrite16<pageidx>(u32 mem, mem16_t value); \
	template void hwWrite32<pageidx>(u32 mem, mem32_t value); \
	template void hwWrite64<pageidx>(u32 mem, mem64_t value); \
	template void TAKES_R128 hwWrite128<pageidx>(u32 mem, r128 srcval);
#endif

InstantizeHwWrite(0x00);	InstantizeHwWrite(0x08);
InstantizeHwWrite(0x01);	InstantizeHwWrite(0x09);
InstantizeHwWrite(0x02);	InstantizeHwWrite(0x0a);
InstantizeHwWrite(0x03);	InstantizeHwWrite(0x0b);
InstantizeHwWrite(0x04);	InstantizeHwWrite(0x0c);
InstantizeHwWrite(0x05);	InstantizeHwWrite(0x0d);
InstantizeHwWrite(0x06);	InstantizeHwWrite(0x0e);
InstantizeHwWrite(0x07);	InstantizeHwWrite(0x0f);
