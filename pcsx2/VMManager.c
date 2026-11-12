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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>
#include <streams/file_stream.h>
#include "common/Pcsx2Defs.h"
#include <retro_atomic.h>
#include <features/features_cpu.h>
#include "VMManager.h"
#include "GS/Renderers/Common/GSCodeReserve.h"


#include <file/file_path.h>

#include "HostFS.h"
#include "../common/FPControl.h"

#include "Counters.h"
#include "CDVD/CDVD.h"
#include "CDVD/CDVD_host.h"
#include "HotConfig.h"
#include "CDVD/cdvd_iso.h"
#include "DEV9/DEV9mmio.h"
#include "Elfheader.h"
#include "FW.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "GameDatabase.h"
#include "GS.h"
#include "GS/GSEntry.h"
#include "Host.h"
#include "IopBios.h"
#include "MTVU.h"
#include <rthreads/rthreads.h>
#include "MemoryCardFile.h"
#include "Patch.h"
#include "PerformanceMetrics.h"
#include "Gif_Unit.h"
#include "SaveState.h"
#include "R3000A.h"
#include "VUmicro.h"
#include "R5900.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#include "Vif_Unpack.h"
#include "PAD/PADsio.h"
#include "Sio.h"
#include "ps2/BiosTools.h"

#ifdef _WIN32
/* timeBeginPeriod, and nothing else of Windows */
#include <windows.h>
#include <mmsystem.h>
#endif

/* Opens the CDVD source, with the gzip index settings of the moment. */
static bool OpenCDVD(void)
{
	cdvd_iso_set_gz_index(EmuConfig.GzipIsoIndexTemplate, EmuFolders.DataRoot);
	return DoCDVDopen();
}

static void SysGetBiosDiscID(char* out, size_t out_size)
{
	strlcpy(out, BiosSerial, out_size);
}

/* This function always returns a valid DiscID -- using the Sony serial when possible, and */
/* falling back on the CRC checksum of the ELF binary if the PS2 software being run is */
/* homebrew or some other serial-less item. */
static void SysGetDiscID(char* out, size_t out_size)
{
	if (DiscSerial[0])
	{
		strlcpy(out, DiscSerial, out_size);
		return;
	}

	if (!ElfCRC)
	{
		/* system is currently running the BIOS */
		SysGetBiosDiscID(out, out_size);
		return;
	}

	snprintf(out, out_size, "%08x", ElfCRC);
}

static void InitializeCPUProviders(void);
static void ShutdownCPUProviders(void);
static void UpdateCPUImplementations(void);
static void ApplyGameFixes(void);
static void LoadPatches(const char* serial, u32 crc);
static void UpdateRunningGame(bool resetting, bool game_starting, bool swapping_disc);
static void SetTimerResolutionIncreased(bool enabled);
static void SetEmuThreadAffinities(void);

static SysMainMemory s_vm_memory;

static retro_atomic_int_t s_state = RETRO_ATOMIC_INT_INITIALIZER((int)VM_STATE_SHUTDOWN);
static bool s_cpu_implementation_changed = false;
/* The EE thread. Its sthread_t* is set by the frontend right after
 * create; its id is captured by Initialize, which runs on it. Affinity
 * is applied by handle when called from another thread (ApplySettings
 * on the main thread with the EE paused) and to the current thread when
 * the EE is applying its own -- Initialize can run before the frontend
 * has stored the handle, and the EE never needs it to pin itself. */
static sthread_t* s_vm_thread = NULL;
static uintptr_t  s_vm_thread_id = 0;

/* The running game's identity. Written and read on the EE thread, or
 * with the EE paused: there is no other reader. */
static char s_disc_path[PCSX2_PATH_MAX];
static u32 s_game_crc;
static u32 s_patches_crc;
static char s_game_serial[64];
static char s_elf_override[PCSX2_PATH_MAX];
static u32 s_active_game_fixes = 0;
/* The cheat archives, as read: kept from the first load to the VM's
 * end, so a reload of the patches reads neither again. */
static u8* s_widescreen_cheats_data;
static size_t s_widescreen_cheats_size;
static bool s_widescreen_cheats_loaded = false;
static u8* s_no_interlacing_cheats_data;
static size_t s_no_interlacing_cheats_size;
static bool s_no_interlacing_cheats_loaded = false;
static s32 s_active_widescreen_patches = 0;
static u32 s_active_no_interlacing_patches = 0;

enum vm_state vm_get_state(void)
{
	return (enum vm_state)retro_atomic_load_acquire_int(&s_state);
}

void vm_set_state(enum vm_state state)
{
	const enum vm_state old_state = (enum vm_state)retro_atomic_load_acquire_int(&s_state);
	SetTimerResolutionIncreased(state == VM_STATE_RUNNING);
	retro_atomic_store_release_int(&s_state, (int)state);

	if (state != VM_STATE_STOPPING && (state == VM_STATE_PAUSED || old_state == VM_STATE_PAUSED))
	{
		if (state != VM_STATE_PAUSED)
			perf_metrics_reset();
	}
	/* Stopping breaks execution as soon as it can. */
	else if (state == VM_STATE_STOPPING && old_state == VM_STATE_RUNNING)
		Cpu->ExitExecution();
}

bool vm_has_valid_vm(void)
{
	const enum vm_state state = (enum vm_state)retro_atomic_load_acquire_int(&s_state);
	return (state >= VM_STATE_RUNNING && state <= VM_STATE_RESETTING);
}

/* The folders the patches and the memory cards come from, as they were
 * before vm_update_emu_folders read them again; static, as four paths
 * are too large for a frame. */
static struct
{
	char cheats[PCSX2_PATH_MAX];
	char cheats_ws[PCSX2_PATH_MAX];
	char cheats_ni[PCSX2_PATH_MAX];
	char memcards[PCSX2_PATH_MAX];
} s_old_folders;

