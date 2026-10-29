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

/* Note on INTC usage: All counters code is always called from inside the context of an
 * event test, so instead of using the iopTestIntc we just set the 0x1070 flags directly.
 * The EventText function will pick it up. */

#include "IopCounters.h"
#include "R3000A.h"
#include "Common.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9.h"
#include "USB/USB.h"
#include "IopHw.h"
#include "IopDma.h"
#include "CDVD/CDVD.h"

#include <math.h>

/* Config.PsxType == 1: PAL:
	 VBlank interlaced	50.00 Hz
	 VBlank non-interlaced	49.76 Hz
	 HBlank			15.625 KHz
   Config.PsxType == 0: NSTC
	 VBlank interlaced	59.94 Hz
	 VBlank non-interlaced	59.82 Hz
	 HBlank			15.73426573 KHz */

/* Misc IOP Clocks */
#define PSXPIXEL ((int)(PSXCLK / 13500000))

psxCounter psxCounters[NUM_COUNTERS];
s32 psxNextDeltaCounter;
u32 psxNextStartCounter;
u8 psxhblankgate = 0;
u8 psxvblankgate = 0;

/* Flags when the gate is off or counter disabled. (do not count) */
#define IOPCNT_STOPPED (0x10000000ul)

/* Used to disable targets until after an overflow */
#define IOPCNT_FUTURE_TARGET (0x1000000000ULL)
#define IOPCNT_MODE_WRITE_MSK 0x63FF
#define IOPCNT_MODE_FLAG_MSK 0x1800

#define IOPCNT_ENABLE_GATE (1 << 0)    // enables gate-based counters
#define IOPCNT_MODE_GATE (3 << 1)      // 0x6  Gate mode (dependant on counter)
#define IOPCNT_MODE_RESET_CNT (1 << 3) // 0x8  resets the counter on target (if interrupt only?)
#define IOPCNT_INT_TARGET (1 << 4)     // 0x10  triggers an interrupt on targets
#define IOPCNT_INT_OVERFLOW (1 << 5)   // 0x20  triggers an interrupt on overflows
#define IOPCNT_INT_REPEAT (1 << 6)     // 0x40  0=One shot (ignore TOGGLE bit 7) 1=Repeat Fire (Check TOGGLE bit 7)
#define IOPCNT_INT_TOGGLE (1 << 7)     // 0x80  0=Pulse (reset on read), 1=toggle each interrupt condition (in 1 shot not reset after fired)
#define IOPCNT_ALT_SOURCE (1 << 8)     // 0x100 uses hblank on counters 1 and 3, and PSXCLOCK on counter 0
#define IOPCNT_INT_REQ (1 << 10)       // 0x400 1=Can fire interrupt, 0=Interrupt Fired (reset on read if not 1 shot)
#define IOPCNT_INT_CMPFLAG  (1 << 11)  // 0x800 1=Target interrupt raised
#define IOPCNT_INT_OFLWFLAG (1 << 12)  // 0x1000 1=Overflow interrupt raised

/* Use an arbitrary value to flag HBLANK counters.
 * These counters will be counted by the hblank gates coming from the EE,
 * which ensures they stay 100% in sync with the EE's hblank counters. */
#define PSXHBLANK 0x2001

static void _rcntSet(int cntidx)
{
	u64 c;
	u64 overflowCap = (cntidx >= 3) ? 0x100000000ULL : 0x10000;
	const psxCounter& counter = psxCounters[cntidx];

	/* psxNextDeltaCounter is relative to the psxRegs.cycle when rcntUpdate() was last called.
	 * However, the current _rcntSet could be called at any cycle count, so we need to take
	 * that into account.  Adding the difference from that cycle count to the current one
	 * will do the trick! */

	if (counter.mode & IOPCNT_STOPPED || counter.rate == PSXHBLANK)
		return;

	if (!(counter.mode & (IOPCNT_INT_TARGET | IOPCNT_INT_OVERFLOW)))
		return;
	/* check for special cases where the overflow or target has just passed
	 * (we probably missed it because we're doing/checking other things) */
	if (counter.count > overflowCap || counter.count > counter.target)
	{
		psxNextDeltaCounter = 4;
		return;
	}

	c = (u64)((overflowCap - counter.count) * counter.rate) - (psxRegs.cycle - counter.startCycle);
	c += psxRegs.cycle - psxNextStartCounter; /* adjust for time passed since last rcntUpdate(); */

	if (c < (u64)psxNextDeltaCounter)
	{
		psxNextDeltaCounter = (u32)c;
		psxSetNextBranch(psxNextStartCounter, psxNextDeltaCounter); /* Need to update on counter resets/target changes */
	}

	if (counter.target & IOPCNT_FUTURE_TARGET)
		return;

	c = (s64)((counter.target - counter.count) * counter.rate) - (psxRegs.cycle - counter.startCycle);
	c += psxRegs.cycle - psxNextStartCounter; /* adjust for time passed since last rcntUpdate(); */

	if (c < (u64)psxNextDeltaCounter)
	{
		psxNextDeltaCounter = (u32)c;
		psxSetNextBranch(psxNextStartCounter, psxNextDeltaCounter); /* Need to update on counter resets/target changes */
	}
}

