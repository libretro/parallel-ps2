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


#include <string.h>

#include "Global.h"
#include "Profiler.h"
#include "spu2.h"
#include "interpolate_table.h"

static __fi void XA_decode_block(s16* buffer, const s16* block, s32 *prev1, s32 *prev2)
{
	static const s32 tbl_XA_Factor[16][2] =
	{
		{0, 0},
		{60, 0},
		{115, -52},
		{98, -55},
		{122, -60}};
	const s32 header = *block;
	const s32 shift = (header & 0xF) + 16;
	const int id = header >> 4 & 0xF;
	const s32 pred1 = tbl_XA_Factor[id][0];
	const s32 pred2 = tbl_XA_Factor[id][1];

	const s8* blockbytes = (s8*)&block[1];
	const s8* blockend = &blockbytes[13];

	for (; blockbytes <= blockend; ++blockbytes)
	{
		s32 data = (s32)(((u32)(u8)(*blockbytes) & 0x0fu) << 28);
		s32 pcm  = (data >> shift) + (((pred1 * (*prev1)) + (pred2 * (*prev2)) + 32) >> 6);
		s32 pcm2;

		pcm = pcsx2_clamp_i(pcm, -0x8000, 0x7fff);
		*(buffer++) = pcm;

		data = (s32)(((u32)(u8)(*blockbytes) >> 4) << 28);
		pcm2 = (data >> shift) + (((pred1 * pcm) + (pred2 * (*prev1)) + 32) >> 6);

		pcm2 = pcsx2_clamp_i(pcm2, -0x8000, 0x7fff);
		*(buffer++) = pcm2;

		(*prev2) = pcm;
		(*prev1) = pcm2;
	}
}

static __fi void IncrementNextA(V_Voice *vc)
{
	int i;

	/* Important!  Both cores signal IRQ when an address is read, 
	 * regardless of which core actually reads the address. */

	if (has_irq_armed)
	{
		for (i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (vc->NextA == Cores[i].IRQA))
				{ has_to_call_irq[i] = true; }
		}
	}

	vc->NextA++;
	vc->NextA &= 0xFFFFF;
}

/* decoded pcm data, used to cache the decoded data so that it needn't be decoded */
/* multiple times.  Cache chunks are decoded when the mixer requests the blocks, and */
/* invalided when DMA transfers and memory writes are performed. */
PcmCacheEntry pcm_cache_data[pcm_BlockCount];
u32 pcm_cache_generation = 1;

void pcm_cache_invalidate_all(void)
{
	/* Only a wrap reaches the entries: their stamps could then match
	 * the generation again. */
	if (++pcm_cache_generation == 0)
	{
		memset(pcm_cache_data, 0, sizeof(pcm_cache_data));
		pcm_cache_generation = 1;
	}
}

/* LOOP/END sets the ENDX bit and sets NAX to LSA, and the voice is muted if LOOP is not set */
/* LOOP seems to only have any effect on the block with LOOP/END set, where it prevents muting the voice */
/* (the documented requirement that every block in a loop has the LOOP bit set is nonsense according to tests) */
/* LOOP/START sets LSA to NAX unless LSA was written manually since sound generation started */
/* (see LoopMode, the method by which this is achieved on the real SPU2 is unknown) */
#define XAFLAG_LOOP_END (1ul << 0)
#define XAFLAG_LOOP (1ul << 1)
#define XAFLAG_LOOP_START (1ul << 2)