void vm_update_emu_folders(void)
{
	strlcpy(s_old_folders.cheats, EmuFolders.Cheats, sizeof(s_old_folders.cheats));
	strlcpy(s_old_folders.cheats_ws, EmuFolders.CheatsWS, sizeof(s_old_folders.cheats_ws));
	strlcpy(s_old_folders.cheats_ni, EmuFolders.CheatsNI, sizeof(s_old_folders.cheats_ni));
	strlcpy(s_old_folders.memcards, EmuFolders.MemoryCards, sizeof(s_old_folders.memcards));
	/* Re-read them: the caller has just changed where the memory cards
	 * live -- the per-content directory is set after load_game has already
	 * configured the folders once -- and the comparisons below are against
	 * what this call produces. Without it nothing here ever differs and
	 * the cards stay in the shared directory. */
	emu_folders_load_config(host_option_memcard_path());

	if (vm_has_valid_vm())
	{
		if (strcmp(EmuFolders.Cheats, s_old_folders.cheats) != 0
			|| strcmp(EmuFolders.CheatsWS, s_old_folders.cheats_ws) != 0
			|| strcmp(EmuFolders.CheatsNI, s_old_folders.cheats_ni) != 0)
			vm_reload_patches();

		if (strcmp(EmuFolders.MemoryCards, s_old_folders.memcards) != 0)
		{
			FileMcd_EmuClose();
			FileMcd_EmuOpen();
			AutoEject_SetAll();
		}
	}
}

void vm_cpu_thread_initialize(void)
{
	/* The hot settings word from the defaults, before the memory and
	 * CPU providers below can read it; LoadSettings resyncs. */
	emu_config_sync_hot();
	/* The GIF paths' packet buffers, before anything that could reach
	 * them. */
	if (!gif_unit_init())
		log_cb(RETRO_LOG_ERROR, "Failed to allocate the GIF path buffers.\n");
	GSinit();
	SPU2_Initialize();
	USBinit();

	sys_memory_init(&s_vm_memory);
	/* Here rather than at the first lookup, which can be the EE starting
	 * a game: the parse allocates. */
	GameDatabase_Load();

	InitializeCPUProviders();

	sys_memory_allocate(&s_vm_memory);
}

void vm_cpu_thread_shutdown(void)
{
	free(s_widescreen_cheats_data);
	free(s_no_interlacing_cheats_data);
	s_widescreen_cheats_data       = NULL;
	s_widescreen_cheats_size       = 0;
	s_no_interlacing_cheats_data   = NULL;
	s_no_interlacing_cheats_size   = 0;
	s_widescreen_cheats_loaded     = false;
	s_no_interlacing_cheats_loaded = false;

	ShutdownCPUProviders();
	sys_memory_destroy(&s_vm_memory);
	/* After the VU1 worker, the last of the paths' users, has gone. */
	gif_unit_shutdown();

	/* Nothing consults the game database without a VM; the next VM's
	 * start reads it again. */
	GameDatabase_Unload();

#if defined(__GLIBC__)
	/* Returning the allocations is not the same as returning the
	 * memory: free() hands them back to the allocator's arena, which
	 * keeps them.  Measured over three load/unload cycles, dropping the
	 * database without this made RSS 21 MB *worse* than keeping it -
	 * the entries were freed and re-parsed into a heap that only grew.
	 * With the trim the same three cycles end 9.7 MB below where they
	 * started, so it is the trim doing the work and the unload only
	 * giving it something to reclaim.  Once per VM teardown, where a
	 * heap walk costs nothing anyone can perceive. */
	malloc_trim(0);
#endif

	USBshutdown();
	SPU2_Shutdown();
	GSshutdown();
}

SysMainMemory* GetVmMemory(void)
{
	return &s_vm_memory;
}

void vm_load_settings(void)
{
	const char* content = host_content_name();
	/* The options set the fields the options set, and nothing else: the
	 * runtime state -- UseBOOT2Injection, the IRX and game-argument
	 * strings, the memory card types, fast boot -- is carried over from
	 * the config as the caller had it.
	 *
	 * Only those fields are set aside, not the whole 25 KB config; kept
	 * off the stack, as only one thread applies settings at a time - the
	 * EE is paused, or it is the EE booting. */
	static Pcsx2Config runtime;
	emu_config_copy_runtime(&runtime, &EmuConfig);
	EmuConfig = *host_option_config();
	emu_config_copy_runtime(&EmuConfig, &runtime);
	emu_config_apply_option_fixups(&EmuConfig);
	if (vtlb_fastmem_unavailable)
		EmuConfig.Cpu.Recompiler.EnableFastmem = false;
	/* The first memory card is named after the content. */
	if (content[0])
	{
		snprintf(EmuConfig.Mcd[0].Filename, sizeof(EmuConfig.Mcd[0].Filename), "%s.ps2", content);
		EmuConfig.Mcd[1].Enabled = false;
	}
	pad_load_config();
	/* The hack masks drop user hacks a global switch has disabled, so a
	 * stale value cannot outlive it; the interlace line is what a
	 * no-interlacing patch needs to take effect; and ApplyGameFixes is the
	 * game database's, without which a game that needs one renders
	 * wrongly. */
	emu_gs_mask_user_hacks(&EmuConfig.GS);
	emu_gs_mask_upscaling_hacks(&EmuConfig.GS);
	if (s_active_no_interlacing_patches > 0 && EmuConfig.GS.InterlaceMode == GS_INTERLACE_AUTOMATIC)
		EmuConfig.GS.InterlaceMode = GS_INTERLACE_OFF;
	if (vm_has_valid_vm())
		ApplyGameFixes();
	cdvdApplySettings(EmuConfig.Speedhacks.fastCDVD);
	emu_config_sync_hot();
}

void vm_request_stop(void)
{
	vm_set_state(VM_STATE_STOPPING);
}

bool vm_paused(void)
{
	return vm_get_state() == VM_STATE_PAUSED;
}

const char* vm_elf_override(void)
{
	return s_elf_override;
}

static void ApplyGameFixes(void)
{
	const GameDatabaseEntry* game;
	s_active_game_fixes = 0;

	if (s_game_crc == 0)
		return;

	game = GameDatabase_FindGame(s_game_serial);
	if (!game)
		return;

	s_active_game_fixes += GameDatabase_ApplyGameFixes(game, &EmuConfig, EmuConfig.EnableGameFixes);
	s_active_game_fixes += GameDatabase_ApplyGSHardwareFixes(game, &EmuConfig.GS);
}

void vm_refresh_running_game_after_state_load(void)
{
	/* The state restored ElfCRC, g_GameStarted and g_GameLoading;
	 * UpdateRunningGame re-derives the serial from the disc and the CRC
	 * from them, and returns early when nothing changed. */
	UpdateRunningGame(false, false, false);
}

/* SetPython2PatchFile; static, as a path is too large for a frame. */
static char s_python2_patch_file[PCSX2_PATH_MAX];

void vm_set_python2_patch_file(const char* path)
{
	strlcpy(s_python2_patch_file, path ? path : "", sizeof(s_python2_patch_file));
}

