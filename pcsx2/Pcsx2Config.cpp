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

#include <file/file_path.h>

extern std::string libretro_content;

// This macro is actually useful for about any and every possible application of C++
// equality operators.
#define OpEqu(field) (field == right.field)

// Default EE/VU control registers have exceptions off, DaZ/FTZ, and the rounding mode set to Chop/Zero.
static constexpr FPControlRegister DEFAULT_FPU_FP_CONTROL_REGISTER = FPControlRegister::GetDefault()
	.DisableExceptions()
	.SetDenormalsAreZero(true)
.SetFlushToZero(true)
	.SetRoundMode(FPRoundMode::ChopZero);
static constexpr FPControlRegister DEFAULT_VU_FP_CONTROL_REGISTER = FPControlRegister::GetDefault()
	.DisableExceptions()
	.SetDenormalsAreZero(true)
.SetFlushToZero(true)
	.SetRoundMode(FPRoundMode::ChopZero);


Pcsx2Config EmuConfig;

const char* SettingInfo::StringDefaultValue() const
{
	return default_value ? default_value : "";
}

bool SettingInfo::BooleanDefaultValue() const
{
	bool value = false;
	if (default_value)
		parse_number_bool(default_value, strlen(default_value), &value);
	return value;
}

s32 SettingInfo::IntegerDefaultValue() const
{
	s32 value = 0;
	if (default_value)
		parse_number_s32(default_value, strlen(default_value), 10, &value);
	return value;
}

s32 SettingInfo::IntegerMinValue() const
{
	static constexpr s32 fallback_value = INT32_MIN;
	s32 value = fallback_value;
	if (min_value)
		parse_number_s32(min_value, strlen(min_value), 10, &value);
	return value;
}

s32 SettingInfo::IntegerMaxValue() const
{
	static constexpr s32 fallback_value = INT32_MAX;
	s32 value = fallback_value;
	if (max_value)
		parse_number_s32(max_value, strlen(max_value), 10, &value);
	return value;
}

s32 SettingInfo::IntegerStepValue() const
{
	static constexpr s32 fallback_value = 1;
	s32 value = fallback_value;
	if (step_value)
		parse_number_s32(step_value, strlen(step_value), 10, &value);
	return value;
}

float SettingInfo::FloatDefaultValue() const
{
	float value = 0.0f;
	if (default_value)
		parse_number_float(default_value, strlen(default_value), &value);
	return value;
}

float SettingInfo::FloatMinValue() const
{
	static constexpr float fallback_value = FLT_MIN;
	float value = fallback_value;
	if (min_value)
		parse_number_float(min_value, strlen(min_value), &value);
	return value;
}

float SettingInfo::FloatMaxValue() const
{
	static constexpr float fallback_value = FLT_MAX;
	float value = fallback_value;
	if (max_value)
		parse_number_float(max_value, strlen(max_value), &value);
	return value;
}

float SettingInfo::FloatStepValue() const
{
	static constexpr float fallback_value = 0.1f;
	float value = fallback_value;
	if (step_value)
		parse_number_float(step_value, strlen(step_value), &value);
	return value;
}

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

const char* const s_speed_hack_names[] =
{
	"mvuFlag",
	"instantVU1",
	"mtvu",
	"eeCycleRate",
};

const char* Pcsx2Config::SpeedhackOptions::GetSpeedHackName(SpeedHack id)
{
	return s_speed_hack_names[static_cast<u32>(id)];
}

bool Pcsx2Config::SpeedhackOptions::ParseSpeedHackName(const char* name, size_t len, SpeedHack* out)
{
	for (u32 i = 0; i < C89_ARRAY_SIZE(s_speed_hack_names); i++)
	{
		if (strlen(s_speed_hack_names[i]) == len && memcmp(s_speed_hack_names[i], name, len) == 0)
		{
			*out = static_cast<SpeedHack>(i);
			return true;
		}
	}

	return false;
}

void Pcsx2Config::SpeedhackOptions::Set(SpeedHack id, int value)
{
	switch (id)
	{
		case SpeedHack::MVUFlag:
			vuFlagHack = (value != 0);
			break;
		case SpeedHack::InstantVU1:
			vu1Instant = (value != 0);
			break;
		case SpeedHack::MTVU:
			vuThread = (value != 0);
			break;
		case SpeedHack::EECycleRate:
			EECycleRate = static_cast<int>(pcsx2_clamp_i(value, MIN_EE_CYCLE_RATE, MAX_EE_CYCLE_RATE));
			break;
		default:
			break;
	}
}