void psxRcntInit(void)
{
	int i;

	memset(psxCounters, 0, sizeof(psxCounters));

	for (i = 0; i < 3; i++)
	{
		psxCounters[i].rate = 1;
		psxCounters[i].mode |= IOPCNT_INT_REQ;
		psxCounters[i].target = IOPCNT_FUTURE_TARGET;
	}
	for (i = 3; i < 6; i++)
	{
		psxCounters[i].rate = 1;
		psxCounters[i].mode |= IOPCNT_INT_REQ;
		psxCounters[i].target = IOPCNT_FUTURE_TARGET;
	}

	psxCounters[0].interrupt = 0x10;
	psxCounters[1].interrupt = 0x20;
	psxCounters[2].interrupt = 0x40;

	psxCounters[3].interrupt = 0x04000;
	psxCounters[4].interrupt = 0x08000;
	psxCounters[5].interrupt = 0x10000;

	psxCounters[6].rate      = 768;
	psxCounters[6].deltaCycles    = psxCounters[6].rate;
	psxCounters[6].mode      = 0x8;

	psxCounters[7].rate      = PSXCLK / 1000;
	psxCounters[7].deltaCycles    = psxCounters[7].rate;
	psxCounters[7].mode      = 0x8;

	for (i = 0; i < 8; i++)
		psxCounters[i].startCycle = psxRegs.cycle;

	/* Tell the IOP to branch ASAP, so that timers can get
	 * configured properly. */
	psxNextDeltaCounter = 1;
	psxNextStartCounter = psxRegs.cycle;
}

static bool _rcntFireInterrupt(int i, bool isOverflow)
{
	bool ret = false;
#
	if (psxCounters[i].mode & IOPCNT_INT_REQ)
	{
		/* IRQ fired */
		psxHu32(0x1070) |= psxCounters[i].interrupt;
		iopTestIntc();
		ret = true;
	}
	else
	{
		if (!(psxCounters[i].mode & IOPCNT_INT_REPEAT)) /* One shot */
			return false;
	}

	if (psxCounters[i].mode & IOPCNT_INT_TOGGLE) /* Toggle mode */
		psxCounters[i].mode ^= IOPCNT_INT_REQ; /* Interrupt flag inverted */
	else
		psxCounters[i].mode &= ~IOPCNT_INT_REQ; /* Interrupt flag set low */

	return ret;
}
static void _rcntTestTarget(int i)
{
	if (psxCounters[i].count < psxCounters[i].target)
		return;

	if (psxCounters[i].mode & IOPCNT_INT_TARGET)
	{
		/* Target interrupt */
		if (_rcntFireInterrupt(i, false))
			psxCounters[i].mode |= IOPCNT_INT_CMPFLAG;
	}

	if (psxCounters[i].mode & IOPCNT_MODE_RESET_CNT) /* Reset on target */
		psxCounters[i].count -= psxCounters[i].target;
	else
		psxCounters[i].target |= IOPCNT_FUTURE_TARGET;
}