/* The Python 2 game's pnach; returns how many patches it held. */
static int LoadPython2Patches(void)
{
	void* data   = NULL;
	int64_t size = 0;
	int count    = 0;
	char* text;

	if (!filestream_read_file(s_python2_patch_file, &data, &size) || size < 0)
	{
		log_cb(RETRO_LOG_ERROR, "Python 2 patch file could not be read: %s\n", s_python2_patch_file);
		free(data);
		return 0;
	}
	/* filestream_read_file terminates what it reads. */
	text = (char*)data;
	count = LoadPatchesFromString(text);
	free(data);
	log_cb(RETRO_LOG_INFO, "Python 2 patches loaded: %d\n", count);
	return count;
}

/* printf onto the end of buf, which holds *len characters; past its
 * size the text is cut, never overrun. */
static void message_append(char* buf, size_t size, size_t* len, const char* fmt, ...)
{
	va_list ap;
	int n;
	if (*len >= size - 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf + *len, size - *len, fmt, ap);
	va_end(ap);
	if (n > 0)
		*len = (*len + (size_t)n < size) ? *len + (size_t)n : size - 1;
}

/* A cheat archive from the resources folder, read once per VM. */
static void LoadCheatArchive(const char* name, u8** data, size_t* size, bool* loaded)
{
	int64_t len = 0;
	if (*loaded)
		return;
	*loaded = true;
	*data   = (u8*)host_read_resource_file(name, &len);
	*size   = (*data && len > 0) ? (size_t)len : 0;
}

static void LoadPatches(const char* serial, u32 crc)
{
	/* Four counts and their joins: never near the size. */
	char message[192];
	size_t message_len = 0;
	int patch_count = 0;
	int cheat_count = 0;
	char crc_string[9];
	snprintf(crc_string, sizeof(crc_string), "%08X", crc);
	s_patches_crc                   = crc;
	s_active_widescreen_patches     = 0;
	s_active_no_interlacing_patches = 0;
	ForgetLoadedPatches();

	if (s_python2_patch_file[0])
		patch_count = LoadPython2Patches();
	else if (EmuConfig.EnablePatches)
	{
		const GameDatabaseEntry* game = GameDatabase_FindGame(serial);
		if (game)
		{
			const char* patches = GameDatabase_FindPatch(game, crc);
			if (patches && (patch_count = LoadPatchesFromString(patches)) > 0)
			{
				log_cb(RETRO_LOG_INFO, "(GameDB) Patches Loaded: %d\n", patch_count);
				message_append(message, sizeof(message), &message_len, "%d game patches", patch_count);
			}

			LoadDynamicPatches(game->dynaPatches, game->nDynaPatches);
		}
	}

	/* the cheats */
	if (EmuConfig.EnableCheats)
	{
		cheat_count = LoadPatchesFromDir(crc_string, EmuFolders.Cheats, "Cheats", true);
		if (cheat_count > 0)
		{
			log_cb(RETRO_LOG_INFO, "Cheats Loaded: %d\n", cheat_count);
			message_append(message, sizeof(message), &message_len, "%s%d cheat patches", (patch_count > 0) ? " and " : "", cheat_count);
		}
	}

	/* widescreen patches */
	if (EmuConfig.EnableWideScreenPatches && crc != 0)
	{
		if ((s_active_widescreen_patches = LoadPatchesFromDir(crc_string, EmuFolders.CheatsWS, "Widescreen hacks", false)) > 0)
			log_cb(RETRO_LOG_INFO, "Found widescreen patches in the cheats_ws folder --> skipping cheats_ws.zip\n");
		else
		{
			/* None in the cheats_ws folder: the archive. */
			LoadCheatArchive("cheats_ws.zip", &s_widescreen_cheats_data,
				&s_widescreen_cheats_size, &s_widescreen_cheats_loaded);
			if (s_widescreen_cheats_size)
			{
				s_active_widescreen_patches = LoadPatchesFromZip(crc_string, s_widescreen_cheats_data, s_widescreen_cheats_size);
				log_cb(RETRO_LOG_INFO, "(Wide Screen Cheats DB) Patches Loaded: %d\n", s_active_widescreen_patches);
			}
		}

		if (s_active_widescreen_patches > 0)
		{
			message_append(message, sizeof(message), &message_len, "%s%d widescreen patches", (patch_count > 0 || cheat_count > 0) ? " and " : "", s_active_widescreen_patches);

			/* TODO/FIXME - 16:9 when the aspect ratio is automatic */
		}
	}

	/* no-interlacing patches */
	if (EmuConfig.EnableNoInterlacingPatches && crc != 0)
	{
		if ((s_active_no_interlacing_patches = LoadPatchesFromDir(crc_string, EmuFolders.CheatsNI, "No-interlacing patches", false)) > 0)
		{
			log_cb(RETRO_LOG_INFO, "Found no-interlacing patches in the cheats_ni folder --> skipping cheats_ni.zip\n");
		}
		else
		{
			/* None in the cheats_ni folder: the archive. */
			LoadCheatArchive("cheats_ni.zip", &s_no_interlacing_cheats_data,
				&s_no_interlacing_cheats_size, &s_no_interlacing_cheats_loaded);
			if (s_no_interlacing_cheats_size)
			{
				s_active_no_interlacing_patches = LoadPatchesFromZip(crc_string, s_no_interlacing_cheats_data, s_no_interlacing_cheats_size);
				log_cb(RETRO_LOG_INFO, "(No-Interlacing Cheats DB) Patches Loaded: %u\n", s_active_no_interlacing_patches);
			}
		}

		if (s_active_no_interlacing_patches > 0)
		{
			message_append(message, sizeof(message), &message_len, "%s%u no-interlacing patches", (patch_count > 0 || cheat_count > 0 || s_active_widescreen_patches > 0) ? " and " : "", s_active_no_interlacing_patches);

			/* and the GS's interlacing off */
			if (EmuConfig.GS.InterlaceMode == GS_INTERLACE_AUTOMATIC)
			{
				EmuConfig.GS.InterlaceMode = GS_INTERLACE_OFF;
				mtgs_apply_settings();
			}
		}
	}
	else
	{
		s_active_no_interlacing_patches = 0;
	}

	if (cheat_count > 0 || s_active_widescreen_patches > 0 || s_active_no_interlacing_patches > 0)
	{
		log_cb(RETRO_LOG_INFO, "%.*s are active.\n", (int)message_len, message);
	}
}

