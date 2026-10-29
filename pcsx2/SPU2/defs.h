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

#include "Mixer.h"
#include "SndOut.h"
#include "Global.h"
#include "../../common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* alignas is C++11 and C11; these sources also build as C89 under MSVC. */
#if defined(_MSC_VER)
#define SPU2_ALIGN(n) __declspec(align(n))
#elif defined(__GNUC__)
#define SPU2_ALIGN(n) __attribute__((aligned(n)))
#else
#define SPU2_ALIGN(n)
#endif

typedef struct V_SPDIF
{
	u16 Out;
	u16 Info;
	u16 Unknown1;
	u16 Mode;
	u16 Media;
	u16 Unknown2;
	u16 Protection;
} V_SPDIF;

typedef struct V_CoreRegs
{
	u32 PMON;
	u32 NON;
	u32 VMIXL;
	u32 VMIXR;
	u32 VMIXEL;
	u32 VMIXER;
	u32 ENDX;

	u16 MMIX;
	u16 STATX;
	u16 ATTR;
	u16 _1AC;
} V_CoreRegs;

typedef struct V_VoiceGates
{
	s32 DryL; /* 'AND Gate' for Direct Output to Left Channel */
	s32 DryR; /* 'AND Gate' for Direct Output for Right Channel */
	s32 WetL; /* 'AND Gate' for Effect Output for Left Channel */
	s32 WetR; /* 'AND Gate' for Effect Output for Right Channel */
} V_VoiceGates;

typedef struct V_CoreGates
{
	s32 InpL; /* Sound Data Input to Direct Output (Left) */
	s32 InpR; /* Sound Data Input to Direct Output (Right) */
	s32 SndL; /* Voice Data to Direct Output (Left) */
	s32 SndR; /* Voice Data to Direct Output (Right) */
	s32 ExtL; /* External Input to Direct Output (Left) */
	s32 ExtR; /* External Input to Direct Output (Right) */
} V_CoreGates;

typedef struct VoiceMixSet
{
	StereoOut32 Dry, Wet;
} VoiceMixSet;


extern V_SPDIF Spdif;

/* Output Buffer Writing Position (the same for all data); */
extern u16 OutPos;
/* Input Buffer Reading Position (the same for all data); */
extern u16 InputPos;
/* SPU Mixing Cycles ("Ticks mixed" counter) */
extern u32 Cycles;
/* DC Filter state. The output side is Q16: see Mix() in Mixer.cpp. */
extern StereoOut32 DCFilterIn;
extern StereoOut64 DCFilterOut;

extern s16 spu2regs[0x010000 / sizeof(s16)];
extern s16 _spu2mem[0x200000 / sizeof(s16)];
extern int PlayMode;

#define GetMemPtr(addr) (_spu2mem + (addr))
#define spu2M_Read(addr) (*GetMemPtr((addr) & 0xfffff))

/* -------------------------------------------------------------------------------------- */
/*  SPU2 Register Table LUT */
/* -------------------------------------------------------------------------------------- */
extern u16 * const regtable[0x401];

/* -------------------------------------------------------------------------------------- */
/*  SPU2 Memory Indexers */
/* -------------------------------------------------------------------------------------- */

#define spu2Rs16(mmem) (*(s16*)((s8*)spu2regs + ((mmem)&0x1fff)))
#define spu2Ru16(mmem) (*(u16*)((s8*)spu2regs + ((mmem)&0x1fff)))

typedef struct V_VolumeLR
{
	s32 Left;
	s32 Right;
} V_VolumeLR;

typedef struct V_VolumeSlide
{
	/* Holds the "original" value of the volume for this voice, prior to slides. */
	/* (ie, the volume as written to the register) */

	union
	{
		u16 Reg_VOL;
		struct
		{
			u16 Step : 2;
			u16 Shift : 5;
			u16 : 5;
			u16 Phase : 1;
			u16 Decr : 1;
			u16 Exp : 1;
			u16 Enable : 1;
		};
	};

	u32 Counter;
	s32 Value;
} V_VolumeSlide;

typedef struct V_VolumeSlideLR
{
	V_VolumeSlide Left;
	V_VolumeSlide Right;
} V_VolumeSlideLR;

#define ADSR_PHASES 5

typedef struct CachedADSR
{
	bool Decr;
	bool Exp;
	u8 Shift;
	s8 Step;
	s32 Target;
} CachedADSR;

#define PHASE_STOPPED 0
#define PHASE_ATTACK 1
#define PHASE_DECAY 2
#define PHASE_SUSTAIN 3
#define PHASE_RELEASE 4

