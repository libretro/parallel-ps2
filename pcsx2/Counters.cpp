/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
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

#include <time.h>
#include <cmath>
#include <cstring> /* memset/memcpy */

#include "Common.h"
#include "R3000A.h"
#include "Counters.h"
#include "IopCounters.h"

#include "GS.h"
#include "GS/GS.h"
#include "VUmicro.h"
#include "Patch.h"

#include "ps2/HwInternal.h"
#include "VMManager.h"
#include "MTGSOwner.h"
#include "CDVD/CDVD.h"     /* cdvdGetDiscRegion */
#include "ps2/BiosTools.h" /* BiosRegion */

#ifdef __LIBRETRO__
extern bool pending_update_av_info;
#endif

//------------------------------------------------------------------
// vSync and hBlank Timing Modes
//------------------------------------------------------------------
#define MODE_VRENDER		0x0 //Set during the Render/Frame Scanlines
#define MODE_GSBLANK		0x2 //Set during the Syncing Scanlines (Delayed GS CSR Swap)
#define MODE_VSYNC		0x3 //Set during the Syncing Scanlines

#define MODE_HRENDER		0x0 //Set for ~5/6 of 1 Scanline
#define MODE_HBLANK		0x1 //Set for the remaining ~1/6 of 1 Scanline

#define SCANLINES_TOTAL_1080	1125 // total number of scanlines for 1080I mode
				    
#define SCANLINES_TOTAL_NTSC_I	525 // total number of scanlines (Interlaced)
#define SCANLINES_TOTAL_NTSC_NI	526 // total number of scanlines (Interlaced)
				   
#define SCANLINES_TOTAL_PAL_I	625 // total number of scanlines per frame (Interlaced)
#define SCANLINES_TOTAL_PAL_NI	628 // total number of scanlines per frame (Not Interlaced)

//------------------------------------------------------------------
// NTSC Timing Information!!! (some scanline info is guessed)
//------------------------------------------------------------------
#define FRAMERATE_NTSC		29.97 // frames per second
				   
//------------------------------------------------------------------
// SPEED HACKS!!! (1 is normal) (They have inverse affects, only set 1 at a time)
//------------------------------------------------------------------
#define HBLANK_COUNTER_SPEED	1 //Set to '3' to double the speed of games like KHII

struct vSyncTimingInfo
{
	u32 VerticalMilliHz;    // vertical frequency in milli-hertz, exact integer
	GS_VideoMode VideoMode; // used to detect change (interlaced/progressive)
	u32 Render;             // time from vblank end to vblank start (cycles)
	u32 Blank;              // time from vblank start to vblank end (cycles)

	u32 GSBlank;            // GS CSR is swapped roughly 3.5 hblank's after vblank start

	u32 hSyncError;         // rounding error after the duration of a rendered frame (cycles)
	u32 hRender;            // time from hblank end to hblank start (cycles)
	u32 hBlank;             // time from hblank start to hblank end (cycles)
	u32 hScanlinesPerFrame; // number of scanlines per frame (525/625 for NTSC/PAL)
};

static vSyncTimingInfo vSyncInfo;

extern u8 psxhblankgate;
static const uint EECNT_FUTURE_TARGET = 0x10000000;
static int gates = 0;

// Counter 4 takes care of scanlines - hSync/hBlanks
// Counter 5 takes care of vSync/vBlanks
Counter counters[4];
static SyncCounter hsyncCounter;
static SyncCounter vsyncCounter;

u64 nextStartCounter; // records the cpuRegs.cycle value of the last call to rcntUpdate()
s32 nextDeltaCounter; // delta from nextStartCounter, in cycles, until the next rcntUpdate()

// For Analog/Double Strike and Interlace modes
static bool IsInterlacedVideoMode(void)
{
	return (           gsVideoMode == GS_VideoMode::PAL 
			|| gsVideoMode == GS_VideoMode::NTSC 
			|| gsVideoMode == GS_VideoMode::DVD_NTSC 
			|| gsVideoMode == GS_VideoMode::DVD_PAL 
			|| gsVideoMode == GS_VideoMode::HDTV_1080I);
}

static bool IsProgressiveVideoMode(void)
{
	// The FIELD register only flips if the CMOD field in SMODE1 is set to anything but 0 and Front Porch bottom bit in SYNCV is set.
	// Also see "isReallyInterlaced()" in GSState.cpp
	return !(*(u32*)PS2GS_BASE(GS_SYNCV) & 0x1) || !(*(u32*)PS2GS_BASE(GS_SMODE1) & 0x6000);
}

void rcntReset(int index)
{
	counters[index].count = 0;
	counters[index].startCycle = cpuRegs.cycle;
}