static __fi void _rcntTestOverflow(int i)
{
	u64 maxTarget = (i < 3) ? 0xffff : 0xfffffffful;
	if (psxCounters[i].count <= maxTarget)
		return;

	if ((psxCounters[i].mode & IOPCNT_INT_OVERFLOW))
	{
		/* Overflow interrupt */
		if (_rcntFireInterrupt(i, true))
			psxCounters[i].mode |= IOPCNT_INT_OFLWFLAG; /* Overflow flag */
	}

	/* Update count.
	 * Count wraps around back to zero, while the target is restored (if not in one shot mode).
	 * (high bit of the target gets set by rcntWtarget when the target is behind
	 * the counter value, and thus should not be flagged until after an overflow) */

	psxCounters[i].count  -= maxTarget + 1;
	psxCounters[i].target &= maxTarget;
}

/*
Gate:
   TM_NO_GATE                   000
   TM_GATE_ON_Count             001
   TM_GATE_ON_ClearStart        011
   TM_GATE_ON_Clear_OFF_Start   101
   TM_GATE_ON_Start             111

   V-blank  ----+    +----------------------------+    +------
                |    |                            |    |
                |    |                            |    |
                +----+                            +----+
 TM_NO_GATE:

                0================================>============

 TM_GATE_ON_Count:

                <---->0==========================><---->0=====

 TM_GATE_ON_ClearStart:

                0====>0================================>0=====

 TM_GATE_ON_Clear_OFF_Start:

                0====><-------------------------->0====><-----

 TM_GATE_ON_Start:

                <---->0==========================>============
*/

static void _psxCheckStartGate(int i)
{
	if (!(psxCounters[i].mode & IOPCNT_ENABLE_GATE))
		return; /* Ignore Gate */

	switch ((psxCounters[i].mode & 0x6) >> 1)
	{
		case 0x0: /* GATE_ON_count - stop count on gate start: */
			/* get the current count at the time of stoppage: */
			psxCounters[i].count = (i < 3)
				? psxRcntRcount16(i)
				: psxRcntRcount32(i);
			psxCounters[i].mode |= IOPCNT_STOPPED;
			return;


		case 0x2: /* GATE_ON_Clear_OFF_Start - start counting on gate start, stop on gate end */
			psxCounters[i].count = 0;
			psxCounters[i].startCycle = psxRegs.cycle;
			psxCounters[i].mode &= ~IOPCNT_STOPPED;
			break;
		case 0x1: /* GATE_ON_ClearStart - count normally with resets after every end gate
			   * do nothing - All counting will be done on a need-to-count basis. */
		case 0x3: /* GATE_ON_Start - start and count normally on gate end (no restarts or stops or clears)
			   * do nothing! */
			return;
	}
	_rcntSet(i);
}

static void _psxCheckEndGate(int i)
{
	if (!(psxCounters[i].mode & IOPCNT_ENABLE_GATE))
		return; /* Ignore Gate */

	switch ((psxCounters[i].mode & 0x6) >> 1)
	{
		case 0x0: // GATE_ON_count - reset and start counting
		case 0x1: // GATE_ON_ClearStart - count normally with resets after every end gate
			psxCounters[i].count = 0;
			psxCounters[i].startCycle = psxRegs.cycle;
			psxCounters[i].mode &= ~IOPCNT_STOPPED;
			break;

		case 0x2: // GATE_ON_Clear_OFF_Start - start counting on gate start, stop on gate end
			psxCounters[i].count = (i < 3) ? psxRcntRcount16(i) : psxRcntRcount32(i);
			psxCounters[i].mode |= IOPCNT_STOPPED;
			return; // do not set the counter

		case 0x3: // GATE_ON_Start - start and count normally (no restarts or stops or clears)
			if (psxCounters[i].mode & IOPCNT_STOPPED)
			{
				psxCounters[i].count = 0;
				psxCounters[i].startCycle = psxRegs.cycle;
				psxCounters[i].mode &= ~IOPCNT_STOPPED;
			}
			break;
	}
	_rcntSet(i);
}

