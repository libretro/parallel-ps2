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

#include <retro_atomic.h>
#include "Common.h"

#include <cstring> /* memset */
#include <list>

#include "Gif_Unit.h"
#include "Counters.h"
#include "Config.h"

alignas(16) u8 g_RealGSMem[Ps2MemSize::GSregs];
retro_atomic_int_t s_GSRegistersWritten = RETRO_ATOMIC_INT_INITIALIZER(0);

void gsSetVideoMode(GS_VideoMode mode)
{
	gsVideoMode = mode;
	UpdateVSyncRate(false);
}

// Make sure framelimiter options are in sync with GS capabilities.
void gsReset(void)
{
	MTGS::ResetGS(true);
	gsVideoMode = GS_VideoMode::Uninitialized;
	memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	UpdateVSyncRate(true);
}

static __fi void gsCSRwrite( const tGS_CSR& csr )
{
	if (csr.RESET) {
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.gsFINISH.gsFINISHFired = true;
		gifUnit.gsFINISH.gsFINISHPending = false;
		// Privilage registers also reset.
		memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
		GSIMR._u32       = 0;
		GSIMR.SIGMSK     = true;
		GSIMR.FINISHMSK  = true;
		GSIMR.HSMSK      = true;
		GSIMR.VSMSK      = true;
		GSIMR.EDWMSK     = true;
		GSIMR._undefined = 0x3;
		gsCSRfield(GS_CSR_FIFO, (u32)CSR_FIFO_EMPTY << GS_CSR_FIFO_SH);
		gsCSRfield(GS_CSR_REV, 0x1Bu << GS_CSR_REV_SH);
		gsCSRfield(GS_CSR_ID, 0x55u << GS_CSR_ID_SH);
		MTGS::ResetGS(false);
	}

	if(csr.SIGNAL)
	{
		// SIGNAL : What's not known here is whether or not the SIGID register should be updated
		//  here or when the IMR is cleared (below).
		if (gifUnit.gsSIGNAL.queued) {
			/* Firing pending signal */
			GSSIGLBLID.SIGID = (GSSIGLBLID.SIGID & ~gifUnit.gsSIGNAL.data[1])
				        | (gifUnit.gsSIGNAL.data[0]&gifUnit.gsSIGNAL.data[1]);

			if (!GSIMR.SIGMSK)
				hwIntcIrq(INTC_GS);
			gsCSRset(GS_CSR_SIGNAL); // Just to be sure :p
		}
		else gsCSRclear(GS_CSR_SIGNAL);
		gifUnit.gsSIGNAL.queued = false;
		gifUnit.Execute(false, true); // Resume paused transfers
	}

	if (csr.FINISH)	{
		gsCSRclear(GS_CSR_FINISH);
		gifUnit.gsFINISH.gsFINISHFired = false; //Clear the previously fired FINISH (YS, Indiecar 2005, MGS3)
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if(csr.HSINT)	gsCSRclear(GS_CSR_HSINT);
	if(csr.VSINT)	gsCSRclear(GS_CSR_VSINT);
	if(csr.EDWINT)	gsCSRclear(GS_CSR_EDWINT);
}

static __fi void IMRwrite(u32 value)
{
	if ((gsCSRload() & 0x1f) & (~value & GSIMR._u32) >> 8)
		hwIntcIrq(INTC_GS);

	GSIMR._u32 = (value & 0x1f00)|0x6000;
}

void gsWrite8(u32 mem, u8 value)
{
	tGS_CSR tmp;
	tmp.FIFO = CSR_FIFO_EMPTY;
	tmp.REV  = 0x1B;
	tmp.ID   = 0x55;
	tmp._u32 = value;
	switch (mem)
	{
		// CSR 8-bit write handlers.
		// I'm quite sure these would just write the CSR portion with the other
		// bits set to 0 (no action).  The previous implementation masked the 8-bit
		// write value against the previous CSR write value, but that really doesn't
		// make any sense, given that the real hardware's CSR circuit probably has no
		// real "memory" where it saves anything.  (for example, you can't write to
		// and change the GS revision or ID portions -- they're all hard wired.) --air

		case GS_CSR: // GS_CSR
		case GS_CSR + 1: // GS_CSR
		case GS_CSR + 2: // GS_CSR
		case GS_CSR + 3: // GS_CSR
			tmp._u32 <<= ((mem - GS_CSR) << 3);
			gsCSRwrite(tmp);
			break;
		default:
			*PS2GS_BASE(mem) = value;
		break;
	}
}

//////////////////////////////////////////////////////////////////////////
// GS Write 16 bit

void gsWrite16(u32 mem, u16 value)
{
	tGS_CSR tmp;
	tmp.FIFO = CSR_FIFO_EMPTY;
	tmp.REV  = 0x1B;
	tmp.ID   = 0x55;
	tmp._u32 = value;
	switch (mem)
	{
		// See note above about CSR 8 bit writes, and handling them as zero'd bits
		// for all but the written parts.
		
		case GS_CSR+2:
			tmp._u32 <<= 16;
			// fallthrough
		case GS_CSR:
			gsCSRwrite(tmp);
			return; // do not write to MTGS memory
		case GS_IMR:
			IMRwrite(value);
			return; // do not write to MTGS memory
	}

	*(u16*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 32 bit

void gsWrite32(u32 mem, u32 value)
{
	tGS_CSR tmp;
	tmp.FIFO = CSR_FIFO_EMPTY;
	tmp.REV  = 0x1B;
	tmp.ID   = 0x55;

	switch (mem)
	{
		case GS_CSR:
			tmp._u32 = value;
			gsCSRwrite(tmp);
			return;

		case GS_IMR:
			IMRwrite(value);
			return;
	}

	*(u32*)PS2GS_BASE(mem) = value;
}

//////////////////////////////////////////////////////////////////////////
// GS Write 64 bit

void gsWrite64_generic(u32 mem, u64 value)
{
	/* Release store, paired with the acquire loads GSState uses to read
	 * these registers from the GS thread.  The registers are 8-byte
	 * aligned within g_RealGSMem, so this is a single store either way -
	 * what changes is that a concurrent reader now sees one whole
	 * written value instead of a byte-wise copy it can observe halfway
	 * through. */
	u8* const dst = PS2GS_BASE(mem);
#if defined(RETRO_ATOMIC_HAS_64)
	if (((uptr)dst & 7) == 0)
		retro_atomic_store_release_64((retro_atomic_64_t*)dst, (int64_t)value);
	else
#endif
		memcpy(dst, &value, sizeof(value));
}

void gsWrite64_page_00(u32 mem, u64 value)
{
	if (mem == GS_DISPFB1 || mem == GS_DISPFB2 || mem == GS_PMODE)
		retro_atomic_store_release_int(&s_GSRegistersWritten, 1);

	if (mem == GS_SMODE1 || mem == GS_SMODE2)
	{
		if (value != *(u64*)PS2GS_BASE(mem))
			UpdateVSyncRate(false);
	}

	gsWrite64_generic( mem, value );
}

void gsWrite64_page_01(u32 mem, u64 value)
{
	tGS_CSR tmp;
	tmp.FIFO = CSR_FIFO_EMPTY;
	tmp.REV  = 0x1B;
	tmp.ID   = 0x55;

	switch( mem )
	{
		case GS_BUSDIR:

			gifUnit.stat.DIR = static_cast<u32>(value) & 1;
			if (gifUnit.stat.DIR) {      // Assume will do local->host transfer
				gifUnit.stat.OPH = true; // Should we set OPH here?
				gifUnit.FlushToMTGS();   // Send any pending GS Primitives to the GS
			}

			gsWrite64_generic( mem, value );
			return;

		case GS_CSR:
			tmp._u64 = value;
			gsCSRwrite(tmp);
			return;

		case GS_IMR:
			IMRwrite(static_cast<u32>(value));
			return;
	}

	gsWrite64_generic( mem, value );
}

//////////////////////////////////////////////////////////////////////////
// GS Write 128 bit

#if PCSX2_MINGW_R128_BY_PTR
void gsWrite128_page_01(u32 mem, const r128* value_ptr)
{
	const r128 value = r128_load(value_ptr);
#else
void TAKES_R128 gsWrite128_page_01(u32 mem, r128 value)
{
#endif
	tGS_CSR tmp;
	tmp.FIFO = CSR_FIFO_EMPTY;
	tmp.REV  = 0x1B;
	tmp.ID   = 0x55;

	switch( mem )
	{
		case GS_CSR:
			tmp._u32 = r128_to_u32(value);
			gsCSRwrite(tmp);
			return;

		case GS_IMR:
			IMRwrite(r128_to_u32(value));
			return;
	}

#if PCSX2_MINGW_R128_BY_PTR
	gsWrite128_generic( mem, value_ptr );
#else
	gsWrite128_generic( mem, value );
#endif
}

#if PCSX2_MINGW_R128_BY_PTR
void gsWrite128_generic(u32 mem, const r128* value_ptr)
{
	r128_store(PS2GS_BASE(mem), r128_load(value_ptr));
}
#else
void TAKES_R128 gsWrite128_generic(u32 mem, r128 value)
{
	r128_store(PS2GS_BASE(mem), value);
}
#endif

u8 gsRead8(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u8*)PS2GS_BASE(mem);
	// Only SIGLBLID and CSR are readable, everything else mirrors CSR
	return *(u8*)PS2GS_BASE(GS_CSR + (mem & 0xF));
}

u16 gsRead16(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u16*)PS2GS_BASE(mem);
	// Only SIGLBLID and CSR are readable, everything else mirrors CSR
	return *(u16*)PS2GS_BASE(GS_CSR + (mem & 0x7));
}

u32 gsRead32(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u32*)PS2GS_BASE(mem);
	// Only SIGLBLID and CSR are readable, everything else mirrors CSR
	return *(u32*)PS2GS_BASE(GS_CSR + (mem & 0xC));
}

u64 gsRead64(u32 mem)
{
	if ((mem & ~0xF) == GS_SIGLBLID)
		return *(u64*)PS2GS_BASE(mem);
	// Only SIGLBLID and CSR are readable, everything else mirrors CSR
	return *(u64*)PS2GS_BASE(GS_CSR + (mem & 0x8));
}

bool gsFreeze(SaveStateBase *s)
{
	SaveState_FreezeMem(s, PS2MEM_GS, 0x2000);
	SaveState_Freeze(s, gsVideoMode);

	return SaveState_IsOkay(s);
}