// Updates the state of the nextDeltaCounter value (if needed) to serve
// any pending events for the given counter.
// Call this method after any modifications to the state of a counter.
static __fi void _rcntSet(int cntidx)
{
	s32 c;

	const Counter& counter = counters[cntidx];

	// Stopped or special hsync gate?
	if (!counter.mode.IsCounting || (counter.mode.ClockSource == 0x3))
		return;

	if (!counter.mode.TargetInterrupt && !counter.mode.OverflowInterrupt)
		return;
	// check for special cases where the overflow or target has just passed
	// (we probably missed it because we're doing/checking other things)
	if (counter.count > 0x10000 || counter.count > counter.target)
	{
		nextDeltaCounter = 4;
		return;
	}

	// nextDeltaCounter is relative to the cpuRegs.cycle when rcntUpdate() was last called.
	// However, the current _rcntSet could be called at any cycle count, so we need to take
	// that into account.  Adding the difference from that cycle count to the current one
	// will do the trick!

	c = ((0x10000 - counter.count) * counter.rate) - (cpuRegs.cycle - counter.startCycle);
	c += cpuRegs.cycle - nextStartCounter; // adjust for time passed since last rcntUpdate();
	if (c < nextDeltaCounter)
	{
		nextDeltaCounter = c;
		// Need to update on counter resets/target changes
		if ((int)(cpuRegs.nextEventCycle - nextStartCounter) > nextDeltaCounter)
			cpuRegs.nextEventCycle = nextStartCounter + nextDeltaCounter;
	}

	// Ignore target diff if target is currently disabled.
	// (the overflow is all we care about since it goes first, and then the
	// target will be turned on afterward, and handled in the next event test).

	if (counter.target & EECNT_FUTURE_TARGET)
		return;

	{
		c = ((counter.target - counter.count) * counter.rate) - (cpuRegs.cycle - counter.startCycle);
		c += cpuRegs.cycle - nextStartCounter; // adjust for time passed since last rcntUpdate();
		if (c < nextDeltaCounter)
		{
			nextDeltaCounter = c;
			// Need to update on counter resets/target changes
			if ((int)(cpuRegs.nextEventCycle - nextStartCounter) > nextDeltaCounter)
				cpuRegs.nextEventCycle = nextStartCounter + nextDeltaCounter;
		}
	}
}

static __fi void cpuRcntSet(void)
{
	int i;

	// Default to next VBlank
	nextStartCounter = cpuRegs.cycle;
	nextDeltaCounter = vsyncCounter.deltaCycles - (cpuRegs.cycle - vsyncCounter.startCycle);

	// Also check next HSync
	s32 nextHsync = hsyncCounter.deltaCycles - (cpuRegs.cycle - hsyncCounter.startCycle);
	if (nextHsync < nextDeltaCounter)
		nextDeltaCounter = nextHsync;

	for (i = 0; i < 4; i++)
		_rcntSet(i);

	// sanity check!
	if (nextDeltaCounter < 0)
		nextDeltaCounter = 0;
	// Need to update on counter resets/target changes
	if ((int)(cpuRegs.nextEventCycle - nextStartCounter) > nextDeltaCounter)
		cpuRegs.nextEventCycle = nextStartCounter + nextDeltaCounter;
}

void rcntInit(void)
{
	int i;

	memset(counters, 0, sizeof(counters));

	for (i = 0; i < 4; i++)
	{
		counters[i].rate   = 2;
		counters[i].target = 0xffff;
	}
	counters[0].interrupt =  9;
	counters[1].interrupt = 10;
	counters[2].interrupt = 11;
	counters[3].interrupt = 12;

	/* Both start their render periods now, with those periods' lengths:
	 * a reset finds the counters in whatever phase the old machine left
	 * them, and a length kept from that phase brings the first vsync
	 * early. */
	hsyncCounter.Mode = MODE_HRENDER;
	hsyncCounter.startCycle = cpuRegs.cycle;
	hsyncCounter.deltaCycles = vSyncInfo.hRender;
	vsyncCounter.Mode = MODE_VRENDER;
	vsyncCounter.startCycle = cpuRegs.cycle;
	vsyncCounter.deltaCycles = vSyncInfo.Render;

	for (i = 0; i < 4; i++)
		rcntReset(i);
	cpuRcntSet();
}

/* Boot-time region used to seed timing while gsVideoMode is still
 * Uninitialized (SetGsCrt not executed yet).  Fast boot skips the BIOS
 * video-mode init, so a PAL game that computes cycle counts from the
 * framerate before setting its own mode would otherwise do so against
 * NTSC defaults (Spyro: Enter the Dragonfly EU cutscene desync).  Disc
 * region from SYSTEM.CNF wins; a BIOS from a PAL zone (Europe, Oceania)
 * is the fallback. */
#define BIOS_REGION_EUROPE  2
#define BIOS_REGION_OCEANIA 3
static bool BootRegionIsPAL(void)
{
	const int disc_region = cdvdGetDiscRegion();
	if (disc_region >= 0)
		return disc_region != 0;
	return BiosRegion == BIOS_REGION_EUROPE || BiosRegion == BIOS_REGION_OCEANIA;
}

/* Pre-init default rates on the milli-hertz grid (see
 * GetVerticalFrequencyMilliHz): the counters must agree with that
 * function about what an Uninitialized-mode PAL rate looks like. */
#define DEFAULT_FRAME_RATE_PAL_MHZ  ((u32)(Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL * 1000.0f))
#define DEFAULT_FRAME_RATE_NTSC_MHZ ((u32)(Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC * 1000.0f))

