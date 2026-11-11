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

// This macro is actually useful for about any and every possible application of C++
// equality operators.
#define OpEqu(field) (field == right.field)

// Default EE/VU control registers have exceptions off, DaZ/FTZ, and the rounding mode set to Chop/Zero.
Pcsx2Config EmuConfig;


namespace EmuFolders
{
	char AppRoot[PCSX2_PATH_MAX];
	char DataRoot[PCSX2_PATH_MAX];
	char Settings[PCSX2_PATH_MAX];
	char Bios[PCSX2_PATH_MAX];
	char MemoryCards[PCSX2_PATH_MAX];
	char Cheats[PCSX2_PATH_MAX];
	char CheatsWS[PCSX2_PATH_MAX];
	char CheatsNI[PCSX2_PATH_MAX];
	char Resources[PCSX2_PATH_MAX];
	char Cache[PCSX2_PATH_MAX];
} // namespace EmuFolders

















Pcsx2Config::Pcsx2Config()
{
	emu_cpu_defaults(&Cpu);
	emu_gs_defaults(&GS);
	emu_dev9_defaults(&DEV9);
	emu_speedhack_defaults(&Speedhacks);
	emu_gamefix_clear_all(&Gamefixes);
	BaseFilenames.Bios[0] = '\0';
	/* char arrays have no default constructor; the std::string fields they
	 * replaced were empty on construction. */
	CurrentIRX[0]      = '\0';
	CurrentGameArgs[0] = '\0';

	bitset = 0;
	// Set defaults for fresh installs / reset settings
	McdEnableEjection = true;
	McdFolderAutoManage = true;
	EnablePatches = true;
	EnableGameFixes = true;

	// To be moved to FileMemoryCard pluign (someday)
	for (uint slot = 0; slot < 8; ++slot)
	{
		Mcd[slot].Enabled = !FileMcd_IsMultitapSlot(slot); // enables main 2 slots
		FileMcd_GetDefaultName(Mcd[slot].Filename, sizeof(Mcd[slot].Filename), slot);
	}

	strlcpy(GzipIsoIndexTemplate, "$(f).pindex.tmp", sizeof(GzipIsoIndexTemplate));
}



bool Pcsx2Config::MultitapEnabled(uint port) const
{
	return (port == 0) ? MultitapPort0_Enabled : MultitapPort1_Enabled;
}

void Pcsx2Config::FullpathToBios(char* out, size_t out_size) const
{
	out[0] = '\0';
	if (BaseFilenames.Bios[0])
		pcsx2_path_join(out, out_size, EmuFolders::Bios,
				BaseFilenames.Bios);
}

void Pcsx2Config::FullpathToMcd(char* out, size_t out_size, uint slot) const
{
	/* A card named by its whole path (an arcade game's dongle) is where
	 * it says. */
	if (path_is_absolute(Mcd[slot].Filename))
		strlcpy(out, Mcd[slot].Filename, out_size);
	else
		pcsx2_path_join(out, out_size, EmuFolders::MemoryCards,
				Mcd[slot].Filename);
}

bool Pcsx2Config::operator==(const Pcsx2Config& right) const
{
	bool equal =
		OpEqu(bitset) &&
		emu_cpu_equal(&Cpu, &right.Cpu) &&
		emu_gs_equal(&GS, &right.GS) &&
		emu_dev9_equal(&DEV9, &right.DEV9) &&
		emu_speedhack_equal(&Speedhacks, &right.Speedhacks) &&
		OpEqu(Gamefixes.bitset) &&
		(strcmp(BaseFilenames.Bios, right.BaseFilenames.Bios) == 0) &&
		(strcmp(GzipIsoIndexTemplate, right.GzipIsoIndexTemplate) == 0);
	for (u32 i = 0; i < sizeof(Mcd) / sizeof(Mcd[0]); i++)
	{
		equal &= OpEqu(Mcd[i].Enabled);
		equal &= (strcmp(Mcd[i].Filename, right.Mcd[i].Filename) == 0);
	}

	return equal;
}

void Pcsx2Config::CopyRuntimeConfig(Pcsx2Config& cfg)
{
	UseBOOT2Injection = cfg.UseBOOT2Injection;
	strlcpy(CurrentIRX, cfg.CurrentIRX, sizeof(CurrentIRX));
	strlcpy(CurrentGameArgs, cfg.CurrentGameArgs, sizeof(CurrentGameArgs));
}

/* The memory cards' settings, for MemoryCardFile.c. */
bool emu_config_mcd(uint slot, char* path, size_t path_size)
{
	path[0] = '\0';
	if (slot >= sizeof(EmuConfig.Mcd) / sizeof(EmuConfig.Mcd[0]))
		return false;
	if (EmuConfig.Mcd[slot].Filename[0])
		EmuConfig.FullpathToMcd(path, path_size, slot);
	return EmuConfig.Mcd[slot].Enabled;
}

bool emu_config_multitap(uint port)
{
	return EmuConfig.MultitapEnabled(port);
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
	EmuConfig.FullpathToBios(out, out_size);
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
	return EmuFolders::Bios;
}

/* For GameDatabase.c. */
const char* emu_folders_resources(void)
{
	return EmuFolders::Resources;
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
	const Pcsx2Config::RecompilerOptions& rec = EmuConfig.Cpu.Recompiler;
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

void EmuFolders::LoadConfig(const char* memcards)
{
	/* The only folder a core option sets is the memory cards'; the rest
	 * are their defaults under DataRoot. */
	SetFolder(Bios, sizeof(Bios), DataRoot, "bios");
	SetFolder(MemoryCards, sizeof(MemoryCards), DataRoot, (memcards && memcards[0]) ? memcards : "memcards");
	SetFolder(Cheats, sizeof(Cheats), DataRoot, "cheats");
	SetFolder(CheatsWS, sizeof(CheatsWS), DataRoot, "cheats_ws");
	SetFolder(CheatsNI, sizeof(CheatsNI), DataRoot, "cheats_ni");
	SetFolder(Cache, sizeof(Cache), DataRoot, "cache");
}

void EmuFolders::EnsureFoldersExist()
{
	if (!path_is_valid(Bios))
		path_mkdir(Bios);
	if (!path_is_valid(MemoryCards))
		path_mkdir(MemoryCards);
	if (!path_is_valid(Cheats))
		path_mkdir(Cheats);
	if (!path_is_valid(CheatsWS))
		path_mkdir(CheatsWS);
	if (!path_is_valid(CheatsNI))
		path_mkdir(CheatsNI);
	if (!path_is_valid(Cache))
		path_mkdir(Cache);
}

/* What LoadSave applied after reading, now applied to the option
 * config: the two Speedhacks clamps and the SkipDraw ordering. The FPU
 * control-register and DEV9 address blocks computed from keys no option
 * set and were identities on the defaults; measured, before this. */
void Pcsx2Config::ApplyOptionFixups()
{
	Speedhacks.EECycleRate = pcsx2_clamp_i(Speedhacks.EECycleRate, EMU_EE_CYCLE_RATE_MIN, EMU_EE_CYCLE_RATE_MAX);
	Speedhacks.EECycleSkip = pcsx2_min_i(Speedhacks.EECycleSkip, EMU_EE_CYCLE_SKIP_MAX);
	GS.SkipDrawEnd         = pcsx2_max_i(GS.SkipDrawStart, GS.SkipDrawEnd);
}
