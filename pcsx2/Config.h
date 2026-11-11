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

#pragma once

#include <string.h>

#include "../common/Pcsx2Defs.h"


#include "../common/General.h"
#include "EmuOptions.h"


enum class CDVD_SourceType : uint8_t;


#include "GS/GSOptionEnums.h"

// --------------------------------------------------------------------------------------
//  Pcsx2Config class
// --------------------------------------------------------------------------------------
// This is intended to be a public class library between the core emulator and GUI only.
//
// When GUI code performs modifications of this class, it must be done with strict thread
// safety, since the emu runs on a separate thread.  Additionally many components of the
// class require special emu-side resets or state save/recovery to be applied.  Please
// use the provided functions to lock the emulation into a safe state and then apply
// chances on the necessary scope (see Core_Pause, Core_ApplySettings, and Core_Resume).
//
struct Pcsx2Config
{
	typedef EmuRecompilerOptions RecompilerOptions;

	// ------------------------------------------------------------------------
	typedef EmuCpuOptions CpuOptions;

	// ------------------------------------------------------------------------
	typedef EmuGSOptions GSOptions;

	typedef EmuDev9Options DEV9Options;

	typedef EmuGamefixOptions GamefixOptions;
	typedef EmuSpeedhackOptions SpeedhackOptions;

	typedef EmuFilenameOptions FilenameOptions;
	/* Options for each memory card. */
	typedef EmuMcdOptions McdOptions;

	// ------------------------------------------------------------------------

	// ------------------------------------------------------------------------

	union
	{
		u32 bitset;
		struct
		{
			bool EnablePatches : 1, // enables patch detection and application
			     EnableCheats  : 1, // enables cheat detection and application
			     EnableWideScreenPatches    : 1,
			     EnableNoInterlacingPatches : 1,
		             EnableGameFixes            : 1, // enables automatic game fixes
			     // When enabled uses BOOT2 injection, skipping sony bios splashes
			     UseBOOT2Injection          : 1,
			     // Enables simulated ejection of memory cards when loading savestates
			     McdEnableEjection          : 1,
			     McdFolderAutoManage        : 1,

			     MultitapPort0_Enabled      : 1,
			     MultitapPort1_Enabled      : 1,

			     HostFs                     : 1;

			// uses automatic NTFS compression when creating new memory cards (Win32 only)
		};
	};

	CpuOptions Cpu;
	GSOptions GS;
	SpeedhackOptions Speedhacks;
	GamefixOptions Gamefixes;
	DEV9Options DEV9;

	FilenameOptions BaseFilenames;

	// Memorycard options - first 2 are default slots, last 6 are multitap 1 and 2
	// slots (3 each)
	McdOptions Mcd[8];
	char GzipIsoIndexTemplate[PCSX2_PATH_MAX]; /* quick-access index for gzipped ISO */

	// Set at runtime, not loaded from config.
	char CurrentIRX[PCSX2_PATH_MAX];
	char CurrentGameArgs[PCSX2_PATH_MAX];

	Pcsx2Config();

	void FullpathToBios(char* out, size_t out_size) const;
	void FullpathToMcd(char* out, size_t out_size, uint slot) const;

	bool MultitapEnabled(uint port) const;

	bool operator==(const Pcsx2Config& right) const;
	bool operator!=(const Pcsx2Config& right) const
	{
		return !this->operator==(right);
	}

	/// Copies runtime configuration settings (e.g. frame limiter state).
	void CopyRuntimeConfig(Pcsx2Config& cfg);
	/* The clamps LoadSave applied after reading, for a config built from options. */
	void ApplyOptionFixups();
};

extern Pcsx2Config EmuConfig;

namespace EmuFolders
{
	/* Fixed buffers rather than std::string: these are paths, they are
	 * read back as characters everywhere, and Path::Combine and
	 * string_view both accept a char array without allocating. */
	extern char AppRoot[PCSX2_PATH_MAX];
	extern char DataRoot[PCSX2_PATH_MAX];
	extern char Settings[PCSX2_PATH_MAX];
	extern char Bios[PCSX2_PATH_MAX];
	extern char MemoryCards[PCSX2_PATH_MAX];
	extern char Cheats[PCSX2_PATH_MAX];
	extern char CheatsWS[PCSX2_PATH_MAX];
	extern char CheatsNI[PCSX2_PATH_MAX];
	extern char Resources[PCSX2_PATH_MAX];
	extern char Cache[PCSX2_PATH_MAX];

	// Assumes that AppRoot and DataRoot have been initialized.
	void LoadConfig(const char* memcards);
	void EnsureFoldersExist();
} // namespace EmuFolders

/////////////////////////////////////////////////////////////////////////////////////////
// Helper Macros for Reading Emu Configurations.
//

// ------------ CPU / Recompiler Options ---------------

/* MTVU runs VU1 through the recompiler on its worker, so it must stand
 * down when VU1 soft float wants the exact interpreter -- otherwise the
 * soft option silently does nothing while MTVU is on, which is how it
 * first shipped here: the provider log line printed, the fps did not
 * move, and only the missing interpreter cost gave it away. */
#define THREAD_VU1 (EmuConfig.Cpu.Recompiler.EnableVU1 && EmuConfig.Speedhacks.vuThread && !EmuConfig.Cpu.Recompiler.EnableVu1SoftFloat)
#define INSTANT_VU1 (EmuConfig.Speedhacks.vu1Instant)
#define CHECK_EEREC (EmuConfig.Cpu.Recompiler.EnableEE)
#define CHECK_CACHE (EmuConfig.Cpu.Recompiler.EnableEECache)
#define CHECK_IOPREC (EmuConfig.Cpu.Recompiler.EnableIOP)
#define CHECK_FASTMEM (EmuConfig.Cpu.Recompiler.EnableEE && EmuConfig.Cpu.Recompiler.EnableFastmem)
/* Recompilers emit calls to the soft COP1 interpreter ops instead of host
 * FP sequences; a toggle reaches emitted code through the Cpu-config diff
 * in CheckForCPUConfigChanges, which clears the execution caches. */
