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

#pragma once

#include <retro_atomic.h>
#include "Common.h"
#include "Gif.h"
#include "GS/GS.h"

#include "../common/SingleRegisterTypes.h"
#include "GSmmio.h"

extern double GetVerticalFrequency();
#ifdef __cplusplus
extern "C"
#endif
u32 GetVerticalFrequencyMilliHz(void);

// --------------------------------------------------------------------------------------
//  tGS_CSR
// --------------------------------------------------------------------------------------
// This is the Control Register for the GS.  It is a dual-instance register that returns
// distinctly different values for most fields when read and written.  In PCSX2 we house
// the written version in the gsRegs buffer, and generate the readback version on-demand
// from various other PCSX2 system statuses.
union tGS_CSR
{
	struct
	{
		// Write:
		//   0 - No action;
		//   1 - Old event is cleared and event is enabled.
		// Read:
		//   0 - No SIGNAL pending.
		//   1 - SIGNAL has been generated.
		u64 SIGNAL : 1;

		// Write:
		//   0 - No action;
		//   1 - FINISH event is enabled.
		// Read:
		//   0 - No FINISH event pending.
		//   1 - FINISH event has been generated.
		u64 FINISH : 1;

		// Hsync Interrupt Control
		// Write:
		//   0 - No action;
		//   1 - Hsync interrupt is enabled.
		// Read:
		//   0 - No Hsync interrupt pending.
		//   1 - Hsync interrupt has been generated.
		u64 HSINT : 1;

		// Vsync Interrupt Control
		// Write:
		//   0 - No action;
		//   1 - Vsync interrupt is enabled.
		// Read:
		//   0 - No Vsync interrupt pending.
		//   1 - Vsync interrupt has been generated.
		u64 VSINT : 1;

		// Rect Area Write Termination Control
		//   0 - No action;
		//   1 - Rect area write interrupt is enabled.
		// Read:
		//   0 - No RAWrite interrupt pending.
		//   1 - RAWrite interrupt has been generated.
		u64 EDWINT : 1;

		u64 _zero1 : 1;
		u64 _zero2 : 1;
		u64 pad1 : 1;

		// FLUSH  (write-only!)
		// Write:
		//   0 - Resume drawing if suspended (?)
		//   1 - Flush the GS FIFO and suspend drawing
		// Read: Always returns 0. (?)
		u64 FLUSH : 1;

		// RESET (write-only!)
		// Write:
		//   0 - Do nothing.
		//   1 - GS soft system reset.  Clears FIFOs and resets IMR to all 1's.
		//       (PCSX2 implementation also clears GIFpaths, though that behavior may differ on real HW).
		// Read: Always returns 0. (?)
		u64 RESET : 1;

		u64 _pad2 : 2;

		// (I have no idea what this reg is-- air)
		// Output value is updated by sampling the VSync. (?)
		u64 NFIELD : 1;

		// Current Field of Display [page flipping] (read-only?)
		//  0 - EVEN
		//  1 - ODD
		u64 FIELD : 1;

		// GS FIFO Status (read-only)
		//  00 - Somewhere in between
		//  01 - Empty
		//  10 - Almost Full
		//  11 - Reserved (unused)
		// Assign values using the CSR_FifoState enum.
		u64 FIFO : 2;

		// Revision number of the GS (fairly arbitrary)
		u64 REV : 8;

		// ID of the GS (also fairly arbitrary)
		u64 ID : 8;
	};

	u64 _u64;

	struct
	{
		u32 _u32; // lower 32 bits (all useful content!)
		u32 _unused32; // upper 32 bits (unused -- should probably be 0)
	};
};

// --------------------------------------------------------------------------------------
//  tGS_IMR
// --------------------------------------------------------------------------------------
union tGS_IMR
{
	struct
	{
		u32 _reserved1 : 8;
		u32 SIGMSK : 1; // Signal evevnt interrupt mask
		u32 FINISHMSK : 1; // Finish event interrupt mask
		u32 HSMSK : 1; // HSync interrupt mask
		u32 VSMSK : 1; // VSync interrupt mask
		u32 EDWMSK : 1; // Rectangle write termination interrupt mask
		u32 _undefined : 2; // undefined bits should be set to 1.
		u32 _reserved2 : 17;
	};
	u32 _u32;
};