static __fi s32 GetNextDataBuffered(V_Core *thiscore, V_Voice *vc, uint voiceidx)
{
	int i;

	if ((vc->SCurrent & 3) == 0)
	{
		IncrementNextA(vc);

		if ((vc->NextA & 7) == 0) /* vc->SCurrent == 24 equivalent */
		{
			if (vc->LoopFlags & XAFLAG_LOOP_END)
			{
				thiscore->Regs.ENDX |= (1 << voiceidx);
				vc->NextA = vc->LoopStartA | 1;
				if (!(vc->LoopFlags & XAFLAG_LOOP))
				{
					vc->ADSR.Value = 0;
					vc->ADSR.Phase = PHASE_STOPPED;
				}
			}
			else
				vc->NextA++; /* no, don't IncrementNextA here.  We haven't read the header yet. */
		}
	}

	if (vc->SCurrent == 28)
	{
		/* We'll need the loop flags and buffer pointers regardless of cache status: */
		s16 *memptr;
		int cacheIdx;
		PcmCacheEntry *cacheLine;

		vc->SCurrent = 0;

		if (has_irq_armed)
			for (i = 0; i < 2; i++)
				if (Cores[i].IRQEnable && Cores[i].IRQA == (vc->NextA & 0xFFFF8))
					{ has_to_call_irq[i] = true; }

		/* What this block will be decoded from, kept for a thaw. */
		vc->BlockPrev1 = vc->Prev1;
		vc->BlockPrev2 = vc->Prev2;

		memptr = GetMemPtr(vc->NextA & 0xFFFF8);
		vc->LoopFlags = *memptr >> 8; /* grab loop flags from the upper byte. */

		if ((vc->LoopFlags & XAFLAG_LOOP_START) && !vc->LoopMode)
			vc->LoopStartA = vc->NextA & 0xFFFF8;

		cacheIdx  = vc->NextA / pcm_WordsPerBlock;
		cacheLine = &pcm_cache_data[cacheIdx];
		vc->SBuffer = cacheLine->Sampledata;

		if (cacheLine->Generation == pcm_cache_generation
				&& vc->Prev1 == cacheLine->Prev1 && vc->Prev2 == cacheLine->Prev2)
		{
			/* Cached block!  Read from the cache directly. */
			/* Make sure to propagate the prev1/prev2 ADPCM: */

			vc->Prev1 = vc->SBuffer[27];
			vc->Prev2 = vc->SBuffer[26];
		}
		else
		{
			/* Only flag the cache if it's a non-dynamic memory range. */
			if (vc->NextA >= SPU2_DYN_MEMLINE)
			{
				cacheLine->Generation = pcm_cache_generation;
				cacheLine->Prev1 = vc->Prev1;
				cacheLine->Prev2 = vc->Prev2;
			}

			XA_decode_block(vc->SBuffer, memptr, &vc->Prev1, &vc->Prev2);
		}
	}

	return vc->SBuffer[vc->SCurrent++];
}

/* Rebuild the decoded block a voice was in the middle of. The cache is
 * not part of a savestate, so after a thaw SBuffer pointed into a zeroed
 * entry and the remainder of the block came out as silence. NextA stays
 * inside the block while it plays -- the header word is skipped on entry
 * and the next block is only reached when SCurrent wraps -- so it names
 * the block, and BlockPrev1/2 are the predictor state it was decoded
 * from. The entry is left unvalidated: the cache's own Prev1/2 are not
 * restored, so it is not offered to other voices as a hit. */
void V_Voice_DecodeCurrentBlock(V_Voice *vc)
{
	const u32 block = vc->NextA & 0xFFFF8;
	s32 p1 = vc->BlockPrev1;
	s32 p2 = vc->BlockPrev2;

	vc->SBuffer = pcm_cache_data[block / pcm_WordsPerBlock].Sampledata;
	XA_decode_block(vc->SBuffer, GetMemPtr(block), &p1, &p2);
}