static void UpdateRunningGame(bool resetting, bool game_starting, bool swapping_disc)
{
	/* The CRC can be known at the BIOS, before the game starts; settings
	 * changed there are not applied as though the game were running. */
	const bool ingame      = (ElfCRC && (g_GameLoading || g_GameStarted));
	u32 new_crc            = ingame ? ElfCRC : 0;
	char new_serial[64];

	if (ingame)
		SysGetDiscID(new_serial, sizeof(new_serial));
	else
		SysGetBiosDiscID(new_serial, sizeof(new_serial));

	if (!resetting && s_game_crc == new_crc && !strcmp(s_game_serial, new_serial))
		return;

	{
		const GameDatabaseEntry* game;
		const char* memcardFilters;
		strlcpy(s_game_serial, new_serial, sizeof(s_game_serial));
		s_game_crc     = new_crc;
		game           = GameDatabase_FindGame(s_game_serial);
		memcardFilters = game ? game->memcardFilters : "";

		sioSetGameSerial(memcardFilters[0] ? memcardFilters : s_game_serial);

		/* If we don't reset the timer here, when using folder memcards the reindex will cause an eject, */
		/* which a bunch of games don't like since they access the memory card on boot. */
		if (game_starting || resetting)
			AutoEject_ClearAll();
	}

	vm_apply_settings();

	if (!swapping_disc)
	{
		/* Clear the memory card eject notification again when booting for the first time, or starting. */
		/* Otherwise, games think the card was removed on boot. */
		if (game_starting || resetting)
			AutoEject_ClearAll();

		/* Check this here, for two cases: dynarec on, and when enable cheats is set per-game. */
		if (s_patches_crc != s_game_crc)
			vm_reload_patches();
	}

	host_on_game_changed(s_disc_path, s_elf_override, s_game_serial, s_game_crc);
}

void vm_reload_patches(void)
{
	LoadPatches(s_game_serial, s_game_crc);
}

bool vm_is_elf_file_name(const char* path)
{
	/* ".elf" in either case, at the end */
	const size_t n = strlen(path);
	return n >= 4 && path[n - 4] == '.' && (path[n - 3] | 0x20) == 'e'
		&& (path[n - 2] | 0x20) == 'l' && (path[n - 1] | 0x20) == 'f';
}

static bool AutoDetectSource(const char* filename)
{
	if (filename[0])
	{
		if (!path_is_valid(filename))
		{
			log_cb(RETRO_LOG_ERROR, "Requested filename '{%s}' does not exist.\n", filename);
			return false;
		}

		if (vm_is_elf_file_name(filename))
		{
			/* An ELF boots as the ELF override, with no disc. */
			CDVDsys_ChangeSource(CDVD_SOURCE_NODISC);
			strlcpy(s_elf_override, filename, sizeof(s_elf_override));
			return true;
		}
		CDVDsys_SetFile(CDVD_SOURCE_ISO, filename);
		CDVDsys_ChangeSource(CDVD_SOURCE_ISO);
		strlcpy(s_disc_path, filename, sizeof(s_disc_path));
	}
	else
	{
		/* No fast boot without a disc. */
		CDVDsys_ChangeSource(CDVD_SOURCE_NODISC);
		EmuConfig.UseBOOT2Injection = false;
	}
	return true;
}

static bool ApplyBootParameters(const VMBootParameters* params)
{
	const char* filename = params->filename ? params->filename : "";
	EmuConfig.UseBOOT2Injection = (params->fast_boot >= 0) ? (params->fast_boot != 0) : host_option_fast_boot();

	strlcpy(s_elf_override, params->elf_override ? params->elf_override : "", sizeof(s_elf_override));
	s_disc_path[0] = '\0';

	if (params->source_type >= 0)
	{
		const CDVD_SourceType type = (CDVD_SourceType)params->source_type;
		if (params->source_type == CDVD_SOURCE_ISO && !path_is_valid(filename))
		{
			log_cb(RETRO_LOG_ERROR, "Requested filename '{%s}' does not exist.\n", filename);
			return false;
		}
		strlcpy(s_disc_path, filename, sizeof(s_disc_path));
		CDVDsys_SetFile(type, s_disc_path);
		CDVDsys_ChangeSource(type);
	}
	else if (!AutoDetectSource(filename))
		return false;

	if (s_elf_override[0])
	{
		if (!path_is_valid(s_elf_override))
		{
			log_cb(RETRO_LOG_ERROR, "Requested boot ELF '{%s}' does not exist.\n", s_elf_override);
			return false;
		}

		Hle_SetElfPath(s_elf_override);
		EmuConfig.UseBOOT2Injection = true;
	}
	else
	{
		Hle_ClearElfPath();
	}

	return true;
}

bool vm_initialize(const VMBootParameters* boot_params)
{
	/* Static, as a path is too large for a frame; only the EE boots. */
	static char bios_path[PCSX2_PATH_MAX];
	retro_atomic_store_release_int(&s_state, (int)VM_STATE_INITIALIZING);
	s_vm_thread_id = sthread_get_current_thread_id();

	if (!ApplyBootParameters(boot_params))
	{
		s_vm_thread_id = 0;
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);
		return false;
	}

	/* No BIOS, no VM. */
	emu_config_fullpath_to_bios(&EmuConfig, bios_path, sizeof(bios_path));
	if (!IsBIOSAvailable(bios_path))
	{
		s_vm_thread_id = 0;
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);
		return false;
	}

	FileMcd_EmuOpen();

	if (!OpenCDVD())
	{
		/* Every failure below this point unwinds what it opened; this
		 * one used to return with the memory cards still open and the
		 * CDVD source still set, which is the path an unreadable or
		 * oversized disc image takes. */
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_id = 0;
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);
		return false;
	}

	SPU2_Open();

	if (PADinit() != 0 || PADopen() != 0)
	{
		SPU2_Close();
		DoCDVDclose();
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_id = 0;
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);
		return false;
	}

	if (DEV9init() != 0 || DEV9open() != 0)
	{
		PADclose();
		PADshutdown();
		SPU2_Close();
		DoCDVDclose();
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_id = 0;
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);
		return false;
	}

	if (!USBopen())
	{
		DEV9close();
		DEV9shutdown();
		PADclose();
		PADshutdown();
		SPU2_Close();
		DoCDVDclose();
		CDVDsys_ClearFiles();
		FileMcd_EmuClose();
		s_vm_thread_id = 0;
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);
		return false;
	}

	FWopen();

	s_cpu_implementation_changed = false;
	UpdateCPUImplementations();
	vm_clear_cpu_execution_caches();
	fpcr_set_current(EmuConfig.Cpu.FPUFPCR);
	memBindConditionalHandlers(EmuConfig.Speedhacks.IntcStat, THREAD_VU1);

	ForgetLoadedPatches();
	UpdateVSyncRate(true);

	cpuReset();
	hwReset();

	retro_atomic_store_release_int(&s_state, (int)VM_STATE_PAUSED);

	UpdateRunningGame(true, false, false);

	SetEmuThreadAffinities();

	perf_metrics_clear();

	return true;
}

