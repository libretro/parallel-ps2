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

#include <sstream>
#include "StringView.h"
#include "FormatString.h"
#include "ParseNumber.h"
#include "common/Pcsx2Defs.h"
#include <fstream>
#include <optional>
#include <utility>


#include <formats/ryaml.h>

#include "HostFS.h"

#include "GameDatabase.h"
#include "GS/GS.h"
#include "Host.h"
#include "vtlb.h"
#include "SLockGuard.h"

namespace GameDatabaseSchema
{
	static const char* getHWFixName(GSHWFixId id);
	static std::optional<GSHWFixId> parseHWFixName(const std::string_view& name);
	static bool isUserHackHWFix(GSHWFixId id);
} // namespace GameDatabaseSchema

namespace GameDatabase
{
	static void parseAndInsert(const char *serial, const ryaml_t *yaml, int node);
	static void initDatabase();
} // namespace GameDatabase

static constexpr char GAMEDB_YAML_FILE_NAME[] = "GameIndex.yaml";

static std::unordered_map<std::string, GameDatabaseSchema::GameEntry> s_game_db;
static slock_t* s_load_once_mutex(void)
{
	/* First use creates it; C++11 makes the init thread-safe, and there is
	 * no static-init order to worry about. */
	static slock_t* lock = slock_new();
	return lock;
}
static bool s_load_once_done = false;

std::string GameDatabaseSchema::GameEntry::memcardFiltersAsString() const
{
	std::string ret;
	for (size_t i = 0; i < memcardFilters.size(); i++)
	{
		if (i != 0)
			ret += '/';
		ret += memcardFilters[i];
	}
	return ret;
}

const std::string* GameDatabaseSchema::GameEntry::findPatch(u32 crc) const
{
	if (crc == 0)
		return nullptr;

	log_cb(RETRO_LOG_INFO, "[GameDB] Searching for patch with CRC '%08X'\n", crc);

	auto it = patches.find(crc);
	if (it != patches.end())
	{
		log_cb(RETRO_LOG_INFO, "[GameDB] Found patch with CRC '%08X'\n", crc);
		return &it->second;
	}

	it = patches.find(0);
	if (it != patches.end())
	{
		log_cb(RETRO_LOG_INFO, "[GameDB] Found and falling back to default patch\n");
		return &it->second;
	}
	log_cb(RETRO_LOG_INFO, "[GameDB] No CRC-specific patch or default patch found\n");
	return nullptr;
}


/* Small adapters over the C reader, so the extraction sites below read
 * the way they did when this used a C++ YAML library. A node handle of
 * RYAML_NONE is inert, so a lookup that missed can be passed straight
 * on without a test at each step. */
static std::string yamlString(const ryaml_t* yaml, int node)
{
	size_t len = 0;
	const char* str = ryaml_val(yaml, node, &len);
	return str ? std::string(str, len) : std::string();
}

static std::string_view yamlKeyView(const ryaml_t* yaml, int node)
{
	size_t len = 0;
	const char* str = ryaml_key(yaml, node, &len);
	return str ? std::string_view(str, len) : std::string_view();
}

static std::string_view yamlValView(const ryaml_t* yaml, int node)
{
	size_t len = 0;
	const char* str = ryaml_val(yaml, node, &len);
	return str ? std::string_view(str, len) : std::string_view();
}

static int yamlInt(const ryaml_t* yaml, int node, int def)
{
	int value = def;
	ryaml_val_int(yaml, node, &value);
	return value;
}