static __fi void GetNextDataDummy(V_Core *thiscore, V_Voice *vc, uint voiceidx)
{
	int i;

	IncrementNextA(vc);

	if ((vc->NextA & 7) == 0) /* vc->SCurrent == 24 equivalent */
	{
		if (vc->LoopFlags & XAFLAG_LOOP_END)
		{
			thiscore->Regs.ENDX |= (1 << voiceidx);
			vc->NextA = vc->LoopStartA | 1;
		}
		else
			vc->NextA++; /* no, don't IncrementNextA here.  We haven't read the header yet. */
	}

	if (vc->SCurrent == 28)
	{
		if (has_irq_armed)
			for (i = 0; i < 2; i++)
				if (Cores[i].IRQEnable && Cores[i].IRQA == (vc->NextA & 0xFFFF8))
					{ has_to_call_irq[i] = true; }

		vc->LoopFlags = *GetMemPtr(vc->NextA & 0xFFFF8) >> 8; /* grab loop flags from the upper byte. */

		if ((vc->LoopFlags & XAFLAG_LOOP_START) && !vc->LoopMode)
			vc->LoopStartA = vc->NextA & 0xFFFF8;

		vc->SCurrent = 0;
	}

	vc->SP -= 0x1000 * (4 - (vc->SCurrent & 3));
	vc->SCurrent += 4 - (vc->SCurrent & 3);
}

static __fi void UpdatePitch(V_Voice *vc, uint coreidx, uint voiceidx)
{
	s32 pitch;
	/* [Air] : re-ordered comparisons: Modulated is much more likely to be zero than voice, */
	/*   and so the way it was before it's have to check both voice and modulated values */
	/*   most of the time.  Now it'll just check Modulated and short-circuit past the voice */
	/*   check (not that it amounts to much, but eh every little bit helps). */
	if ((vc->Modulated == 0) || (voiceidx == 0))
		pitch     = vc->Pitch;
	else
		pitch     = pcsx2_clamp_i((vc->Pitch * (32768 + Cores[coreidx].Voices[voiceidx - 1].OutX)) >> 15, 0, 0x3fff);

	pitch     = pcsx2_min_i(pitch, 0x3FFF);
	vc->SP    += pitch;
}

static __fi s32 GetVoiceValues(V_Core *thiscore, V_Voice *vc, uint voiceidx)
{
	s32 mu, pv4, pv3, pv2, pv1, i;

	while (vc->SP >= 0)
	{
		vc->PV4 = vc->PV3;
		vc->PV3 = vc->PV2;
		vc->PV2 = vc->PV1;
		vc->PV1 = GetNextDataBuffered(thiscore, vc, voiceidx);
		vc->SP -= 0x1000;
	}

	mu  = vc->SP + 0x1000;
	pv4 = vc->PV4;
	pv3 = vc->PV3;
	pv2 = vc->PV2;
	pv1 = vc->PV1;
	i   = (mu & 0x0ff0) >> 4;

	return (s32)(
	   ((interpTable[i][0] * pv4) >> 15)
	 + ((interpTable[i][1] * pv3) >> 15)
	 + ((interpTable[i][2] * pv2) >> 15)
	 + ((interpTable[i][3] * pv1) >> 15));
}

/* This is Dr. Hell's noise algorithm as implemented in pcsxr */
/* Supposedly this is 100% accurate */
static __fi void UpdateNoise(V_Core *thiscore)
{
	static const uint8_t noise_add[64] = {
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		1, 0, 0, 1, 0, 1, 1, 0,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1,
		0, 1, 1, 0, 1, 0, 0, 1};

	static const uint16_t noise_freq_add[5] = {
		0, 84, 140, 180, 210};


	u32 level = 0x8000 >> (thiscore->NoiseClk >> 2);
	level <<= 16;

	thiscore->NoiseCnt += 0x10000;

	thiscore->NoiseCnt += noise_freq_add[thiscore->NoiseClk & 3];
	if ((thiscore->NoiseCnt & 0xffff) >= noise_freq_add[4])
	{
		thiscore->NoiseCnt += 0x10000;
		thiscore->NoiseCnt -= noise_freq_add[thiscore->NoiseClk & 3];
	}

	if (thiscore->NoiseCnt >= level)
	{
		while (thiscore->NoiseCnt >= level)
			thiscore->NoiseCnt -= level;

		thiscore->NoiseOut = (thiscore->NoiseOut << 1) | noise_add[(thiscore->NoiseOut >> 10) & 63];
	}
}

/* writes a signed value to the SPU2 RAM
 * Performs no cache invalidation -- use only for dynamic memory ranges
 * of the SPU2 (between 0x0000 and SPU2_DYN_MEMLINE) */