typedef struct V_ADSR
{
	union
	{
		u32 reg32;

		struct
		{
			u16 regADSR1;
			u16 regADSR2;
		};

		struct
		{
			u32 SustainLevel : 4;
			u32 DecayShift : 4;
			u32 AttackStep : 2;
			u32 AttackShift : 5;
			u32 AttackMode : 1;
			u32 ReleaseShift : 5;
			u32 ReleaseMode : 1;
			u32 SustainStep : 2;
			u32 SustainShift : 5;
			u32 : 1;
			u32 SustainDir : 1;
			u32 SustainMode : 1;
		};
	};

	struct CachedADSR CachedPhases[ADSR_PHASES];

	u32 Counter;
	s32 Value; /* Ranges from 0 to 0x7fff (signed values are clamped to 0) [Reg_ENVX] */
	u8 Phase; /* monitors current phase of ADSR envelope */
} V_ADSR;

static __fi void ADSR_Release(V_ADSR *v)
{
	if (v->Phase != PHASE_STOPPED)
	{
		v->Phase   = PHASE_RELEASE;
		v->Counter = 0;
	}
}

static __fi bool ADSR_Calculate(V_ADSR *v)
{
	/* v->Phase is by construction always in [PHASE_STOPPED .. PHASE_RELEASE]
	 * (range 0..4), bounded by every site that writes to it: ADSR_Release
	 * sets PHASE_RELEASE, the KeyOn path sets PHASE_ATTACK, the v->Phase++
	 * below is gated by the PHASE_RELEASE termination check, and the
	 * scattered PHASE_STOPPED/PHASE_SUSTAIN assignments are also in range.
	 * Use unchecked indexing to skip the .at() bounds check on every call;
	 * this function runs per-active-voice per-sample at 48 kHz. */
	struct CachedADSR *p = &v->CachedPhases[v->Phase];

	/* maybe not correct for the "infinite" settings */
	s32 counter_shift = p->Shift - 11;
	s32 level_shift   = 11 - p->Shift;
	u32 counter_inc = 0x8000 >> (counter_shift > 0 ? counter_shift : 0);
	/* p->Step can be negative; shift in the unsigned domain (same bits). */
	s32 level_inc   = (s32)((u32)p->Step << (level_shift > 0 ? level_shift : 0));

	if (p->Exp)
	{
		if (p->Decr)
			level_inc = (s16)((level_inc * v->Value) >> 15);
		else
		{
			if (v->Value > 0x6000)
				counter_inc >>= 2;
		}
	}

	if (counter_inc == 0) counter_inc = 1;
	v->Counter  += counter_inc;

	if (v->Counter >= 0x8000)
	{
		s32 next   = v->Value + level_inc;
		if (next < 0)         next = 0;
		if (next > INT16_MAX) next = INT16_MAX;
		v->Counter  = 0;
		v->Value    = next;
	}

	/* Stay in sustain until key off or silence */
	if (v->Phase == PHASE_SUSTAIN)
		return v->Value != 0;

	/* Check if target is reached to advance phase */
	if ((!p->Decr && v->Value >= p->Target) || (p->Decr && v->Value <= p->Target))
		v->Phase++;

	/* All phases done, stop the voice */
	if (v->Phase > PHASE_RELEASE)
		return false;

	return true;
}

void ADSR_UpdateCache(V_ADSR *v);

/* V_Voice field layout is optimized for cache line access in the mixer */
/* hot path. */
/* With SPU2_ALIGN(64) on Voices[] and sizeof == 192 (3 x 64), each voice occupies */
/* exactly 3 cache lines: */
/*   CL0 (0-63):   ADSR + SBuffer - ADSR_Calculate stays within one cache line */
/*   CL1 (64-127): pitch, interpolation, volume - all remaining per-sample fields */
/*   CL2 (128-191): cold - block boundary (every 28 samples) and KeyOn only */
typedef struct V_Voice
{
	V_ADSR ADSR;        /* 56 bytes: Phase(+52) checked first for stopped-voice early exit */
	s16* SBuffer;        /* ADPCM cache entry pointer */

	/* --- Cache line 1: per-sample mixer fields --- */

	s32 SP;              /* Sample pointer (19:12 bit fixed point) */
	u16 Pitch;           /* also Reg_PITCH */
	bool Modulated;      /* Pitch modulated by previous voice */
	bool Noise;          /* Source (Wave/Noise) */
	s32 OutX;            /* Last output, used for voice modulation of next voice */
	s32 SCurrent;        /* Sample position within current decoded packet */
	u32 NextA;           /* Next Read Data address (also Reg_NAXH/L) */
	s8 LoopFlags;

	s8 LoopMode;
	u16 _pad0;
	s32 PV1;             /* Previous sample values for interpolation */
	s32 PV2;
	s32 PV3;
	s32 PV4;
	V_VolumeSlideLR Volume; /* 24 bytes: Left.Value(+112) Right.Value(+124) */

	/* --- Cache line 2: cold - accessed on block boundary or KeyOn --- */
	u32 LoopStartA;      /* Loop Start address (also Reg_LSAH/L) */
	u32 StartA;          /* Sound Start address (also Reg_SSAH/L) */
	s32 Prev1;           /* Voice Decoding State (ADPCM predictor) */
	s32 Prev2;
	/* The predictor state the block SBuffer holds was decoded from. The
	 * block cache is not saved, so a thaw decodes that block again from
	 * these; without them the rest of the block played as silence. */
	s32 BlockPrev1;
	s32 BlockPrev2;
	u8 _pad1[40];
} V_Voice;

