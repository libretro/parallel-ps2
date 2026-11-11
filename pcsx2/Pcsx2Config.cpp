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

#include <math.h>
#include <compat/strl.h>
#include "ParseNumber.h"
#include <float.h>
#include <stdint.h>
#include "HostFS.h"

#include "Config.h"
#include "HotConfig.h"
#include "GS.h"
#include "CDVD/CDVDcommon.h"
#include "MemoryCardFile.h"
#include "R5900.h"
#include "R3000A.h"
#include "IopBios.h"
#include "VUmicro.h"
#include "ps2/HwInternal.h"
#include "ps2/BiosTools.h"
#include "PerformanceMetrics.h"
#include "GameDatabase.h"

#include <file/file_path.h>

extern std::string libretro_content;

Pcsx2Config EmuConfig;
struct EmuFoldersData EmuFolders;

/* EmuConfig holds the defaults from the start, as it did when it was a
 * class: settings are read into it before the VM first runs. */
static struct EmuConfigDefaults
{
	EmuConfigDefaults() { emu_config_init(&EmuConfig); }
} s_emu_config_defaults;

















void emu_config_init(Pcsx2Config* cfg)
{
	uint slot;

	memset(cfg, 0, sizeof(*cfg));
	emu_cpu_defaults(&cfg->Cpu);
	emu_gs_defaults(&cfg->GS);
	emu_dev9_defaults(&cfg->DEV9);
	emu_speedhack_defaults(&cfg->Speedhacks);
	emu_gamefix_clear_all(&cfg->Gamefixes);

	/* Defaults for fresh installs and reset settings. */
	cfg->McdEnableEjection   = true;
	cfg->McdFolderAutoManage = true;
	cfg->EnablePatches       = true;
	cfg->EnableGameFixes     = true;

	/* The two main slots on, the multitap slots off. */
	for (slot = 0; slot < 8; ++slot)
	{
		cfg->Mcd[slot].Enabled = !FileMcd_IsMultitapSlot(slot);
		FileMcd_GetDefaultName(cfg->Mcd[slot].Filename, sizeof(cfg->Mcd[slot].Filename), slot);
	}

	strlcpy(cfg->GzipIsoIndexTemplate, "$(f).pindex.tmp", sizeof(cfg->GzipIsoIndexTemplate));
}

bool emu_config_multitap_enabled(const Pcsx2Config* cfg, uint port)
{
	return (port == 0) ? cfg->MultitapPort0_Enabled : cfg->MultitapPort1_Enabled;
}

void emu_config_fullpath_to_bios(const Pcsx2Config* cfg, char* out, size_t out_size)
{
	out[0] = '\0';
	if (cfg->BaseFilenames.Bios[0])
		pcsx2_path_join(out, out_size, EmuFolders.Bios, cfg->BaseFilenames.Bios);
}

void emu_config_fullpath_to_mcd(const Pcsx2Config* cfg, char* out, size_t out_size, uint slot)
{
	/* A card named by its whole path (an arcade game's dongle) is where
	 * it says. */
	if (path_is_absolute(cfg->Mcd[slot].Filename))
		strlcpy(out, cfg->Mcd[slot].Filename, out_size);
	else
		pcsx2_path_join(out, out_size, EmuFolders.MemoryCards, cfg->Mcd[slot].Filename);
}

bool emu_config_equal(const Pcsx2Config* a, const Pcsx2Config* b)
{
	u32 i;
	if (a->bitset != b->bitset
		|| !emu_cpu_equal(&a->Cpu, &b->Cpu)
		|| !emu_gs_equal(&a->GS, &b->GS)
		|| !emu_dev9_equal(&a->DEV9, &b->DEV9)
		|| !emu_speedhack_equal(&a->Speedhacks, &b->Speedhacks)
		|| a->Gamefixes.bitset != b->Gamefixes.bitset
		|| strcmp(a->BaseFilenames.Bios, b->BaseFilenames.Bios) != 0
		|| strcmp(a->GzipIsoIndexTemplate, b->GzipIsoIndexTemplate) != 0)
		return false;
	for (i = 0; i < sizeof(a->Mcd) / sizeof(a->Mcd[0]); i++)
		if (a->Mcd[i].Enabled != b->Mcd[i].Enabled
			|| strcmp(a->Mcd[i].Filename, b->Mcd[i].Filename) != 0)
			return false;
	return true;
}

