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
#include "CDVD/Ps1CD.h"
#include "FW.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "IopCounters.h"
#include "IopDma.h"
#include "ps2/BiosTools.h"

#include "ps2/pgif.h"
#include "Mdec.h"

namespace IopMemory
{

//////////////////////////////////////////////////////////////////////////////////////////
//
mem8_t iopHwRead8_Page1( u32 addr )
{
	const u32 masked_addr = addr & 0x0fff;

	mem8_t ret = 0; // using a return var can be helpful in debugging.
	switch( masked_addr )
	{
		case (HW_SIO_DATA & 0x0fff):
			ret = sio0_get_rx_data();
			break;
		case (HW_SIO_STAT & 0x0fff):
		case (HW_SIO_MODE & 0x0fff):
		case (HW_SIO_CTRL & 0x0fff):
		case (HW_SIO_BAUD & 0x0fff):
			break;

		// for use of serial port ignore for now
		//case 0x50: ret = serial_read8(); break;

		mcase(HW_DEV9_DATA): ret = DEV9read8( addr ); break;

		mcase(HW_CDR_DATA0): ret = cdrRead0(); break;
		mcase(HW_CDR_DATA1): ret = cdrRead1(); break;
		mcase(HW_CDR_DATA2): ret = cdrRead2(); break;
		mcase(HW_CDR_DATA3): ret = cdrRead3(); break;

		default:
			if( masked_addr >= 0x100 && masked_addr < 0x130 )
				ret = psxHu8( addr );
			else if( masked_addr >= 0x480 && masked_addr < 0x4a0 )
				ret = psxHu8( addr );
			else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
				ret = USBread8( addr );
			else
				ret = psxHu8(addr);
		return ret;
	}
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem8_t iopHwRead8_Page3( u32 addr )
{
	if( addr == 0x1f803100 )	/* TOOL config switches */
	{
		/* All high is a CEX console's. A COH-H board has no CD-ROM drive
		 * and says so with bit 3 alone, which its BIOS checks. */
		return BiosIsArcade ? 0x08 : 0xFF;
	}
	if( addr == 0x1f803204 && BiosIsArcade )	/* TOOL board id */
		return 0x7c;
	return psxHu8( addr );
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem8_t iopHwRead8_Page8( u32 addr )
{
	if (addr == HW_SIO2_FIFO)
		return sio2_read();
	return psxHu8(addr);
}
//////////////////////////////////////////////////////////////////////////////////////////
//
template< typename T >
static __fi T _HwRead_16or32_Page1( u32 addr )
{
	u32 masked_addr = pgmsk( addr );
	T ret = 0;

	// ------------------------------------------------------------------------
	// Counters, 16-bit varieties!
	//
	if( masked_addr >= 0x100 && masked_addr < 0x130 )
	{
		int cntidx = ( masked_addr >> 4 ) & 0xf;
		switch( masked_addr & 0xf )
		{
			case 0x0:
				ret = (T)psxRcntRcount16( cntidx );
			break;

			case 0x4:
				ret = psxCounters[cntidx].mode;

				// hmm!  The old code only did this bitwise math for 16 bit reads.
				// Logic indicates it should do the math consistently.  Question is,
				// should it do the logic for both 16 and 32, or not do logic at all?

				psxCounters[cntidx].mode &= ~0x1800;
			break;

			case 0x8:
				ret = psxCounters[cntidx].target;
			break;

			default:
				ret = psxHu32(addr);
			break;
		}
	}
	// ------------------------------------------------------------------------
	// Counters, 32-bit varieties!
	//
	else if( masked_addr >= 0x480 && masked_addr < 0x4b0 )
	{
		int cntidx = (( masked_addr >> 4 ) & 0xf) - 5;
		switch( masked_addr & 0xf )
		{
			case 0x0:
				ret = (T)psxRcntRcount32( cntidx );
			break;

			case 0x2:
				ret = (T)(psxRcntRcount32( cntidx ) >> 16);
			break;

			case 0x4:
				ret = psxCounters[cntidx].mode;
				// hmm!  The old code only did the following bitwise math for 16 bit reads.
				// Logic indicates it should do the math consistently.  Question is,
				// should it do the logic for both 16 and 32, or not do logic at all?

				psxCounters[cntidx].mode &= ~0x1800;
			break;

			case 0x8:
				ret = psxCounters[cntidx].target;
			break;

			case 0xa:
				ret = psxCounters[cntidx].target >> 16;
			break;

			default:
				ret = psxHu32(addr);
			break;
		}
	}
	// ------------------------------------------------------------------------
	// USB, with both 16 and 32 bit interfaces
	//
	else if( (masked_addr >= pgmsk(HW_USB_START)) && (masked_addr < pgmsk(HW_USB_END)) )
	{
		ret = (sizeof(T) == 2) ? USBread16( addr ) : USBread32( addr );
	}
	// ------------------------------------------------------------------------
	// SPU2, accessible in 16 bit mode only!
	//
	else if( masked_addr >= pgmsk(HW_SPU2_START) && masked_addr < pgmsk(HW_SPU2_END) )
	{
		if( sizeof(T) == 2 )
			ret = SPU2read( addr );
		else
			ret = psxHu32(addr);
	}
	// ------------------------------------------------------------------------
	// PS1 GPU access
	//
	else if( (masked_addr >= pgmsk(HW_PS1_GPU_START)) && (masked_addr < pgmsk(HW_PS1_GPU_END)) )
		ret = psxDma2GpuR(addr);
	else
	{
		switch( masked_addr )
		{
			// ------------------------------------------------------------------------
			case (HW_SIO_DATA & 0x0fff):
				ret = sio0_get_rx_data();
				ret |= sio0_get_rx_data() << 8;
				if (sizeof(T) == 4)
				{
					ret |= sio0_get_rx_data() << 16;
					ret |= sio0_get_rx_data() << 24;
				}
				break;
			case (HW_SIO_STAT & 0x0fff):
				ret = sio0_get_stat();
				break;
			case (HW_SIO_MODE & 0x0fff):
				ret = sio0.mode;
				break;
			case (HW_SIO_CTRL & 0x0fff):
				ret = sio0.ctrl;
				break;
			case (HW_SIO_BAUD & 0x0fff):
				ret = sio0.baud;
				break;

			// ------------------------------------------------------------------------
			//Serial port stuff not support now ;P
			// case 0x050: hard = serial_read32(); break;
			//	case 0x054: hard = serial_status_read(); break;
			//	case 0x05a: hard = serial_control_read(); break;
			//	case 0x05e: hard = serial_baud_read(); break;

			mcase(HW_ICTRL):
				ret = psxHu32(0x1078);
				psxHu32(0x1078) = 0;
			break;

			mcase(HW_ICTRL+2):
				ret = psxHu16(0x107a);
				psxHu32(0x1078) = 0;	// most likely should clear all 32 bits here.
			break;

			// ------------------------------------------------------------------------
			// Legacy GPU  emulation
			//
			mcase(0x1f8010ac) :
				ret = psxHu32(addr);
			break;

			mcase(HW_PS1_GPU_DATA) :
				ret = psxGPUr(addr);
			break;

			mcase(HW_PS1_GPU_STATUS) :
				ret = psxGPUr(addr);
			break;

			mcase (0x1f801820): // MDEC
				ret = mdecRead0();
			break;

			mcase (0x1f801824): // MDEC
				ret = mdecRead1();
			break;

			// ------------------------------------------------------------------------

			mcase(0x1f80146e):
				ret = DEV9read16( addr );
			break;

			default:
				ret = psxHu32(addr);
			break;
		}
	}

	return ret;
}

// Some Page 2 mess?  I love random question marks for comments!
//case 0x1f802030: hard =   //int_2000????
//case 0x1f802040: hard =//dip switches...??

//////////////////////////////////////////////////////////////////////////////////////////
//
mem16_t iopHwRead16_Page1( u32 addr )
{
	return _HwRead_16or32_Page1<mem16_t>( addr );
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem16_t iopHwRead16_Page3( u32 addr )
{
	return psxHu16(addr);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem16_t iopHwRead16_Page8( u32 addr )
{
	return psxHu16(addr);
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem32_t iopHwRead32_Page1( u32 addr )
{
	return _HwRead_16or32_Page1<mem32_t>( addr );
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem32_t iopHwRead32_Page3( u32 addr )
{
	const mem32_t ret = psxHu32(addr);
	return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////
//
mem32_t iopHwRead32_Page8( u32 addr )
{
	u32 masked_addr = addr & 0x0fff;
	if( masked_addr >= 0x200 )
	{
		if( masked_addr < 0x240 )
		{
			const int parm = (masked_addr-0x200) / 4;
			return sio2.send3[parm];
		}
		else if( masked_addr < 0x260 )
		{
			// SIO2 Send commands alternate registers.  First reg maps to Send1, second
			// to Send2, third to Send1, etc.  And the following clever code does this:
			const int parm = (masked_addr-0x240) / 8;
			return (masked_addr & 4) ? sio2.send2[parm] : sio2.send1[parm];
		}
		else if( masked_addr <= 0x280 )
		{
			switch( masked_addr )
			{
				case (HW_SIO2_DATAIN & 0x0fff):
					return psxHu32(addr);
				case (HW_SIO2_FIFO & 0x0fff):
					return psxHu32(addr);
				case (HW_SIO2_CTRL & 0x0fff):
					return sio2.ctrl;
				case (HW_SIO2_RECV1 & 0xfff):
					return sio2.recv1;
				case (HW_SIO2_RECV2 & 0x0fff):
					return sio2.recv2;
				case (HW_SIO2_RECV3 & 0x0fff):
					return sio2.recv3;
				case (0x1f808278 & 0x0fff):
					return sio2.unknown1;
				case (0x1f80827C & 0x0fff):
					return sio2.unknown2;
				case (HW_SIO2_INTR & 0x0fff):
					return sio2.iStat;
				default:
					return psxHu32(addr);
			}
		}
		else if( masked_addr >= pgmsk(HW_FW_START) && masked_addr <= pgmsk(HW_FW_END) )
			return FWread32( addr );
	}
	return psxHu32(addr);
}

}

