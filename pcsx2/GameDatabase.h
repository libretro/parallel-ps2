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

#pragma once

#include "Config.h"
#include "Patch.h"

#include "../common/FPControl.h"

enum GamefixId;

namespace GameDatabaseSchema
{
	enum class ClampMode
	{
		Undefined = -1,
		Disabled = 0,
		Normal,
		Extra,
		Full,
		Count
	};

	enum class GSHWFixId : u32
	{
		// boolean settings
		CPUFramebufferConversion,
		FlushTCOnClose,
		DisableDepthSupport,
		PreloadFrameData,
		DisablePartialInvalidation,
		TextureInsideRT,
		AlignSprite,
		MergeSprite,
		Mipmap,
		ForceEvenSpritePosition,
		BilinearUpscale,
		NativePaletteDraw,
		EstimateTextureRegion,
		PCRTCOffsets,
		PCRTCOverscan,

		// integer settings
		TrilinearFiltering,
		SkipDrawStart,
		SkipDrawEnd,
		HalfBottomOverride,
		HalfPixelOffset,
		RoundSprite,
		NativeScaling,
		TexturePreloading,
		Deinterlace,
		CPUSpriteRenderBW,
		CPUSpriteRenderLevel,
		CPUCLUTRender,
		GPUPaletteConversion,
		MinimumBlendingLevel,
		MaximumBlendingLevel,
		RecommendedBlendingLevel,
		GetSkipCount,
		BeforeDraw,
		MoveHandler,
		Limit24BitDepth,

		Count
	};
};

/* A speed hack or GS hardware fix the database sets, and its value. */
struct GameDatabaseSpeedHack
{
	SpeedHack id;
	int value;
};

struct GameDatabaseHWFix
{
	GameDatabaseSchema::GSHWFixId id;
	s32 value;
};

/* A patch block, by the ELF CRC it is for; 0 is the default block. */
struct GameDatabasePatch
{
	u32 crc;
	const char* text;
};

/* One game's entry. Everything it points at belongs to the loaded
 * database and goes with GameDatabase_Unload. */
struct GameDatabaseEntry
{
	const char* serial; /* lower case, as looked up */
	const char* name;
	const char* region;
	FPRoundMode eeRoundMode;
	FPRoundMode eeDivRoundMode;
	FPRoundMode vu0RoundMode;
	FPRoundMode vu1RoundMode;
	GameDatabaseSchema::ClampMode eeClampMode;
	GameDatabaseSchema::ClampMode vu0ClampMode;
	GameDatabaseSchema::ClampMode vu1ClampMode;
	const GamefixId* gameFixes;
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
};

/* Loads the database once; later calls return at once. */
void GameDatabase_EnsureLoaded(void);
/* Drops the loaded database. Safe at any time: a lookup after it loads
 * the file again. */
void GameDatabase_Unload(void);
/* The entry for a serial, in either case, loading the database first;
 * NULL when there is none. */
const GameDatabaseEntry* GameDatabase_FindGame(const char* serial);
/* The patch block for the CRC, or the default block; NULL for a CRC of 0
 * or when there is neither. */
const char* GameDatabase_FindPatch(const GameDatabaseEntry* entry, u32 crc);
/* Applies the core game fixes to a config; returns how many applied. */
u32 GameDatabase_ApplyGameFixes(const GameDatabaseEntry* entry, Pcsx2Config& config, bool applyAuto);
/* Applies the GS hardware fixes to a config; returns how many applied. */
u32 GameDatabase_ApplyGSHardwareFixes(const GameDatabaseEntry* entry, Pcsx2Config::GSOptions& config);
