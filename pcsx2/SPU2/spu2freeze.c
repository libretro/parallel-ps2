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

#include <stddef.h>
#include <string.h>

#include "Global.h"
#include "spu2.h" /* hopefully temporary, until I resolve lClocks depdendency */
#include "../IopMem.h"

/* Arbitrary ID to identify SPU2 saves. */
#define SPU2_SAVE_ID 0x1227521

/* Incremented whenever the savestate layout changes. 0x0010 carries the
 * per-voice BlockPrev1/2 in what was padding, so it is the same bytes as
 * 0x000f and a 0x000f block still loads -- it just cannot rebuild the
 * block each voice was playing, which is what those bytes are for.
 * 0x0011 carries the output DC filter in the block's tail padding; an
 * older block loads with the filter at rest. */
#define SPU2_SAVE_VERSION       0x0011
#define SPU2_SAVE_VERSION_OLDEST 0x000f

/* What the block has to be aligned to for the members inside it. */
#define SPU2_SAVE_ALIGN 64

struct SPU2Savestate_DataBlock
{
	u32 spu2id;          /* SPU2 state identifier lets ZeroGS/PeopsSPU2 know this isn't their state) */
	u8 unkregs[0x10000]; /* SPU2 raw register memory */
	u8 mem[0x200000];    /* SPU2 raw sample memory */

	u32 version; /* SPU2 version identifier */
	V_Core Cores[2];
	V_SPDIF Spdif;
	u16 OutPos;
	u16 InputPos;
	u32 Cycles;
	u32 lClocks;
	int PlayMode;
	StereoOut32 DCFilterIn;
	StereoOut64 DCFilterOut;
};

/* The filter lives in what was the block's tail padding: the block is the
 * size it was before it, so every state still has the size it had. */
typedef char spu2_dc_filter_in_padding[
	(sizeof(struct SPU2Savestate_DataBlock)
	 == ((offsetof(struct SPU2Savestate_DataBlock, DCFilterIn) + SPU2_SAVE_ALIGN - 1)
	     & ~(size_t)(SPU2_SAVE_ALIGN - 1))) ? 1 : -1];

/* The block carries V_Core, whose Voices[] is 64-byte aligned. */
#ifdef __cplusplus
static_assert(alignof(struct SPU2Savestate_DataBlock) == SPU2_SAVE_ALIGN,
              "SPU2_SAVE_ALIGN no longer matches the block");
#endif

/* The block is the frontend's savestate buffer, which carries no
 * alignment guarantee, and the block embeds V_Core (64-byte aligned). So
 * nothing here touches the block as a struct: every field is copied at
 * its offset, once, straight between the buffer and the emulator. */
#define SPU2_AT(field) offsetof(struct SPU2Savestate_DataBlock, field)
#define SPU2_PUT(b, field, src) memcpy((b) + SPU2_AT(field), (src), sizeof(((struct SPU2Savestate_DataBlock *)0)->field))
#define SPU2_GET(dst, b, field) memcpy((dst), (b) + SPU2_AT(field), sizeof(((struct SPU2Savestate_DataBlock *)0)->field))
/* The bytes between the end of @field and @next, which no field covers. */
#define SPU2_ZERO_GAP(b, field, next) memset((b) + SPU2_AT(field) + sizeof(((struct SPU2Savestate_DataBlock *)0)->field), 0, \
	SPU2_AT(next) - SPU2_AT(field) - sizeof(((struct SPU2Savestate_DataBlock *)0)->field))

/* A DMA pointer as an offset into IOP memory, -1 for none; and back. */
static u16 *spu2_ptr_to_offset(const u16 *p)
{
	return p ? (u16 *)(uintptr_t)((const u8 *)p - &iopMem->Main[0]) : (u16 *)(uintptr_t)-1;
}

static u16 *spu2_offset_to_ptr(const u16 *x)
{
	return (x == (const u16 *)(uintptr_t)-1) ? NULL : (u16 *)(&iopMem->Main[0] + (size_t)(uintptr_t)x);
}