// --------------------------------------------------------------------------------------
//  GSRegSIGBLID
// --------------------------------------------------------------------------------------
struct GSRegSIGBLID
{
	u32 SIGID;
	u32 LBLID;
};


#define CSRreg ((tGS_CSR&)*(PS2MEM_GS + 0x1000))

#define GSCSRr ((u32&)*(PS2MEM_GS + 0x1000))

#define GSIMR ((tGS_IMR&)*(PS2MEM_GS + 0x1010))
#define GSSIGLBLID ((GSRegSIGBLID&)*(PS2MEM_GS + 0x1080))

#include "GSVideoMode.h"

/////////////////////////////////////////////////////////////////////////////
// MTGS Threaded Class Declaration

enum MTGS_RingCommand
{
	GS_RINGTYPE_VSYNC,
	GS_RINGTYPE_FREEZE,
	GS_RINGTYPE_RESET, // issues a GSreset() command.
	GS_RINGTYPE_GSPACKET,
	GS_RINGTYPE_MTVU_GSPACKET,
	GS_RINGTYPE_INIT_AND_READ_FIFO
};


struct MTGS_FreezeData
{
	freezeData* fdata;
};

// --------------------------------------------------------------------------------------
// MTGS
// --------------------------------------------------------------------------------------
/* A hardware context that has to be taken before the GS touches it and
 * given back afterwards. NULL when the context in use needs no such
 * thing, which is every context but one: whoever needs it installs the
 * pair, and everyone who brackets GS work tests the pointer and calls
 * nothing otherwise. Installed and cleared together. */
extern void (*gs_hw_context_begin)(void);
extern void (*gs_hw_context_end)(void);

#define GS_HW_CONTEXT_BEGIN() do { if (gs_hw_context_begin) gs_hw_context_begin(); } while (0)
#define GS_HW_CONTEXT_END()   do { if (gs_hw_context_end)   gs_hw_context_end();   } while (0)

namespace MTGS
{
	bool IsOpen();

	// Waits for the GS to empty out the entire ring buffer contents.
	void WaitGS(bool isMTVU);
	void ResetGS(bool hardware_reset);

	void WaitForClose();
	void Freeze(FreezeAction mode, MTGS_FreezeData& data);

	void PostVsyncStart();
	void InitAndReadFIFO(u8* mem, u32 qwc);

	/* Returns false only when the bounded libretro-thread wait timed
	 * out before a vsync was consumed; flush_all callers always get
	 * true. */
	bool MainLoop(bool flush_all);

	void ApplySettings();

	void TryOpenGS(void);
	void CloseGS(void);
};

/////////////////////////////////////////////////////////////////////////////
// Generalized GS Functions and Stuff

/* GS.cpp; hwReset (Hw.c) calls it. */
#ifdef __cplusplus
extern "C" {
#endif
extern void gsReset(void);
#ifdef __cplusplus
}
#endif

extern retro_atomic_int_t s_GSRegistersWritten;

// Size of the ringbuffer as a power of 2 -- size is a multiple of simd128s.
// MTVU_GSPACK_QUEUE_RECORDS, the MTVU packet queue's size, is in Gif_Unit.h.

// MTGS command-ring size in u128 qwords (must be a power of two).
//
// The upstream 8 MiB ring is sized for a free-running GS thread that can lag
// the EE by many frames. In the libretro topology MTGS::PostVsyncStart blocks
// the EE on WaitGS every vsync, so the ring never accumulates more than roughly
// a frame of packets. Measured peak occupancy on real content: ~22k qwords in
// the worst case (Shadow of the Colossus colossus fights); Tekken Tag, DMC3 and
// DoA2 all stayed under ~3.1k. 65536 (1 MiB) gives ~3x headroom over the worst
// observed peak while reclaiming 7 MiB of static storage vs the old 8 MiB ring.
#define MTGS_RINGBUFFERSIZE 65536