bool Pcsx2Config::SpeedhackOptions::operator==(const SpeedhackOptions& right) const
{
	return OpEqu(bitset) && OpEqu(EECycleRate) && OpEqu(EECycleSkip);
}

bool Pcsx2Config::SpeedhackOptions::operator!=(const SpeedhackOptions& right) const
{
	return !operator==(right);
}

Pcsx2Config::SpeedhackOptions::SpeedhackOptions()
{
	DisableAll();

	// Set recommended speedhacks to enabled by default. They'll still be off globally on resets.
	WaitLoop = true;
	IntcStat = true;
	vuFlagHack = true;
	vu1Instant = true;
}

Pcsx2Config::SpeedhackOptions& Pcsx2Config::SpeedhackOptions::DisableAll()
{
	bitset = 0;
	EECycleRate = 0;
	EECycleSkip = 0;

	return *this;
}


Pcsx2Config::RecompilerOptions::RecompilerOptions()
{
	bitset = 0;

	// All recs are enabled by default.

	EnableEE = true;
	EnableEECache = false;
	EnableFpuSoftFloat = false;
	EnableVu0SoftFloat = false;
	EnableVu1SoftFloat = false;
	EnableVuExactMul = false;
	EnableVuExactDiv = false;
	EnableVuAccurateAddSub = false;
	/* Default-on per the fpaudit price tables: closes EE add/sub
	 * exactly and gates the ABS/NEG and CVT.S.W accuracy fixes, at a
	 * per-op cost of about two nanoseconds that measures within run
	 * noise on real game frame throughput (three-rep Tekken
	 * workload). The divide family stays on FpuSoftFloat, opt-in. */
	EnableFpuAccurateArith = true;
	EnableIOP = true;
	EnableVU0 = true;
	EnableVU1 = true;
	EnableFastmem = true;

	// vu and fpu clamping default to standard overflow.
	vu0Overflow = true;
	//vu0ExtraOverflow = false;
	//vu0SignOverflow = false;
	//vu0Underflow = false;
	vu1Overflow = true;
	//vu1ExtraOverflow = false;
	//vu1SignOverflow = false;
	//vu1Underflow = false;

	fpuOverflow = true;
	//fpuExtraOverflow = false;
	//fpuFullMode = false;
}

void Pcsx2Config::RecompilerOptions::ApplySanityCheck()
{
	bool fpuIsRight = true;

	if (fpuExtraOverflow)
		fpuIsRight = fpuOverflow;

	if (fpuFullMode)
		fpuIsRight = fpuOverflow && fpuExtraOverflow;

	if (!fpuIsRight)
	{
		// Values are wonky; assume the defaults.
		fpuOverflow = RecompilerOptions().fpuOverflow;
		fpuExtraOverflow = RecompilerOptions().fpuExtraOverflow;
		fpuFullMode = RecompilerOptions().fpuFullMode;
	}

	bool vuIsOk = true;

	if (vu0ExtraOverflow)
		vuIsOk = vuIsOk && vu0Overflow;
	if (vu0SignOverflow)
		vuIsOk = vuIsOk && vu0ExtraOverflow;

	if (!vuIsOk)
	{
		// Values are wonky; assume the defaults.
		vu0Overflow = RecompilerOptions().vu0Overflow;
		vu0ExtraOverflow = RecompilerOptions().vu0ExtraOverflow;
		vu0SignOverflow = RecompilerOptions().vu0SignOverflow;
		vu0Underflow = RecompilerOptions().vu0Underflow;
	}

	vuIsOk = true;

	if (vu1ExtraOverflow)
		vuIsOk = vuIsOk && vu1Overflow;
	if (vu1SignOverflow)
		vuIsOk = vuIsOk && vu1ExtraOverflow;

	if (!vuIsOk)
	{
		// Values are wonky; assume the defaults.
		vu1Overflow = RecompilerOptions().vu1Overflow;
		vu1ExtraOverflow = RecompilerOptions().vu1ExtraOverflow;
		vu1SignOverflow = RecompilerOptions().vu1SignOverflow;
		vu1Underflow = RecompilerOptions().vu1Underflow;
	}
}


bool Pcsx2Config::CpuOptions::CpusChanged(const CpuOptions& right) const
{
	return (Recompiler.EnableEE != right.Recompiler.EnableEE ||
			Recompiler.EnableIOP != right.Recompiler.EnableIOP ||
			Recompiler.EnableVU0 != right.Recompiler.EnableVU0 ||
			Recompiler.EnableVU1 != right.Recompiler.EnableVU1);
}

