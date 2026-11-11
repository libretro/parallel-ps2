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

/* The game database's fixes, applied to a config. Kept apart from the
 * reader (GameDatabase.c) because they write the C++ config structures. */

#include "common/Pcsx2Defs.h"

#include "GameDatabase.h"
#include "vtlb.h"

static_assert(FPROUND_COUNT == GAMEDB_ROUND_UNSET,
	"GAMEDB_ROUND_UNSET is one past the last FPRoundMode");
static_assert(SPEEDHACK_COUNT <= 256 && GamefixId_COUNT <= 256,
	"the database keeps fix and hack ids in a byte");

static const char* const s_round_modes[GAMEDB_ROUND_UNSET] = {
	"Nearest",
	"NegativeInfinity",
	"PositiveInfinity",
	"Chop"
};

u32 GameDatabase_ApplyGameFixes(const GameDatabaseEntry* e, Pcsx2Config& config, bool applyAuto)
{
	u32 i;
	/* Only apply core game fixes if the user has enabled them. */
	if (!applyAuto)
		log_cb(RETRO_LOG_WARN, "[GameDB] Game Fixes are disabled\n");

	u32 num_applied_fixes = 0;

	if (e->eeDivRoundMode < GAMEDB_ROUND_UNSET)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU divison roundmode to %d [%s]\n", (int)e->eeDivRoundMode, s_round_modes[e->eeDivRoundMode]);
			config.Cpu.FPUDivFPCR = fpcr_set_round_mode(config.Cpu.FPUDivFPCR, (enum FPRoundMode)e->eeDivRoundMode);
		}
		else
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU divison roundmode to %d [%s]\n", (int)e->eeDivRoundMode, s_round_modes[e->eeDivRoundMode]);
		}
	}

	if (e->eeRoundMode < GAMEDB_ROUND_UNSET)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing EE/FPU roundmode to %d [%s]\n", (int)e->eeRoundMode, s_round_modes[e->eeRoundMode]);
			config.Cpu.FPUFPCR = fpcr_set_round_mode(config.Cpu.FPUFPCR, (enum FPRoundMode)e->eeRoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing EE/FPU roundmode to %d [%s]\n", (int)e->eeRoundMode, s_round_modes[e->eeRoundMode]);
	}

	if (e->vu0RoundMode < GAMEDB_ROUND_UNSET)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU0 roundmode to %d [%s]\n", (int)e->vu0RoundMode, s_round_modes[e->vu0RoundMode]);
			config.Cpu.VU0FPCR = fpcr_set_round_mode(config.Cpu.VU0FPCR, (enum FPRoundMode)e->vu0RoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU0 roundmode to %d [%s]\n", (int)e->vu0RoundMode, s_round_modes[e->vu0RoundMode]);
	}

	if (e->vu1RoundMode < GAMEDB_ROUND_UNSET)
	{
		if (applyAuto)
		{
			log_cb(RETRO_LOG_INFO, "(GameDB) Changing VU1 roundmode to %d [%s]\n", (int)e->vu1RoundMode, s_round_modes[e->vu1RoundMode]);
			config.Cpu.VU1FPCR = fpcr_set_round_mode(config.Cpu.VU1FPCR, (enum FPRoundMode)e->vu1RoundMode);
		}
		else
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping changing VU1 roundmode to %d [%s]\n", (int)e->vu1RoundMode, s_round_modes[e->vu1RoundMode]);
	}

	if (e->eeClampMode != GAMEDB_CLAMP_UNDEFINED)
	{
		const int clampMode = (int)e->eeClampMode;
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

	if (e->vu0ClampMode != GAMEDB_CLAMP_UNDEFINED)
	{
		const int clampMode = (int)e->vu0ClampMode;
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

	if (e->vu1ClampMode != GAMEDB_CLAMP_UNDEFINED)
	{
		const int clampMode = (int)e->vu1ClampMode;
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

	/* TODO - config - this could be simplified with maps instead of bitfields and enums */
	for (i = 0; i < e->nSpeedHacks; i++)
	{
		const GameDatabaseSpeedHack* it = &e->speedHacks[i];
		if (!applyAuto)
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping setting Speedhack '%s' to [mode=%d]\n",
					emu_speedhack_name((enum SpeedHack)it->id), it->value);
			continue;
		}
		/* Legacy note - speedhacks are setup in the GameDB as integer values, but */
		/* are effectively booleans like the gamefixes */
		emu_speedhack_set(&config.Speedhacks, (enum SpeedHack)it->id, it->value);
		log_cb(RETRO_LOG_INFO, "(GameDB) Setting Speedhack '%s' to [mode=%d]\n",
				emu_speedhack_name((enum SpeedHack)it->id), it->value);
		num_applied_fixes++;
	}

	/* TODO - config - this could be simplified with maps instead of bitfields and enums */
	for (i = 0; i < e->nGameFixes; i++)
	{
		const GamefixId id = (GamefixId)e->gameFixes[i];
		if (!applyAuto)
		{
			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping Gamefix: %s\n", emu_gamefix_name(id));
			continue;
		}
		/* if the fix is present, it is said to be enabled */
		emu_gamefix_set(&config.Gamefixes, id, true);
		log_cb(RETRO_LOG_INFO, "(GameDB) Enabled Gamefix: %s\n", emu_gamefix_name(id));
		num_applied_fixes++;

		/* The LUT is only used for 1 game so we allocate it only when the gamefix is enabled (save 4MB) */
		if (id == Fix_GoemonTlbMiss && true)
			vtlb_Alloc_Ppmap();
	}

	return num_applied_fixes;
}

