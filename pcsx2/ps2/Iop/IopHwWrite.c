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

#include "IopHw_Internal.h"
#include "Sif.h"
#include "Sio.h"
#include "FW.h"
#include "CDVD/Ps1CD.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9mmio.h"
#include "USB/USB.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "R3000A.h"

#include "ps2/pgifio.h"
#include "Mdec.h"

#include <string.h>

void iopHwWrite8_generic( u32 addr, mem8_t val )	{ psxHu8(addr) = val; }
void iopHwWrite16_generic( u32 addr, mem16_t val )	{ psxHu16(addr) = val; }
void iopHwWrite32_generic( u32 addr, mem32_t val )	{ psxHu32(addr) = val; }

/* A narrow read returns the low bytes of the register word at addr, which
 * on the little-endian hosts the core runs on are the bytes at addr; so
 * each reads its own width and none reads past the end of the block. */
mem8_t iopHwRead8_generic( u32 addr )	{ return psxHu8(addr); }
mem16_t iopHwRead16_generic( u32 addr )	{ return psxHu16(addr); }
mem32_t iopHwRead32_generic( u32 addr )	{ return psxHu32(addr); }

void iopHwWrite8_Page1( u32 addr, mem8_t val )
{
	u32 masked_addr = pgmsk( addr );

	switch( masked_addr )
	{
		case (HW_SIO_DATA & 0x0fff):
			sio0_set_tx_data(val);
			break;
		case (HW_SIO_STAT & 0x0fff):
		case (HW_SIO_MODE & 0x0fff):
		case (HW_SIO_CTRL & 0x0fff):
		case (HW_SIO_BAUD & 0x0fff):
			break;
		/* for use of serial port ignore for now */
		/* case 0x50: serial_write8( val ); break; */

		mcase(HW_DEV9_DATA): DEV9write8( addr, val ); break;

		mcase(HW_CDR_DATA0): cdrWrite0( val ); break;
		mcase(HW_CDR_DATA1): cdrWrite1( val ); break;
		mcase(HW_CDR_DATA2): cdrWrite2( val ); break;
		mcase(HW_CDR_DATA3): cdrWrite3( val ); break;

		default:
			if( masked_addr >= 0x100 && masked_addr < 0x130 )
				psxHu8( addr ) = val;
			else if( masked_addr >= 0x480 && masked_addr < 0x4a0 )
				psxHu8( addr ) = val;
			else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
			{
				USBwrite8( addr, val );
			}
			else
			{
				psxHu8(addr) = val;
			}
		break;
	}
}

void iopHwWrite8_Page3( u32 addr, mem8_t val )
{
	psxHu8( addr ) = val;
}

void iopHwWrite8_Page8( u32 addr, mem8_t val )
{
	if (addr == HW_SIO2_DATAIN)
		sio2_write(val);
	else
		psxHu8(addr) = val;

}

/* Templated handler for both 32 and 16 bit write operations, to Page 1 registers. */

/* Stores a write's own width (size bytes) of val at its register: a
 * 16-bit write leaves the two bytes above it alone. memcpy because the
 * guest may pass any alignment. */
static __fi void _HwStore_16or32( u32 addr, u32 val, int size )
{
	if (size == 2)
	{
		const u16 v16 = (u16)val;
		memcpy(&iopHw[addr & 0xffff], &v16, sizeof(v16));
	}
	else
		iop_hw_store32(addr, val);
}

/* The 16- and 32-bit Page 1 writes; size is the access width in bytes and
 * val, for a 16-bit write, the halfword zero-extended. */