Pcsx2Config::CpuOptions::CpuOptions()
{
	FPUFPCR = DEFAULT_FPU_FP_CONTROL_REGISTER;

	// Rounding defaults to nearest to match old behavior.
	// TODO: Make it default to the same as the rest of the FPU operations, at some point.
	FPUDivFPCR = FPControlRegister(DEFAULT_FPU_FP_CONTROL_REGISTER).SetRoundMode(FPRoundMode::Nearest);

	VU0FPCR = DEFAULT_VU_FP_CONTROL_REGISTER;
	VU1FPCR = DEFAULT_VU_FP_CONTROL_REGISTER;
	AffinityControlMode = 0;
}

void Pcsx2Config::CpuOptions::ApplySanityCheck()
{
	AffinityControlMode = pcsx2_min_u(AffinityControlMode, 6);

	Recompiler.ApplySanityCheck();
}


Pcsx2Config::GSOptions::GSOptions()
{
	bitset = 0;

	PCRTCAntiBlur = true;
	DisableInterlaceOffset = false;
	PCRTCOffsets = false;
	PCRTCOverscan = false;
	UseDebugDevice = false;
	DisableShaderCache = false;
	DisableFramebufferFetch = false;
	DisableVertexShaderExpand = false;
	SkipDuplicateFrames = false;

	HWDownloadMode = GSHardwareDownloadMode::Enabled;
	GPUPaletteConversion = false;
	PreloadFrameWithGSData = false;
	Mipmap = true;

	ManualUserHacks = false;
	NativeScalingSet = false;
	UserHacks_AlignSpriteX = false;
	UserHacks_CPUFBConversion = false;
	UserHacks_ReadTCOnClose = false;
	UserHacks_DisableDepthSupport = false;
	UserHacks_DisablePartialInvalidation = false;
	UserHacks_DisableSafeFeatures = false;
	UserHacks_DisableRenderFixes = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_NativePaletteDraw = false;
	UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;

}

bool Pcsx2Config::GSOptions::operator==(const GSOptions& right) const
{
	return (
		OpEqu(FramerateNTSC) &&
		OpEqu(FrameratePAL) &&

		OptionsAreEqual(right));
}

bool Pcsx2Config::GSOptions::OptionsAreEqual(const GSOptions& right) const
{
	return (
		OpEqu(bitset) &&

		OpEqu(InterlaceMode) &&

		OpEqu(Renderer) &&
		OpEqu(UpscaleMultiplier) &&

		OpEqu(AccurateBlendingUnit) &&
		OpEqu(HWMipmapMode) &&
		OpEqu(TextureFiltering) &&
		OpEqu(TexturePreloading) &&
		OpEqu(HWDownloadMode) &&
		OpEqu(Dithering) &&
		OpEqu(MaxAnisotropy) &&
		OpEqu(SWExtraThreads) &&
		OpEqu(SWExtraThreadsHeight) &&
		OpEqu(TriFilter) &&
		OpEqu(GetSkipCountFunctionId) &&
		OpEqu(BeforeDrawFunctionId) &&
		OpEqu(MoveHandlerFunctionId) &&
		OpEqu(SkipDrawEnd) &&
		OpEqu(SkipDrawStart) &&

		OpEqu(UserHacks_HalfPixelOffset) &&
		OpEqu(UserHacks_RoundSprite) &&
		OpEqu(UserHacks_NativeScaling) &&
		OpEqu(NativeScalingSet) &&
		OpEqu(UserHacks_TCOffsetX) &&
		OpEqu(UserHacks_TCOffsetY) &&
		OpEqu(UserHacks_CPUSpriteRenderBW) &&
		OpEqu(UserHacks_CPUSpriteRenderLevel) &&
		OpEqu(UserHacks_CPUCLUTRender) &&
		OpEqu(UserHacks_TextureInsideRt) &&
		OpEqu(UserHacks_Limit24BitDepth) &&
		OpEqu(OverrideTextureBarriers) &&
		OpEqu(Adapter) &&
		OpEqu(PGSSuperSampling) &&
		OpEqu(PGSHighResScanout) &&
		OpEqu(PGSDisableMipmaps) &&
		OpEqu(PGSSharpBackbuffer) &&
		OpEqu(PGSSuperSampleTextures)
		);
}

bool Pcsx2Config::GSOptions::operator!=(const GSOptions& right) const
{
	return !operator==(right);
}

bool Pcsx2Config::GSOptions::RestartOptionsAreEqual(const GSOptions& right) const
{
	return OpEqu(Renderer) &&
		   OpEqu(Adapter) &&
		   OpEqu(UseDebugDevice) &&
		   OpEqu(DisableShaderCache) &&
		   OpEqu(DisableFramebufferFetch) &&
		   OpEqu(DisableVertexShaderExpand) &&
		   OpEqu(OverrideTextureBarriers);
}