static void vSyncInfoCalc(vSyncTimingInfo* info, u32 vertical_mHz, u32 scansPerFrame)
{
	// All-integer timing derivation.  The old double math was
	// IEEE-deterministic only as long as no platform built this file with
	// -ffast-math (the mips platform branch already does) or an x87 code
	// path; integer division is deterministic under any flags on any
	// machine.  The value grid is vertical frequency in exact
	// milli-hertz; frame length in 1/10000ths of an EE cycle follows in
	// one u64 division (PS2CLK * 10000 * 2000 = 5.9e18 fits with
	// headroom), and every figure below was verified byte-identical to
	// the old double path across the full video-mode matrix before the
	// switch.
	const u64 Frame = (u64)PS2CLK * 10000ULL * 2000ULL / vertical_mHz;
	const u64 Scanline = Frame / scansPerFrame;

	// There are two renders and blanks per frame. This matches the PS2 test results.
	// The PAL and NTSC VBlank periods respectively lasts for approximately 22 and 26 scanlines.
	// An older test suggests that these periods are actually the periods that VBlank is off, but
	// Legendz Gekitou! Saga Battle runs very slowly if the VBlank period is inverted.
	// Some of the more timing sensitive games and their symptoms when things aren't right:
	// Dynasty Warriors 3 Xtreme Legends - fake save corruption when loading save
	// Jak II - random speedups
	// Shadow of Rome - FMV audio issues
	const bool ntsc_hblank = gsVideoMode != GS_VideoMode::PAL && gsVideoMode != GS_VideoMode::DVD_PAL &&
		(gsVideoMode != GS_VideoMode::Uninitialized || vertical_mHz != DEFAULT_FRAME_RATE_PAL_MHZ);
	const u64 HalfFrame = Frame / 2;
	// Scanline counts carried as numerators over 2: the vblank period is
	// 22.5 (NTSC) / 24.5 (PAL) scanlines, plus 0.5 / 1.5 in progressive
	// modes; the GS CSR swap happens 3.5 / 3 scanlines after vblank
	// start, with the same progressive extra.
	const u64 prog    = IsProgressiveVideoMode() ? 1 : 0;
	const u64 blank2  = (ntsc_hblank ? 45u : 49u) + prog * (ntsc_hblank ? 1u : 3u);
	const u64 gsb2    = (ntsc_hblank ?  7u :  6u) + prog * (ntsc_hblank ? 1u : 3u);
	const u64 Blank   = Scanline * blank2 / 2;
	const u64 Render  = HalfFrame - Blank;
	const u64 GSBlank = Scanline * gsb2 / 2;

	// Important!  The hRender/hBlank timers should be 50/50 for best results.
	//  (this appears to be what the real EE's timing crystal does anyway)

	u64 hBlank = Scanline / 2;
	u64 hRender = Scanline - hBlank;

	if (!IsInterlacedVideoMode())
	{
		hBlank /= 2;
		hRender /= 2;
	}

	info->VerticalMilliHz = vertical_mHz;
	info->GSBlank = (u32)(GSBlank / 10000);
	info->Render = (u32)(Render / 10000);
	info->Blank = (u32)(Blank / 10000);

	info->hRender = (u32)(hRender / 10000);
	info->hBlank = (u32)(hBlank / 10000);
	info->hScanlinesPerFrame = scansPerFrame;

	if ((Render % 10000) >= 5000)
		info->Render++;
	if ((Blank % 10000) >= 5000)
		info->Blank++;

	if ((hRender % 10000) >= 5000)
		info->hRender++;
	if ((hBlank % 10000) >= 5000)
		info->hBlank++;

	info->hSyncError         = 0;

	// Calculate accumulative hSync rounding error per half-frame:
	if (IsInterlacedVideoMode()) // gets off the chart in that mode
	{
		u32 hSyncCycles  = ((info->hRender + info->hBlank) * scansPerFrame) / 2;
		u32 vSyncCycles  = (info->Render + info->Blank);
		info->hSyncError = vSyncCycles - hSyncCycles;
	}
	// Note: In NTSC modes there is some small rounding error in the vsync too,
	// however it would take thousands of frames for it to amount to anything and
	// is thus not worth the effort at this time.
}

