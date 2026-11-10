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

#include <string.h>

#include "Common.h"
#include "../common/Align.h"
#include "VUmicro.h"

PCSX2_ALIGN(16) VURegs vuRegs[2];

/* VU0/1 on-chip memory */
#define VU_MEMORY_RESERVE_ALIGNED pcsx2_page_align(VU_MEMORY_RESERVE_SIZE)

void vu_memory_assign(const struct VirtualMemoryManager* main_map)
{
	u8* curpos = vmm_alloc(main_map, HOSTMEM_VUMEM_OFFSET, VU_MEMORY_RESERVE_ALIGNED);

	vuRegs[0].Micro = curpos;
	curpos += VU0_PROGSIZE;
	vuRegs[0].Mem   = curpos;
	curpos += VU0_MEMSIZE;
	vuRegs[1].Micro = curpos;
	curpos += VU1_PROGSIZE;
	vuRegs[1].Mem   = curpos;
}

void vu_memory_release(const struct VirtualMemoryManager* main_map)
{
	if (vuRegs[0].Micro)
		vmm_free(main_map, vuRegs[0].Micro, VU_MEMORY_RESERVE_ALIGNED);

	vuRegs[0].Micro = NULL;
	vuRegs[0].Mem   = NULL;
	vuRegs[1].Micro = NULL;
	vuRegs[1].Mem   = NULL;
}

void vu_memory_reset(void)
{
	memset(vuRegs[0].Micro, 0, VU_MEMORY_RESERVE_ALIGNED);

	/* === VU0 Initialization === */
	memset(&vuRegs[0].ACC, 0, sizeof(vuRegs[0].ACC));
	memset(vuRegs[0].VF, 0, sizeof(vuRegs[0].VF));
	memset(vuRegs[0].VI, 0, sizeof(vuRegs[0].VI));
	vuRegs[0].VF[0].f.x = 0.0f;
	vuRegs[0].VF[0].f.y = 0.0f;
	vuRegs[0].VF[0].f.z = 0.0f;
	vuRegs[0].VF[0].f.w = 1.0f;
	vuRegs[0].VI[0].UL  = 0;

	/* === VU1 Initialization === */
	memset(&vuRegs[1].ACC, 0, sizeof(vuRegs[1].ACC));
	memset(vuRegs[1].VF, 0, sizeof(vuRegs[1].VF));
	memset(vuRegs[1].VI, 0, sizeof(vuRegs[1].VI));
	vuRegs[1].VF[0].f.x = 0.0f;
	vuRegs[1].VF[0].f.y = 0.0f;
	vuRegs[1].VF[0].f.z = 0.0f;
	vuRegs[1].VF[0].f.w = 1.0f;
	vuRegs[1].VI[0].UL  = 0;
}