static __fi void spu2M_WriteFast(u32 addr, s16 value)
{
	int i;

	if (has_irq_armed)
	{
		for (i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && Cores[i].IRQA == addr)
				{ has_to_call_irq[i] = true; }
		}
	}
	*GetMemPtr(addr) = value;
}

static void V_VolumeSlide_Update(V_VolumeSlide *vs)
{
	s32 step_size = 7 - vs->Step;
	u32 counter_inc;
	s32 level_inc;

	if (vs->Decr)
		step_size = ~step_size;

	counter_inc = 0x8000 >> pcsx2_max_i(0, vs->Shift - 11);
	/* step_size can be negative; shift in the unsigned domain (same bits). */
	level_inc   = (s32)((u32)step_size << pcsx2_max_i(0, 11 - vs->Shift));

	if (vs->Exp)
	{
		if (vs->Decr)
			level_inc = (s16)((level_inc * vs->Value) >> 15);
		else if (vs->Value > 0x6000)
			counter_inc >>= 2;
	}

	/* Allow counter_inc to be zero only in when all bits */
	/* of the rate field are set */
	if (vs->Step != 3 && vs->Shift != 0x1f)
		counter_inc = pcsx2_max_u(1, counter_inc);
	vs->Counter += counter_inc;

	/* If negative phase "increase" to -0x8000 or "decrease" towards 0 */
	/* Unless in Exp + Decr modes */
	if (!(vs->Exp && vs->Decr) && vs->Phase)
		level_inc = -level_inc;

	if (vs->Counter >= 0x8000)
	{
		vs->Counter = 0;

		if (!vs->Decr)
			vs->Value = pcsx2_clamp_i(vs->Value + level_inc, INT16_MIN, INT16_MAX);
		else
		{
			if (vs->Phase)
			{
				s32 low  = INT16_MIN;
				s32 high = 0;
				if (vs->Exp)
				{
					low  = 0;
					high = INT16_MAX;
				}
				vs->Value = pcsx2_clamp_i(vs->Value + level_inc, low, high);
			}
			else
				vs->Value = pcsx2_clamp_i(vs->Value + level_inc, 0, INT16_MAX);
		}
	}
}

static __fi StereoOut32 MixVoice(V_Core *thiscore, V_Voice *vc, uint coreidx, uint voiceidx)
{
	StereoOut32 voiceOut;
	s32 Value;

	voiceOut.Left  = 0;
	voiceOut.Right = 0;

	/* Stopped voices: advance pitch + NextA for IRQ tracking only, and */
	/* keep the volume slides running. A sweep programmed on a stopped */
	/* voice must still make progress: games fade by starting a VOLL/VOLR */
	/* slide and polling VOLXL/VOLXR (via sceSdGetParam) until it reaches */
	/* the target, and they do this on silent voices too. Freezing the */
	/* slide here left VOLX stuck at its starting value forever, which */
	/* wedged Onimusha 2's stream-handoff fade-wait on the IOP: the file */
	/* server thread never came back, every disc RPC queued behind it, */
	/* and triggered cutscenes hung the game on their first frame. */
	/* Sample decode is still skipped - output is zero regardless. */
	if (vc->ADSR.Phase == PHASE_STOPPED)
	{
		if (vc->Volume.Left.Enable)
			V_VolumeSlide_Update(&vc->Volume.Left);
		if (vc->Volume.Right.Enable)
			V_VolumeSlide_Update(&vc->Volume.Right);

		UpdatePitch(vc, coreidx, voiceidx);
		while (vc->SP >= 0)
			GetNextDataDummy(thiscore, vc, voiceidx);

		if (voiceidx == 1)
			spu2M_WriteFast(((0 == coreidx) ? 0x400 : 0xc00) + OutPos, 0);
		else if (voiceidx == 3)
			spu2M_WriteFast(((0 == coreidx) ? 0x600 : 0xe00) + OutPos, 0);

		return voiceOut;
	}

	/* Active voice path. */

	/* Most games don't use much volume slide effects.  So only call the UpdateVolume */
	/* methods when needed by checking the flag outside the method here... */
	/* (Note: Ys 6 : Ark of Nephistm uses these effects) */

	if (vc->Volume.Left.Enable)
		V_VolumeSlide_Update(&vc->Volume.Left);
	if (vc->Volume.Right.Enable)
		V_VolumeSlide_Update(&vc->Volume.Right);

	/* SPU2 Note: The spu2 continues to process voices for eternity, always, so we */
	/* have to run through all the motions of updating the voice regardless of it's */
	/* audible status.  Otherwise IRQs might not trigger and emulation might fail. */

	UpdatePitch(vc, coreidx, voiceidx);

	if (vc->Noise)
		Value = (s16)thiscore->NoiseOut;
	else
		Value = GetVoiceValues(thiscore, vc, voiceidx);

	/* Update and Apply ADSR  (applies to normal and noise sources) */

	if (vc->ADSR.Phase == PHASE_STOPPED)
		vc->ADSR.Value = 0;
	else if (!ADSR_Calculate(&vc->ADSR))
	{
		vc->ADSR.Value = 0;
		vc->ADSR.Phase = PHASE_STOPPED;
	}
	Value     = (Value * vc->ADSR.Value) >> 15;
	vc->OutX   = Value;

	voiceOut.Left   = (Value * vc->Volume.Left.Value)  >> 15;
	voiceOut.Right  = (Value * vc->Volume.Right.Value) >> 15;

	/* Write-back of raw voice data (post ADSR applied) */
	if (voiceidx == 1)
		spu2M_WriteFast(((0 == coreidx) ? 0x400 : 0xc00) + OutPos, Value);
	else if (voiceidx == 3)
		spu2M_WriteFast(((0 == coreidx) ? 0x600 : 0xe00) + OutPos, Value);

	return voiceOut;
}