void GameDatabase::parseAndInsert(const char *serial, const ryaml_t *yaml, int node)
{
	GameDatabaseSchema::GameEntry gameEntry;
	gameEntry.name   = yamlString(yaml, ryaml_find_child(yaml, node, "name"));
	gameEntry.region = yamlString(yaml, ryaml_find_child(yaml, node, "region"));
	{
		const int roundModes = ryaml_find_child(yaml, node, "roundModes");
		{
			const int eeVal = yamlInt(yaml,
				ryaml_find_child(yaml, roundModes, "eeRoundMode"), -1);
			if (eeVal >= 0 && eeVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.eeRoundMode = static_cast<FPRoundMode>(eeVal);
		}
		{
			const int eeVal = yamlInt(yaml,
				ryaml_find_child(yaml, roundModes, "eeDivRoundMode"), -1);
			if (eeVal >= 0 && eeVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.eeDivRoundMode = static_cast<FPRoundMode>(eeVal);
		}
		{
			const int vuVal = yamlInt(yaml,
				ryaml_find_child(yaml, roundModes, "vuRoundMode"), -1);
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
			{
				gameEntry.vu0RoundMode = static_cast<FPRoundMode>(vuVal);
				gameEntry.vu1RoundMode = static_cast<FPRoundMode>(vuVal);
			}
		}
		{
			const int vuVal = yamlInt(yaml,
				ryaml_find_child(yaml, roundModes, "vu0RoundMode"), -1);
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.vu0RoundMode = static_cast<FPRoundMode>(vuVal);
		}
		{
			const int vuVal = yamlInt(yaml,
				ryaml_find_child(yaml, roundModes, "vu1RoundMode"), -1);
			if (vuVal >= 0 && vuVal < static_cast<int>(FPRoundMode::MaxCount))
				gameEntry.vu1RoundMode = static_cast<FPRoundMode>(vuVal);
		}
	}
	{
		const int clampModes = ryaml_find_child(yaml, node, "clampModes");
		if (ryaml_has_child(yaml, clampModes, "eeClampMode"))
		{
			const int eeVal = yamlInt(yaml,
				ryaml_find_child(yaml, clampModes, "eeClampMode"), -1);
			gameEntry.eeClampMode = static_cast<GameDatabaseSchema::ClampMode>(eeVal);
		}
		if (ryaml_has_child(yaml, clampModes, "vuClampMode"))
		{
			const int vuVal = yamlInt(yaml,
				ryaml_find_child(yaml, clampModes, "vuClampMode"), -1);
			gameEntry.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
			gameEntry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
		if (ryaml_has_child(yaml, clampModes, "vu0ClampMode"))
		{
			const int vuVal = yamlInt(yaml,
				ryaml_find_child(yaml, clampModes, "vu0ClampMode"), -1);
			gameEntry.vu0ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
		if (ryaml_has_child(yaml, clampModes, "vu1ClampMode"))
		{
			const int vuVal = yamlInt(yaml,
				ryaml_find_child(yaml, clampModes, "vu1ClampMode"), -1);
			gameEntry.vu1ClampMode = static_cast<GameDatabaseSchema::ClampMode>(vuVal);
		}
	}

	// Validate game fixes, invalid ones will be dropped!
	{
		const int gameFixes = ryaml_find_child(yaml, node, "gameFixes");
		for (int n = ryaml_first_child(yaml, gameFixes); n >= 0;
			n = ryaml_next_sibling(yaml, n))
		{
			bool fixValidated = false;
			std::string fix(yamlValView(yaml, n));

			// Enum values don't end with Hack, but gamedb does, so remove it before comparing.
			if (StringView::EndsWith(fix, "Hack"))
			{
				fix.erase(fix.size() - 4);
				for (GamefixId id = GamefixId_FIRST; id < pxEnumEnd; ++id)
				{
					if (fix.compare(EnumToString(id)) == 0)
					{
						const GamefixId* fixes = gameEntry.gameFixes.data();
						const size_t nfixes = gameEntry.gameFixes.size();
						size_t fi;
						for (fi = 0; fi < nfixes; fi++)
						{
							if (fixes[fi] == id)
								break;
						}
						if (fi == nfixes)
						{
							gameEntry.gameFixes.push_back(id);
							fixValidated = true;
							break;
						}
					}
				}
			}

			if (!fixValidated)
				log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid gamefix: '{%s}', specified for serial: '{%s}'. Dropping!\n", fix.c_str(), serial);
		}
	}

	{
		const int speedHacks = ryaml_find_child(yaml, node, "speedHacks");
		for (int n = ryaml_first_child(yaml, speedHacks); n >= 0;
			n = ryaml_next_sibling(yaml, n))
		{
			const std::string_view id_view = yamlKeyView(yaml, n);
			const std::string_view value_view = yamlValView(yaml, n);
			const std::optional<SpeedHack> id = Pcsx2Config::SpeedhackOptions::ParseSpeedHackName(id_view);
			const std::optional<int> value = ParseNumber::FromChars<int>(value_view);

			bool inserted = false;
			if (id.has_value() && value.has_value())
			{
				const std::pair<SpeedHack, int>* hacks = gameEntry.speedHacks.data();
				const size_t nhacks = gameEntry.speedHacks.size();
				size_t hi;
				for (hi = 0; hi < nhacks; hi++)
				{
					if (hacks[hi].first == id.value())
						break;
				}
				if (hi == nhacks)
				{
					gameEntry.speedHacks.emplace_back(id.value(), value.value());
					inserted = true;
				}
			}
			if (!inserted)
				log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid speedhack: '{%s}', specified for serial: '{%.*s}'. Dropping!\n", std::string(id_view).c_str(), (int)value_view.size(), value_view.data());
		}
	}

	{
		const int gsHWFixes = ryaml_find_child(yaml, node, "gsHWFixes");
		for (int n = ryaml_first_child(yaml, gsHWFixes); n >= 0;
			n = ryaml_next_sibling(yaml, n))
		{
			const std::string_view id_name = yamlKeyView(yaml, n);
			std::optional<GameDatabaseSchema::GSHWFixId> id = GameDatabaseSchema::parseHWFixName(id_name);
			std::optional<s32> value;
			if (id.has_value() && (id.value() == GameDatabaseSchema::GSHWFixId::GetSkipCount ||
									  id.value() == GameDatabaseSchema::GSHWFixId::BeforeDraw ||
									  id.value() == GameDatabaseSchema::GSHWFixId::MoveHandler))
			{
				const std::string_view str_value = yamlValView(yaml, n);
				if (id.value() == GameDatabaseSchema::GSHWFixId::GetSkipCount)
					value = GSLookupGetSkipCountFunctionId(str_value.data(), str_value.size());
				else if (id.value() == GameDatabaseSchema::GSHWFixId::BeforeDraw)
					value = GSLookupBeforeDrawFunctionId(str_value.data(), str_value.size());
				else if (id.value() == GameDatabaseSchema::GSHWFixId::MoveHandler)
					value = GSLookupMoveHandlerFunctionId(str_value.data(), str_value.size());

				if (value.value_or(-1) < 0)
				{
					log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid GS HW Fix Value for '{%s}' in '{%s}': '{%s}'\n", std::string(id_name).c_str(), serial, std::string(str_value).c_str());
					continue;
				}
			}
			else
				value = ryaml_has_val(yaml, n)
					? ParseNumber::FromChars<s32>(yamlValView(yaml, n))
					: std::optional<s32>(1);

			if (!id.has_value() || !value.has_value())
			{
				log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid GS HW Fix: '{%s}' specified for serial '{%s}'. Dropping!\n", std::string(id_name).c_str(), serial);
				continue;
			}

			gameEntry.gsHWFixes.emplace_back(id.value(), value.value());
		}
	}

	// Memory Card Filters - Store as a vector to allow flexibility in the future
	// - currently they are used as a '\n' delimited string in the app
	{
		const int memcardFilters = ryaml_find_child(yaml, node, "memcardFilters");
		for (int n = ryaml_first_child(yaml, memcardFilters); n >= 0;
			n = ryaml_next_sibling(yaml, n))
			gameEntry.memcardFilters.emplace_back(yamlValView(yaml, n));
	}

	// Game Patches
	{
		const int patches = ryaml_find_child(yaml, node, "patches");
		for (int n = ryaml_first_child(yaml, patches); n >= 0;
			n = ryaml_next_sibling(yaml, n))
		{
			// use a crc of 0 for default patches
			const std::string_view crc_str = yamlKeyView(yaml, n);
			const std::optional<u32> crc = ((crc_str.length() == 7) && (Strncasecmp(crc_str.data(), "default", 7) == 0)) ? std::optional<u32>(0) : ParseNumber::FromChars<u32>(crc_str, 16);
			if (!crc.has_value())
			{
				log_cb(RETRO_LOG_ERROR, "[GameDB] Invalid CRC '{%s}' found for serial: '{%s}'. Skipping!\n", std::string(crc_str).c_str(), serial);
				continue;
			}
			if (gameEntry.patches.find(crc.value()) != gameEntry.patches.end())
			{
				log_cb(RETRO_LOG_ERROR, "[GameDB] Duplicate CRC '{%s}' found for serial: '{%s}'. Skipping, CRCs are case-insensitive!\n", std::string(crc_str).c_str(), std::string(serial).c_str());
				continue;
			}

			std::string patch =
				yamlString(yaml, ryaml_find_child(yaml, n, "content"));
			gameEntry.patches.emplace(crc.value(), std::move(patch));
		}
	}

	{
		const int dynaPatches = ryaml_find_child(yaml, node, "dynaPatches");
		for (int n = ryaml_first_child(yaml, dynaPatches); n >= 0;
			n = ryaml_next_sibling(yaml, n))
		{
			DynamicPatch patch;
			const int pattern = ryaml_find_child(yaml, n, "pattern");

			if (ryaml_has_children(yaml, pattern))
			{
				const int replacement = ryaml_find_child(yaml, n, "replacement");
				int e;

				for (e = ryaml_first_child(yaml, pattern); e >= 0;
					e = ryaml_next_sibling(yaml, e))
				{
					DynamicPatchEntry entry = {};
					ryaml_val_uint(yaml, ryaml_find_child(yaml, e, "offset"), &entry.offset);
					ryaml_val_uint(yaml, ryaml_find_child(yaml, e, "value"), &entry.value);

					patch.pattern.push_back(entry);
				}
				for (e = ryaml_first_child(yaml, replacement); e >= 0;
					e = ryaml_next_sibling(yaml, e))
				{
					DynamicPatchEntry entry = {};
					ryaml_val_uint(yaml, ryaml_find_child(yaml, e, "offset"), &entry.offset);
					ryaml_val_uint(yaml, ryaml_find_child(yaml, e, "value"), &entry.value);

					patch.replacement.push_back(entry);
				}
			}
			gameEntry.dynaPatches.push_back(patch);
		}
	}

	s_game_db.emplace(std::move(serial), std::move(gameEntry));
}

static const char* s_round_modes[static_cast<u32>(FPRoundMode::MaxCount)] = {
	"Nearest",
	"NegativeInfinity",
	"PositiveInfinity",
	"Chop"
};

static const char* s_gs_hw_fix_names[] = {
	"autoFlush",
	"cpuFramebufferConversion",
	"readTCOnClose",
	"disableDepthSupport",
	"preloadFrameData",
	"disablePartialInvalidation",
	"textureInsideRT",
	"alignSprite",
	"mergeSprite",
	"mipmap",
	"forceEvenSpritePosition",
	"bilinearUpscale",
	"nativePaletteDraw",
	"estimateTextureRegion",
	"PCRTCOffsets",
	"PCRTCOverscan",
	"trilinearFiltering",
	"skipDrawStart",
	"skipDrawEnd",
	"halfBottomOverride",
	"halfPixelOffset",
	"roundSprite",
	"nativeScaling",
	"texturePreloading",
	"deinterlace",
	"cpuSpriteRenderBW",
	"cpuSpriteRenderLevel",
	"cpuCLUTRender",
	"gpuPaletteConversion",
	"minimumBlendingLevel",
	"maximumBlendingLevel",
	"recommendedBlendingLevel",
	"getSkipCount",
	"beforeDraw",
	"moveHandler",
	"limit24BitDepth",
};

const char* GameDatabaseSchema::getHWFixName(GSHWFixId id)
{
	return s_gs_hw_fix_names[static_cast<u32>(id)];
}

static std::optional<GameDatabaseSchema::GSHWFixId> GameDatabaseSchema::parseHWFixName(const std::string_view& name)
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(s_gs_hw_fix_names); i++)
	{
		if (name.compare(s_gs_hw_fix_names[i]) == 0)
			return static_cast<GameDatabaseSchema::GSHWFixId>(i);
	}

	return std::nullopt;
}

bool GameDatabaseSchema::isUserHackHWFix(GSHWFixId id)
{
	switch (id)
	{
		case GSHWFixId::Deinterlace:
		case GSHWFixId::Mipmap:
		case GSHWFixId::TexturePreloading:
		case GSHWFixId::TrilinearFiltering:
		case GSHWFixId::MinimumBlendingLevel:
		case GSHWFixId::MaximumBlendingLevel:
		case GSHWFixId::RecommendedBlendingLevel:
		case GSHWFixId::PCRTCOffsets:
		case GSHWFixId::PCRTCOverscan:
		case GSHWFixId::GetSkipCount:
		case GSHWFixId::BeforeDraw:
		case GSHWFixId::MoveHandler:
			return false;
		default:
			break;
	}
	return true;
}

u32 GameDatabaseSchema::GameEntry::applyGameFixes(Pcsx2Config& config, bool applyAuto) const
{
	// Only apply core game fixes if the user has enabled them.
	if (!applyAuto)
		log_cb(RETRO_LOG_WARN, "[GameDB] Game Fixes are disabled\n");

	u32 num_applied_fixes = 0;

	if (eeDivRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU divison roundmode to %d [%s]\n", (int)eeRoundMode, s_round_modes[static_cast<u8>(eeDivRoundMode)]);
			config.Cpu.FPUDivFPCR.SetRoundMode(eeDivRoundMode);
		}
		else
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU roundmode to %d [%s]\n", (int)eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
		}
	}

	if (eeRoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU roundmode to %d [%s]\n", (int)eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
			config.Cpu.FPUFPCR.SetRoundMode(eeRoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU roundmode to %d [%s]\n", (int)eeRoundMode, s_round_modes[static_cast<u8>(eeRoundMode)]);
	}

	if (vu0RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU0 roundmode to %d [%s]\n", (int)vu0RoundMode, s_round_modes[static_cast<u8>(vu0RoundMode)]);
			config.Cpu.VU0FPCR.SetRoundMode(vu0RoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU0 roundmode to %d [%s]\n", (int)vu0RoundMode, s_round_modes[static_cast<u8>(vu0RoundMode)]);
	}

	if (vu1RoundMode < FPRoundMode::MaxCount)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU1 roundmode to %d [%s]\n", (int)vu1RoundMode, s_round_modes[static_cast<u8>(vu1RoundMode)]);
			config.Cpu.VU1FPCR.SetRoundMode(vu1RoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU1 roundmode to %d [%s]\n", (int)vu1RoundMode, s_round_modes[static_cast<u8>(vu1RoundMode)]);
	}

	if (eeClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(eeClampMode);
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU clamp mode [mode=%d]\n", clampMode);
			config.Cpu.Recompiler.fpuOverflow = (clampMode >= 1);
			config.Cpu.Recompiler.fpuExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.fpuFullMode = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU clamp mode [mode=%d]\n", clampMode);
	}

	if (vu0ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(vu0ClampMode);
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU0 clamp mode [mode=%d]\n", clampMode);
			config.Cpu.Recompiler.vu0Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu0ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu0SignOverflow = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU0 clamp mode [mode=%d]\n", clampMode);
	}

	if (vu1ClampMode != GameDatabaseSchema::ClampMode::Undefined)
	{
		const int clampMode = enum_cast(vu1ClampMode);
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU1 clamp mode [mode=%d]\n", clampMode);
			config.Cpu.Recompiler.vu1Overflow = (clampMode >= 1);
			config.Cpu.Recompiler.vu1ExtraOverflow = (clampMode >= 2);
			config.Cpu.Recompiler.vu1SignOverflow = (clampMode >= 3);
			num_applied_fixes++;
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU1 clamp mode [mode=%d]\n", clampMode);
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (const auto& it : speedHacks)
	{
		if (!applyAuto)
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping setting Speedhack '%s' to [mode=%d]\n",
					Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it.first), it.second);
			continue;
		}
		// Legacy note - speedhacks are setup in the GameDB as integer values, but
		// are effectively booleans like the gamefixes
		config.Speedhacks.Set(it.first, it.second);
		log_cb(RETRO_LOG_INFO, "(GameDB) Setting Speedhack '%s' to [mode=%d]\n",
				Pcsx2Config::SpeedhackOptions::GetSpeedHackName(it.first), it.second);
		num_applied_fixes++;
	}

	// TODO - config - this could be simplified with maps instead of bitfields and enums
	for (const GamefixId id : gameFixes)
	{
		if (!applyAuto)
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping Gamefix: %s\n", EnumToString(id));
			continue;
		}
		// if the fix is present, it is said to be enabled
		config.Gamefixes.Set(id, true);
		log_cb(RETRO_LOG_INFO, "(GameDB) Enabled Gamefix: %s\n", EnumToString(id));
		num_applied_fixes++;

		// The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB)
		if (id == Fix_GoemonTlbMiss && true)
			vtlb_Alloc_Ppmap();
	}

	return num_applied_fixes;
}