void Pcsx2Config::GSOptions::MaskUserHacks()
{
	if (ManualUserHacks)
		return;

	UserHacks_AlignSpriteX = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_NativePaletteDraw = false;
	UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;
	UserHacks_DisableSafeFeatures = false;
	UserHacks_DisableRenderFixes = false;
	UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
	UserHacks_RoundSprite = 0;
	if (!NativeScalingSet)
		UserHacks_NativeScaling = GSNativeScaling::NativeScaling_Normal;
	GPUPaletteConversion = false;
	PreloadFrameWithGSData = false;
	UserHacks_DisablePartialInvalidation = false;
	UserHacks_DisableDepthSupport = false;
	UserHacks_CPUFBConversion = false;
	UserHacks_ReadTCOnClose = false;
	UserHacks_TextureInsideRt = GSTextureInRtMode::Disabled;
	UserHacks_EstimateTextureRegion = false;
	UserHacks_TCOffsetX = 0;
	UserHacks_TCOffsetY = 0;
	UserHacks_CPUSpriteRenderBW = 0;
	UserHacks_CPUSpriteRenderLevel = 0;
	UserHacks_CPUCLUTRender = 0;
	SkipDrawStart = 0;
	SkipDrawEnd = 0;
}

void Pcsx2Config::GSOptions::MaskUpscalingHacks()
{
	if (UpscaleMultiplier > 1.0f && ManualUserHacks)
		return;

	UserHacks_AlignSpriteX = false;
	UserHacks_MergePPSprite = false;
	UserHacks_ForceEvenSpritePosition = false;
	UserHacks_NativePaletteDraw = false;
	UserHacks_Limit24BitDepth = GSLimit24BitDepth::Disabled;
	UserHacks_HalfPixelOffset = GSHalfPixelOffset::Off;
	UserHacks_RoundSprite = 0;
	if (!NativeScalingSet)
		UserHacks_NativeScaling = GSNativeScaling::NativeScaling_Normal;
	UserHacks_TCOffsetX = 0;
	UserHacks_TCOffsetY = 0;
}

bool Pcsx2Config::GSOptions::UseHardwareRenderer() const
{
	return (Renderer != GSRendererType::SW);
}

const char* Pcsx2Config::DEV9Options::NetApiNames[] = {
	"Unset",
	"PCAP Bridged",
	"PCAP Switched",
	"TAP",
	"Sockets",
	nullptr};

const char* Pcsx2Config::DEV9Options::DnsModeNames[] = {
	"Manual",
	"Auto",
	"Internal",
	nullptr};

Pcsx2Config::DEV9Options::DEV9Options()
{
	HddFile = "DEV9hdd.raw";
}


void Pcsx2Config::DEV9Options::LoadIPHelper(u8* field, const std::string& setting)
{
	const char* p = setting.c_str();
	u8          octets[4];
	int         i;

	for (i = 0; i < 4; i++)
	{
		unsigned int v      = 0;
		int          digits = 0;

		while (*p >= '0' && *p <= '9')
		{
			v = v * 10 + (unsigned int)(*p - '0');
			if (v > 255)
				goto fail;
			p++;
			digits++;
		}

		if (digits == 0)
			goto fail;

		octets[i] = (u8)v;

		if (i < 3)
		{
			if (*p != '.')
				goto fail;
			p++;
		}
	}

	if (*p != '\0')
		goto fail;

	field[0] = octets[0];
	field[1] = octets[1];
	field[2] = octets[2];
	field[3] = octets[3];
	return;

fail:
	memset(field, 0, sizeof(field[0]) * 4);
}

static const char* const tbl_GamefixNames[] =
{
	"FpuMul",
	"GoemonTlb",
	"SkipMPEG",
	"OPHFlag",
	"EETiming",
	"InstantDMA",
	"DMABusy",
	"GIFFIFO",
	"VIFFIFO",
	"VIF1Stall",
	"Ibit",
	"VUSync",
	"VUOverflow",
	"XGKick",
	"BlitInternalFPS",
	"FullVU0Sync",
	"VuAddSub",
};

const char* EnumToString(GamefixId id)
{
	return tbl_GamefixNames[id];
}

// all gamefixes are disabled by default.
Pcsx2Config::GamefixOptions::GamefixOptions()
{
	DisableAll();
}

Pcsx2Config::GamefixOptions& Pcsx2Config::GamefixOptions::DisableAll()
{
	bitset = 0;
	return *this;
}