u32 GetVerticalFrequencyMilliHz(void)
{
	// Note about NTSC/PAL "double strike" modes:
	// NTSC and PAL can be configured in such a way to produce a non-interlaced signal.
	// This involves modifying the signal slightly by either adding or subtracting a line (526/524 instead of 525)
	// which has the function of causing the odd and even fields to strike the same lines.
	// Doing this modifies the vertical refresh rate slightly. Beatmania is sensitive to this and
	// not accounting for it will cause the audio and video to become desynced.
	//
	// In the case of the GS, I believe it adds a halfline to the vertical back porch but more research is needed.
	// For now I'm just going to subtract off the config setting.
	//
	// According to the GS:
	// NTSC (interlaced): 59.94
	// NTSC (non-interlaced): 59.82
	// PAL (interlaced): 50.00
	// PAL (non-interlaced): 49.76
	//
	// More Information:
	// https://web.archive.org/web/20201031235528/https://wiki.nesdev.com/w/index.php/NTSC_video
	// https://web.archive.org/web/20201102100937/http://forums.nesdev.com/viewtopic.php?t=7909
	// https://web.archive.org/web/20120629231826fw_/http://ntsc-tv.com/index.html
	// https://web.archive.org/web/20200831051302/https://www.hdretrovision.com/240p/

	switch (gsVideoMode)
	{
		case GS_VideoMode::PAL:
		case GS_VideoMode::DVD_PAL:
		{
			// Config floats snap to the milli-hertz grid once, here; the
			// progressive "double strike" adjustment (-0.24 Hz) is then an
			// exact integer, where the old float subtraction put binary
			// noise into a rate Beatmania is documented sensitive to.
			const u32 mHz = (u32)lrintf(EmuConfig.GS.FrameratePAL * 1000.0f);
			return IsProgressiveVideoMode() ? mHz - 240 : mHz;
		}
		case GS_VideoMode::NTSC:
		case GS_VideoMode::DVD_NTSC:
		{
			const u32 mHz = (u32)lrintf(EmuConfig.GS.FramerateNTSC * 1000.0f);
			return IsProgressiveVideoMode() ? mHz - 110 : mHz;
		}
		case GS_VideoMode::HDTV_1080P:
		case GS_VideoMode::HDTV_1080I:
		case GS_VideoMode::HDTV_720P:
		case GS_VideoMode::SDTV_576P:
		case GS_VideoMode::VESA:
			return 60000;
		case GS_VideoMode::Uninitialized:
			/* SetGsCrt hasn't executed yet: seed from the boot region so
			 * pre-init cycle math is computed against the rate the BIOS
			 * would have programmed. */
			return BootRegionIsPAL() ? DEFAULT_FRAME_RATE_PAL_MHZ : DEFAULT_FRAME_RATE_NTSC_MHZ;
		case GS_VideoMode::SDTV_480P:
		default:
			// Pass NTSC vertical frequency value when unknown video mode is detected.
			break;
	}
	return 59940; // FRAMERATE_NTSC * 2
}

double GetVerticalFrequency(void)
{
	// Reporting/RTC convenience wrapper; core timing consumes the exact
	// milli-hertz value and never this double.
	return GetVerticalFrequencyMilliHz() / 1000.0;
}

void UpdateVSyncRate(bool force)
{
	// Notice:  (and I probably repeat this elsewhere, but it's worth repeating)
	//  The PS2's vsync timer is an *independent* crystal that is fixed to either 59.94 (NTSC)
	//  or 50.0 (PAL) Hz.  It has *nothing* to do with real TV timings or the real vsync of
	//  the GS's output circuit.  It is the same regardless if the GS is outputting interlace
	//  or progressive scan content.

	const u32 vertical_mHz = GetVerticalFrequencyMilliHz();

	if (vSyncInfo.VerticalMilliHz != vertical_mHz || vSyncInfo.VideoMode != gsVideoMode || force)
	{
		u32 total_scanlines = 0;

		switch (gsVideoMode)
		{
			case GS_VideoMode::PAL:
			case GS_VideoMode::DVD_PAL:
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_PAL_I;
				else
					total_scanlines = SCANLINES_TOTAL_PAL_NI;
				break;
			case GS_VideoMode::SDTV_480P:
			case GS_VideoMode::SDTV_576P:
			case GS_VideoMode::HDTV_720P:
			case GS_VideoMode::VESA:
				total_scanlines = SCANLINES_TOTAL_NTSC_I;
				break;
			case GS_VideoMode::HDTV_1080P:
			case GS_VideoMode::HDTV_1080I:
				total_scanlines = SCANLINES_TOTAL_1080;
				break;
			case GS_VideoMode::Uninitialized: // SYSCALL instruction hasn't executed yet, give some temporary values.
				if (BootRegionIsPAL())
					total_scanlines = gsIsInterlaced ? SCANLINES_TOTAL_PAL_I : SCANLINES_TOTAL_PAL_NI;
				else
					total_scanlines = gsIsInterlaced ? SCANLINES_TOTAL_NTSC_I : SCANLINES_TOTAL_NTSC_NI;
				break;
			case GS_VideoMode::Unknown:
			case GS_VideoMode::NTSC:
			case GS_VideoMode::DVD_NTSC:
			default:
				if (gsIsInterlaced)
					total_scanlines = SCANLINES_TOTAL_NTSC_I;
				else
					total_scanlines = SCANLINES_TOTAL_NTSC_NI;
				break;
		}

		const bool video_mode_initialized = gsVideoMode != GS_VideoMode::Uninitialized;

		// NBA Jam 2004 PAL will fail to display 3D on the menu if this value isn't correct on reset.
		if (video_mode_initialized && vSyncInfo.VideoMode != gsVideoMode)
			gsCSRset(GS_CSR_FIELD);

		vSyncInfo.VideoMode = gsVideoMode;

		vSyncInfoCalc(&vSyncInfo, vertical_mHz, total_scanlines);

		hsyncCounter.deltaCycles = (hsyncCounter.Mode == MODE_HBLANK) ? vSyncInfo.hBlank : vSyncInfo.hRender;
		vsyncCounter.deltaCycles = (vsyncCounter.Mode == MODE_GSBLANK) ?
								  vSyncInfo.GSBlank :
								  ((vsyncCounter.Mode == MODE_VSYNC) ? vSyncInfo.Blank : vSyncInfo.Render);
		cpuRcntSet();
#ifdef __LIBRETRO__
		pending_update_av_info = true;
#endif
	}
}