void SPU2Savestate_FreezeIt(struct SPU2Savestate_DataBlock *spud)
{
	u8 *b           = (u8 *)spud;
	const u32 id    = SPU2_SAVE_ID;
	const u32 ver   = SPU2_SAVE_VERSION;
	u32 i;

	SPU2_PUT(b, spu2id, &id);
	SPU2_PUT(b, unkregs, spu2regs);
	SPU2_PUT(b, mem, _spu2mem);
	SPU2_PUT(b, version, &ver);
	SPU2_PUT(b, Cores, Cores);

	/* The DMA pointers go in as offsets, over the host addresses the
	 * copy above put there; each voice's pointer into the block cache,
	 * which a thaw rebuilds, goes in as NULL. A state holds no host
	 * address. */
	for (i = 0; i < 2; i++)
	{
		u8 *core      = b + SPU2_AT(Cores) + i * sizeof(V_Core);
		u16 *dma      = spu2_ptr_to_offset(Cores[i].DMAPtr);
		u16 *dmar     = spu2_ptr_to_offset(Cores[i].DMARPtr);
		s16 *none     = NULL;
		u32 v;
		memcpy(core + offsetof(V_Core, DMAPtr),  &dma,  sizeof(dma));
		memcpy(core + offsetof(V_Core, DMARPtr), &dmar, sizeof(dmar));
		for (v = 0; v < SPU2_NUM_VOICES; v++)
			memcpy(core + offsetof(V_Core, Voices) + v * sizeof(V_Voice)
				+ offsetof(V_Voice, SBuffer), &none, sizeof(none));
	}

	SPU2_PUT(b, Spdif, &Spdif);
	SPU2_PUT(b, OutPos, &OutPos);
	SPU2_PUT(b, InputPos, &InputPos);
	SPU2_PUT(b, Cycles, &Cycles);
	SPU2_PUT(b, lClocks, &lClocks);
	SPU2_PUT(b, PlayMode, &PlayMode);
	SPU2_PUT(b, DCFilterIn, &DCFilterIn);
	SPU2_PUT(b, DCFilterOut, &DCFilterOut);

	/* The block's padding goes out as zeros: the buffer is the
	 * frontend's, and a state is the same bytes whatever it held. */
	SPU2_ZERO_GAP(b, version, Cores);
	SPU2_ZERO_GAP(b, Cores, Spdif);
	SPU2_ZERO_GAP(b, Spdif, OutPos);
	SPU2_ZERO_GAP(b, OutPos, InputPos);
	SPU2_ZERO_GAP(b, InputPos, Cycles);
	SPU2_ZERO_GAP(b, Cycles, lClocks);
	SPU2_ZERO_GAP(b, lClocks, PlayMode);
	SPU2_ZERO_GAP(b, PlayMode, DCFilterIn);
	SPU2_ZERO_GAP(b, DCFilterIn, DCFilterOut);
	memset(b + SPU2_AT(DCFilterOut) + sizeof(DCFilterOut), 0,
		sizeof(struct SPU2Savestate_DataBlock) - SPU2_AT(DCFilterOut) - sizeof(DCFilterOut));

	/* note: Don't save the cache.  PCSX2 doesn't offer a safe method of predicting */
	/* the required size of the savestate prior to saving, plus this is just too */
	/* "implementation specific" for the intended spec of a savestate.  Let's just */
	/* force the user to rebuild their cache instead. */
}

s32 SPU2Savestate_ThawIt(struct SPU2Savestate_DataBlock *spud)
{
	const u8 *b = (const u8 *)spud;
	u32 id;
	u32 version;
	u32 i;
	int c;
	int v;

	SPU2_GET(&id, b, spu2id);
	SPU2_GET(&version, b, version);

	if (id != SPU2_SAVE_ID || version < SPU2_SAVE_VERSION_OLDEST)
	{
		/* Do *not* reset the cores. */
		/* We'll need some "hints" as to how the cores should be initialized, and the */
		/* only way to get that is to use the game's existing core settings and hope */
		/* they kinda match the settings for the savestate (IRQ enables and such). */

		/* adpcm cache : Clear all the cache flags and buffers. */
		memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
		return 0;
	}

	SPU2_GET(spu2regs, b, unkregs);
	SPU2_GET(_spu2mem, b, mem);
	SPU2_GET(Cores, b, Cores);
	for (i = 0; i < 2; i++)
	{
		Cores[i].DMAPtr  = spu2_offset_to_ptr(Cores[i].DMAPtr);
		Cores[i].DMARPtr = spu2_offset_to_ptr(Cores[i].DMARPtr);
	}

	SPU2_GET(&Spdif, b, Spdif);
	SPU2_GET(&OutPos, b, OutPos);
	SPU2_GET(&InputPos, b, InputPos);
	SPU2_GET(&Cycles, b, Cycles);
	SPU2_GET(&lClocks, b, lClocks);
	SPU2_GET(&PlayMode, b, PlayMode);
	if (version >= 0x0011)
	{
		SPU2_GET(&DCFilterIn, b, DCFilterIn);
		SPU2_GET(&DCFilterOut, b, DCFilterOut);
	}
	else
	{
		memset(&DCFilterIn, 0, sizeof(DCFilterIn));
		memset(&DCFilterOut, 0, sizeof(DCFilterOut));
	}

	pcm_cache_invalidate_all();

	/* Point every voice back into the cache, and put back what it
	 * was reading: the cache is not saved, and a voice reads its
	 * current block straight from SBuffer until SCurrent wraps, so
	 * a zeroed entry played the rest of that block as silence. A
	 * block from before the predictor state was kept gets the
	 * silence it always got. */
	for (c = 0; c < 2; c++)
	{
		for (v = 0; v < 24; v++)
		{
			V_Voice *vc = &Cores[c].Voices[v];
			if (version >= 0x0010)
				V_Voice_DecodeCurrentBlock(vc);
			else
			{
				vc->SBuffer = pcm_cache_data[vc->NextA / pcm_WordsPerBlock].Sampledata;
				memset(vc->SBuffer, 0, pcm_DecodedSamplesPerBlock * sizeof(s16));
			}
		}
	}
	return 0;
}

s32 SPU2Savestate_SizeIt(void)
{
	return sizeof(struct SPU2Savestate_DataBlock);
}