bool vuMicroFreeze(SaveStateBase *s)
{
	if(SaveState_IsSaving(s))
		mtvu_wait_vu();

	if (!(SaveState_FreezeTag(s,  "vuMicroRegs" )))
		return false;

	/* VU0 state information */

	SaveState_Freeze(s, vuRegs[0].ACC);
	SaveState_Freeze(s, vuRegs[0].VF);
	SaveState_Freeze(s, vuRegs[0].VI);
	SaveState_Freeze(s, vuRegs[0].q);

	SaveState_Freeze(s, vuRegs[0].cycle);
	SaveState_Freeze(s, vuRegs[0].flags);
	SaveState_Freeze(s, vuRegs[0].code);
	SaveState_Freeze(s, vuRegs[0].start_pc);
	SaveState_Freeze(s, vuRegs[0].branch);
	SaveState_Freeze(s, vuRegs[0].branchpc);
	SaveState_Freeze(s, vuRegs[0].delaybranchpc);
	SaveState_Freeze(s, vuRegs[0].takedelaybranch);
	SaveState_Freeze(s, vuRegs[0].ebit);
	SaveState_Freeze(s, vuRegs[0].pending_q);
	SaveState_Freeze(s, vuRegs[0].pending_p);
	SaveState_Freeze(s, vuRegs[0].micro_macflags);
	SaveState_Freeze(s, vuRegs[0].micro_clipflags);
	SaveState_Freeze(s, vuRegs[0].micro_statusflags);
	SaveState_Freeze(s, vuRegs[0].macflag);
	SaveState_Freeze(s, vuRegs[0].statusflag);
	SaveState_Freeze(s, vuRegs[0].clipflag);
	SaveState_Freeze(s, vuRegs[0].nextBlockCycles);
	SaveState_Freeze(s, vuRegs[0].VIBackupCycles);
	SaveState_Freeze(s, vuRegs[0].VIOldValue);
	SaveState_Freeze(s, vuRegs[0].VIRegNumber);
	SaveState_Freeze(s, vuRegs[0].fmac);
	SaveState_Freeze(s, vuRegs[0].fmacreadpos);
	SaveState_Freeze(s, vuRegs[0].fmacwritepos);
	SaveState_Freeze(s, vuRegs[0].fmaccount);
	SaveState_Freeze(s, vuRegs[0].fdiv);
	SaveState_Freeze(s, vuRegs[0].efu);
	SaveState_Freeze(s, vuRegs[0].ialu);
	SaveState_Freeze(s, vuRegs[0].ialureadpos);
	SaveState_Freeze(s, vuRegs[0].ialuwritepos);
	SaveState_Freeze(s, vuRegs[0].ialucount);

	/* VU1 state information */
	SaveState_Freeze(s, vuRegs[1].ACC);
	SaveState_Freeze(s, vuRegs[1].VF);
	SaveState_Freeze(s, vuRegs[1].VI);
	SaveState_Freeze(s, vuRegs[1].q);
	SaveState_Freeze(s, vuRegs[1].p);

	SaveState_Freeze(s, vuRegs[1].cycle);
	SaveState_Freeze(s, vuRegs[1].flags);
	SaveState_Freeze(s, vuRegs[1].code);
	SaveState_Freeze(s, vuRegs[1].start_pc);
	SaveState_Freeze(s, vuRegs[1].branch);
	SaveState_Freeze(s, vuRegs[1].branchpc);
	SaveState_Freeze(s, vuRegs[1].delaybranchpc);
	SaveState_Freeze(s, vuRegs[1].takedelaybranch);
	SaveState_Freeze(s, vuRegs[1].ebit);
	SaveState_Freeze(s, vuRegs[1].pending_q);
	SaveState_Freeze(s, vuRegs[1].pending_p);
	SaveState_Freeze(s, vuRegs[1].micro_macflags);
	SaveState_Freeze(s, vuRegs[1].micro_clipflags);
	SaveState_Freeze(s, vuRegs[1].micro_statusflags);
	SaveState_Freeze(s, vuRegs[1].macflag);
	SaveState_Freeze(s, vuRegs[1].statusflag);
	SaveState_Freeze(s, vuRegs[1].clipflag);
	SaveState_Freeze(s, vuRegs[1].nextBlockCycles);
	SaveState_Freeze(s, vuRegs[1].xgkickaddr);
	SaveState_Freeze(s, vuRegs[1].xgkickdiff);
	SaveState_Freeze(s, vuRegs[1].xgkicksizeremaining);
	SaveState_Freeze(s, vuRegs[1].xgkicklastcycle);
	SaveState_Freeze(s, vuRegs[1].xgkickcyclecount);
	SaveState_Freeze(s, vuRegs[1].xgkickenable);
	SaveState_Freeze(s, vuRegs[1].xgkickendpacket);
	SaveState_Freeze(s, vuRegs[1].VIBackupCycles);
	SaveState_Freeze(s, vuRegs[1].VIOldValue);
	SaveState_Freeze(s, vuRegs[1].VIRegNumber);
	SaveState_Freeze(s, vuRegs[1].fmac);
	SaveState_Freeze(s, vuRegs[1].fmacreadpos);
	SaveState_Freeze(s, vuRegs[1].fmacwritepos);
	SaveState_Freeze(s, vuRegs[1].fmaccount);
	SaveState_Freeze(s, vuRegs[1].fdiv);
	SaveState_Freeze(s, vuRegs[1].efu);
	SaveState_Freeze(s, vuRegs[1].ialu);
	SaveState_Freeze(s, vuRegs[1].ialureadpos);
	SaveState_Freeze(s, vuRegs[1].ialuwritepos);
	SaveState_Freeze(s, vuRegs[1].ialucount);

	/* The four pipeline positions index fmac[4] and ialu[4], and every
	 * runtime step masks with & 3 -- but after the access, not before it,
	 * and the loops that walk the pipelines take the unmasked value as
	 * their starting index. So the first use after a load is of whatever
	 * the state said, and _vuClearFMAC memsets a whole fmacPipe at it.
	 * The counts are loop bounds over the same arrays. */
	if (SaveState_IsLoading(s))
	{
		u32 i;

		for (i = 0; i < 2; i++)
		{
			vuRegs[i].fmacreadpos  &= 3;
			vuRegs[i].fmacwritepos &= 3;
			vuRegs[i].ialureadpos  &= 3;
			vuRegs[i].ialuwritepos &= 3;

			if (vuRegs[i].fmaccount > 4)
				vuRegs[i].fmaccount = 0;
			if (vuRegs[i].ialucount > 4)
				vuRegs[i].ialucount = 0;
		}
	}

	return SaveState_IsOkay(s);
}