void vm_shutdown(void)
{
	/* Stopping, if not already, so nothing called below takes the VM
	 * as valid. */
	retro_atomic_store_release_int(&s_state, (int)VM_STATE_STOPPING);

	SetTimerResolutionIncreased(false);

	/* everything synced */
	if (THREAD_VU1)
		mtvu_wait_vu();
	mtgs_wait_gs(false);

	{
		LastELF[0] = '\0';
		DiscSerial[0] = '\0';
		ElfCRC = 0;
		ElfEntry = 0;

		s_disc_path[0] = '\0';
		s_elf_override[0] = '\0';
		s_game_crc = 0;
		s_patches_crc = 0;
		s_game_serial[0] = '\0';
		host_on_game_changed(s_disc_path, s_elf_override, s_game_serial, 0);
	}
	s_active_game_fixes = 0;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;

	fpcr_set_current(fpcr_default());

	ForgetLoadedPatches();
	irxIomanReset();
	vtlb_Shutdown();
	USBclose();
	SPU2_Close();
	PADclose();
	DEV9close();

	cdvdSaveNVRAM();

	DoCDVDclose();
	CDVDsys_ClearFiles();
	FWclose();
	FileMcd_EmuClose();

	mtgs_wait_for_close();

	PADshutdown();
	DEV9shutdown();

	retro_atomic_store_release_int(&s_state, (int)VM_STATE_SHUTDOWN);

	/* none of the last game's settings left */
	vm_load_settings();
}

void vm_reset(void)
{
	bool game_was_started;
	/* Running, this is likely the vsync's event test, in the middle of
	 * event handling, where resetting at once breaks games (TC:NYC): the
	 * recompiler is told to exit, and the reset follows. Paused, nothing
	 * is executing and it resets at once. */
	if ((enum vm_state)retro_atomic_load_acquire_int(&s_state) == VM_STATE_RUNNING)
	{
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_RESETTING);
		return;
	}

	mtvu_wait_vu();
	mtvu_reset();
	mtgs_wait_gs(false);

	game_was_started = g_GameStarted;

	s_active_game_fixes = 0;
	s_active_widescreen_patches = 0;
	s_active_no_interlacing_patches = 0;

	vm_clear_cpu_execution_caches();
	memBindConditionalHandlers(EmuConfig.Speedhacks.IntcStat, THREAD_VU1);
	UpdateVSyncRate(true);

	cpuReset();
	hwReset();

	/* the game's identity changed: its settings again */
	if (game_was_started)
		UpdateRunningGame(true, false, false);

	/* Paused, the state never went to resetting: it stays paused. */
	if ((enum vm_state)retro_atomic_load_acquire_int(&s_state) == VM_STATE_RESETTING)
		retro_atomic_store_release_int(&s_state, (int)VM_STATE_RUNNING);
}

bool vm_change_disc(CDVD_SourceType source, const char* path)
{
	/* What to go back to, taken before the change replaces it; static,
	 * as a path is too large for a frame. */
	static char old_path[PCSX2_PATH_MAX];
	const CDVD_SourceType old_type = CDVDsys_GetSourceType();
	bool result;
	strlcpy(old_path, CDVDsys_GetFile(old_type), sizeof(old_path));

	CDVDsys_ChangeSource(source);
	if (path && path[0])
		CDVDsys_SetFile(source, path);

	result = OpenCDVD();
	if (!result)
	{
		/* Failed to open new disc image '{}'. Reverting to old image */
		CDVDsys_ChangeSource(old_type);
		if (old_path[0])
			CDVDsys_SetFile(old_type, old_path);
		if (!OpenCDVD())
		{
			/* Failed to switch back to old disc image. Removing disc. */
			CDVDsys_ChangeSource(CDVD_SOURCE_NODISC);
			OpenCDVD();
		}
	}
	cdvd.Tray.cdvdActionSeconds = 1;
	cdvd.Tray.trayState         = CDVD_DISC_OPEN;
	return result;
}

static void InitializeCPUProviders(void)
{
	/* arm64 has no x86 recompilers / SSE VIF unpack; the interpreters are used. */
#ifndef ARCH_ARM64
	recCpu.Reserve();
	psxRec.Reserve();

	vucpu_rec_vu0_reserve();
	vucpu_rec_vu1_reserve();

	dVifReserve(0);
	dVifReserve(1);
#else
	/* arm64: reserve the IOP recompiler (psxRec, Phase C.2b) and the EE */
	/* recompiler (recCpu, Phase C.3) so their code caches/self-tests come up. */
	psxRec.Reserve();
	recCpu.Reserve();
	/* C.28-4: the armsx2 microVU1 transplant is the default VU1 provider. */
	/* Its Reserve() also opens vu1Thread (idempotent with the Open() below). */
	vucpu_rec_vu1_reserve();
	/* C.30-1: microVU0 for VU0 micro programs (VCALLMS/VCALLMSR). */
	vucpu_rec_vu0_reserve();
	/* The MTVU worker thread is normally spawned by vucpu_rec_vu1_reserve() */
	/* (x86/microVU.c), which doesn't exist in this build -- with vuThread */
	/* enabled the EE would push to vu1Thread's ring and wait on its WorkSema */
	/* forever (boot hang / black screen). The worker runs VU1 through */
	/* CpuVU1->Execute(), so it works with the VU interpreter provider too. */
	mtvu_open();
	/* arm64 VIF unpack dynarec (C.19, NEON port transplanted from ARMSX2). */
	dVifReserve(0);
	dVifReserve(1);
#endif

	gs_code_reserve_assign(&s_vm_memory.code);

	VifUnpackSSE_Init(); /* on arm64 this generates the NEON nVifUpk kernels (C.19) */
}

static void ShutdownCPUProviders(void)
{
	gs_code_reserve_release();

#ifndef ARCH_ARM64
	dVifRelease(1);
	dVifRelease(0);

	VifUnpackSSE_Destroy();

	vucpu_rec_vu1.Shutdown();
	vucpu_rec_vu0.Shutdown();

	psxRec.Shutdown();
	recCpu.Shutdown();
#else
	vucpu_rec_vu0.Shutdown();
	vucpu_rec_vu1.Shutdown(); /* waits on the MTVU worker, then mVUclose */
	mtvu_close();
	dVifRelease(1);
	dVifRelease(0);
	VifUnpackSSE_Destroy();
	psxRec.Shutdown();
	recCpu.Shutdown();
#endif
}