static __fi void MixCoreVoices(VoiceMixSet *dest, const uint coreidx)
{
	V_Core *thiscore = &Cores[coreidx];
	uint voiceidx;

	for (voiceidx = 0; voiceidx < SPU2_NUM_VOICES; ++voiceidx)
	{
		/* Prefetch next voice's hot cache lines while processing this one.
		 * Voices are 192-byte aligned (3 cache lines); CL0 = ADSR,
		 * CL1 = pitch/volume. */
		const char *next = (const char *)(&thiscore->Voices[voiceidx + 1]);
		V_Voice *vc;
		StereoOut32 VVal;

		__prefetch_r(next);
		__prefetch_r(next + 64);

		vc   = &thiscore->Voices[voiceidx];
		VVal = MixVoice(thiscore, vc, coreidx, voiceidx);

		/* Note: Results from MixVoice are ranged at 16 bits. */

		dest->Dry.Left  += VVal.Left  & thiscore->VoiceGates[voiceidx].DryL;
		dest->Dry.Right += VVal.Right & thiscore->VoiceGates[voiceidx].DryR;
		dest->Wet.Left  += VVal.Left  & thiscore->VoiceGates[voiceidx].WetL;
		dest->Wet.Right += VVal.Right & thiscore->VoiceGates[voiceidx].WetR;
	}
}


/* ------------------------------------------------------------------ */
/* A VoiceMixSet is Dry.Left, Dry.Right, Wet.Left, Wet.Right -- four
 * adjacent int32 -- so saturating the set is one pair of packed min and
 * max rather than four compare-and-move pairs. The result is the same
 * integers; only the instruction count differs.
 *
 * Unaligned loads and stores: VoiceMixSet has no alignment of its own and
 * lives on the stack and inside V_Core, so nothing guarantees 16 bytes.
 * ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || \
    defined(__i386__) || defined(_M_IX86)
#define SPU2_MIX_X86 1
#include <emmintrin.h>
#if defined(__SSE4_1__) || defined(__AVX__)
#include <smmintrin.h>
#define SPU2_MIX_SSE41 1
#endif
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#define SPU2_MIX_NEON 1
#include <arm_neon.h>
#endif

/* The four accumulators are read as one run of int32, so pin that: a
 * reorder of VoiceMixSet or StereoOut32 would change which value is
 * saturated into which slot, and nothing would fail to compile. */