static __fi void _cpuTestTarget(int i)
{
	if (counters[i].count < counters[i].target)
		return;

	if (counters[i].mode.TargetInterrupt)
	{
		if (!counters[i].mode.TargetReached)
		{
			counters[i].mode.TargetReached = 1;
			hwIntcIrq(counters[i].interrupt);
		}
	}

	if (counters[i].mode.ZeroReturn)
		counters[i].count -= counters[i].target; // Reset on target
	else
		counters[i].target |= EECNT_FUTURE_TARGET; // OR with future target to prevent a retrigger
}

static __fi void _cpuTestOverflow(int i)
{
	if (counters[i].count <= 0xffff)
		return;

	if (counters[i].mode.OverflowInterrupt)
	{
		if (!counters[i].mode.OverflowReached)
		{
			counters[i].mode.OverflowReached = 1;
			hwIntcIrq(counters[i].interrupt);
		}
	}

	// wrap counter back around zero, and enable the future target:
	counters[i].count -= 0x10000;
	counters[i].target &= 0xffff;
}

static __fi u32 rcntRcount_inl(int index);

// mode - 0 means hblank source, 8 means vblank source.
static __fi void rcntStartGate(bool isVblank, u64 sCycle)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		//if ((mode == 0) && ((counters[i].mode & 0x83) == 0x83))
		if (!isVblank && counters[i].mode.IsCounting && (counters[i].mode.ClockSource == 3))
		{
			// Update counters using the hblank as the clock.  This keeps the hblank source
			// nicely in sync with the counters and serves as an optimization also, since these
			// counter won't receive special rcntUpdate scheduling.

			// Note: Target and overflow tests must be done here since they won't be done
			// currectly by rcntUpdate (since it's not being scheduled for these counters)

			counters[i].count += HBLANK_COUNTER_SPEED;
			_cpuTestOverflow(i);
			_cpuTestTarget(i);
		}

		if (!(gates & (1 << i)))
			continue;
		if ((!!counters[i].mode.GateSource) != isVblank)
			continue;

		switch (counters[i].mode.GateMode)
		{
			case 0x0: //Count When Signal is low (off)

				// Just set the start cycle -- counting will be done as needed
				// for events (overflows, targets, mode changes, and the gate off below)

				counters[i].count = rcntRcount_inl(i);
				counters[i].mode.IsCounting = 0;
				counters[i].startCycle = sCycle;
				break;

			case 0x2:	// reset and start counting on vsync end
				// this is the vsync start so do nothing.
				break;

			case 0x1: //Reset and start counting on Vsync start
			case 0x3: //Reset and start counting on Vsync start and end
				counters[i].mode.IsCounting = 1;
				counters[i].count = 0;
				counters[i].target &= 0xffff;
				counters[i].startCycle = sCycle;
				break;
		}
	}

	// No need to update actual counts here.  Counts are calculated as needed by reads to
	// rcntRcount().  And so long as startCycle is set properly, any targets or overflows
	// will be scheduled and handled.

	// Note: No need to set counters here.  They'll get set when control returns to
	// rcntUpdate, since we're being called from there anyway.
}


static __fi void VSyncStart(u64 sCycle)
{
	int i;
	for (i = 0; static_cast<size_t>(i) < Patch.size(); i++)
	{
		int _place = Patch[i].placetopatch;
		if ( (_place == PPT_CONTINUOUSLY)
		  || (_place == PPT_COMBINED_0_1))
			_ApplyPatch(&Patch[i]);
	}

	//These are done at VSync Start.  Drawing is done when VSync is off, then output the screen when Vsync is on
	//The GS needs to be told at the start of a vsync else it loses half of its picture (could be responsible for some halfscreen issues)
	//We got away with it before i think due to our awful GS timing, but now we have it right (ish)
	MTGS::PostVsyncStart();
	/* A pause stops the EE only at a vsync a drain consumed: MTGSOwner.h. */
	if (MTGS_EE_STOPS(VMManager::Internal::IsExecutionInterrupted(),
			VMManager::GetState() == VMState::Paused, mtgs_vsync_drained))
		Cpu->ExitExecution();

	hwIntcIrq(INTC_VBLANK_S);
	psxVBlankStart();

	if (gates)
		rcntStartGate(true, sCycle); // Counters Start Gate code
}

static __fi void GSVSync(void)
{
	// CSR is swapped and GS vBlank IRQ is triggered roughly 3.5 hblanks after VSync Start

	if (IsProgressiveVideoMode())
		gsCSRset(GS_CSR_FIELD);  /* SetField  */
	else
		gsCSRflip(GS_CSR_FIELD); /* SwapField */

	if (!(gsCSRload() & GS_CSR_VSINT))
	{
		gsCSRset(GS_CSR_VSINT);
		if (!GSIMR.VSMSK)
			hwIntcIrq(INTC_GS);
	}
}