static __fi void _HwWrite_16or32_Page1( u32 addr, u32 val, int size )
{
	u32 masked_addr = addr & 0x0fff;

	/* ------------------------------------------------------------------------ */
	/* Counters, 16-bit varieties! */

	if( masked_addr >= 0x100 && masked_addr < 0x130 )
	{
		const int cntidx = ( masked_addr >> 4 ) & 0xf;
		switch( masked_addr & 0xf )
		{
			case 0x0:
				psxRcntWcount16( cntidx, (u16)val );
			break;

			case 0x4:
				psxRcntWmode16( cntidx, val );
			break;

			case 0x8:
				psxRcntWtarget16( cntidx, val );
			break;

			default:
				_HwStore_16or32( addr, val, size );
			break;
		}
	}
	/* ------------------------------------------------------------------------ */
	/* Counters, 32-bit varieties! */

	else if( masked_addr >= 0x480 && masked_addr < 0x4b0 )
	{
		int cntidx = (( masked_addr >> 4 ) & 0xf) - 5;
		switch( masked_addr & 0xf )
		{
			case 0x0:
				psxRcntWcount32( cntidx, val );
			break;

			case 0x2:	/* Count HiWord */
				psxRcntWcount32( cntidx, (u32)val << 16 );
			break;

			case 0x4:
				psxRcntWmode32( cntidx, val );
			break;

			case 0x8:
				psxRcntWtarget32( cntidx, val );
			break;

			case 0xa:	/* Target HiWord */
				psxRcntWtarget32( cntidx, (u32)val << 16);
			break;

			default:
				_HwStore_16or32( addr, val, size );
			break;
		}
	}
	/* ------------------------------------------------------------------------ */
	/* USB, with both 16 and 32 bit interfaces */

	else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
	{
		if( size == 2 ) USBwrite16( addr, (u16)val ); else USBwrite32( addr, val );
	}
	/* ------------------------------------------------------------------------ */
	/* SPU2, accessible in 16 bit mode only! */

	else if( (masked_addr >= pgmsk(HW_SPU2_START)) && (masked_addr < pgmsk(HW_SPU2_END)) )
	{
		if( size == 2 )
			SPU2write( addr, (u16)val );
	}
	/* ------------------------------------------------------------------------ */
	/* PS1 GPU access */

	else if( (masked_addr >= pgmsk(HW_PS1_GPU_START)) && (masked_addr < pgmsk(HW_PS1_GPU_END)) )
	{
		psxDma2GpuW(addr, val);
	}
	else
	{
		switch( masked_addr )
		{
			/* ------------------------------------------------------------------------ */
			case (HW_SIO_DATA & 0x0fff):
				break;
			case (HW_SIO_STAT & 0x0fff):
				break;
			case (HW_SIO_MODE & 0x0fff):
				sio0.mode = (u16)val;
				break;

			case (HW_SIO_CTRL & 0x0fff):
				sio0_set_ctrl((u16)val);
				break;

			case (HW_SIO_BAUD & 0x0fff):
				sio0.baud = (u16)val;
				break;

			/* ------------------------------------------------------------------------ */
			/* Serial port stuff not support now ;P */
			/* case 0x050: serial_write16( val ); break; */
			/* case 0x054: serial_status_write( val ); break; */
			/* case 0x05a: serial_control_write( val ); break; */
			/* case 0x05e: serial_baud_write( val ); break; */

			mcase(HW_IREG):
				iop_hw_store32(addr, iop_hw_load32(addr) & val);
				if (val == 0xffffffff) {
					psxHu32(addr) |= 1 << 2;
					psxHu32(addr) |= 1 << 3;
				}
			break;

			mcase(HW_IREG+2):
				iop_hw_store32(addr, iop_hw_load32(addr) & val);
			break;

			mcase(HW_IMASK):
				iop_hw_store32(addr, val);
				iopTestIntc();
			break;

			mcase(HW_IMASK+2):
				iop_hw_store32(addr, val);
				iopTestIntc();
			break;

			mcase(HW_ICTRL):
				iop_hw_store32(addr, val);
				iopTestIntc();
			break;

			mcase(HW_ICTRL+2):
				iop_hw_store32(addr, val);
				iopTestIntc();
			break;

			/* ------------------------------------------------------------------------ */

			mcase(0x1f801088) :	/* DMA0 CHCR -- MDEC IN */
				/* psx mode */
				HW_DMA0_CHCR = val;
				psxDma0(HW_DMA0_MADR, HW_DMA0_BCR, HW_DMA0_CHCR);
			break;

			mcase(0x1f801098):	/* DMA1 CHCR -- MDEC OUT */
				/* psx mode */
				HW_DMA1_CHCR = val;
				psxDma1(HW_DMA1_MADR, HW_DMA1_BCR, HW_DMA1_CHCR);
			break;
			mcase(0x1f8010ac):
				iop_hw_store32(addr, val);
			break;

			mcase(0x1f8010a8) :	/* DMA2 CHCR -- GPU */
				/* BIOS functions */
				/* send_gpu_linked_list: [1F8010A8h]=1000401h */
				/* gpu_abort_dma: [1F8010A8h]=401h */
				/* gpu_send_dma: [1F8010A8h]=1000201h */
				iop_hw_store32(addr, val);
				DmaExec(2);
			break;

			mcase(0x1f8010b8):	/* DMA3 CHCR -- CDROM */
				iop_hw_store32(addr, val);
				DmaExec(3);
			break;

			mcase(0x1f8010c8):	/* DMA4 CHCR -- SPU2 Core 1 */
				iop_hw_store32(addr, val);
				DmaExec(4);
			break;

			mcase(0x1f8010e8):	/* DMA6 CHCR -- OT clear */
				iop_hw_store32(addr, val);
				DmaExec(6);
			break;

			mcase(0x1f801508):	/* DMA7 CHCR -- SPU2 core 2 */
				iop_hw_store32(addr, val);
				DmaExec2(7);
			break;

			mcase(0x1f801518):	/* DMA8 CHCR -- DEV9 */
				iop_hw_store32(addr, val);
				DmaExec2(8);
			break;

			mcase(0x1f801528):	/* DMA9 CHCR -- SIF0 */
				iop_hw_store32(addr, val);
				DmaExec2(9);
			break;

			mcase(0x1f801538):	/* DMA10 CHCR -- SIF1 */
				iop_hw_store32(addr, val);
				DmaExec2(10);
			break;

			mcase(0x1f801548):	/* DMA11 CHCR -- SIO2 IN */
				iop_hw_store32(addr, val);
				DmaExec2(11);
			break;

			mcase(0x1f801558):	/* DMA12 CHCR -- SIO2 OUT */
				iop_hw_store32(addr, val);
				DmaExec2(12);
			break;

			/* ------------------------------------------------------------------------ */
			/* DMA ICR handlers -- General XOR behavior! */

			mcase(0x1f8010f4):
			{
				u32 newtmp = (HW_DMA_ICR & 0xff000000) | (val & 0xffffff);
				newtmp &= ~(val & 0x7F000000);
				if (((newtmp >> 15) & 0x1) || (((newtmp >> 23) & 0x1) == 0x1 && (((newtmp & 0x7F000000) >> 8) & (newtmp & 0x7F0000)) != 0)) {
					newtmp |= 0x80000000;
				}
				else {
					newtmp &= ~0x80000000;
				}
				iop_hw_store32(addr, newtmp);
				if ((HW_DMA_ICR >> 15) & 0x1) {
					psxRegs.CP0.n.Cause &= ~0x7C;
					iopIntcIrq(3);
				}
				else {
					psxDmaInterrupt(33);
				}
			}
			break;

			mcase(0x1f8010f6):		/* ICR_hi (16 bit?) [dunno if it ever happens] */
			{
				const u32 val2 = (u32)val << 16;
				const u32 tmp = (~val2) & HW_DMA_ICR;
				iop_hw_store32(addr, (((tmp ^ val2) & 0xffffff) ^ tmp) >> 16);
			}
			break;

			mcase(0x1f801574):
			{
				u32 newtmp = (HW_DMA_ICR2 & 0xff000000) | (val & 0xffffff);
				newtmp &= ~(val & 0x7F000000);
				if (((newtmp >> 15) & 0x1) || (((newtmp >> 23) & 0x1) == 0x1 && (((newtmp & 0x7F000000) >> 8) & (newtmp & 0x7F0000)) != 0)) {
					newtmp |= 0x80000000;
				}
				else {
					newtmp &= ~0x80000000;
				}
				iop_hw_store32(addr, newtmp);
				if ((HW_DMA_ICR2 >> 15) & 0x1) {
					psxRegs.CP0.n.Cause &= ~0x7C;
					iopIntcIrq(3);
				}
				else {
					psxDmaInterrupt2(33);
				}
			}
			break;

			mcase(0x1f801576):		/* ICR2_hi (16 bit?) [dunno if it ever happens] */
			{
				const u32 val2 = (u32)val << 16;
				const u32 tmp = (~val2) & HW_DMA_ICR2;
				iop_hw_store32(addr, (((tmp ^ val2) & 0xffffff) ^ tmp) >> 16);
			}
			break;

			/* ------------------------------------------------------------------------ */
			/* Legacy GPU  emulation */

			mcase(HW_PS1_GPU_DATA) : /* HW_PS1_GPU_DATA = 0x1f801810 */
				iop_hw_store32(addr, val); /* guess */
				psxGPUw(addr, val);
			break;
			mcase (HW_PS1_GPU_STATUS): /* HW_PS1_GPU_STATUS = 0x1f801814 */
				iop_hw_store32(addr, val); /* guess */
				psxGPUw(addr, val);
			break;
			mcase (0x1f801820): /* MDEC */
				iop_hw_store32(addr, val); /* guess */
				mdecWrite0(val);
			break;
			mcase (0x1f801824): /* MDEC */
				iop_hw_store32(addr, val); /* guess */
				mdecWrite1(val);
			break;

				/* ------------------------------------------------------------------------ */

			mcase(HW_DEV9_DATA):
				DEV9write16( addr, (u16)val );
				iop_hw_store32(addr, val);
			break;

			default:
				_HwStore_16or32( addr, val, size );
			break;
		}
	}
}

