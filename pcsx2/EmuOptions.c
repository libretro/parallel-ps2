/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023  PCSX2 Dev Team
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

#include "EmuOptions.h"

static const char* const s_gamefix_names[GamefixId_COUNT] =
{
	"FpuMul",
	"GoemonTlb",
	"SkipMPEG",
	"OPHFlag",
	"EETiming",
	"InstantDMA",
	"DMABusy",
	"GIFFIFO",
	"VIFFIFO",
	"VIF1Stall",
	"Ibit",
	"VUSync",
	"VUOverflow",
	"XGKick",
	"BlitInternalFPS",
	"FullVU0Sync",
	"VuAddSub",
};

static const char* const s_speed_hack_names[SPEEDHACK_COUNT] =
{
	"mvuFlag",
	"instantVU1",
	"mtvu",
	"eeCycleRate",
};

void emu_recompiler_defaults(EmuRecompilerOptions* o)
{
	o->bitset = 0;

	/* All recs are enabled by default. */
	o->EnableEE  = true;
	o->EnableIOP = true;
	o->EnableVU0 = true;
	o->EnableVU1 = true;
	o->EnableFastmem = true;
	/* Default-on per the fpaudit price tables: closes EE add/sub
	 * exactly and gates the ABS/NEG and CVT.S.W accuracy fixes, at a
	 * per-op cost of about two nanoseconds that measures within run
	 * noise on real game frame throughput (three-rep Tekken
	 * workload). The divide family stays on FpuSoftFloat, opt-in. */
	o->EnableFpuAccurateArith = true;

	/* VU and FPU clamping default to standard overflow. */
	o->vu0Overflow = true;
	o->vu1Overflow = true;
	o->fpuOverflow = true;
}

void emu_cpu_defaults(EmuCpuOptions* o)
{
	emu_recompiler_defaults(&o->Recompiler);
	o->FPUFPCR = fpcr_emu_default();
	/* Division rounds to nearest, as it always has. */
	o->FPUDivFPCR = fpcr_set_round_mode(fpcr_emu_default(), FPROUND_NEAREST);
	o->VU0FPCR    = fpcr_emu_default();
	o->VU1FPCR    = fpcr_emu_default();
	o->AffinityControlMode = 0;
}

bool emu_cpu_equal(const EmuCpuOptions* a, const EmuCpuOptions* b)
{
	return a->Recompiler.bitset == b->Recompiler.bitset
		&& a->FPUFPCR.bitmask == b->FPUFPCR.bitmask
		&& a->FPUDivFPCR.bitmask == b->FPUDivFPCR.bitmask
		&& a->VU0FPCR.bitmask == b->VU0FPCR.bitmask
		&& a->VU1FPCR.bitmask == b->VU1FPCR.bitmask
		&& a->AffinityControlMode == b->AffinityControlMode;
}

bool emu_cpu_recompilers_changed(const EmuCpuOptions* a, const EmuCpuOptions* b)
{
	return a->Recompiler.EnableEE != b->Recompiler.EnableEE
		|| a->Recompiler.EnableIOP != b->Recompiler.EnableIOP
		|| a->Recompiler.EnableVU0 != b->Recompiler.EnableVU0
		|| a->Recompiler.EnableVU1 != b->Recompiler.EnableVU1;
}

void emu_dev9_defaults(EmuDev9Options* o)
{
	memset(o, 0, sizeof(*o));
	o->EthApi      = EMU_NETAPI_UNSET;
	o->AutoMask    = true;
	o->AutoGateway = true;
	o->ModeDNS1    = EMU_DNS_AUTO;
	o->ModeDNS2    = EMU_DNS_AUTO;
	memcpy(o->HddFile, "DEV9hdd.raw", sizeof("DEV9hdd.raw"));
	o->HddSizeSectors = 40u * (1024 * 1024 * 1024 / 512);
}

bool emu_dev9_equal(const EmuDev9Options* a, const EmuDev9Options* b)
{
	return a->EthEnable == b->EthEnable
		&& a->EthApi == b->EthApi
		&& strcmp(a->EthDevice, b->EthDevice) == 0
		&& a->EthLogDNS == b->EthLogDNS
		&& a->InterceptDHCP == b->InterceptDHCP
		&& memcmp(a->PS2IP, b->PS2IP, 4) == 0
		&& memcmp(a->Mask, b->Mask, 4) == 0
		&& memcmp(a->Gateway, b->Gateway, 4) == 0
		&& memcmp(a->DNS1, b->DNS1, 4) == 0
		&& memcmp(a->DNS2, b->DNS2, 4) == 0
		&& a->AutoMask == b->AutoMask
		&& a->AutoGateway == b->AutoGateway
		&& a->ModeDNS1 == b->ModeDNS1
		&& a->ModeDNS2 == b->ModeDNS2
		&& a->HddEnable == b->HddEnable
		&& strcmp(a->HddFile, b->HddFile) == 0
		&& strcmp(a->HddIdFile, b->HddIdFile) == 0
		&& a->HddSizeSectors == b->HddSizeSectors;
}

void emu_gamefix_clear_all(EmuGamefixOptions* o)
{
	o->bitset = 0;
}