void psxCheckStartGate16(int i)
{
	if (i == 0) /* hSync counting */
	{
		/* AlternateSource/scanline counters for Gates 1 and 3.
		 * We count them here so that they stay nicely synced 
		 * with the EE's hsync.
		 */

		const u32 altSourceCheck = IOPCNT_ALT_SOURCE | IOPCNT_ENABLE_GATE;
		const u32 stoppedGateCheck = (IOPCNT_STOPPED | altSourceCheck);

		// count if alt source is enabled and either:
		//  * the gate is enabled and not stopped.
		//  * the gate is disabled.

		if (       (psxCounters[1].mode & altSourceCheck)   == IOPCNT_ALT_SOURCE
			|| (psxCounters[1].mode & stoppedGateCheck) == altSourceCheck)
		{
			psxCounters[1].count++;
			_rcntTestOverflow(1);
			_rcntTestTarget(1);
		}

		if (       (psxCounters[3].mode & altSourceCheck)   == IOPCNT_ALT_SOURCE
			|| (psxCounters[3].mode & stoppedGateCheck) == altSourceCheck)
		{
			psxCounters[3].count++;
			_rcntTestOverflow(3);
			_rcntTestTarget(3);
		}
	}

	_psxCheckStartGate(i);
}

void psxCheckEndGate16(int i)
{
	_psxCheckEndGate(i);
}

void psxVBlankStart(void)
{
	cdvdVsync();
	iopIntcIrq(0);
	if (psxvblankgate & (1 << 1))
		psxCheckStartGate16(1);
	/* 32 bit gate is called for gate 3 only.  Ever. */
	if (psxvblankgate & (1 << 3))
		_psxCheckStartGate(3);
}

void psxVBlankEnd(void)
{
	iopIntcIrq(11);
	if (psxvblankgate & (1 << 1))
		psxCheckEndGate16(1);
	if (psxvblankgate & (1 << 3))
		_psxCheckEndGate(3);
}

void psxRcntUpdate(void)
{
	int i;

	psxNextDeltaCounter = 0x7fffffff;
	psxNextStartCounter = psxRegs.cycle;

	for (i = 0; i <= 5; i++)
	{
		/* don't count disabled or hblank counters...
		 * We can't check the ALTSOURCE flag because the PSXCLOCK source *should*
		 * be counted here */

		if (psxCounters[i].mode & IOPCNT_STOPPED)
			continue;

		/* Repeat IRQ mode Pulsed, resets a few cycles after the interrupt, this should do */
		if ((psxCounters[i].mode & IOPCNT_INT_REPEAT) && !(psxCounters[i].mode & IOPCNT_INT_TOGGLE))
			psxCounters[i].mode |= IOPCNT_INT_REQ;

		if (psxCounters[i].rate == PSXHBLANK)
			continue;

		if (psxCounters[i].rate != 1)
		{
			const u32 change = (psxRegs.cycle - psxCounters[i].startCycle) / psxCounters[i].rate;

			if (change <= 0)
				continue;

			psxCounters[i].count      += change;
			psxCounters[i].startCycle += change * psxCounters[i].rate;
		}
		else
		{
			psxCounters[i].count      += psxRegs.cycle - psxCounters[i].startCycle;
			psxCounters[i].startCycle  = psxRegs.cycle;
		}
	}

	/* Do target/overflow testing
	 * Optimization Note: This approach is very sound.  Please do not try to unroll it
	 * as the size of the Test functions will cause code cache clutter and slowness. */

	for (i = 0; i < 6; i++)
	{
		/* Don't do target/overflow checks for hblankers. 
		 * Those checks are done when the counters 
		 * are updated. */
		if (psxCounters[i].rate == PSXHBLANK)
			continue;
		if (psxCounters[i].mode & IOPCNT_STOPPED)
			continue;

		_rcntTestOverflow(i);
		_rcntTestTarget(i);
	}

	const u32 spu2_delta = (psxRegs.cycle - lClocks) % 768;
	psxCounters[6].startCycle = psxRegs.cycle;
	psxCounters[6].deltaCycles = psxCounters[6].rate - spu2_delta;
	TimeUpdate(psxRegs.cycle);
	psxNextDeltaCounter = psxCounters[6].deltaCycles;

	DEV9async(1);
	const s32 diffusb = psxRegs.cycle - psxCounters[7].startCycle;
	s32 cusb = psxCounters[7].deltaCycles;

	if (diffusb >= psxCounters[7].deltaCycles)
	{
		USBasync(diffusb);
		psxCounters[7].startCycle += psxCounters[7].rate * (diffusb / psxCounters[7].rate);
		psxCounters[7].deltaCycles   = psxCounters[7].rate;
	}
	else
		cusb -= diffusb;

	if (cusb < psxNextDeltaCounter)
		psxNextDeltaCounter = cusb;

	for (i = 0; i < 6; i++)
		_rcntSet(i);
}