void emu_config_copy_runtime(Pcsx2Config* dst, const Pcsx2Config* src)
{
	dst->UseBOOT2Injection = src->UseBOOT2Injection;
	strlcpy(dst->CurrentIRX, src->CurrentIRX, sizeof(dst->CurrentIRX));
	strlcpy(dst->CurrentGameArgs, src->CurrentGameArgs, sizeof(dst->CurrentGameArgs));
}

/* The memory cards' settings, for MemoryCardFile.c. */
bool emu_config_mcd(uint slot, char* path, size_t path_size)
{
	path[0] = '\0';
	if (slot >= sizeof(EmuConfig.Mcd) / sizeof(EmuConfig.Mcd[0]))
		return false;
	if (EmuConfig.Mcd[slot].Filename[0])
		emu_config_fullpath_to_mcd(&EmuConfig, path, path_size, slot);
	return EmuConfig.Mcd[slot].Enabled;
}

bool emu_config_multitap(uint port)
{
	return emu_config_multitap_enabled(&EmuConfig, port);
}

bool emu_config_mcd_ejection(void)
{
	return EmuConfig.McdEnableEjection;
}

/* For IopHw.c's reset, which arms the fast boot from it. */
bool emu_config_boot2_injection(void)
{
	return EmuConfig.UseBOOT2Injection;
}

bool emu_config_irx_injected(void)
{
	return strlen(EmuConfig.CurrentIRX) > 3;
}

/* For BiosTools.c. */
void emu_config_bios_path(char* out, size_t out_size)
{
	emu_config_fullpath_to_bios(&EmuConfig, out, out_size);
}

const char* emu_config_bios_name(void)
{
	return EmuConfig.BaseFilenames.Bios;
}

const char* emu_config_current_irx(void)
{
	return EmuConfig.CurrentIRX;
}

const char* emu_folders_bios(void)
{
	return EmuFolders.Bios;
}

/* For GameDatabase.c. */
const char* emu_folders_resources(void)
{
	return EmuFolders.Resources;
}


/* For PerformanceMetrics.c. */
bool emu_config_blit_internal_fps_hack(void)
{
	return EmuConfig.Gamefixes.BlitInternalFPSHack;
}

bool emu_config_host_fs(void)
{
	return EmuConfig.HostFs;
}

bool emu_config_ophflag_hack(void)
{
	return CHECK_OPHFLAGHACK;
}

bool emu_config_giffifo_hack(void)
{
	return CHECK_GIFFIFOHACK;
}

bool emu_config_xgkick_hack(void)
{
	return CHECK_XGKICKHACK;
}

bool emu_config_enable_ee(void)
{
	return EmuConfig.Cpu.Recompiler.EnableEE;
}

bool emu_config_enable_vu1(void)
{
	return EmuConfig.Cpu.Recompiler.EnableVU1;
}

bool emu_config_dmabusy_hack(void)
{
	return CHECK_DMABUSYHACK;
}

bool emu_config_viffifo_hack(void)
{
	return CHECK_VIFFIFOHACK;
}

bool emu_config_vif1stall_hack(void)
{
	return CHECK_VIF1STALLHACK;
}

bool emu_config_vusync_hack(void)
{
	return EmuConfig.Gamefixes.VUSyncHack;
}

u32 emu_hot;
s32 emu_hot_ee_cycle_rate;
u32 emu_hot_ee_cycle_skip;

void emu_config_sync_hot(void)
{
	const EmuRecompilerOptions& rec = EmuConfig.Cpu.Recompiler;
	u32 w = 0;
	if (rec.EnableEE)                          w |= EMU_HOT_EEREC;
	if (rec.EnableEECache)                     w |= EMU_HOT_EECACHE;
	if (rec.EnableFastmem)                     w |= EMU_HOT_FASTMEM;
	if (EmuConfig.Speedhacks.WaitLoop)         w |= EMU_HOT_WAITLOOP;
	if (EmuConfig.Gamefixes.GoemonTlbHack)     w |= EMU_HOT_GOEMON_TLB;
	if (EmuConfig.Gamefixes.InstantDMAHack)    w |= EMU_HOT_INSTANT_DMA;
	if (EmuConfig.Gamefixes.EETimingHack)      w |= EMU_HOT_EE_TIMING;
	if (THREAD_VU1)                            w |= EMU_HOT_VU1_THREAD;
	if (INSTANT_VU1)                           w |= EMU_HOT_VU1_INSTANT;
	if (rec.vu0ExtraOverflow)                  w |= EMU_HOT_VU0_EXTRA_OVERFLOW;
	emu_hot               = w;
	emu_hot_ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
	emu_hot_ee_cycle_skip = EmuConfig.Speedhacks.EECycleSkip;
}