void iopHwWrite16_Page1( u32 addr, mem16_t val )
{
	_HwWrite_16or32_Page1( addr, val, 2 );
}

void iopHwWrite16_Page3( u32 addr, mem16_t val )
{
	psxHu16(addr) = val;
}

void iopHwWrite16_Page8( u32 addr, mem16_t val )
{
	psxHu16(addr) = val;
}

void iopHwWrite32_Page1( u32 addr, mem32_t val )
{
	_HwWrite_16or32_Page1( addr, val, 4 );
}

void iopHwWrite32_Page3( u32 addr, mem32_t val )
{
	psxHu32(addr) = val;
}

void iopHwWrite32_Page8( u32 addr, mem32_t val )
{
	u32 masked_addr = addr & 0x0fff;

	if( masked_addr >= 0x200 )
	{
		if( masked_addr < 0x240 )
		{
			const int parm = (masked_addr - 0x200) / 4;
			sio2_set_send3(parm, val);
		}
		else if( masked_addr < 0x260 )
		{
			/* SIO2 Send commands alternate registers.  First reg maps to Send1, second */
			/* to Send2, third to Send1, etc.  And the following clever code does this: */

			const int parm = (masked_addr - 0x240) / 8;

			if (masked_addr & 4)
				sio2.send2[parm] = val;
			else
				sio2.send1[parm] = val;
		}
		else if( masked_addr <= 0x280 )
		{
			switch( masked_addr )
			{
				case (HW_SIO2_DATAIN & 0x0fff):
					break;
				case (HW_SIO2_FIFO & 0x0fff):
					break;
				case (HW_SIO2_CTRL & 0x0fff):
					sio2_set_ctrl(val);
					break;
				case (HW_SIO2_RECV1 & 0x0fff):
					sio2.recv1 = val;
					break;
				case (HW_SIO2_RECV2 & 0x0fff):
					sio2.recv2 = val;
					break;
				case (HW_SIO2_RECV3 & 0x0fff):
					sio2.recv3 = val;
					break;
				case (HW_SIO2_8278 & 0x0fff):
					sio2.unknown1 = val;
					break;
				case (HW_SIO2_827C & 0x0fff):
					sio2.unknown2 = val;
					break;
				case (HW_SIO2_INTR & 0x0fff):
					sio2.iStat &= ~val;
					break;
				/* Other SIO2 registers are read-only, no-ops on write. */
				default:
					psxHu32(addr) = val;
				break;
			}
		}
		else if( masked_addr >= pgmsk(HW_FW_START) && masked_addr <= pgmsk(HW_FW_END) )
		{
			FWwrite32( addr, val );
		}
	}
	else psxHu32(addr) = val;
}