bool GameDatabaseSchema::GameEntry::configMatchesHWFix(const Pcsx2Config::GSOptions& config, GSHWFixId id, int value)
{
	switch (id)
	{
		case GSHWFixId::AutoFlush:
			return (static_cast<int>(config.UserHacks_AutoFlush) == value);

		case GSHWFixId::CPUFramebufferConversion:
			return (static_cast<int>(config.UserHacks_CPUFBConversion) == value);

		case GSHWFixId::FlushTCOnClose:
			return (static_cast<int>(config.UserHacks_ReadTCOnClose) == value);

		case GSHWFixId::DisableDepthSupport:
			return (static_cast<int>(config.UserHacks_DisableDepthSupport) == value);

		case GSHWFixId::PreloadFrameData:
			return (static_cast<int>(config.PreloadFrameWithGSData) == value);

		case GSHWFixId::DisablePartialInvalidation:
			return (static_cast<int>(config.UserHacks_DisablePartialInvalidation) == value);

		case GSHWFixId::TextureInsideRT:
			return (static_cast<int>(config.UserHacks_TextureInsideRt) == value);

		case GSHWFixId::AlignSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_AlignSpriteX) == value);

		case GSHWFixId::MergeSprite:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_MergePPSprite) == value);

		case GSHWFixId::ForceEvenSpritePosition:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_ForceEvenSpritePosition) == value);

		case GSHWFixId::BilinearUpscale:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_BilinearHack) == value);

		case GSHWFixId::NativePaletteDraw:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_NativePaletteDraw) == value);

		case GSHWFixId::EstimateTextureRegion:
			return (static_cast<int>(config.UserHacks_EstimateTextureRegion) == value);

		case GSHWFixId::PCRTCOffsets:
			return (static_cast<int>(config.PCRTCOffsets) == value);

		case GSHWFixId::PCRTCOverscan:
			return (static_cast<int>(config.PCRTCOverscan) == value);

		case GSHWFixId::Mipmap:
			return (static_cast<int>(config.HWMipmapMode) == value);

		case GSHWFixId::Limit24BitDepth:
			return (static_cast<int>(config.UserHacks_Limit24BitDepth) == value);

		case GSHWFixId::TrilinearFiltering:
			return (config.TriFilter == TriFiltering::Automatic || static_cast<int>(config.TriFilter) == value);

		case GSHWFixId::SkipDrawStart:
			return (config.SkipDrawStart == value);

		case GSHWFixId::SkipDrawEnd:
			return (config.SkipDrawEnd == value);

		case GSHWFixId::HalfPixelOffset:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_HalfPixelOffset == static_cast<GSHalfPixelOffset>(value));

		case GSHWFixId::RoundSprite:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_RoundSprite == value);

		case GSHWFixId::NativeScaling:
			return (static_cast<int>(config.UserHacks_NativeScaling) == value);

		case GSHWFixId::TexturePreloading:
			return (static_cast<int>(config.TexturePreloading) <= value);

		case GSHWFixId::Deinterlace:
			return (config.InterlaceMode == GSInterlaceMode::Automatic || static_cast<int>(config.InterlaceMode) == value);

		case GSHWFixId::CPUSpriteRenderBW:
			return (config.UserHacks_CPUSpriteRenderBW == value);

		case GSHWFixId::CPUSpriteRenderLevel:
			return (config.UserHacks_CPUSpriteRenderLevel == value);

		case GSHWFixId::CPUCLUTRender:
			return (config.UserHacks_CPUCLUTRender == value);

		case GSHWFixId::GPUPaletteConversion:
			return (config.GPUPaletteConversion == ((value > 1) ? (config.TexturePreloading == TexturePreloadingLevel::Full) : (value != 0)));

		case GSHWFixId::MinimumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) >= value);

		case GSHWFixId::MaximumBlendingLevel:
			return (static_cast<int>(config.AccurateBlendingUnit) <= value);

		case GSHWFixId::RecommendedBlendingLevel:
			return true;

		case GSHWFixId::GetSkipCount:
			return (static_cast<int>(config.GetSkipCountFunctionId) == value);

		case GSHWFixId::BeforeDraw:
			return (static_cast<int>(config.BeforeDrawFunctionId) == value);
		case GSHWFixId::MoveHandler:
			return (static_cast<int>(config.MoveHandlerFunctionId) == value);

		default:
			return false;
	}
}

