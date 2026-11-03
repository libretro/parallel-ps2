/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
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

#include <string>
#include <vector>
#include <optional>

#include "Config.h"
#include <rthreads/rthreads.h>

enum class CDVD_SourceType : uint8_t;

enum class VMState
{
	Shutdown = 0,
	Initializing,
	Running,
	Paused,
	Resetting,
	Stopping,
};

struct VMBootParameters
{
	std::string filename;
	std::string elf_override;
	std::optional<CDVD_SourceType> source_type;

	std::optional<bool> fast_boot;
};

namespace VMManager
{
	/// The stack size to use for threads running recompilers
	static constexpr std::size_t EMU_THREAD_STACK_SIZE = 2 * 1024 * 1024; // µVU likes recursion
									      //
	/// Returns the current state of the VM.
	VMState GetState();

	/// Alters the current state of the VM.
	void SetState(VMState state);

	/// Returns true if there is an active virtual machine.
	bool HasValidVM();
	/// True when this host has enough hardware threads for the MTVU worker.
	bool MtvuHardwareAllowed();

	/// Returns the serial of the disc/executable currently running.
	const char* GetDiscSerial();

	/// Loads global settings (i.e. EmuConfig).
	void LoadSettings();

	/// Initializes all system components.
	bool Initialize(VMBootParameters boot_params);

	/// The EE thread's handle, set by the frontend right after creating it.
	void SetVMThread(sthread_t* thread);

	/// Destroys all system components.
	void Shutdown();

	/// Resets all subsystems to a cold boot.
	void Reset();

	/// Runs the VM until the CPU execution is canceled.
	void Execute();

	/// Changes the pause state of the VM, resetting anything needed when unpausing.
	void SetPaused(bool paused);

	/// Reloads settings, and applies any changes present.
	void ApplySettings();

	/// Re-derives the running-game identity (serial, ELF CRC, GameDB entry)
	/// from the current VM state and re-applies per-game settings if the
	/// identity changed. Called after loading a savestate: the state restores
	/// ElfCRC and the game-started flags, but the boot-path hook that normally
	/// runs the GameDB apply (UpdateRunningGame) has already fired at the BIOS,
	/// leaving stale identity and no per-game fixes - e.g. RetroArch's Auto
	/// Load State (issue #127). Internally guarded: when the identity is
	/// unchanged (mid-session load of the same game) it returns without
	/// rebuilding any configuration, so it is safe to call on every
	/// unserialize.
	void RefreshRunningGameAfterStateLoad();

	/// Reloads cheats/patches. If verbose is set, the number of patches loaded will be shown in the OSD.
	void ReloadPatches();

	/// Returns true if the specified path is an ELF.
	bool IsElfFileName(const std::string_view path);

	/// Changes the disc in the virtual CD/DVD drive. Passing an empty will remove any current disc.
	/// Returns false if the new disc can't be opened.
	bool ChangeDisc(CDVD_SourceType source, std::string path);

	/// Initializes default configuration in the specified file.
	/// The defaults that depend on the machine: MTVU and affinity control
	/// by core count, fastmem by platform. Applied to the option config
	/// before the options are, so an option still wins.
	void ApplyHardwareDefaults(Pcsx2Config& c);

	/// Returns a list of processors in the system, and their corresponding affinity mask.
	/// This list is ordered by most performant to least performant for pinning threads to.
	/* The processors in affinity order, *count of them. */
	const u32* GetSortedProcessorList(u32* count);

	/// Internal callbacks, implemented in the emu core.
	namespace Internal
	{
		/// Initializes common host state, called on the CPU thread.
		void CPUThreadInitialize(void);

		/// Cleans up common host state, called on the CPU thread.
		void CPUThreadShutdown();

		/// Updates the variables in the EmuFolders namespace, reloading subsystems if needed.
		void UpdateEmuFolders();

		/// Resets/clears all execution/code caches.
		void ClearCPUExecutionCaches();

		const std::string& GetElfOverride();
		bool IsExecutionInterrupted();
		void EntryPointCompilingOnCPUThread();
		void GameStartingOnCPUThread();
	} // namespace Internal

	// Set from the pcsx2_mtvu core option (libretro); read at boot by the arm64
	// MTVU hardware-default hook. Replaces the LRPS2_MTVU/LRPS2_NO_MTVU env bridge.
	extern bool g_MtvuMenuDefault;
	extern bool g_FastmemMenuDefault;
} // namespace VMManager


namespace Host
{
	/// Called with the settings lock held, when system settings are being loaded (should load input sources, etc).
	/// Provided by the host; called when the running executable changes.
	void OnGameChanged(const std::string& disc_path, const std::string& elf_override, const std::string& game_serial,
		u32 game_crc);
} // namespace Host
