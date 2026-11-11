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

#include "Config.h"
#include "CDVD/CDVDcommon.h"
#include <rthreads/rthreads.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The VM's life, in the order it goes through it. */
enum vm_state
{
	VM_STATE_SHUTDOWN = 0,
	VM_STATE_INITIALIZING,
	VM_STATE_RUNNING,
	VM_STATE_PAUSED,
	VM_STATE_RESETTING,
	VM_STATE_STOPPING
};

/* What to boot. The strings are the caller's, read until vm_initialize
 * returns. */
typedef struct VMBootParameters
{
	const char* filename;     /* NULL or empty: no disc */
	const char* elf_override; /* NULL or empty: none */
	int source_type;          /* a CDVD_SOURCE_*, or -1: by the filename */
	int fast_boot;            /* 1 or 0, or -1: the option's */
} VMBootParameters;

enum vm_state vm_get_state(void);
void vm_set_state(enum vm_state state);
/* Running, paused or resetting: there is a VM to talk to. */
bool vm_has_valid_vm(void);
/* Enough physical cores for the MTVU worker. */
bool vm_mtvu_hardware_allowed(void);

/* EmuConfig from the options, keeping its runtime fields. */
void vm_load_settings(void);
bool vm_initialize(const VMBootParameters* params);
/* The EE thread's handle, set by the frontend right after creating it. */
void vm_set_vm_thread(sthread_t* thread);
void vm_shutdown(void);
/* A cold boot. While running it is left to the EE, at its next event
 * test. */
void vm_reset(void);
/* Runs the EE until execution is cancelled. */
void vm_execute(void);
void vm_set_paused(bool paused);
/* The options again, and whatever changed applied. */
void vm_apply_settings(void);

/* After a state load: the state restored the game's identity but not
 * what the boot applies for it (issue #127). Nothing is rebuilt when the
 * identity is unchanged, so every load may call it. */
void vm_refresh_running_game_after_state_load(void);
void vm_reload_patches(void);
/* A pnach loaded in place of the GameDB's patches, whatever the CRC: a
 * Konami Python 2 game's. NULL or empty for none. */
void vm_set_python2_patch_file(const char* path);
bool vm_is_elf_file_name(const char* path);
/* NULL or empty path removes the disc. False when the new disc does not
 * open; the old one is put back. */
bool vm_change_disc(CDVD_SourceType source, const char* path);

/* The defaults that depend on the machine (MTVU, affinity, fastmem),
 * applied to the option config before the options are. */
void vm_apply_hardware_defaults(Pcsx2Config* c);
/* retro_init, before any thread that asks: the processors in affinity
 * order, read once and read-only afterwards. */
void vm_initialize_cpu_info(void);
const u32* vm_get_sorted_processor_list(u32* count);

/* On the EE thread. */
void vm_cpu_thread_initialize(void);
void vm_cpu_thread_shutdown(void);
/* The folders again, after the memory cards' moved. */
void vm_update_emu_folders(void);
void vm_clear_cpu_execution_caches(void);
void vm_entry_point_compiling_on_cpu_thread(void);

/* The pcsx2_mtvu and fastmem options, kept through a settings reset. */
extern bool vm_mtvu_menu_default;
extern bool vm_fastmem_menu_default;

#ifdef __cplusplus
}
#endif