void Pcsx2Config::GamefixOptions::Set(GamefixId id, bool enabled)
{
	switch (id)
	{
		case Fix_FpuMultiply:         FpuMulHack              = enabled; break;
		case Fix_XGKick:              XgKickHack              = enabled; break;
		case Fix_EETiming:            EETimingHack            = enabled; break;
		case Fix_InstantDMA:          InstantDMAHack          = enabled; break;
		case Fix_SkipMpeg:            SkipMPEGHack            = enabled; break;
		case Fix_OPHFlag:             OPHFlagHack             = enabled; break;
		case Fix_DMABusy:             DMABusyHack             = enabled; break;
		case Fix_VIFFIFO:             VIFFIFOHack             = enabled; break;
		case Fix_VIF1Stall:           VIF1StallHack           = enabled; break;
		case Fix_GIFFIFO:             GIFFIFOHack             = enabled; break;
		case Fix_GoemonTlbMiss:       GoemonTlbHack           = enabled; break;
		case Fix_Ibit:                IbitHack                = enabled; break;
		case Fix_VUSync:              VUSyncHack              = enabled; break;
		case Fix_VUOverflow:          VUOverflowHack          = enabled; break;
		case Fix_BlitInternalFPS:     BlitInternalFPSHack     = enabled; break;
		case Fix_FullVU0Sync:         FullVU0SyncHack         = enabled; break;
		case Fix_VuAddSub:            VuAddSubHack            = enabled; break;
		default:
					      break;
	}
}

bool Pcsx2Config::GamefixOptions::Get(GamefixId id) const
{
	switch (id)
	{
		case Fix_FpuMultiply:         return FpuMulHack;
		case Fix_XGKick:              return XgKickHack;
		case Fix_EETiming:            return EETimingHack;
		case Fix_InstantDMA:          return InstantDMAHack;
		case Fix_SkipMpeg:            return SkipMPEGHack;
		case Fix_OPHFlag:             return OPHFlagHack;
		case Fix_DMABusy:             return DMABusyHack;
		case Fix_VIFFIFO:             return VIFFIFOHack;
		case Fix_VIF1Stall:           return VIF1StallHack;
		case Fix_GIFFIFO:             return GIFFIFOHack;
		case Fix_GoemonTlbMiss:       return GoemonTlbHack;
		case Fix_Ibit:                return IbitHack;
		case Fix_VUSync:              return VUSyncHack;
		case Fix_VUOverflow:          return VUOverflowHack;
		case Fix_BlitInternalFPS:     return BlitInternalFPSHack;
		case Fix_FullVU0Sync:         return FullVU0SyncHack;
		case Fix_VuAddSub:            return VuAddSubHack;
		default:
					      break;
	}
	return false; // unreachable, but we still need to suppress warnings >_<
}


Pcsx2Config::FilenameOptions::FilenameOptions()
{
	/* A char array has no default constructor; std::string did. */
	Bios[0] = '\0';
}


Pcsx2Config::Pcsx2Config()
{
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
		OpEqu(Cpu) &&
		OpEqu(GS) &&
		OpEqu(DEV9) &&
		OpEqu(Speedhacks) &&
		OpEqu(Gamefixes) &&
		OpEqu(BaseFilenames) &&
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
	const FPControlRegister prev = FPControlRegister::GetCurrent();
	FPControlRegister::SetCurrent(unit ? EmuConfig.Cpu.VU1FPCR : EmuConfig.Cpu.VU0FPCR);
	return prev.bitmask;
}

u32 emu_config_framerate_mhz(bool pal)
{
	return (u32)lrintf((pal ? EmuConfig.GS.FrameratePAL : EmuConfig.GS.FramerateNTSC) * 1000.0f);
}

u32 emu_config_default_framerate_mhz(bool pal)
{
	return (u32)((pal ? Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_PAL
		: Pcsx2Config::GSOptions::DEFAULT_FRAME_RATE_NTSC) * 1000.0f);
}

u64 emu_fpcr_set_fpu(void)
{
	const FPControlRegister prev = FPControlRegister::GetCurrent();
	FPControlRegister::SetCurrent(EmuConfig.Cpu.FPUFPCR);
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
	FPControlRegister::SetCurrent(r);
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
	Speedhacks.EECycleRate = pcsx2_clamp_i(Speedhacks.EECycleRate, SpeedhackOptions::MIN_EE_CYCLE_RATE, SpeedhackOptions::MAX_EE_CYCLE_RATE);
	Speedhacks.EECycleSkip = pcsx2_min_i(Speedhacks.EECycleSkip, SpeedhackOptions::MAX_EE_CYCLE_SKIP);
	GS.SkipDrawEnd         = pcsx2_max_i(GS.SkipDrawStart, GS.SkipDrawEnd);
}