u32 GameDatabaseSchema::GameEntry::applyGSHardwareFixes(Pcsx2Config::GSOptions& config) const
{
	std::string disabled_fixes;

	// Only apply GS HW fixes if the user hasn't manually enabled HW fixes.
	const bool apply_auto_fixes = !config.ManualUserHacks;
	if (!apply_auto_fixes)
		log_cb(RETRO_LOG_WARN, "[GameDB] Manual GS hardware renderer fixes are enabled, not using automatic hardware renderer fixes from GameDB.\n");

	u32 num_applied_fixes = 0;
	for (const auto& [id, value] : gsHWFixes)
	{
		if (isUserHackHWFix(id) && !apply_auto_fixes)
		{
			if (configMatchesHWFix(config, id, value))
				continue;

			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping GS Hardware Fix: %s to [mode=%d]\n", getHWFixName(id), value);
			disabled_fixes += FormatString::Format("%s %s = %d", disabled_fixes.empty() ? "  " : "\n  ", getHWFixName(id), value);
			continue;
		}

		switch (id)
		{
			case GSHWFixId::AutoFlush:
			{
				if (value >= 0 && value <= static_cast<int>(GSHWAutoFlushLevel::Enabled))
					config.UserHacks_AutoFlush = static_cast<GSHWAutoFlushLevel>(value);
			}
			break;

			case GSHWFixId::CPUFramebufferConversion:
				config.UserHacks_CPUFBConversion = (value > 0);
				break;

			case GSHWFixId::FlushTCOnClose:
				config.UserHacks_ReadTCOnClose = (value > 0);
				break;

			case GSHWFixId::DisableDepthSupport:
				config.UserHacks_DisableDepthSupport = (value > 0);
				break;

			case GSHWFixId::PreloadFrameData:
				config.PreloadFrameWithGSData = (value > 0);
				break;

			case GSHWFixId::DisablePartialInvalidation:
				config.UserHacks_DisablePartialInvalidation = (value > 0);
				break;

			case GSHWFixId::TextureInsideRT:
			{
				if (value >= 0 && value <= static_cast<int>(GSTextureInRtMode::MergeTargets))
					config.UserHacks_TextureInsideRt = static_cast<GSTextureInRtMode>(value);
			}
			break;

			case GSHWFixId::AlignSprite:
				config.UserHacks_AlignSpriteX = (value > 0);
				break;

			case GSHWFixId::MergeSprite:
				config.UserHacks_MergePPSprite = (value > 0);
				break;

			case GSHWFixId::ForceEvenSpritePosition:
				config.UserHacks_ForceEvenSpritePosition = (value > 0);
				break;

			case GSHWFixId::BilinearUpscale:
				if (value >= 0 && value < static_cast<int>(GSBilinearDirtyMode::MaxCount))
					config.UserHacks_BilinearHack = static_cast<GSBilinearDirtyMode>(value);
				break;

			case GSHWFixId::NativePaletteDraw:
				config.UserHacks_NativePaletteDraw = (value > 0);
				break;

			case GSHWFixId::EstimateTextureRegion:
				config.UserHacks_EstimateTextureRegion = (value > 0);
				break;

			case GSHWFixId::PCRTCOffsets:
				config.PCRTCOffsets = (value > 0);
				break;

			case GSHWFixId::PCRTCOverscan:
				config.PCRTCOverscan = (value > 0);
				break;

			case GSHWFixId::Mipmap:
				if (value >= 0 && value < static_cast<int>(GSHWMipmapMode::MaxCount))
				{
					config.HWMipmapMode = static_cast<GSHWMipmapMode>(value > 0);
					// Mipmapping is split across HWMipmapMode and the Mipmap master
					// switch (consumed by GSState::UpdateSettings into m_mipmap). A
					// database mipmap fix must drive both, or the renderer-level
					// gate keeps LOD sampling disabled regardless of the mode.
					config.Mipmap = (value > 0);
				}
				break;

			case GSHWFixId::Limit24BitDepth:
				if (value >= 0 && value <= static_cast<int>(GSLimit24BitDepth::PrioritizeLower))
					config.UserHacks_Limit24BitDepth = static_cast<GSLimit24BitDepth>(value);
				break;

			case GSHWFixId::TrilinearFiltering:
			{
				if (value >= 0 && value <= static_cast<int>(TriFiltering::Forced))
				{
					if (config.TriFilter == TriFiltering::Automatic)
						config.TriFilter = static_cast<TriFiltering>(value);
					else if (config.TriFilter > TriFiltering::Off)
						log_cb(RETRO_LOG_WARN, "[GameDB] Game requires trilinear filtering to be disabled.\n");
				}
			}
			break;

			case GSHWFixId::SkipDrawStart:
				config.SkipDrawStart = value;
				break;

			case GSHWFixId::SkipDrawEnd:
				config.SkipDrawEnd = value;
				break;

			case GSHWFixId::HalfPixelOffset:
				if (value >= 0 && value < static_cast<int>(GSHalfPixelOffset::MaxCount))
					config.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(value);
				break;

			case GSHWFixId::RoundSprite:
				config.UserHacks_RoundSprite = value;
				break;

			case GSHWFixId::NativeScaling:
				/* The user's own setting of the option stands over the fix. */
				if (!config.NativeScalingSet)
					config.UserHacks_NativeScaling = static_cast<GSNativeScaling>(value);
				break;

			case GSHWFixId::TexturePreloading:
			{
				if (value >= 0 && value <= static_cast<int>(TexturePreloadingLevel::Full))
					config.TexturePreloading = (TexturePreloadingLevel)pcsx2_min_i((int)config.TexturePreloading, value);
			}
			break;

			case GSHWFixId::Deinterlace:
			{
				if (value >= static_cast<int>(GSInterlaceMode::Automatic) && value < static_cast<int>(GSInterlaceMode::Count))
				{
					if (config.InterlaceMode == GSInterlaceMode::Automatic)
						config.InterlaceMode = static_cast<GSInterlaceMode>(value);
					else
						log_cb(RETRO_LOG_WARN, "[GameDB] Game requires different deinterlace mode but it has been overridden by user setting.\n");
				}
			}
			break;

			case GSHWFixId::CPUSpriteRenderBW:
				config.UserHacks_CPUSpriteRenderBW = value;
				break;

			case GSHWFixId::CPUSpriteRenderLevel:
				config.UserHacks_CPUSpriteRenderLevel = value;
				break;

			case GSHWFixId::CPUCLUTRender:
				config.UserHacks_CPUCLUTRender = value;
				break;

			case GSHWFixId::GPUPaletteConversion:
			{
				// if 2, enable paltex when preloading is full, otherwise leave as-is
				if (value > 1)
					config.GPUPaletteConversion = (config.TexturePreloading == TexturePreloadingLevel::Full) ? true : config.GPUPaletteConversion;
				else
					config.GPUPaletteConversion = (value != 0);
			}
			break;

			case GSHWFixId::MinimumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = (AccBlendLevel)pcsx2_max_i((int)config.AccurateBlendingUnit, value);
			}
			break;

			case GSHWFixId::MaximumBlendingLevel:
			{
				if (value >= 0 && value <= static_cast<int>(AccBlendLevel::Maximum))
					config.AccurateBlendingUnit = (AccBlendLevel)pcsx2_min_i((int)config.AccurateBlendingUnit, value);
			}
			break;

			case GSHWFixId::RecommendedBlendingLevel:
				break;

			case GSHWFixId::GetSkipCount:
				config.GetSkipCountFunctionId = static_cast<s16>(value);
				break;

			case GSHWFixId::BeforeDraw:
				config.BeforeDrawFunctionId = static_cast<s16>(value);
				break;

			case GSHWFixId::MoveHandler:
				config.MoveHandlerFunctionId = static_cast<s16>(value);
				break;

			default:
				break;
		}

		log_cb(RETRO_LOG_INFO, "[GameDB] Enabled GS Hardware Fix: %s to [mode=%d]\n", getHWFixName(id), value);
		num_applied_fixes++;
	}

	// fixup skipdraw range just in case the db has a bad range (but the linter should catch this)
	config.SkipDrawEnd = pcsx2_max_i(config.SkipDrawStart, config.SkipDrawEnd);

	return num_applied_fixes;
}