static void UpdateCPUImplementations(void)
{
#ifndef ARCH_ARM64
	Cpu    = CHECK_EEREC ? &recCpu : &intCpu;
	psxCpu = CHECK_IOPREC ? &psxRec : &psxInt;
#else
	/* arm64: IOP uses the arm64 recompiler (Phase C.2b). EE uses the arm64 EE */
	/* recompiler (Phase C.3) when CHECK_EEREC is set; C.3-1 blocks call the */
	/* interpreter so behaviour is identical while the JIT plumbing runs. */
	Cpu    = CHECK_EEREC ? &recCpu : &intCpu;
	psxCpu = &psxRec;
#endif

	CpuVU0 = &vucpu_interp_vu0;
	CpuVU1 = &vucpu_interp_vu1;

	/* Soft float per unit: the interpreter computes every VU float op
	 * through the ps2float model, so "soft" for a unit means keeping that
	 * unit on the interpreter even when its recompiler is enabled. Same
	 * routing on both architectures. A per-op soft dispatch inside
	 * microVU can replace this later without changing the option. */
	if (CHECK_VU_SOFT_REC(0) && EmuConfig.Cpu.Recompiler.EnableVU0)
		log_cb(RETRO_LOG_INFO, "VU0 soft float: micro programs on the exact interpreter (macro-mode COP2 is not covered).\n");
	if (CHECK_VU_SOFT_REC(1) && EmuConfig.Cpu.Recompiler.EnableVU1)
		log_cb(RETRO_LOG_INFO, "VU1 soft float: running on the exact interpreter.\n");

#ifdef ARCH_ARM64
	/* C.30-1: microVU0 runs VU0 micro programs natively (macro-mode COP2 */
	/* stays on the C.29-1 inline interpreter calls until C.30-2). */
	if (EmuConfig.Cpu.Recompiler.EnableVU0 && !CHECK_VU_SOFT_REC(0))
		CpuVU0 = &vucpu_rec_vu0;
#endif

#ifndef ARCH_ARM64
	if (EmuConfig.Cpu.Recompiler.EnableVU0 && !CHECK_VU_SOFT_REC(0))
		CpuVU0 = &vucpu_rec_vu0;

	if (EmuConfig.Cpu.Recompiler.EnableVU1 && !CHECK_VU_SOFT_REC(1))
		CpuVU1 = &vucpu_rec_vu1;
#else
	/* C.28-4: microVU1 (the armsx2 transplant, native VU codegen) is the VU1 */
	/* provider -- verified register-exact against the interpreter. */
	if (EmuConfig.Cpu.Recompiler.EnableVU1 && !CHECK_VU_SOFT_REC(1))
	{
		CpuVU1 = &vucpu_rec_vu1;
		log_cb(RETRO_LOG_INFO, "arm64 VU1 rec: microVU1 (native codegen) is the default provider.\n");
	}
#endif
}

void vm_clear_cpu_execution_caches(void)
{
	Cpu->Reset();
	psxCpu->Reset();

#ifndef ARCH_ARM64
	/* mVU's VU0 needs to be properly initialized for macro mode even if it's not used for micro mode! */
	if (CHECK_EEREC && !EmuConfig.Cpu.Recompiler.EnableVU0)
		vucpu_rec_vu0.Reset();
#endif

	CpuVU0->Reset();
	CpuVU1->Reset();

#ifndef ARCH_ARM64
	dVifReset(0);
	dVifReset(1);
#endif
}

void vm_execute(void)
{
	/* An interpreter/recompiler switch: the new ones in, and reset. */
	if (s_cpu_implementation_changed)
	{
		s_cpu_implementation_changed = false;
		UpdateCPUImplementations();
		vm_clear_cpu_execution_caches();
		vtlb_ResetFastmem();
	}

	/* until asked to stop */
	Cpu->Execute();
}

void vm_set_paused(bool paused)
{
	if (!vm_has_valid_vm())
		return;

	if (paused)
	{
		log_cb(RETRO_LOG_DEBUG, "(VMManager) Pausing...\n");
		vm_set_state(VM_STATE_PAUSED);
	}
	else
	{
		log_cb(RETRO_LOG_DEBUG, "(VMManager) Resuming...\n");
		vm_set_state(VM_STATE_RUNNING);
	}
}

bool vm_execution_interrupted(void)
{
	return (enum vm_state)retro_atomic_load_acquire_int(&s_state) != VM_STATE_RUNNING || s_cpu_implementation_changed;
}

void vm_entry_point_compiling_on_cpu_thread(void)
{
	/* The running game is not updated until its entry point runs, as
	 * that can change settings and flush the recompiler; but a game whose
	 * entry point is in a patch (WRC 4) needs its patches now. */
	{
		char serial[64];
		SysGetDiscID(serial, sizeof(serial));
		LoadPatches(serial, ElfCRC);
	}
	ApplyLoadedPatches(1u << PPT_ONCE_ON_LOAD);
}

void vm_game_starting_on_cpu_thread(void)
{
	UpdateRunningGame(false, true, false);
	ApplyLoadedPatches((1u << PPT_ONCE_ON_LOAD) | (1u << PPT_COMBINED_0_1));
}

static void CheckForCPUConfigChanges(const Pcsx2Config* old_config)
{
	if (emu_cpu_equal(&EmuConfig.Cpu, &old_config->Cpu) &&
		EmuConfig.Gamefixes.bitset == old_config->Gamefixes.bitset &&
		emu_speedhack_equal(&EmuConfig.Speedhacks, &old_config->Speedhacks)
		)
		return;

	log_cb(RETRO_LOG_INFO, "Updating CPU configuration...\n");
	fpcr_set_current(EmuConfig.Cpu.FPUFPCR);
	vm_clear_cpu_execution_caches();
	memBindConditionalHandlers(EmuConfig.Speedhacks.IntcStat, THREAD_VU1);

	if (EmuConfig.Cpu.Recompiler.EnableFastmem != old_config->Cpu.Recompiler.EnableFastmem)
		vtlb_ResetFastmem();

	/* A recompiler toggled: switched at the next vm_execute, as the CPU
	 * is still executing here. */
	if (emu_cpu_recompilers_changed(&EmuConfig.Cpu, &old_config->Cpu))
		s_cpu_implementation_changed = true;

	if (EmuConfig.Cpu.AffinityControlMode != old_config->Cpu.AffinityControlMode ||
		EmuConfig.Speedhacks.vuThread != old_config->Speedhacks.vuThread)
		SetEmuThreadAffinities();
}