void emu_gamefix_set(EmuGamefixOptions* o, enum GamefixId id, bool enabled)
{
	switch (id)
	{
		case Fix_FpuMultiply:     o->FpuMulHack          = enabled; break;
		case Fix_XGKick:          o->XgKickHack          = enabled; break;
		case Fix_EETiming:        o->EETimingHack        = enabled; break;
		case Fix_InstantDMA:      o->InstantDMAHack      = enabled; break;
		case Fix_SkipMpeg:        o->SkipMPEGHack        = enabled; break;
		case Fix_OPHFlag:         o->OPHFlagHack         = enabled; break;
		case Fix_DMABusy:         o->DMABusyHack         = enabled; break;
		case Fix_VIFFIFO:         o->VIFFIFOHack         = enabled; break;
		case Fix_VIF1Stall:       o->VIF1StallHack       = enabled; break;
		case Fix_GIFFIFO:         o->GIFFIFOHack         = enabled; break;
		case Fix_GoemonTlbMiss:   o->GoemonTlbHack       = enabled; break;
		case Fix_Ibit:            o->IbitHack            = enabled; break;
		case Fix_VUSync:          o->VUSyncHack          = enabled; break;
		case Fix_VUOverflow:      o->VUOverflowHack      = enabled; break;
		case Fix_BlitInternalFPS: o->BlitInternalFPSHack = enabled; break;
		case Fix_FullVU0Sync:     o->FullVU0SyncHack     = enabled; break;
		case Fix_VuAddSub:        o->VuAddSubHack        = enabled; break;
		default:                  break;
	}
}

bool emu_gamefix_get(const EmuGamefixOptions* o, enum GamefixId id)
{
	switch (id)
	{
		case Fix_FpuMultiply:     return o->FpuMulHack;
		case Fix_XGKick:          return o->XgKickHack;
		case Fix_EETiming:        return o->EETimingHack;
		case Fix_InstantDMA:      return o->InstantDMAHack;
		case Fix_SkipMpeg:        return o->SkipMPEGHack;
		case Fix_OPHFlag:         return o->OPHFlagHack;
		case Fix_DMABusy:         return o->DMABusyHack;
		case Fix_VIFFIFO:         return o->VIFFIFOHack;
		case Fix_VIF1Stall:       return o->VIF1StallHack;
		case Fix_GIFFIFO:         return o->GIFFIFOHack;
		case Fix_GoemonTlbMiss:   return o->GoemonTlbHack;
		case Fix_Ibit:            return o->IbitHack;
		case Fix_VUSync:          return o->VUSyncHack;
		case Fix_VUOverflow:      return o->VUOverflowHack;
		case Fix_BlitInternalFPS: return o->BlitInternalFPSHack;
		case Fix_FullVU0Sync:     return o->FullVU0SyncHack;
		case Fix_VuAddSub:        return o->VuAddSubHack;
		default:                  break;
	}
	return false;
}

const char* emu_gamefix_name(u32 id)
{
	return id < GamefixId_COUNT ? s_gamefix_names[id] : NULL;
}

void emu_speedhack_clear_all(EmuSpeedhackOptions* o)
{
	o->bitset      = 0;
	o->EECycleRate = 0;
	o->EECycleSkip = 0;
}

void emu_speedhack_defaults(EmuSpeedhackOptions* o)
{
	emu_speedhack_clear_all(o);
	/* The recommended ones; still off globally on resets. */
	o->WaitLoop   = true;
	o->IntcStat   = true;
	o->vuFlagHack = true;
	o->vu1Instant = true;
}

void emu_speedhack_set(EmuSpeedhackOptions* o, enum SpeedHack id, int value)
{
	switch (id)
	{
		case SPEEDHACK_MVU_FLAG:
			o->vuFlagHack = (value != 0);
			break;
		case SPEEDHACK_INSTANT_VU1:
			o->vu1Instant = (value != 0);
			break;
		case SPEEDHACK_MTVU:
			o->vuThread = (value != 0);
			break;
		case SPEEDHACK_EE_CYCLE_RATE:
			o->EECycleRate = (s8)pcsx2_clamp_i(value, EMU_EE_CYCLE_RATE_MIN, EMU_EE_CYCLE_RATE_MAX);
			break;
		default:
			break;
	}
}

bool emu_speedhack_equal(const EmuSpeedhackOptions* a, const EmuSpeedhackOptions* b)
{
	return a->bitset == b->bitset && a->EECycleRate == b->EECycleRate
		&& a->EECycleSkip == b->EECycleSkip;
}

const char* emu_speedhack_name(enum SpeedHack id)
{
	return (unsigned)id < SPEEDHACK_COUNT ? s_speed_hack_names[id] : "?";
}

bool emu_speedhack_parse_name(const char* name, size_t len, enum SpeedHack* out)
{
	u32 i;
	for (i = 0; i < SPEEDHACK_COUNT; i++)
	{
		if (strlen(s_speed_hack_names[i]) == len && memcmp(s_speed_hack_names[i], name, len) == 0)
		{
			*out = (enum SpeedHack)i;
			return true;
		}
	}
	return false;
}