#define CHECK_FPU_SOFT_REC (EmuConfig.Cpu.Recompiler.EnableFpuSoftFloat)
#define CHECK_VU_SOFT_REC(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.EnableVu0SoftFloat : EmuConfig.Cpu.Recompiler.EnableVu1SoftFloat)
#define CHECK_VU_EXACTMUL (EmuConfig.Cpu.Recompiler.EnableVuExactMul)
#define CHECK_VU_EXACTDIV (EmuConfig.Cpu.Recompiler.EnableVuExactDiv)
#define CHECK_VU_ACC_ADDSUB (EmuConfig.Cpu.Recompiler.EnableVuAccurateAddSub)
#define CHECK_FPU_ACC_ARITH (EmuConfig.Cpu.Recompiler.EnableFpuAccurateArith)

//------------ SPECIAL GAME FIXES!!! ---------------
#define CHECK_FPUMULHACK (EmuConfig.Gamefixes.FpuMulHack) // Special Fix for Tales of Destiny hangs.
#define CHECK_XGKICKHACK (EmuConfig.Gamefixes.XgKickHack) // Special Fix for Erementar Gerad, adds more delay to VU XGkick instructions. Corrects the color of some graphics.
#define CHECK_EETIMINGHACK (EmuConfig.Gamefixes.EETimingHack) // Fix all scheduled events to happen in 1 cycle.
#define CHECK_INSTANTDMAHACK (EmuConfig.Gamefixes.InstantDMAHack) // Attempt to finish DMA's instantly, useful for games which rely on cache emulation.
#define CHECK_SKIPMPEGHACK (EmuConfig.Gamefixes.SkipMPEGHack) // Finds sceMpegIsEnd pattern to tell the game the mpeg is finished (Katamari and a lot of games need this)
#define CHECK_OPHFLAGHACK (EmuConfig.Gamefixes.OPHFlagHack) // Bleach Blade Battlers
#define CHECK_DMABUSYHACK (EmuConfig.Gamefixes.DMABusyHack) // Denies writes to the DMAC when it's busy. This is correct behaviour but bad timing can cause problems.
#define CHECK_VIFFIFOHACK (EmuConfig.Gamefixes.VIFFIFOHack) // Pretends to fill the non-existant VIF FIFO Buffer.
#define CHECK_VIF1STALLHACK (EmuConfig.Gamefixes.VIF1StallHack) // Like above, processes FIFO data before the stall is allowed (to make sure data goes over).
#define CHECK_GIFFIFOHACK (EmuConfig.Gamefixes.GIFFIFOHack) // Enabled the GIF FIFO (more correct but slower)
/* CHECK_VUOVERFLOWHACK removed: overflow detection is unconditional
 * on both recompilers (fpaudit promotion). The VUOverflowHack config
 * bit and the "VUOverflow" GameDB name remain parsed-and-ignored so
 * existing GameDB files and user configs stay valid. */
#define CHECK_FULLVU0SYNCHACK (EmuConfig.Gamefixes.FullVU0SyncHack)
#define CHECK_VUADDSUBHACK (EmuConfig.Gamefixes.VuAddSubHack)

//------------ Advanced Options!!! ---------------
#define CHECK_VU_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0Overflow : EmuConfig.Cpu.Recompiler.vu1Overflow)
#define CHECK_VU_EXTRA_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0ExtraOverflow : EmuConfig.Cpu.Recompiler.vu1ExtraOverflow) // If enabled, Operands are clamped before being used in the VU recs
#define CHECK_VU_SIGN_OVERFLOW(vunum) (((vunum) == 0) ? EmuConfig.Cpu.Recompiler.vu0SignOverflow : EmuConfig.Cpu.Recompiler.vu1SignOverflow)

#define CHECK_FPU_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuOverflow)
#define CHECK_FPU_EXTRA_OVERFLOW (EmuConfig.Cpu.Recompiler.fpuExtraOverflow) // If enabled, Operands are checked for infinities before being used in the FPU recs
#define CHECK_FPU_FULL (EmuConfig.Cpu.Recompiler.fpuFullMode)

//------------ EE Recompiler defines - Comment to disable a recompiler ---------------

#define SHIFT_RECOMPILE // Speed majorly reduced if disabled
#define BRANCH_RECOMPILE // Speed extremely reduced if disabled - more then shift

// Disabling all the recompilers in this block is interesting, as it still runs at a reasonable rate.
// It also adds a few glitches. Really reminds me of the old Linux 64-bit version. --arcum42
#define ARITHMETICIMM_RECOMPILE
#define ARITHMETIC_RECOMPILE
#define MULTDIV_RECOMPILE
#define JUMP_RECOMPILE
#define LOADSTORE_RECOMPILE
#define MOVE_RECOMPILE
#define MMI_RECOMPILE
#define MMI0_RECOMPILE
#define MMI1_RECOMPILE
#define MMI2_RECOMPILE
#define MMI3_RECOMPILE
#define FPU_RECOMPILE
#define CP0_RECOMPILE
#define CP2_RECOMPILE

/* You can't recompile ARITHMETICIMM without ARITHMETIC. */
#ifndef ARITHMETIC_RECOMPILE
#undef ARITHMETICIMM_RECOMPILE
#endif