static void CheckForGSConfigChanges(const Pcsx2Config* old_config)
{
	if (emu_gs_equal(&EmuConfig.GS, &old_config->GS))
		return;
	UpdateVSyncRate(true);
	mtgs_apply_settings();
}

static void CheckForPatchConfigChanges(const Pcsx2Config* old_config)
{
	if (EmuConfig.EnableCheats == old_config->EnableCheats &&
		EmuConfig.EnableWideScreenPatches == old_config->EnableWideScreenPatches &&
		EmuConfig.EnablePatches == old_config->EnablePatches)
		return;

	vm_reload_patches();
}

static void CheckForDEV9ConfigChanges(const Pcsx2Config* old_config)
{
	if (emu_dev9_equal(&EmuConfig.DEV9, &old_config->DEV9))
		return;

	DEV9CheckChanges(old_config);
}

static void CheckForMemoryCardConfigChanges(const Pcsx2Config* old_config)
{
	bool changed = false;
	size_t i;
	u32 port, slot;

	for (i = 0; i < C89_ARRAY_SIZE(EmuConfig.Mcd); i++)
	{
		if (EmuConfig.Mcd[i].Enabled != old_config->Mcd[i].Enabled ||
			strcmp(EmuConfig.Mcd[i].Filename, old_config->Mcd[i].Filename) != 0)
		{
			changed = true;
			break;
		}
	}

	changed |= (EmuConfig.McdEnableEjection != old_config->McdEnableEjection);
	changed |= (EmuConfig.McdFolderAutoManage != old_config->McdFolderAutoManage);

	if (!changed)
		return;

	FileMcd_EmuClose();
	FileMcd_EmuOpen();

	/* a card whose file changed is ejected */
	for (port = 0; port < 2; port++)
	{
		for (slot = 0; slot < 4; slot++)
		{
			const uint index = FileMcd_ConvertToSlot(port, slot);
			if (EmuConfig.Mcd[index].Enabled != old_config->Mcd[index].Enabled ||
				strcmp(EmuConfig.Mcd[index].Filename, old_config->Mcd[index].Filename) != 0)
				AutoEject_Set(port, slot);
		}
	}

	/* and the folder cards reindexed */
	{
		const GameDatabaseEntry* game = GameDatabase_FindGame(s_game_serial);
		const char* filters           = game ? game->memcardFilters : "";
		sioSetGameSerial(filters[0] ? filters : s_game_serial);
	}
}

static void CheckForConfigChanges(const Pcsx2Config* old_config)
{
	if (vm_has_valid_vm())
	{
		CheckForCPUConfigChanges(old_config);
		CheckForPatchConfigChanges(old_config);
		CheckForDEV9ConfigChanges(old_config);
		CheckForMemoryCardConfigChanges(old_config);

		if (EmuConfig.EnableCheats != old_config->EnableCheats ||
			EmuConfig.EnableWideScreenPatches != old_config->EnableWideScreenPatches ||
			EmuConfig.EnableNoInterlacingPatches != old_config->EnableNoInterlacingPatches)
			vm_reload_patches();

		CheckForGSConfigChanges(old_config);
	}
}

void vm_apply_settings(void)
{
	/* vm_load_settings builds the config afresh from the options and the
	 * runtime fields, so nothing optional keeps a stale value; the old one
	 * is kept to compare against, off the stack (see vm_load_settings). */
	static Pcsx2Config old_config;
	/* Running, the threads are synced first. */
	const bool running = ((enum vm_state)retro_atomic_load_acquire_int(&s_state) == VM_STATE_RUNNING);
	if (running)
	{
		if (THREAD_VU1)
			mtvu_wait_vu();
		mtgs_wait_gs(false);
	}

	old_config = EmuConfig;
	vm_load_settings();
	CheckForConfigChanges(&old_config);
}

bool vm_mtvu_menu_default    = true;
bool vm_fastmem_menu_default = true;


#ifdef _WIN32

static bool s_timer_resolution_increased = false;

static void SetTimerResolutionIncreased(bool enabled)
{
	if (s_timer_resolution_increased == enabled)
		return;

	if (enabled)
		s_timer_resolution_increased = (timeBeginPeriod(1) == TIMERR_NOERROR);
	else if (s_timer_resolution_increased)
	{
		timeEndPeriod(1);
		s_timer_resolution_increased = false;
	}
}

#else

static void SetTimerResolutionIncreased(bool enabled)
{
	(void)enabled;
}

#endif

/* Filled once in retro_init (vm_initialize_cpu_info), before any
 * thread that reads it exists, and never written again. */
static u32    s_processor_list[64];
static size_t s_processor_count;
static bool   s_processor_list_initialized = false;

void vm_initialize_cpu_info(void)
{
	unsigned order[64];
	char     line[64 * 4 + 32];
	size_t   i, count, pos;

	if (s_processor_list_initialized)
		return;
	s_processor_list_initialized = true;

	fpcr_set_current(fpcr_default());

	/* features_cpu ranks the processors: strongest core first, and an SMT
	 * sibling behind the processor it shares a core with, which is the
	 * order the affinity assignment indexes into. Platforms that publish
	 * no topology answer in ascending order; one that cannot name its
	 * processors answers with nothing, and affinity control stays off. */
	count = cpu_features_get_processor_order(order, C89_ARRAY_SIZE(order));
	if (count == 0)
	{
		log_cb(RETRO_LOG_ERROR, "No processor list available\n");
		return;
	}

	pos = (size_t)snprintf(line, sizeof(line), "Ordered processor list: ");
	for (i = 0; i < count; i++)
	{
		s_processor_list[i] = (u32)order[i];
		if (pos < sizeof(line))
			pos += (size_t)snprintf(line + pos, sizeof(line) - pos, i ? ", %u" : "%u", order[i]);
	}
	s_processor_count = count;
	log_cb(RETRO_LOG_INFO, "%s\n", line);
}

/* The MTVU and instant-VU1 defaults keep the platform guard they had: the
 * processor list is available everywhere now, but which platforms write
 * these defaults is a separate question from how the CPU is queried. */