static bool configMatchesHWFix(const EmuGSOptions& config, u32 id, int value)
{
	switch (id)
	{
		case GAMEDB_HWFIX_CPU_FRAMEBUFFER_CONVERSION:
			return (static_cast<int>(config.UserHacks_CPUFBConversion) == value);

		case GAMEDB_HWFIX_FLUSH_TC_ON_CLOSE:
			return (static_cast<int>(config.UserHacks_ReadTCOnClose) == value);

		case GAMEDB_HWFIX_DISABLE_DEPTH_SUPPORT:
			return (static_cast<int>(config.UserHacks_DisableDepthSupport) == value);

		case GAMEDB_HWFIX_PRELOAD_FRAME_DATA:
			return (static_cast<int>(config.PreloadFrameWithGSData) == value);

		case GAMEDB_HWFIX_DISABLE_PARTIAL_INVALIDATION:
			return (static_cast<int>(config.UserHacks_DisablePartialInvalidation) == value);

		case GAMEDB_HWFIX_TEXTURE_INSIDE_RT:
			return (static_cast<int>(config.UserHacks_TextureInsideRt) == value);

		case GAMEDB_HWFIX_ALIGN_SPRITE:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_AlignSpriteX) == value);

		case GAMEDB_HWFIX_MERGE_SPRITE:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_MergePPSprite) == value);

		case GAMEDB_HWFIX_FORCE_EVEN_SPRITE_POSITION:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_ForceEvenSpritePosition) == value);

		case GAMEDB_HWFIX_NATIVE_PALETTE_DRAW:
			return (config.UpscaleMultiplier <= 1.0f || static_cast<int>(config.UserHacks_NativePaletteDraw) == value);

		case GAMEDB_HWFIX_ESTIMATE_TEXTURE_REGION:
			return (static_cast<int>(config.UserHacks_EstimateTextureRegion) == value);

		case GAMEDB_HWFIX_PCRTC_OFFSETS:
			return (static_cast<int>(config.PCRTCOffsets) == value);

		case GAMEDB_HWFIX_PCRTC_OVERSCAN:
			return (static_cast<int>(config.PCRTCOverscan) == value);

		case GAMEDB_HWFIX_MIPMAP:
			return (static_cast<int>(config.HWMipmapMode) == value);

		case GAMEDB_HWFIX_LIMIT_24BIT_DEPTH:
			return (static_cast<int>(config.UserHacks_Limit24BitDepth) == value);

		case GAMEDB_HWFIX_TRILINEAR_FILTERING:
			return (config.TriFilter == GS_TRIFILTER_AUTOMATIC || static_cast<int>(config.TriFilter) == value);

		case GAMEDB_HWFIX_SKIP_DRAW_START:
			return (config.SkipDrawStart == value);

		case GAMEDB_HWFIX_SKIP_DRAW_END:
			return (config.SkipDrawEnd == value);

		case GAMEDB_HWFIX_HALF_PIXEL_OFFSET:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_HalfPixelOffset == static_cast<GSHalfPixelOffset>(value));

		case GAMEDB_HWFIX_ROUND_SPRITE:
			return (config.UpscaleMultiplier <= 1.0f || config.UserHacks_RoundSprite == value);

		case GAMEDB_HWFIX_NATIVE_SCALING:
			return (static_cast<int>(config.UserHacks_NativeScaling) == value);

		case GAMEDB_HWFIX_TEXTURE_PRELOADING:
			return (static_cast<int>(config.TexturePreloading) <= value);

		case GAMEDB_HWFIX_DEINTERLACE:
			return (config.InterlaceMode == GS_INTERLACE_AUTOMATIC || static_cast<int>(config.InterlaceMode) == value);

		case GAMEDB_HWFIX_CPU_SPRITE_RENDER_BW:
			return (config.UserHacks_CPUSpriteRenderBW == value);

		case GAMEDB_HWFIX_CPU_SPRITE_RENDER_LEVEL:
			return (config.UserHacks_CPUSpriteRenderLevel == value);

		case GAMEDB_HWFIX_CPU_CLUT_RENDER:
			return (config.UserHacks_CPUCLUTRender == value);

		case GAMEDB_HWFIX_GPU_PALETTE_CONVERSION:
			return (config.GPUPaletteConversion == ((value > 1) ? (config.TexturePreloading == GS_PRELOAD_FULL) : (value != 0)));

		case GAMEDB_HWFIX_MINIMUM_BLENDING_LEVEL:
			return (static_cast<int>(config.AccurateBlendingUnit) >= value);

		case GAMEDB_HWFIX_MAXIMUM_BLENDING_LEVEL:
			return (static_cast<int>(config.AccurateBlendingUnit) <= value);

		case GAMEDB_HWFIX_RECOMMENDED_BLENDING_LEVEL:
			return true;

		case GAMEDB_HWFIX_GET_SKIP_COUNT:
			return (static_cast<int>(config.GetSkipCountFunctionId) == value);

		case GAMEDB_HWFIX_BEFORE_DRAW:
			return (static_cast<int>(config.BeforeDrawFunctionId) == value);
		case GAMEDB_HWFIX_MOVE_HANDLER:
			return (static_cast<int>(config.MoveHandlerFunctionId) == value);

		default:
			return false;
	}
}