PCSX2_STATIC_ASSERT(sizeof(StereoOut32) == 8, "StereoOut32 is a packed pair");
PCSX2_STATIC_ASSERT(offsetof(StereoOut32, Left) == 0, "Left is the low lane");
PCSX2_STATIC_ASSERT(offsetof(StereoOut32, Right) == 4, "Right is the high lane");
PCSX2_STATIC_ASSERT(offsetof(VoiceMixSet, Wet) == offsetof(VoiceMixSet, Dry) + 8,
                    "Dry and Wet are one contiguous run of four");

static __fi void ClampMixSet(VoiceMixSet *dst, const VoiceMixSet *src)
{
#if defined(SPU2_MIX_SSE41)
	__m128i v = _mm_loadu_si128((const __m128i*)src);
	v = _mm_min_epi32(_mm_max_epi32(v, _mm_set1_epi32(-0x8000)),
	                  _mm_set1_epi32(0x7fff));
	_mm_storeu_si128((__m128i*)dst, v);
#elif defined(SPU2_MIX_X86)
	/* No packed 32-bit min or max before SSE4.1, so select on the
	 * comparison, which is what the instruction does anyway. */
	__m128i v  = _mm_loadu_si128((const __m128i*)src);
	__m128i lo = _mm_set1_epi32(-0x8000);
	__m128i hi = _mm_set1_epi32(0x7fff);
	__m128i ml = _mm_cmpgt_epi32(lo, v);
	__m128i mh;
	v  = _mm_or_si128(_mm_and_si128(ml, lo), _mm_andnot_si128(ml, v));
	mh = _mm_cmpgt_epi32(v, hi);
	v  = _mm_or_si128(_mm_and_si128(mh, hi), _mm_andnot_si128(mh, v));
	_mm_storeu_si128((__m128i*)dst, v);
#elif defined(SPU2_MIX_NEON)
	int32x4_t v = vld1q_s32((const int32_t*)src);
	v = vminq_s32(vmaxq_s32(v, vdupq_n_s32(-0x8000)), vdupq_n_s32(0x7fff));
	vst1q_s32((int32_t*)dst, v);
#else
	dst->Dry.Left  = pcsx2_clamp_i(src->Dry.Left, -0x8000, 0x7fff);
	dst->Dry.Right = pcsx2_clamp_i(src->Dry.Right, -0x8000, 0x7fff);
	dst->Wet.Left  = pcsx2_clamp_i(src->Wet.Left, -0x8000, 0x7fff);
	dst->Wet.Right = pcsx2_clamp_i(src->Wet.Right, -0x8000, 0x7fff);
#endif
}