// mode - 0 means hblank signal, 8 means vblank signal.
static __fi void rcntEndGate(bool isVblank, u64 sCycle)
{
	int i;

	for (i = 0; i < 4; i++)
	{
		//Gates for counters
		if (!(gates & (1 << i)))
			continue;
		if ((!!counters[i].mode.GateSource) != isVblank)
			continue;

		switch (counters[i].mode.GateMode)
		{
			case 0x0: //Count When Signal is low (off)

				// Set the count here.  Since the timer is being turned off it's
				// important to record its count at this point (it won't be counted by
				// calls to rcntUpdate).
				counters[i].mode.IsCounting = 1;
				counters[i].startCycle = cpuRegs.cycle;
				break;

			case 0x1: // Reset and start counting on Vsync start
				  // this is the vsync end so do nothing
				break;

			case 0x2: //Reset and start counting on Vsync end
			case 0x3: //Reset and start counting on Vsync start and end
				counters[i].mode.IsCounting = 1;
				counters[i].count = 0;
				counters[i].target &= 0xffff;
				counters[i].startCycle = sCycle;
				break;
		}
	}
	// Note: No need to set counters here.  They'll get set when control returns to
	// rcntUpdate, since we're being called from there anyway.
}

static __fi void VSyncEnd(u64 sCycle)
{
	hwIntcIrq(INTC_VBLANK_E); // HW Irq
	psxVBlankEnd(); // psxCounters vBlank End
	if (gates)
		rcntEndGate(true, sCycle); // Counters End Gate Code
}

static __fi void rcntUpdate_hScanline(void)
{
	if (hsyncCounter.Mode == MODE_HBLANK) // HBLANK Start
	{
		// Setup the hRender's start and end cycle information:
		hsyncCounter.startCycle += vSyncInfo.hBlank; // start  (absolute cycle value)
		hsyncCounter.deltaCycles = vSyncInfo.hRender; // endpoint (delta from start value)
		hsyncCounter.Mode = MODE_HRENDER;

		rcntStartGate(false, hsyncCounter.startCycle);
		psxCheckStartGate16(0);
	}
	else
	{ //HBLANK END / HRENDER Begin
		if (!(gsCSRload() & GS_CSR_HSINT))
		{
			gsCSRset(GS_CSR_HSINT);
			if (!GSIMR.HSMSK)
				hwIntcIrq(INTC_GS);
		}

		// set up the hblank's start and end cycle information:
		hsyncCounter.startCycle += vSyncInfo.hRender; // start (absolute cycle value)
		hsyncCounter.deltaCycles = vSyncInfo.hBlank;	// endpoint (delta from start value)
		hsyncCounter.Mode = MODE_HBLANK;

		if (gates)
			rcntEndGate(false, hsyncCounter.startCycle);
		if (psxhblankgate)
			psxCheckEndGate16(0);
	}
}

static __fi void rcntUpdate_vSync(void)
{
	if (vsyncCounter.Mode == MODE_VSYNC)
	{
		vsyncCounter.startCycle += vSyncInfo.Blank;
		vsyncCounter.deltaCycles = vSyncInfo.Render;
		vsyncCounter.Mode = MODE_VRENDER;

		VSyncEnd(vsyncCounter.startCycle);
	}
	else if (vsyncCounter.Mode == MODE_GSBLANK) // GS CSR Swap and interrupt
	{
		GSVSync();

		vsyncCounter.Mode = MODE_VSYNC;
		// Don't set the start cycle, makes it easier to calculate the correct Vsync End time
		vsyncCounter.deltaCycles = vSyncInfo.Blank;
	}
	else	// VSYNC end / VRENDER begin
	{
		vsyncCounter.startCycle += vSyncInfo.Render;
		vsyncCounter.deltaCycles = vSyncInfo.GSBlank;
		vsyncCounter.Mode = MODE_GSBLANK;

		// Accumulate hsync rounding errors:
		hsyncCounter.startCycle += vSyncInfo.hSyncError;

		VSyncStart(vsyncCounter.startCycle);
	}
}

void rcntUpdate(void)
{
	if (cpuTestCycle(vsyncCounter.startCycle, vsyncCounter.deltaCycles))
		rcntUpdate_vSync();
	// HBlank after as VSync can do error compensation
	if (cpuTestCycle(hsyncCounter.startCycle, hsyncCounter.deltaCycles))
		rcntUpdate_hScanline();

	// Update counters so that we can perform overflow and target tests.

	for (int i = 0; i < 4; i++)
	{
		// We want to count gated counters (except the hblank which exclude below, and are
		// counted by the hblank timer instead)

		if (!counters[i].mode.IsCounting)
			continue;

		if (counters[i].mode.ClockSource != 0x3) // don't count hblank sources
		{
			const u32 change = (cpuRegs.cycle - counters[i].startCycle) / counters[i].rate;
			counters[i].count += change;
			counters[i].startCycle += change * counters[i].rate;

			// Check Counter Targets and Overflows:
			// Check Overflow first, in case the target is 0
			_cpuTestOverflow(i);
			_cpuTestTarget(i);
		}
		else
			counters[i].startCycle = cpuRegs.cycle;
	}

	cpuRcntSet();
}

