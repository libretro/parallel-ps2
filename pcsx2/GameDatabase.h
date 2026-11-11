/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#ifndef PCSX2_GAMEDATABASE_H
#define PCSX2_GAMEDATABASE_H

#include "common/Pcsx2Defs.h"
#include "Patch.h"
#include "EmuOptions.h"

#include "Config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An entry's clamp mode; UNDEFINED when it sets none. */
enum gamedb_clamp_mode
{
	GAMEDB_CLAMP_UNDEFINED = -1,
	GAMEDB_CLAMP_DISABLED  = 0,
	GAMEDB_CLAMP_NORMAL,
	GAMEDB_CLAMP_EXTRA,
	GAMEDB_CLAMP_FULL
};

/* An entry's round modes are FPRoundMode values; this, one past the
 * last of them, when it sets none. */
#define GAMEDB_ROUND_UNSET 4

/* The GS hardware fixes, in the order of their names in the database. */
enum gamedb_hwfix
{
	/* boolean settings */
	GAMEDB_HWFIX_CPU_FRAMEBUFFER_CONVERSION,
	GAMEDB_HWFIX_FLUSH_TC_ON_CLOSE,
	GAMEDB_HWFIX_DISABLE_DEPTH_SUPPORT,
	GAMEDB_HWFIX_PRELOAD_FRAME_DATA,
	GAMEDB_HWFIX_DISABLE_PARTIAL_INVALIDATION,
	GAMEDB_HWFIX_TEXTURE_INSIDE_RT,
	GAMEDB_HWFIX_ALIGN_SPRITE,
	GAMEDB_HWFIX_MERGE_SPRITE,
	GAMEDB_HWFIX_MIPMAP,
	GAMEDB_HWFIX_FORCE_EVEN_SPRITE_POSITION,
	GAMEDB_HWFIX_NATIVE_PALETTE_DRAW,
	GAMEDB_HWFIX_ESTIMATE_TEXTURE_REGION,
	GAMEDB_HWFIX_PCRTC_OFFSETS,
	GAMEDB_HWFIX_PCRTC_OVERSCAN,

	/* integer settings */
	GAMEDB_HWFIX_TRILINEAR_FILTERING,
	GAMEDB_HWFIX_SKIP_DRAW_START,
	GAMEDB_HWFIX_SKIP_DRAW_END,
	GAMEDB_HWFIX_HALF_BOTTOM_OVERRIDE,
	GAMEDB_HWFIX_HALF_PIXEL_OFFSET,
	GAMEDB_HWFIX_ROUND_SPRITE,
	GAMEDB_HWFIX_NATIVE_SCALING,
	GAMEDB_HWFIX_TEXTURE_PRELOADING,
	GAMEDB_HWFIX_DEINTERLACE,
	GAMEDB_HWFIX_CPU_SPRITE_RENDER_BW,
	GAMEDB_HWFIX_CPU_SPRITE_RENDER_LEVEL,
	GAMEDB_HWFIX_CPU_CLUT_RENDER,
	GAMEDB_HWFIX_GPU_PALETTE_CONVERSION,
	GAMEDB_HWFIX_MINIMUM_BLENDING_LEVEL,
	GAMEDB_HWFIX_MAXIMUM_BLENDING_LEVEL,
	GAMEDB_HWFIX_RECOMMENDED_BLENDING_LEVEL,
	GAMEDB_HWFIX_GET_SKIP_COUNT,
	GAMEDB_HWFIX_BEFORE_DRAW,
	GAMEDB_HWFIX_MOVE_HANDLER,
	GAMEDB_HWFIX_LIMIT_24BIT_DEPTH,

	GAMEDB_HWFIX_COUNT
};

/* A speed hack (SpeedHack) or GS hardware fix (gamedb_hwfix) the
 * database sets, and its value. */
typedef struct GameDatabaseSpeedHack
{
	u32 id;
	s32 value;
} GameDatabaseSpeedHack;

typedef struct GameDatabaseHWFix
{
	u32 id;
	s32 value;
} GameDatabaseHWFix;

/* A patch block, by the ELF CRC it is for; 0 is the default block. */
typedef struct GameDatabasePatch
{
	u32 crc;
	const char* text;
} GameDatabasePatch;

/* One game's entry. Everything it points at belongs to the loaded
 * database and goes with GameDatabase_Unload. */
typedef struct GameDatabaseEntry
{
	const char* serial; /* lower case, as looked up */
	const char* name;
	const char* region;
	u8 eeRoundMode;
	u8 eeDivRoundMode;
	u8 vu0RoundMode;
	u8 vu1RoundMode;
	s8 eeClampMode;
	s8 vu0ClampMode;
	s8 vu1ClampMode;
	const u8* gameFixes; /* GamefixId values */
	u32 nGameFixes;
	const GameDatabaseSpeedHack* speedHacks;
	u32 nSpeedHacks;
	const GameDatabaseHWFix* gsHWFixes;
	u32 nGSHWFixes;
	/* The memory card serials, '/' between them; "" when there are none. */
	const char* memcardFilters;
	const GameDatabasePatch* patches;
	u32 nPatches;
	const DynamicPatch* dynaPatches;
	u32 nDynaPatches;
} GameDatabaseEntry;

/* The VM's thread, as the VM starts and as it ends: Load reads the
 * database (once; a second call returns at once), Unload drops it.
 * Lookups are made on the EE thread or with the EE paused, between the
 * two, and only read. */
void GameDatabase_Load(void);
void GameDatabase_Unload(void);
/* The entry for a serial, in either case; NULL when there is none or the
 * database is not loaded. */
const GameDatabaseEntry* GameDatabase_FindGame(const char* serial);
/* The patch block for the CRC, or the default block; NULL for a CRC of 0
 * or when there is neither. */
const char* GameDatabase_FindPatch(const GameDatabaseEntry* entry, u32 crc);

/* A GS hardware fix's name in the database. */
const char* gamedb_hwfix_name(u32 id);
/* Whether a fix is one the user's manual hacks setting can override. */
bool gamedb_hwfix_is_user_hack(u32 id);

/* What GameDatabase.c reads from elsewhere: the database built into the
 * core (GameDatabaseBuiltin.c); whether to prefer GameIndex.yaml in the
 * resources folder (main.cpp) and that folder (Pcsx2Config.c); and the
 * GS function names three fixes take (GSHwHack.cpp). */
extern const unsigned char g_gameDatabaseBuiltin[];
extern const size_t g_gameDatabaseBuiltinSize;
bool emu_config_external_gameindex(void);
const char* emu_folders_resources(void);
s16 GSLookupGetSkipCountFunctionId(const char* name, size_t length);
s16 GSLookupBeforeDrawFunctionId(const char* name, size_t length);
s16 GSLookupMoveHandlerFunctionId(const char* name, size_t length);

/* GameDatabaseFixes.c. Applies the core game fixes to a config;
 * returns how many applied. */
u32 GameDatabase_ApplyGameFixes(const GameDatabaseEntry* entry, Pcsx2Config* config, bool applyAuto);
/* Applies the GS hardware fixes to a config; returns how many applied. */
u32 GameDatabase_ApplyGSHardwareFixes(const GameDatabaseEntry* entry, EmuGSOptions* config);

#ifdef __cplusplus
}
#endif

#endif