static StereoOut32 V_Core_Mix(V_Core *c, const VoiceMixSet *inVoices, const StereoOut32 *Input, const StereoOut32 *Ext)
{
	StereoOut32 TD;
	StereoOut32 TW;
	StereoOut32 RV;
	VoiceMixSet Voices;

	if (c->MasterVol.Left.Enable)
		V_VolumeSlide_Update(&c->MasterVol.Left);
	if (c->MasterVol.Right.Enable)
		V_VolumeSlide_Update(&c->MasterVol.Right);
	UpdateNoise(c);

	/* Saturate final result to standard 16 bit range. */
	ClampMixSet(&Voices, inVoices);

	/* Write Mixed results To Output Area */
	if (c->Index == 0)
	{
		spu2M_WriteFast(0x1000 + OutPos, Voices.Dry.Left);
		spu2M_WriteFast(0x1200 + OutPos, Voices.Dry.Right);
		spu2M_WriteFast(0x1400 + OutPos, Voices.Wet.Left);
		spu2M_WriteFast(0x1600 + OutPos, Voices.Wet.Right);
	}
	else
	{
		spu2M_WriteFast(0x1800 + OutPos, Voices.Dry.Left);
		spu2M_WriteFast(0x1A00 + OutPos, Voices.Dry.Right);
		spu2M_WriteFast(0x1C00 + OutPos, Voices.Wet.Left);
		spu2M_WriteFast(0x1E00 + OutPos, Voices.Wet.Right);
	}

	/* Mix in the Input data */
	TD.Left   = Input->Left & c->DryGate.InpL;
	TD.Right  = Input->Right & c->DryGate.InpR;

	/* Mix in the Voice data */
	TD.Left  += Voices.Dry.Left & c->DryGate.SndL;
	TD.Right += Voices.Dry.Right & c->DryGate.SndR;

	/* Mix in the External (nothing/core0) data */
	TD.Left  += Ext->Left & c->DryGate.ExtL;
	TD.Right += Ext->Right & c->DryGate.ExtR;

	/* ---------------------------------------------------------------------------- */
	/*    Reverberation Effects Processing */
	/* ---------------------------------------------------------------------------- */
	/* SPU2 has an FxEnable bit which seems to disable all reverb processing *and* */
	/* output, but does *not* disable the advancing buffers.  IRQs are not triggered */
	/* and reverb is rendered silent. */
	/* */
	/* Technically we should advance the buffers even when fx are disabled.  However */
	/* there are two things that make this very unlikely to matter: */
	/* */
	/*  1. Any SPU2 app wanting to avoid noise or pops needs to clear the reverb buffers */
	/*     when adjusting settings anyway; so the read/write positions in the reverb */
	/*     buffer after FxEnabled is set back to 1 doesn't really matter. */
	/* */
	/*  2. Writes to ESA (and possibly EEA) reset the buffer pointers to 0. */
	/* */
	/* On the other hand, updating the buffer is cheap and easy, so might as well. ;) */

	/* Mix Input, Voice, and External data: */
	TW.Left  = Input->Left & c->WetGate.InpL;
	TW.Right = Input->Right & c->WetGate.InpR;

	TW.Left  += Voices.Wet.Left & c->WetGate.SndL;
	TW.Right += Voices.Wet.Right & c->WetGate.SndR;
	TW.Left  += Ext->Left & c->WetGate.ExtL;
	TW.Right += Ext->Right & c->WetGate.ExtR;

	RV = V_Core_DoReverb(c, TW);

	/* Mix Dry + Wet
	 * (master volume is applied later to the result of both outputs added together). */
	TD.Left  += (RV.Left  * c->FxVol.Left)  >> 15;
	TD.Right += (RV.Right * c->FxVol.Right) >> 15;
	return TD;
}