static __fi void _rcntSetGate(int index)
{
	if (counters[index].mode.EnableGate)
	{
		// If the Gate Source is hblank and the clock selection is also hblank
		// then the gate is disabled and the counter acts as a normal hblank source.

		if (!(counters[index].mode.GateSource == 0 && counters[index].mode.ClockSource == 3))
		{
			gates |= (1 << index);
			// FIXME: Test required - should the counter be stopped here? I feel like it should only stop and reset on the gate signal happening.
			counters[index].mode.IsCounting = 0;
			return;
		}
	}

	gates &= ~(1 << index);
}

static __fi void rcntWmode(int index, u32 value)
{
	if (counters[index].mode.IsCounting)
	{
		if (counters[index].mode.ClockSource != 0x3)
		{
			const u32 change = (cpuRegs.cycle - counters[index].startCycle) / counters[index].rate;
			counters[index].count += change;
			counters[index].startCycle += change * counters[index].rate;
		}
	}
	else
		counters[index].startCycle = cpuRegs.cycle;

	// Clear OverflowReached and TargetReached flags (0xc00 mask), but *only* if they are set to 1 in the
	// given value.  (yes, the bits are cleared when written with '1's).

	counters[index].modeval &= ~(value & 0xc00);
	counters[index].modeval = (counters[index].modeval & 0xc00) | (value & 0x3ff);

	//Clock rate divisers *2, they use BUSCLK speed not PS2CLK
	switch (counters[index].mode.ClockSource)
	{
		case 0: counters[index].rate = 2; break;
		case 1: counters[index].rate = 32; break;
		case 2: counters[index].rate = 512; break;
		case 3: counters[index].rate = vSyncInfo.hBlank+vSyncInfo.hRender; break;
	}

	_rcntSetGate(index);
	_rcntSet(index);
}

static __fi void rcntWcount(int index, u32 value)
{
	// re-calculate the start cycle of the counter based on elapsed time since the last counter update:
	if (counters[index].mode.IsCounting)
	{
		if (counters[index].mode.ClockSource != 0x3)
		{
			const u32 change = (cpuRegs.cycle - counters[index].startCycle) / counters[index].rate;
			counters[index].startCycle += change * counters[index].rate;
		}
	}
	else
		counters[index].startCycle = cpuRegs.cycle;

	counters[index].count = value & 0xffff;

	// reset the target, and make sure we don't get a premature target.
	counters[index].target &= 0xffff;

	if (counters[index].count > counters[index].target)
		counters[index].target |= EECNT_FUTURE_TARGET;

	_rcntSet(index);
}

static __fi void rcntWtarget(int index, u32 value)
{
	counters[index].target = value & 0xffff;

	// guard against premature (instant) targeting.
	// If the target is behind the current count, set it up so that the counter must
	// overflow first before the target fires:

	if (counters[index].mode.IsCounting)
	{
		if (counters[index].mode.ClockSource != 0x3)
		{
			const u32 change = (cpuRegs.cycle - counters[index].startCycle) / counters[index].rate;
			counters[index].count += change;
			counters[index].startCycle += change * counters[index].rate;
		}
	}

	if (counters[index].target <= counters[index].count)
		counters[index].target |= EECNT_FUTURE_TARGET;

	_rcntSet(index);
}

#define rcntWhold(index, value) (counters[(index)].hold = (value))

static __fi u32 rcntRcount_inl(int index)
{
	u32 ret;

	// only count if the counter is turned on (0x80) and is not an hsync gate (!0x03)
	if (counters[index].mode.IsCounting && (counters[index].mode.ClockSource != 0x3))
		ret = counters[index].count + ((cpuRegs.cycle - counters[index].startCycle) / counters[index].rate);
	else
		ret = counters[index].count;

	// Never show the guest a boundary crossing -- a wrap, or a target reset --
	// before the interrupt for it has actually been delivered. On hardware the
	// crossing and the interrupt are the same edge, so with interrupts enabled
	// the handler preempts before any later read can run, and a wrapped count
	// paired with pre-overflow ISR state is not an observable combination.
	// Under the recompilers that window is real, in two phases: the count is
	// derived from the live cpuRegs.cycle and can cross before the scheduled
	// rcntUpdate event runs, and after rcntUpdate has wrapped it and raised
	// the INTC the exception still waits for the next event test, which
	// short-block tails can defer past the reader's whole load sequence.
	//
	// A game keeping a 64-bit clock as an overflow-ISR wrap accumulator plus a
	// live COUNT read then sees time move backwards by a wrap period. Clamp to
	// just before the boundary until delivery. The deliverability guard keeps
	// this exact: with interrupts blocked (EXL/ERL, or inside the handler) or
	// the source masked, the wrapped count stays observable as it is on
	// hardware.
	const u32 target = counters[index].target & 0xffff;
	const bool intc_pending_delivery =
		(psHu32(INTC_STAT) & psHu32(INTC_MASK) & (1u << counters[index].interrupt)) &&
		(cpuRegs.CP0.n.Status.val & 0x400) &&
		cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && !cpuRegs.CP0.n.Status.b.ERL;

	if (counters[index].mode.ZeroReturn)
	{
		if (target != 0 && (ret >= target || (counters[index].mode.TargetReached && intc_pending_delivery)))
			ret = target - 1;
	}
	else if (ret > 0xffff || (counters[index].mode.OverflowReached && intc_pending_delivery))
	{
		ret = 0xffff;
	}

	return ret;
}