u32 GameDatabase_ApplyGSHardwareFixes(const GameDatabaseEntry* e, EmuGSOptions& config)
{
	u32 i;

	/* Only apply GS HW fixes if the user hasn't manually enabled HW fixes. */
	const bool apply_auto_fixes = !config.ManualUserHacks;
	if (!apply_auto_fixes)
		log_cb(RETRO_LOG_WARN, "[GameDB] Manual GS hardware renderer fixes are enabled, not using automatic hardware renderer fixes from GameDB.\n");

	u32 num_applied_fixes = 0;
	for (i = 0; i < e->nGSHWFixes; i++)
	{
		const u32 id = e->gsHWFixes[i].id;
		const s32 value = e->gsHWFixes[i].value;
		if (gamedb_hwfix_is_user_hack(id) && !apply_auto_fixes)
		{
			if (configMatchesHWFix(config, id, value))
				continue;

			log_cb(RETRO_LOG_WARN, "[GameDB] Skipping GS Hardware Fix: %s to [mode=%d]\n", gamedb_hwfix_name(id), value);
			continue;
		}

		switch (id)
		{
			case GAMEDB_HWFIX_CPU_FRAMEBUFFER_CONVERSION:
				config.UserHacks_CPUFBConversion = (value > 0);
				break;

			case GAMEDB_HWFIX_FLUSH_TC_ON_CLOSE:
				config.UserHacks_ReadTCOnClose = (value > 0);
				break;

			case GAMEDB_HWFIX_DISABLE_DEPTH_SUPPORT:
				config.UserHacks_DisableDepthSupport = (value > 0);
				break;

			case GAMEDB_HWFIX_PRELOAD_FRAME_DATA:
				config.PreloadFrameWithGSData = (value > 0);
				break;

			case GAMEDB_HWFIX_DISABLE_PARTIAL_INVALIDATION:
				config.UserHacks_DisablePartialInvalidation = (value > 0);
				break;

			case GAMEDB_HWFIX_TEXTURE_INSIDE_RT:
			{
				if (value >= 0 && value <= static_cast<int>(GS_TEXINRT_MERGE_TARGETS))
					config.UserHacks_TextureInsideRt = static_cast<GSTextureInRtMode>(value);
			}
			break;

			case GAMEDB_HWFIX_ALIGN_SPRITE:
				config.UserHacks_AlignSpriteX = (value > 0);
				break;

			case GAMEDB_HWFIX_MERGE_SPRITE:
				config.UserHacks_MergePPSprite = (value > 0);
				break;

			case GAMEDB_HWFIX_FORCE_EVEN_SPRITE_POSITION:
				config.UserHacks_ForceEvenSpritePosition = (value > 0);
				break;

			case GAMEDB_HWFIX_NATIVE_PALETTE_DRAW:
				config.UserHacks_NativePaletteDraw = (value > 0);
				break;

			case GAMEDB_HWFIX_ESTIMATE_TEXTURE_REGION:
				config.UserHacks_EstimateTextureRegion = (value > 0);
				break;

			case GAMEDB_HWFIX_PCRTC_OFFSETS:
				config.PCRTCOffsets = (value > 0);
				break;

			case GAMEDB_HWFIX_PCRTC_OVERSCAN:
				config.PCRTCOverscan = (value > 0);
				break;

			case GAMEDB_HWFIX_MIPMAP:
				if (value >= 0 && value < static_cast<int>(GS_MIPMAP_COUNT))
				{
					config.HWMipmapMode = static_cast<GSHWMipmapMode>(value > 0);
					/* Mipmapping is split across HWMipmapMode and the Mipmap master */
					/* switch (consumed by GSState::UpdateSettings into m_mipmap). A */
					/* database mipmap fix must drive both, or the renderer-level */
					/* gate keeps LOD sampling disabled regardless of the mode. */
					config.Mipmap = (value > 0);
				}
				break;

			case GAMEDB_HWFIX_LIMIT_24BIT_DEPTH:
				if (value >= 0 && value <= static_cast<int>(GS_LIMIT24_PRIORITIZE_LOWER))
					config.UserHacks_Limit24BitDepth = static_cast<GSLimit24BitDepth>(value);
				break;

			case GAMEDB_HWFIX_TRILINEAR_FILTERING:
			{
				if (value >= 0 && value <= static_cast<int>(GS_TRIFILTER_FORCED))
				{
					if (config.TriFilter == GS_TRIFILTER_AUTOMATIC)
						config.TriFilter = static_cast<TriFiltering>(value);
					else if (config.TriFilter > GS_TRIFILTER_OFF)
						log_cb(RETRO_LOG_WARN, "[GameDB] Game requires trilinear filtering to be disabled.\n");
				}
			}
			break;

			case GAMEDB_HWFIX_SKIP_DRAW_START:
				config.SkipDrawStart = value;
				break;

			case GAMEDB_HWFIX_SKIP_DRAW_END:
				config.SkipDrawEnd = value;
				break;

			case GAMEDB_HWFIX_HALF_PIXEL_OFFSET:
				if (value >= 0 && value < static_cast<int>(GS_HPO_COUNT))
					config.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(value);
				break;

			case GAMEDB_HWFIX_ROUND_SPRITE:
				config.UserHacks_RoundSprite = value;
				break;

			case GAMEDB_HWFIX_NATIVE_SCALING:
				/* The user's own setting of the option stands over the fix. */
				if (!config.NativeScalingSet)
					config.UserHacks_NativeScaling = static_cast<GSNativeScaling>(value);
				break;

			case GAMEDB_HWFIX_TEXTURE_PRELOADING:
			{
				if (value >= 0 && value <= static_cast<int>(GS_PRELOAD_FULL))
					config.TexturePreloading = (TexturePreloadingLevel)pcsx2_min_i((int)config.TexturePreloading, value);
			}
			break;

			case GAMEDB_HWFIX_DEINTERLACE:
			{
				if (value >= static_cast<int>(GS_INTERLACE_AUTOMATIC) && value < static_cast<int>(GS_INTERLACE_COUNT))
				{
					if (config.InterlaceMode == GS_INTERLACE_AUTOMATIC)
						config.InterlaceMode = static_cast<GSInterlaceMode>(value);
					else
						log_cb(RETRO_LOG_WARN, "[GameDB] Game requires different deinterlace mode but it has been overridden by user setting.\n");
				}
			}
			break;

			case GAMEDB_HWFIX_CPU_SPRITE_RENDER_BW:
				config.UserHacks_CPUSpriteRenderBW = value;
				break;

			case GAMEDB_HWFIX_CPU_SPRITE_RENDER_LEVEL:
				config.UserHacks_CPUSpriteRenderLevel = value;
				break;

			case GAMEDB_HWFIX_CPU_CLUT_RENDER:
				config.UserHacks_CPUCLUTRender = value;
				break;

			case GAMEDB_HWFIX_GPU_PALETTE_CONVERSION:
			{
				/* if 2, enable paltex when preloading is full, otherwise leave as-is */
				if (value > 1)
					config.GPUPaletteConversion = (config.TexturePreloading == GS_PRELOAD_FULL) ? true : config.GPUPaletteConversion;
				else
					config.GPUPaletteConversion = (value != 0);
			}
			break;

			case GAMEDB_HWFIX_MINIMUM_BLENDING_LEVEL:
			{
				if (value >= 0 && value <= static_cast<int>(GS_BLEND_MAXIMUM))
					config.AccurateBlendingUnit = (AccBlendLevel)pcsx2_max_i((int)config.AccurateBlendingUnit, value);
			}
			break;

			case GAMEDB_HWFIX_MAXIMUM_BLENDING_LEVEL:
			{
				if (value >= 0 && value <= static_cast<int>(GS_BLEND_MAXIMUM))
					config.AccurateBlendingUnit = (AccBlendLevel)pcsx2_min_i((int)config.AccurateBlendingUnit, value);
			}
			break;

			case GAMEDB_HWFIX_RECOMMENDED_BLENDING_LEVEL:
				break;

			case GAMEDB_HWFIX_GET_SKIP_COUNT:
				config.GetSkipCountFunctionId = static_cast<s16>(value);
				break;

			case GAMEDB_HWFIX_BEFORE_DRAW:
				config.BeforeDrawFunctionId = static_cast<s16>(value);
				break;

			case GAMEDB_HWFIX_MOVE_HANDLER:
				config.MoveHandlerFunctionId = static_cast<s16>(value);
				break;

			default:
				break;
		}

		log_cb(RETRO_LOG_INFO, "[GameDB] Enabled GS Hardware Fix: %s to [mode=%d]\n", gamedb_hwfix_name(id), value);
		num_applied_fixes++;
	}

	/* fixup skipdraw range just in case the db has a bad range (but the linter should catch this) */
	config.SkipDrawEnd = pcsx2_max_i(config.SkipDrawStart, config.SkipDrawEnd);

	return num_applied_fixes;
}