void Mix(short *out_left, short *out_right)
{
	StereoOut32 Out;
	StereoOut32 empty;
	StereoOut32 Ext;
	/* Note: Playmode 4 is SPDIF, which overrides other inputs. */
	StereoOut32 InputData[2];
	/* Mixed voice data for each core. */
	VoiceMixSet VoiceData[2];
	const int profile_prev = PROFILE_ZONE_BEGIN(ZONE_SPU2_MIX);
	/* SPDIF is on Core 0:
	 * Fixme:
	 * 1. We do not have an AC3 decoder for the bitstream.
	 * 2. Games usually provide a normal ADMA stream as well and want to see it getting read!
	 */

	empty.Left = empty.Right = 0;
	if (PlayMode & 8)
		InputData[1] = empty;
	else
	{
		const StereoOut32 data = V_Core_ReadInput(&Cores[1]);
		InputData[1].Left  = (data.Left  * Cores[1].InpVol.Left)  >> 15;
		InputData[1].Right = (data.Right * Cores[1].InpVol.Right) >> 15;
	}
	{
		const StereoOut32 data = V_Core_ReadInput(&Cores[0]);
		InputData[0].Left  = (data.Left  * Cores[0].InpVol.Left)  >> 15;
		InputData[0].Right = (data.Right * Cores[0].InpVol.Right) >> 15;
	}

	VoiceData[0].Dry.Left    = 0;
	VoiceData[0].Dry.Right   = 0;
	VoiceData[0].Wet.Left    = 0;
	VoiceData[0].Wet.Right   = 0;
	VoiceData[1].Dry.Left    = 0;
	VoiceData[1].Dry.Right   = 0;
	VoiceData[1].Wet.Left    = 0;
	VoiceData[1].Wet.Right   = 0;
	MixCoreVoices(&VoiceData[0], 0);
	MixCoreVoices(&VoiceData[1], 1);

	Ext = V_Core_Mix(&Cores[0], &VoiceData[0], &InputData[0], &empty);

	if ((PlayMode & 4) || (Cores[0].Mute != 0))
		Ext = empty;
	else
	{
		Ext.Left  = pcsx2_clamp_i(Ext.Left, -0x8000, 0x7fff);
		Ext.Right = pcsx2_clamp_i(Ext.Right, -0x8000, 0x7fff);
		Ext.Left  = (Ext.Left  * Cores[0].MasterVol.Left.Value)  >> 15;
		Ext.Right = (Ext.Right * Cores[0].MasterVol.Right.Value) >> 15;
	}

	/* Commit Core 0 output to ram before mixing Core 1: */
	spu2M_WriteFast(0x800 + OutPos, Ext.Left);
	spu2M_WriteFast(0xA00 + OutPos, Ext.Right);

	Ext.Left  = (Ext.Left  * Cores[1].ExtVol.Left)  >> 15;
	Ext.Right = (Ext.Right * Cores[1].ExtVol.Right) >> 15;
	Out       = V_Core_Mix(&Cores[1], &VoiceData[1], &InputData[1], &Ext);

	/* Experimental CDDA support
	 * The CDDA overrides all other mixer output.  It's a direct feed */
	if (PlayMode & 8)
		Out       = V_Core_ReadInput_HiFi(&Cores[1]);
	else
	{
		Out.Left  = pcsx2_clamp_i(Out.Left, -0x8000, 0x7fff);
		Out.Right = pcsx2_clamp_i(Out.Right, -0x8000, 0x7fff);
		Out.Left  = (Out.Left  * Cores[1].MasterVol.Left.Value)  >> 15;
		Out.Right = (Out.Right * Cores[1].MasterVol.Right.Value) >> 15;
	}

	/* A simple DC blocking high-pass filter, y[n] = x[n] - x[n-1] + a*y[n-1].
	 * Implementation from http://peabody.sapp.org/class/dmp2/lab/dcblock/
	 * The magic number 0x7f5c is ceil(INT16_MAX * 0.995).
	 *
	 * This filter is the emulator's own -- the SPU2 has nothing like it --
	 * so its precision is ours to pick, and it is carried at 1/65536 of an
	 * output step. A pole at 0.995 has a loop gain of about 200, which is
	 * what the state's own rounding error gets multiplied by before it
	 * reaches the output, so the state has to be finer than the thing it
	 * feeds. The 64-bit product also keeps the multiply in range across the
	 * whole travel of the numerator. */
	DCFilterOut.Left  = ((DCFilterOut.Left  * 0x7f5c) >> 15)
	                  + (s64)(Out.Left  - DCFilterIn.Left)  * 65536;
	DCFilterOut.Right = ((DCFilterOut.Right * 0x7f5c) >> 15)
	                  + (s64)(Out.Right - DCFilterIn.Right) * 65536;
	DCFilterIn.Left   = Out.Left;
	DCFilterIn.Right  = Out.Right;

	/* Round back to an output step, then clamp: 16 bits from here on. */
	*out_left         = (int16_t)pcsx2_clamp_i((s32)((DCFilterOut.Left  + 0x8000) >> 16), -0x8000, 0x7fff);
	*out_right        = (int16_t)pcsx2_clamp_i((s32)((DCFilterOut.Right + 0x8000) >> 16), -0x8000, 0x7fff);

	/* Update AutoDMA output positioning */
	OutPos++;
	if (OutPos >= 0x200)
		OutPos = 0;

	PROFILE_ZONE_END(ZONE_SPU2_MIX, profile_prev);
}