typedef struct V_Reverb
{
	s16 IN_COEF_L;
	s16 IN_COEF_R;

	u32 APF1_SIZE;
	u32 APF2_SIZE;

	s16 APF1_VOL;
	s16 APF2_VOL;

	u32 SAME_L_SRC;
	u32 SAME_R_SRC;
	u32 DIFF_L_SRC;
	u32 DIFF_R_SRC;
	u32 SAME_L_DST;
	u32 SAME_R_DST;
	u32 DIFF_L_DST;
	u32 DIFF_R_DST;

	s16 IIR_VOL;
	s16 WALL_VOL;

	u32 COMB1_L_SRC;
	u32 COMB1_R_SRC;
	u32 COMB2_L_SRC;
	u32 COMB2_R_SRC;
	u32 COMB3_L_SRC;
	u32 COMB3_R_SRC;
	u32 COMB4_L_SRC;
	u32 COMB4_R_SRC;

	s16 COMB1_VOL;
	s16 COMB2_VOL;
	s16 COMB3_VOL;
	s16 COMB4_VOL;

	u32 APF1_L_DST;
	u32 APF1_R_DST;
	u32 APF2_L_DST;
	u32 APF2_R_DST;
} V_Reverb;

#define SPU2_NUM_VOICES 24

typedef struct V_Core
{
	u32 Index; /* Core index identifier. */

	/* Voice Gates -- These are SSE-related values, and must always be */
	/* first to ensure 16 byte alignment */

	V_VoiceGates VoiceGates[SPU2_NUM_VOICES];
	V_CoreGates DryGate;
	V_CoreGates WetGate;

	V_VolumeSlideLR MasterVol; /* Master Volume */
	V_VolumeLR ExtVol; /* Volume for External Data Input */
	V_VolumeLR InpVol; /* Volume for Sound Data Input */
	V_VolumeLR FxVol; /* Volume for Output from Effects */

	SPU2_ALIGN(64) V_Voice Voices[SPU2_NUM_VOICES];

	u32 IRQA; /* Interrupt Address */
	u32 TSA; /* DMA Transfer Start Address */
	u32 ActiveTSA; /* Active DMA TSA - Required for NFL 2k5 which overwrites it mid transfer */

	bool IRQEnable; /* Interrupt Enable */
	bool FxEnable; /* Effect Enable */
	bool Mute; /* Mute */
	bool AdmaInProgress;

	s8 DMABits; /* DMA related? */
	u8 NoiseClk; /* Noise Clock */
	u32 NoiseCnt; /* Noise Counter */
	u32 NoiseOut; /* Noise Output */
	u16 AutoDMACtrl; /* AutoDMA Status */
	s32 DMAICounter; /* DMA Interrupt Counter */
	u32 LastClock; /* DMA Interrupt Clock Cycle Counter */
	u32 InputDataLeft; /* Input Buffer */
	u32 InputDataTransferred; /* Used for simulating MADR increase (GTA VC) */
	u32 InputPosWrite;
	u32 InputDataProgress;

	V_Reverb Revb; /* Reverb Registers */

	s16 RevbDownBuf[2][64 * 2]; /* Downsample buffer for reverb, one for each channel */
	s16 RevbUpBuf[2][64 * 2]; /* Upsample buffer for reverb, one for each channel */
	u32 RevbSampleBufPos;
	u32 EffectsStartA;
	u32 EffectsEndA;

	V_CoreRegs Regs; /* Registers */

	/* Preserves the channel processed last cycle */
	StereoOut32 LastEffect;

	u8 CoreEnabled;

	u8 AttrBit0;
	u8 DmaMode;

	/* new dma only */
	bool DmaStarted;
	u32 AutoDmaFree;

	/* old dma only */
	u16* DMAPtr;
	u16* DMARPtr; /* Mem pointer for DMA Reads */
	u32 ReadSize;
	bool IsDMARead;

	u32 KeyOn;
	u32 KeyOff;

	/* psxmode caches */
	u16 psxSoundDataTransferControl;
	u16 psxSPUSTAT;

} V_Core;