void GameDatabase::initDatabase()
{
	auto buf = Host::ReadResourceFileToString(GAMEDB_YAML_FILE_NAME);
	if (!buf.has_value())
	{
		log_cb(RETRO_LOG_ERROR, "[GameDB] Unable to open GameDB file, file does not exist.\n");
		return;
	}

	/* The reader borrows these bytes rather than copying them, so the
	 * buffer has to outlive the tree; it does, both are scoped here. */
	size_t err_line = 0;
	size_t err_col = 0;
	ryaml_t* yaml = ryaml_parse_ex(buf.value().data(), buf.value().size(),
		&err_line, &err_col);
	if (!yaml)
	{
		log_cb(RETRO_LOG_ERROR, "[GameDB] Parsing error at {%zu}:{%zu}\n", err_line, err_col);
		return;
	}

	const int root = ryaml_root(yaml);

	for (int n = ryaml_first_child(yaml, root); n >= 0;
		n = ryaml_next_sibling(yaml, n))
	{
		auto serial = StringView::toLower(std::string(yamlKeyView(yaml, n)));

		// Serials and CRCs must be inserted as lower-case, as that is how they are retrieved
		// this is because the application may pass a lowercase CRC or serial along
		//
		// However, YAML's keys are as expected case-sensitive, so we have to explicitly do our own duplicate checking
		if (s_game_db.count(serial) == 1)
		{
			log_cb(RETRO_LOG_ERROR, "[GameDB] Duplicate serial '{%s}' found in GameDB. Skipping, Serials are case-insensitive!\n", serial.c_str());
			continue;
		}
		if (ryaml_is_map(yaml, n))
			parseAndInsert(serial.c_str(), yaml, n);
	}

	ryaml_free(yaml);
}