u32 rcntRcount(int index)
{
	return rcntRcount_inl(index);
}

u16 rcntRead32(u32 mem)
{
	// Important DevNote:
	// Yes this uses a u16 return value on purpose!  The upper bits 16 of the counter registers
	// are all fixed to 0, so we always truncate everything in these two pages using a u16
	// return value! --air

	switch(mem)
	{
		case(RCNT0_COUNT):
			return (u16)rcntRcount_inl(0);
		case(RCNT0_MODE):
			return (u16)counters[0].modeval;
		case(RCNT0_TARGET):
			return (u16)counters[0].target;
		case(RCNT0_HOLD):
			return (u16)counters[0].hold;

		case(RCNT1_COUNT):
			return (u16)rcntRcount_inl(1);
		case(RCNT1_MODE):
			return (u16)counters[1].modeval;
		case(RCNT1_TARGET):
			return (u16)counters[1].target;
		case(RCNT1_HOLD):
			return (u16)counters[1].hold;

		case(RCNT2_COUNT):
			return (u16)rcntRcount_inl(2);
		case(RCNT2_MODE):
			return (u16)counters[2].modeval;
		case(RCNT2_TARGET):
			return (u16)counters[2].target;

		case(RCNT3_COUNT):
			return (u16)rcntRcount_inl(3);
		case(RCNT3_MODE):
			return (u16)counters[3].modeval;
		case(RCNT3_TARGET):
			return (u16)counters[3].target;
	}

	return psHu16(mem);
}

bool rcntWrite32(u32 mem, mem32_t *value)
{
	// [TODO] : counters should actually just use the EE's hw register space for storing
	// count, mode, target, and hold. This will allow for a simplified handler for register
	// reads.

	switch(mem)
	{
		case(RCNT0_COUNT):
			return rcntWcount(0, *value), false;
		case(RCNT0_MODE):
			return rcntWmode(0, *value), false;
		case(RCNT0_TARGET):
			return rcntWtarget(0, *value), false;
		case(RCNT0_HOLD):
			return rcntWhold(0, *value), false;

		case(RCNT1_COUNT):
			return rcntWcount(1, *value), false;
		case(RCNT1_MODE):
			return rcntWmode(1, *value), false;
		case(RCNT1_TARGET):
			return rcntWtarget(1, *value), false;
		case(RCNT1_HOLD):
			return rcntWhold(1, *value), false;

		case(RCNT2_COUNT):
			return rcntWcount(2, *value), false;
		case(RCNT2_MODE):
			return rcntWmode(2, *value), false;
		case(RCNT2_TARGET):
			return rcntWtarget(2, *value), false;

		case(RCNT3_COUNT):
			return rcntWcount(3, *value), false;
		case(RCNT3_MODE):
			return rcntWmode(3, *value), false;
		case(RCNT3_TARGET):
			return rcntWtarget(3, *value), false;
	}

	// unhandled .. do memory writeback.
	return true;
}

bool rcntFreeze(SaveStateBase *s)
{
	SaveState_Freeze(s, counters);
	SaveState_Freeze(s, hsyncCounter);
	SaveState_Freeze(s, vsyncCounter);
	SaveState_Freeze(s, nextDeltaCounter);
	SaveState_Freeze(s, nextStartCounter);
	SaveState_Freeze(s, vSyncInfo);
	SaveState_Freeze(s, gsVideoMode);
	SaveState_Freeze(s, gsIsInterlaced);
	SaveState_Freeze(s, gates);

	if (SaveState_IsLoading(s))
	{
		int i;

		/* rate is a divisor -- rcntRcount and the update path all divide
		 * the elapsed cycles by it -- and it is never written directly,
		 * only derived from the mode register's clock source. Restoring
		 * it raw allows a zero, which is a trap on x86 the moment the
		 * counters next tick. Derive it again from the mode that came
		 * back with it, exactly as rcntWmode does. */
		for (i = 0; i < 4; i++)
		{
			switch (counters[i].mode.ClockSource)
			{
				case 0:  counters[i].rate = 2; break;
				case 1:  counters[i].rate = 32; break;
				case 2:  counters[i].rate = 512; break;
				default: counters[i].rate = vSyncInfo.hBlank + vSyncInfo.hRender; break;
			}
			if (!counters[i].rate)
				counters[i].rate = 2;
		}
	}

	return SaveState_IsOkay(s);
}

/* Re-seed the pre-init counter timing once the disc region is known.
 * VMManager::Initialize computes the first vsync rate before the CDVD
 * reset path parses SYSTEM.CNF, so a PAL disc fast-booted on an NTSC
 * BIOS would otherwise keep the NTSC seed until SetGsCrt.  The rate
 * change guard inside UpdateVSyncRate makes this a no-op whenever the
 * seed did not actually move, and once SetGsCrt has run the video mode
 * is no longer Uninitialized and the seed is irrelevant. */
void rcntRefreshBootTiming(void)
{
	if (gsVideoMode == GS_VideoMode::Uninitialized)
		UpdateVSyncRate(false);
}