extern V_Core Cores[2];

void        V_Core_Init(V_Core *c, int index);

void        V_Core_WriteRegPS1(V_Core *c, u32 mem, u16 value);
u16         V_Core_ReadRegPS1(V_Core *c, u32 mem);

/* Mixer */
StereoOut32 V_Core_DoReverb(V_Core *c, StereoOut32 Input);
void        V_Voice_DecodeCurrentBlock(V_Voice *vc);

StereoOut32 V_Core_ReadInput(V_Core *c);
StereoOut32 V_Core_ReadInput_HiFi(V_Core *c);

/* DMA */
void        V_Core_DoDMAwrite(V_Core *c, u16 *pMem, u32 size);
void        V_Core_DoDMAread(V_Core *c, u16 *pMem, u32 size);
void        V_Core_FinishDMAread(V_Core *c);

void        V_Core_AutoDMAReadBuffer(V_Core *c, int mode);
void        V_Core_StartADMAWrite(V_Core *c, u16 *pMem, u32 sz);
void        V_Core_PlainDMAWrite(V_Core *c, u16 *pMem, u32 sz);
void        V_Core_FinishDMAwrite(V_Core *c);

static __fi u16 V_Core_DmaRead(V_Core *c)
{
	const u16 ret = (u16)spu2M_Read(c->ActiveTSA);
	++c->ActiveTSA;
	c->ActiveTSA &= 0xfffff;
	c->TSA = c->ActiveTSA;
	return ret;
}

StereoOut32 ReverbUpsample(V_Core *core);
s32         ReverbDownsample(V_Core *core, bool right);

extern bool has_to_call_irq[2];
extern bool has_to_call_irq_dma[2];

/* True while either core has its SPU IRQ enabled. Recomputed once per
 * TimeUpdate batch (IRQEnable is stable across a batch, since register
 * writes are flushed through TimeUpdate before they take effect). When
 * false, the per-address IRQA comparisons scattered through the sample
 * hot path are skipped wholesale: with no core armed none of them could
 * ever set has_to_call_irq, so the result is unchanged. */
extern bool has_irq_armed;

struct SPU2Savestate_DataBlock;

void SPU2Savestate_FreezeIt(struct SPU2Savestate_DataBlock *spud);
s32  SPU2Savestate_ThawIt(struct SPU2Savestate_DataBlock *spud);
s32  SPU2Savestate_SizeIt(void);

/* -------------------------------------------------------------------------------------- */
/*  ADPCM Decoder Cache */
/* -------------------------------------------------------------------------------------- */
/*  the cache data size is determined by taking the number of adpcm blocks */
/*  (2MB / 16) and multiplying it by the decoded block size (28 samples). */
/*  Thus: pcm_cache_data = 7,340,032 bytes (ouch!) */
/*  Expanded: 16 bytes expands to 56 bytes [3.5:1 ratio] */
/*    Resulting in 2MB * 3.5. */

/* The SPU2 has a dynamic memory range which is used for several internal operations, such as */
/* registers, CORE 1/2 mixing, AutoDMAs, and some other fancy stuff.  We exclude this range */
/* from the cache here: */
#define SPU2_DYN_MEMLINE 0x2800

/* 8 short words per encoded PCM block. (as stored in SPU2 ram) */
#define pcm_WordsPerBlock 8

/* number of cachable ADPCM blocks (any blocks above the SPU2_DYN_MEMLINE) */
#define pcm_BlockCount (0x100000 / pcm_WordsPerBlock)

/* 28 samples per decoded PCM block (as stored in our cache) */
#define pcm_DecodedSamplesPerBlock 28

/* An entry is valid while its Generation is the cache's: invalidating one
 * entry sets it to 0, invalidating them all moves the cache's on. */
typedef struct PcmCacheEntry
{
	u32 Generation;
	s16 Sampledata[pcm_DecodedSamplesPerBlock];
	s32 Prev1;
	s32 Prev2;
} PcmCacheEntry;

extern PcmCacheEntry pcm_cache_data[pcm_BlockCount];
extern u32 pcm_cache_generation;

/* Every entry invalid at once, without touching them. */
void pcm_cache_invalidate_all(void);

#ifdef __cplusplus
}
#endif