void GameDatabase::ensureLoaded()
{
	// call_once semantics under the campaign lock family: double load
	// impossible (mutex), racing callers both observe the completed
	// state (done set before unlock, read under lock).
	SLockGuard lock(s_load_once_mutex());
	if (!s_load_once_done)
	{
		log_cb(RETRO_LOG_INFO, "[GameDB] Has not been initialized yet, initializing...\n");
		initDatabase();
		log_cb(RETRO_LOG_INFO, "[GameDB] %zu games on record\n", s_game_db.size());
		s_load_once_done = true;
	}
}

void GameDatabase::unload()
{
	/* Held under the same lock ensureLoaded() takes, so a concurrent
	 * lookup either completes before the clear or reloads after it -
	 * it cannot observe a half-emptied map.
	 *
	 * The parse is 12806 entries for about 7 MB resident and ~50 ms of
	 * work, and every consumer of it - gamefixes, patches, the memcard
	 * and disc-swap paths - is a VM-lifetime event.  With no VM there
	 * is nothing to look a game up for, so the cost is worth carrying
	 * only while content is loaded.  swap() rather than clear() because
	 * clear() on an unordered_map keeps the bucket array. */
	SLockGuard lock(s_load_once_mutex());
	if (!s_load_once_done)
		return;
	decltype(s_game_db)().swap(s_game_db);
	s_load_once_done = false;
}

const GameDatabaseSchema::GameEntry* GameDatabase::findGame(const std::string_view& serial)
{
	GameDatabase::ensureLoaded();

	auto iter = s_game_db.find(StringView::toLower(serial));
	return (iter != s_game_db.end()) ? &iter->second : nullptr;
}