void psxRcntWcount16(int index, u16 value)
{
	if (psxCounters[index].rate != PSXHBLANK)
	{
		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}

	psxCounters[index].count   = value & 0xffff;
	psxCounters[index].target &= 0xffff;

	if (psxCounters[index].count > psxCounters[index].target)
	{
		// Count already higher than Target
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;
	}

	_rcntSet(index);
}

void psxRcntWcount32(int index, u32 value)
{
	if (psxCounters[index].rate != PSXHBLANK)
	{
		// Re-adjust the startCycle to match where the counter is currently
		// (remainder of the rate divided into the time passed will do the trick)

		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}

	psxCounters[index].count   = value;
	psxCounters[index].target &= 0xffffffff;

	if (psxCounters[index].count > psxCounters[index].target)
	{
		// Count already higher than Target
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;
	}

	_rcntSet(index);
}

void psxRcntWmode16(int index, u32 value)
{
	psxCounter& counter = psxCounters[index];

	counter.mode = (value & IOPCNT_MODE_WRITE_MSK) | (counter.mode & IOPCNT_MODE_FLAG_MSK); // Write new value, preserve flags
	counter.mode |= IOPCNT_INT_REQ; // IRQ Enable

	if (index == 2)
	{
		switch (value & 0x200)
		{
			case 0x000:
				psxCounters[2].rate = 1;
				break;
			case 0x200:
				psxCounters[2].rate = 8;
				break;
			default:
				break;
		}

		if ((counter.mode & 0x7) == 0x7 || (counter.mode & 0x7) == 0x1)
			counter.mode |= IOPCNT_STOPPED;
	}
	else
	{
		// Counters 0 and 1 can select PIXEL or HSYNC as an alternate source:
		counter.rate = 1;

		if (value & IOPCNT_ALT_SOURCE)
			counter.rate = (index == 0) ? PSXPIXEL : PSXHBLANK;

		if (counter.mode & IOPCNT_ENABLE_GATE)
		{
			// gated counters are added up as per the h/vblank timers.
			// (the PIXEL alt source becomes a vsync gate)
			counter.mode |= IOPCNT_STOPPED;
			if (index == 0)
				psxhblankgate |= 1; // fixme: these gate flags should be one var >_<
			else
				psxvblankgate |= 1 << 1;
		}
		else
		{
			if (index == 0)
				psxhblankgate &= ~1;
			else
				psxvblankgate &= ~(1 << 1);
		}
	}

	counter.count = 0;
	counter.startCycle = psxRegs.cycle;

	counter.target &= 0xffff;

	_rcntSet(index);
}

void psxRcntWmode32(int index, u32 value)
{
	psxCounter& counter = psxCounters[index];

	counter.mode = (value & IOPCNT_MODE_WRITE_MSK) | (counter.mode & IOPCNT_MODE_FLAG_MSK); // Write new value, preserve flags
	counter.mode |= IOPCNT_INT_REQ; // IRQ Enable

	if (index == 3)
	{
		// Counter 3 has the HBlank as an alternate source.
		counter.rate = 1;
		if (value & IOPCNT_ALT_SOURCE)
			counter.rate = PSXHBLANK;

		if (counter.mode & IOPCNT_ENABLE_GATE)
		{
			counter.mode |= IOPCNT_STOPPED;
			psxvblankgate |= 1 << 3;
		}
		else
			psxvblankgate &= ~(1 << 3);
	}
	else
	{
		switch (value & 0x6000)
		{
			case 0x0000:
				counter.rate = 1;
				break;
			case 0x2000:
				counter.rate = 8;
				break;
			case 0x4000:
				counter.rate = 16;
				break;
			case 0x6000:
				counter.rate = 256;
				break;
		}

		// Need to set a rate and target
		if ((counter.mode & 0x7) == 0x7 || (counter.mode & 0x7) == 0x1)
			counter.mode |= IOPCNT_STOPPED;
	}

	counter.count = 0;
	counter.startCycle = psxRegs.cycle;
	counter.target &= 0xffffffff;
	_rcntSet(index);
}