void emu_config_disable_fastmem(void)
{
	EmuConfig.Cpu.Recompiler.EnableFastmem = false;
	emu_config_sync_hot();
}

u64 emu_fpcr_set_vu(int unit)
{
	const FPControlRegister prev = fpcr_get_current();
	fpcr_set_current(unit ? EmuConfig.Cpu.VU1FPCR : EmuConfig.Cpu.VU0FPCR);
	return prev.bitmask;
}

u32 emu_config_framerate_mhz(bool pal)
{
	return (u32)lrintf((pal ? EmuConfig.GS.FrameratePAL : EmuConfig.GS.FramerateNTSC) * 1000.0f);
}

u32 emu_config_default_framerate_mhz(bool pal)
{
	return (u32)((pal ? EMU_GS_DEFAULT_FRAME_RATE_PAL : EMU_GS_DEFAULT_FRAME_RATE_NTSC) * 1000.0f);
}

u64 emu_fpcr_set_fpu(void)
{
	const FPControlRegister prev = fpcr_get_current();
	fpcr_set_current(EmuConfig.Cpu.FPUFPCR);
	return prev.bitmask;
}

const char* emu_config_game_args(void)
{
	return EmuConfig.CurrentGameArgs;
}

void emu_fpcr_restore(u64 saved)
{
	FPControlRegister r;
	r.bitmask = (decltype(r.bitmask))saved;
	fpcr_set_current(r);
}


/* A folder under DataRoot unless given absolute. */
static void SetFolder(char* out, size_t out_size, const char* root, const char* value)
{
	if (path_is_absolute(value))
		strlcpy(out, value, out_size);
	else
		pcsx2_path_join(out, out_size, root, value);
}

void emu_folders_load_config(const char* memcards)
{
	struct EmuFoldersData* f = &EmuFolders;
	/* The only folder a core option sets is the memory cards'; the rest
	 * are their defaults under DataRoot. */
	SetFolder(f->Bios, sizeof(f->Bios), f->DataRoot, "bios");
	SetFolder(f->MemoryCards, sizeof(f->MemoryCards), f->DataRoot, (memcards && memcards[0]) ? memcards : "memcards");
	SetFolder(f->Cheats, sizeof(f->Cheats), f->DataRoot, "cheats");
	SetFolder(f->CheatsWS, sizeof(f->CheatsWS), f->DataRoot, "cheats_ws");
	SetFolder(f->CheatsNI, sizeof(f->CheatsNI), f->DataRoot, "cheats_ni");
	SetFolder(f->Cache, sizeof(f->Cache), f->DataRoot, "cache");
}

void emu_folders_ensure_exist(void)
{
	const char* const dirs[] = {
		EmuFolders.Bios, EmuFolders.MemoryCards, EmuFolders.Cheats,
		EmuFolders.CheatsWS, EmuFolders.CheatsNI, EmuFolders.Cache,
	};
	size_t i;
	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (!path_is_valid(dirs[i]))
			path_mkdir(dirs[i]);
}

/* What LoadSave applied after reading, now applied to the option
 * config: the two Speedhacks clamps and the SkipDraw ordering. The FPU
 * control-register and DEV9 address blocks computed from keys no option
 * set and were identities on the defaults; measured, before this. */
void emu_config_apply_option_fixups(Pcsx2Config* cfg)
{
	cfg->Speedhacks.EECycleRate = pcsx2_clamp_i(cfg->Speedhacks.EECycleRate, EMU_EE_CYCLE_RATE_MIN, EMU_EE_CYCLE_RATE_MAX);
	cfg->Speedhacks.EECycleSkip = pcsx2_min_i(cfg->Speedhacks.EECycleSkip, EMU_EE_CYCLE_SKIP_MAX);
	cfg->GS.SkipDrawEnd         = pcsx2_max_i(cfg->GS.SkipDrawStart, cfg->GS.SkipDrawEnd);
}
