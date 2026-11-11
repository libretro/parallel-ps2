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

/* The emulation core's options: the recompilers, the game fixes, the
 * speed hacks, the BIOS and the memory cards. Plain C, so C units and
 * Pcsx2Config alike hold and read them. */

#ifndef PCSX2_EMUOPTIONS_H
#define PCSX2_EMUOPTIONS_H

#include "../common/Pcsx2Defs.h"
#include "../common/FPControl.h"
#include "GS/GSOptionEnums.h"

#ifdef __cplusplus
extern "C" {
#endif

enum GamefixId
{
	GamefixId_FIRST = 0,

	Fix_FpuMultiply = GamefixId_FIRST,
	Fix_GoemonTlbMiss,
	Fix_SkipMpeg,
	Fix_OPHFlag,
	Fix_EETiming,
	Fix_InstantDMA,
	Fix_DMABusy,
	Fix_GIFFIFO,
	Fix_VIFFIFO,
	Fix_VIF1Stall,
	Fix_Ibit,
	Fix_VUSync,
	Fix_VUOverflow,
	Fix_XGKick,
	Fix_BlitInternalFPS,
	Fix_FullVU0Sync,
	Fix_VuAddSub,

	GamefixId_COUNT
};

enum SpeedHack
{
	SPEEDHACK_MVU_FLAG,
	SPEEDHACK_INSTANT_VU1,
	SPEEDHACK_MTVU,
	SPEEDHACK_EE_CYCLE_RATE,
	SPEEDHACK_COUNT
};

#define EMU_EE_CYCLE_RATE_MIN (-3)
#define EMU_EE_CYCLE_RATE_MAX 3
#define EMU_EE_CYCLE_SKIP_MAX 3

typedef struct EmuRecompilerOptions
{
	union
	{
		u32 bitset;
		struct
		{
			bool
				EnableEE         : 1,
				EnableIOP        : 1,
			        EnableVU0        : 1,
			        EnableVU1        : 1;

			bool
				vu0Overflow      : 1,
				vu0ExtraOverflow : 1,
			        vu0SignOverflow  : 1,
			        vu0Underflow     : 1;

			bool
				vu1Overflow      : 1,
				vu1ExtraOverflow : 1,
				vu1SignOverflow  : 1,
				vu1Underflow     : 1;

			bool
				fpuOverflow      : 1,
				fpuExtraOverflow : 1,
				fpuFullMode      : 1;

			bool    EnableEECache    : 1;
			bool    EnableFastmem    : 1;
			/* Route recompiled EE FPU arithmetic through ps2float. */
			bool    EnableFpuSoftFloat : 1;
			/* Per-unit VU soft float, audit-shaped: the bit keeps that
			 * unit on the interpreter, whose float ops already compute
			 * through the ps2float model (VUops.c), instead of the
			 * SSE recompiler. Exactness first; a per-op recompiler
			 * dispatch can replace the routing later without touching
			 * this surface. VU0 macro mode (COP2 through the EE rec)
			 * is NOT covered by the VU0 bit -- micro programs only. */
			bool    EnableVu0SoftFloat : 1;
			bool    EnableVu1SoftFloat : 1;
			/* Hardware-exact VU multiply in the VU recompilers. */
			bool    EnableVuExactMul : 1;
			bool    EnableVuExactDiv : 1;
			/* Exponent-distance operand masking for VU ADD/SUB. */
			bool    EnableVuAccurateAddSub : 1;
			/* EE FPU add/sub/mul family through the double pipeline. */
			bool    EnableFpuAccurateArith : 1;
		};
	};
} EmuRecompilerOptions;

/* The CPUs: which recompilers run, and the host float control each unit
 * runs under. */
typedef struct EmuCpuOptions
{
	EmuRecompilerOptions Recompiler;

	FPControlRegister FPUFPCR;
	FPControlRegister FPUDivFPCR;
	FPControlRegister VU0FPCR;
	FPControlRegister VU1FPCR;

	u32 AffinityControlMode;
} EmuCpuOptions;

#define EMU_GS_DEFAULT_FRAME_RATE_NTSC 59.94f
#define EMU_GS_DEFAULT_FRAME_RATE_PAL  50.00f

/* The GS: the renderer and how it draws. */
typedef struct EmuGSOptions
{
	union
	{
		u64 bitset;

		struct
		{
			bool
				PCRTCAntiBlur : 1,
				DisableInterlaceOffset : 1,
				PCRTCOffsets : 1,
				PCRTCOverscan : 1,
				UseDebugDevice : 1,
				DisableShaderCache : 1,
				DisableFramebufferFetch : 1,
				DisableVertexShaderExpand : 1,
				SkipDuplicateFrames : 1;

			bool
				GPUPaletteConversion : 1,
				PreloadFrameWithGSData : 1,
				Mipmap : 1,
				ManualUserHacks : 1,
				/* The native scaling option is set by the user: it is not
				 * masked with the hacks, and the game database's fix for it
				 * does not apply. */
				NativeScalingSet : 1,
				UserHacks_AlignSpriteX : 1,
				UserHacks_CPUFBConversion : 1,
				UserHacks_ReadTCOnClose : 1,
				UserHacks_DisableDepthSupport : 1,
				UserHacks_DisablePartialInvalidation : 1,
				UserHacks_DisableSafeFeatures : 1,
				UserHacks_DisableRenderFixes : 1,
				UserHacks_MergePPSprite : 1,
				UserHacks_ForceEvenSpritePosition : 1,
				UserHacks_NativePaletteDraw : 1,
				UserHacks_EstimateTextureRegion : 1;
		};
	};

	float FramerateNTSC;
	float FrameratePAL;

	GSInterlaceMode InterlaceMode;

	GSRendererType Renderer;
	float UpscaleMultiplier;

	AccBlendLevel AccurateBlendingUnit;
	GSHWMipmapMode HWMipmapMode;
	BiFiltering TextureFiltering;
	TexturePreloadingLevel TexturePreloading;
	GSHardwareDownloadMode HWDownloadMode;
	u8 Dithering;
	u8 MaxAnisotropy;
	s16 GetSkipCountFunctionId;
	s16 BeforeDrawFunctionId;
	s16 MoveHandlerFunctionId;
	int SkipDrawStart;
	int SkipDrawEnd;

	GSHalfPixelOffset UserHacks_HalfPixelOffset;
	s8 UserHacks_RoundSprite;
	GSNativeScaling UserHacks_NativeScaling;
	s32 UserHacks_TCOffsetX;
	s32 UserHacks_TCOffsetY;
	u8 UserHacks_CPUSpriteRenderBW;
	u8 UserHacks_CPUSpriteRenderLevel;
	u8 UserHacks_CPUCLUTRender;
	GSTextureInRtMode UserHacks_TextureInsideRt;
	GSLimit24BitDepth UserHacks_Limit24BitDepth;
	TriFiltering TriFilter;
	s8 OverrideTextureBarriers;
	u8 PGSSuperSampling;
	u8 PGSHighResScanout;
	u8 PGSDisableMipmaps;
	u8 PGSSuperSampleTextures;
	u8 PGSSharpBackbuffer;

	u16 SWExtraThreads;
	u16 SWExtraThreadsHeight;
} EmuGSOptions;

/* The network adapter API DEV9 uses. */
enum EmuNetApi
{
	EMU_NETAPI_UNSET         = 0,
	EMU_NETAPI_PCAP_BRIDGED  = 1,
	EMU_NETAPI_PCAP_SWITCHED = 2,
	EMU_NETAPI_TAP           = 3,
	EMU_NETAPI_SOCKETS       = 4
};

/* Where a DNS server address the DHCP server hands out comes from. */
enum EmuDnsMode
{
	EMU_DNS_MANUAL   = 0,
	EMU_DNS_AUTO     = 1,
	EMU_DNS_INTERNAL = 2
};

/* DEV9: the network adapter and the hard drive. */
typedef struct EmuDev9Options
{
	bool EthEnable;
	enum EmuNetApi EthApi;
	/* The adapter to use; "Auto" picks one. */
	char EthDevice[256];
	bool EthLogDNS;

	bool InterceptDHCP;
	u8 PS2IP[4];
	u8 Mask[4];
	u8 Gateway[4];
	u8 DNS1[4];
	u8 DNS2[4];
	bool AutoMask;
	bool AutoGateway;
	enum EmuDnsMode ModeDNS1;
	enum EmuDnsMode ModeDNS2;

	bool HddEnable;
	char HddFile[PCSX2_PATH_MAX];
	/* The drive's SCE security block; empty, <HddFile>.hddid is used
	 * when there is one. */
	char HddIdFile[PCSX2_PATH_MAX];
	/* The PS2's HDD max size is 2TB, 2^32 512-byte sectors; without
	 * 48-bit LBA the limit here is lower. */
	u32 HddSizeSectors;
} EmuDev9Options;

/* NOTE: the order of the bits is the order of the game fixes panel. */
typedef struct EmuGamefixOptions
{
	union
	{
		u32 bitset;
		struct
		{
			bool
				FpuMulHack    : 1, /* Tales of Destiny hangs. */
				GoemonTlbHack : 1, /* Goemon TLB miss hack. The game need to access unmapped virtual address. Instead to handle it as exception, TLB are preloaded at startup */
			        SkipMPEGHack   : 1, /* Skips MPEG videos (Katamari and other games need this) */
			        OPHFlagHack    : 1, /* Bleach Blade Battlers */
			        EETimingHack   : 1, /* General purpose timing hack. */
		                InstantDMAHack : 1, /* Instantly complete DMA's if possible, good for cache emulation problems. */
				DMABusyHack    : 1, /* Denies writes to the DMAC when it's busy. This is correct behaviour but bad timing can cause problems. */
				GIFFIFOHack    : 1, /* Enabled the GIF FIFO (more correct but slower) */
				VIFFIFOHack    : 1, /* Pretends to fill the non-existant VIF FIFO Buffer. */
				VIF1StallHack  : 1, /* Like above, processes FIFO data before the stall is allowed (to make sure data goes over). */
				IbitHack       : 1, /* I bit hack. Needed to stop constant VU recompilation in some games */
				VUSyncHack     : 1, /* Makes microVU run behind the EE to avoid VU register reading/writing sync issues. Useful for M-Bit games */
				VUOverflowHack : 1, /* DEPRECATED no-op: overflow detection is now unconditional; kept so GameDB/config files parse */
				XgKickHack     : 1, /* Erementar Gerad, adds more delay to VU XGkick instructions. Corrects the color of some graphics, but breaks Tri-ace games and others. */
				BlitInternalFPSHack : 1, /* Disables privileged register write-based FPS detection. */
				FullVU0SyncHack     : 1, /* Forces tight VU0 sync on every COP2 instruction. */
				/* Tri-Ace add/sub: per-title trigger for the masked
				 * accurate add/sub path when the global option is
				 * off. GameDB carries VuAddSubHack on the Star
				 * Ocean 3 / Radiata Stories / Valkyrie Profile 2
				 * family; the emission takes the union with the
				 * global option, so turning the option on covers
				 * these titles identically and the hack adds
				 * nothing. */
				VuAddSubHack        : 1;
		};
	};
} EmuGamefixOptions;

typedef struct EmuSpeedhackOptions
{
	union
	{
		u32 bitset;
		struct
		{
			bool fastCDVD   : 1, /* enables fast CDVD access */
			     IntcStat   : 1, /* fast-forward through INTC_STAT waits. */
			     WaitLoop   : 1, /* enables constant loop detection and fast-forwarding */
			     vuFlagHack : 1, /* microVU specific flag hack */
			     vuThread   : 1, /* Enable Threaded VU1 */
			     vu1Instant : 1; /* Enable Instant VU1 (Without MTVU only) */
		};
	};

	s8 EECycleRate; /* EE cycle rate selector (1.0, 1.5, 2.0) */
	u8 EECycleSkip; /* EE cycle skip factor (0, 1, 2, or 3) */
} EmuSpeedhackOptions;

typedef struct EmuFilenameOptions
{
	/* A BIOS filename, not a full path -- it is joined under the BIOS
	 * folder. PCSX2_PATH_MAX is generous for a leaf name and matches
	 * what the rest of the tree uses. */
	char Bios[PCSX2_PATH_MAX];
} EmuFilenameOptions;

/* One memory card slot. */
typedef struct EmuMcdOptions
{
	char Filename[PCSX2_PATH_MAX]; /* user-configured location of this card */
	bool Enabled; /* if false, the card does not show up in-game */
} EmuMcdOptions;

/* Defaults: every recompiler on, standard overflow clamping, the
 * accurate EE FPU arithmetic on. */
void emu_recompiler_defaults(EmuRecompilerOptions* o);

/* Defaults: the recompilers' as above; every unit in the emulator's float
 * control (fpcr_emu_default), except division, which rounds to nearest. */
void emu_cpu_defaults(EmuCpuOptions* o);
bool emu_cpu_equal(const EmuCpuOptions* a, const EmuCpuOptions* b);
/* Whether a different set of recompilers runs: the CPUs need a reset. */
bool emu_cpu_recompilers_changed(const EmuCpuOptions* a, const EmuCpuOptions* b);

/* Defaults: the renderer chosen for the frontend's API, native
 * resolution, the game database's interlacing and trilinear choices,
 * basic blending, every user hack off. */
void emu_gs_defaults(EmuGSOptions* o);
/* Whether two configurations draw alike, the frame rates included. */
bool emu_gs_equal(const EmuGSOptions* a, const EmuGSOptions* b);
/* With manual hacks off, every user hack back to its default. */
void emu_gs_mask_user_hacks(EmuGSOptions* o);
/* Unless upscaling with manual hacks, the upscaling hacks back to theirs. */
void emu_gs_mask_upscaling_hacks(EmuGSOptions* o);

/* Any renderer but the software one. */
static PCSX2_INLINE bool emu_gs_hardware_renderer(const EmuGSOptions* o)
{
	return o->Renderer != GS_RENDERER_SW;
}

/* Defaults: no network, an automatic mask, gateway and DNS, and a 40 GB
 * drive in DEV9hdd.raw, both off. */
void emu_dev9_defaults(EmuDev9Options* o);
bool emu_dev9_equal(const EmuDev9Options* a, const EmuDev9Options* b);

/* Every game fix off. */
void emu_gamefix_clear_all(EmuGamefixOptions* o);
bool emu_gamefix_get(const EmuGamefixOptions* o, enum GamefixId id);
void emu_gamefix_set(EmuGamefixOptions* o, enum GamefixId id, bool enabled);
/* A game fix's name, "VuAddSub" for Fix_VuAddSub; NULL past the last. */
const char* emu_gamefix_name(u32 id);

/* Defaults: the recommended hacks (wait loop, INTC_STAT, the mVU flag
 * hack, instant VU1) on, the rest off. */
void emu_speedhack_defaults(EmuSpeedhackOptions* o);
/* Every hack off. */
void emu_speedhack_clear_all(EmuSpeedhackOptions* o);
void emu_speedhack_set(EmuSpeedhackOptions* o, enum SpeedHack id, int value);
bool emu_speedhack_equal(const EmuSpeedhackOptions* a, const EmuSpeedhackOptions* b);
/* The hack's name in the game database, "mtvu" for SPEEDHACK_MTVU. */
const char* emu_speedhack_name(enum SpeedHack id);
/* The hack a name (a run of len characters) names; false if none. */
bool emu_speedhack_parse_name(const char* name, size_t len, enum SpeedHack* out);

#ifdef __cplusplus
}
#endif

#endif