void psxRcntWtarget16(int index, u32 value)
{
	psxCounters[index].target = value & 0xffff;

	// Pulse mode reset
	if (!(psxCounters[index].mode & IOPCNT_INT_TOGGLE))
		psxCounters[index].mode |= IOPCNT_INT_REQ; // Interrupt flag reset to high

	if (!(psxCounters[index].mode & IOPCNT_STOPPED) &&
		(psxCounters[index].rate != PSXHBLANK))
	{
		// Re-adjust the startCycle to match where the counter is currently
		// (remainder of the rate divided into the time passed will do the trick)

		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].count += change;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}

	// protect the target from an early arrival.
	// if the target is behind the current count, then set the target overflow
	// flag, so that the target won't be active until after the next overflow.

	if (psxCounters[index].target <= psxCounters[index].count)
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;

	_rcntSet(index);
}

void psxRcntWtarget32(int index, u32 value)
{
	psxCounters[index].target = value;

	// Pulse mode reset
	if (!(psxCounters[index].mode & IOPCNT_INT_TOGGLE))
		psxCounters[index].mode |= IOPCNT_INT_REQ; // Interrupt flag reset to high
					

	if (      !(psxCounters[index].mode & IOPCNT_STOPPED)
		&& (psxCounters[index].rate != PSXHBLANK))
	{
		// Re-adjust the startCycle to match where the counter is currently
		// (remainder of the rate divided into the time passed will do the trick)

		const u32 change = (psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate;
		psxCounters[index].count += change;
		psxCounters[index].startCycle += change * psxCounters[index].rate;
	}
	// protect the target from an early arrival.
	// if the target is behind the current count, then set the target overflow
	// flag, so that the target won't be active until after the next overflow.

	if (psxCounters[index].target <= psxCounters[index].count)
		psxCounters[index].target |= IOPCNT_FUTURE_TARGET;

	_rcntSet(index);
}

u16 psxRcntRcount16(int index)
{
	u32 retval = (u32)psxCounters[index].count;

	// Don't count HBLANK timers
	// Don't count stopped gates either.

	if (      !(psxCounters[index].mode & IOPCNT_STOPPED)
		&& (psxCounters[index].rate != PSXHBLANK))
	{
		u32 delta = (u32)((psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate);
		retval += delta;
	}

	return (u16)retval;
}

u32 psxRcntRcount32(int index)
{
	u32 retval = (u32)psxCounters[index].count;

	if (      !(psxCounters[index].mode & IOPCNT_STOPPED)
		&& (psxCounters[index].rate != PSXHBLANK))
	{
		u32 delta = (u32)((psxRegs.cycle - psxCounters[index].startCycle) / psxCounters[index].rate);
		retval += delta;
	}

	return retval;
}

void psxRcntSetGates(void)
{
	if (psxCounters[0].mode & IOPCNT_ENABLE_GATE)
		psxhblankgate |= 1;
	else
		psxhblankgate &= ~1;

	if (psxCounters[1].mode & IOPCNT_ENABLE_GATE)
		psxvblankgate |= 1 << 1;
	else
		psxvblankgate &= ~(1 << 1);

	if (psxCounters[3].mode & IOPCNT_ENABLE_GATE)
		psxvblankgate |= 1 << 3;
	else
		psxvblankgate &= ~(1 << 3);
}

bool psxRcntFreeze(SaveStateBase *s)
{
	if (!(SaveState_FreezeTag(s, "iopCounters")))
		return false;

	SaveState_Freeze(s, psxCounters);
	SaveState_Freeze(s, psxNextDeltaCounter);
	SaveState_Freeze(s, psxNextStartCounter);
	SaveState_Freeze(s, psxvblankgate);
	SaveState_Freeze(s, psxhblankgate);

	if (!SaveState_IsOkay(s))
		return false;

	if (SaveState_IsLoading(s))
	{
		int i;

		/* rate divides the elapsed cycles in psxRcntUpdate, which is the
		 * very next thing this function calls. The guards there are
		 * against PSXHBLANK and against 1 -- the two values that mean
		 * "do not divide" -- and never against zero, because the runtime
		 * only ever assigns a rate it computed. 1 is the neutral one. */
		for (i = 0; i < NUM_COUNTERS; i++)
		{
			if (!psxCounters[i].rate)
				psxCounters[i].rate = 1;
		}
	}

	return true;
}