#if defined(__linux__) || defined(_WIN32)
static void SetMTVUAndAffinityControlDefault(Pcsx2Config* c)
{
	/* arm64 MTVU status: the worker thread spawns (InitializeCPUProviders) and */
	/* sets VPU_STAT busy around interpreter VU1 programs (MTVU.c), which makes */
	/* MTVU produce correct XGKICK output. However, games driving a CONTINUOUS */
	/* VU1 program (an endless microprogram streaming XGKICK packets, e.g. GT3's */
	/* arcade attract) deadlock the one-packet-per-program MTVU handoff with the */
	/* VU1 INTERPRETER: the worker drip-feeds gsPack mid-program until path1's */
	/* 9MB buffer fills (gif_path_copy -> WaitGS(true) with an empty */
	/* gsPackQueue), while MTGS sits in semaXGkick.Wait() for a program end that */
	/* never comes. Needs a partial-packet flush protocol (or a VU recompiler */
	/* with upstream's XGKICK handling) -- until C.28-4 MTVU was opt-in */
	/* (helps VU-light titles; MMX7 verified pixel-identical). */
	/* C.28-4: with microVU1 as the default VU1 provider (native codegen, */
	/* verified register-exact against the interpreter), MTVU and instant VU1 are */
	/* safe defaults again -- the C.13c livelock and the 3M-cycle-budget wall */
	/* were interpreter-provider problems, and microVU1 is now the only VU1 */
	/* provider. */
	const bool mtvu = vm_mtvu_hardware_allowed() && vm_mtvu_menu_default;
	log_cb(RETRO_LOG_INFO, "%s\n", mtvu ? "  MTVU enabled (pcsx2_mtvu; requires >= 3 physical cores)."
	                     : "  MTVU disabled.");
	c->Speedhacks.vuThread = mtvu;
	/* Instant VU1 assumes the VU1 provider finishes a program quickly (x86 */
	/* microVU). With the VU1 INTERPRETER, a continuous microprogram (endless */
	/* loop streaming XGKICK -- GT3's arcade attract) burns the full */
	/* vu1RunCycles=3M budget on EVERY kick: the EE thread spends seconds per */
	/* frame inside InterpVU1::Execute and the frontend appears hung. Run VU1 */
	/* in small interleaved slices instead (upstream's non-instant scheduling). */
	log_cb(RETRO_LOG_INFO, "  Instant VU1 enabled (microVU1 native provider).\n");
	c->Speedhacks.vu1Instant = true;
	/* Fastmem follows the same shape as MTVU: this default-setter can run
	 * again on a settings reset after check_variables(true) has consumed
	 * the option, so the option's value has to ride a VMManager global to
	 * survive it. The truthful "which mode is this run in" line lives in
	 * LoadSettings, printed from the config the recompilers actually
	 * read - never from here, where the global may not be fed yet. */
	c->Cpu.Recompiler.EnableFastmem = vm_fastmem_menu_default;
}

#else
static void SetMTVUAndAffinityControlDefault(Pcsx2Config* c) { (void)c; }
#endif

/* Single source of truth for the MTVU hardware gate.  The worker is only
 * worth spawning (and only spawned) with >= 3 physical cores - EE, VU
 * and GS each want a core of their own, and two SMT threads on one core
 * are not that (a 2-core/4-thread part used to pass on its thread count
 * and put the three on two cores); every writer of EmuCore/Speedhacks
 * vuThread must apply this same predicate, otherwise THREAD_VU1 can come
 * up true with no worker alive and mtgs_main_loop waits on a handoff
 * that never arrives (boot hang). */
bool vm_mtvu_hardware_allowed(void)
{
	return cpu_features_get_core_amount_physical() >= 3;
}

void vm_set_vm_thread(sthread_t* thread)
{
	s_vm_thread = thread;
}

static bool set_ee_affinity(u64 mask)
{
	if (sthread_get_current_thread_id() == s_vm_thread_id)
		return sthread_set_current_affinity(mask);
	return s_vm_thread ? sthread_set_affinity(s_vm_thread, mask) : false;
}

static void SetEmuThreadAffinities(void)
{
	static const u8 processor_assignment[7][2][3] = {
		/* EE xx GS  EE VU GS */
		{{0, 2, 1}, {0, 1, 2}}, /* Disabled */
		{{0, 2, 1}, {0, 1, 2}}, /* EE > VU > GS */
		{{0, 2, 1}, {0, 2, 1}}, /* EE > GS > VU */
		{{0, 2, 1}, {1, 0, 2}}, /* VU > EE > GS */
		{{1, 2, 0}, {2, 0, 1}}, /* VU > GS > EE */
		{{1, 2, 0}, {1, 2, 0}}, /* GS > EE > VU */
		{{1, 2, 0}, {2, 1, 0}}, /* GS > VU > EE */
	};
	const u8* this_proc_assigment;
	u32 ee_index, vu_index;
	u64 ee_affinity;
	/* not supported on this platform */
	if (s_processor_count == 0)
		return;

	if (EmuConfig.Cpu.AffinityControlMode == 0 ||
		s_processor_count < (EmuConfig.Speedhacks.vuThread ? 3u : 2u))
	{
		if (EmuConfig.Cpu.AffinityControlMode != 0)
			log_cb(RETRO_LOG_ERROR, "Insufficient processors for affinity control.\n");

		sthread_set_affinity(mtvu_thread(), 0);
		set_ee_affinity(0);
		return;
	}

	/* without MTVU, the EE takes the VU's processor */
	this_proc_assigment = processor_assignment[EmuConfig.Cpu.AffinityControlMode][EmuConfig.Speedhacks.vuThread];
	ee_index = s_processor_list[this_proc_assigment[0]];
	vu_index = s_processor_list[this_proc_assigment[1]];
	log_cb(RETRO_LOG_INFO, "Processor order assignment: EE=%u, VU=%u, GS=%u\n",
		this_proc_assigment[0], this_proc_assigment[1], this_proc_assigment[2]);

	ee_affinity = (u64)1 << ee_index;
	log_cb(RETRO_LOG_INFO, "EE thread is on processor %u (0x%llx)\n", ee_index, (unsigned long long)ee_affinity);
	set_ee_affinity(ee_affinity);

	if (EmuConfig.Speedhacks.vuThread)
	{
		const u64 vu_affinity = (u64)1 << vu_index;
		log_cb(RETRO_LOG_INFO, "VU thread is on processor %u (0x%llx)\n", vu_index, (unsigned long long)vu_affinity);
		sthread_set_affinity(mtvu_thread(), vu_affinity);
	}
	else
		sthread_set_affinity(mtvu_thread(), 0);
}

void vm_apply_hardware_defaults(Pcsx2Config* c)
{
	SetMTVUAndAffinityControlDefault(c);
}

const u32* vm_get_sorted_processor_list(u32* count)
{
	*count = (u32)s_processor_count;
	return s_processor_list;
}
