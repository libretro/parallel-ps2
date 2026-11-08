/* SPU2 DMA and reverb at their edges, on the real SPU2 units. C89.
 *
 * - An auto-DMA transfer whose length is not whole 0x100-halfword blocks
 *   finishes, in both buffer modes.
 * - DMA in either direction that runs past the end of IOP RAM wraps to its
 *   start, as MADR does, and touches nothing after it.
 * - A reverb work area whose end masks down below its start is silent.
 * - A savestate written to a buffer at any alignment reads back the DMA
 *   pointers and the output filter, and the block is the same bytes
 *   whatever the alignment.
 * - A state thawed and frozen again is the same bytes: it holds no host
 *   address, such as a voice's pointer into the block cache.
 * - Invalidating the whole PCM cache leaves no entry valid, across the
 *   wrap of its generation too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Global.h"
#include "spu2.h"
#include "Dma.h"
#include "R3000A.h"
#include "IopCounters.h"
#include "IopHw.h"
#include "MemoryTypes.h"

/* What the SPU2 expects of the rest of the emulator. */
u8 iopHw[PS2MEM_IOP_HARDWARE];
IopVM_MemoryAllocMess* iopMem;
psxRegisters psxRegs;
psxCounter psxCounters[NUM_COUNTERS];
s32 psxNextDeltaCounter;
u32 psxNextStartCounter;
void psxDmaInterrupt(int n) { (void)n; }
void psxDmaInterrupt2(int n) { (void)n; }
void spu2Irq(void) {}
void FWmixSubboardAudio(s32* l, s32* r) { (void)l; (void)r; }
s16* retro_audio_reserve(int n) { static s16 buf[4096]; (void)n; return buf; }
void retro_audio_commit(int n) { (void)n; }

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); failures++; } } while (0)

static void reset(void)
{
	memset(Cores, 0, sizeof(Cores));
	memset(spu2regs, 0, sizeof(spu2regs));
	memset(_spu2mem, 0, sizeof(_spu2mem));
	memset(&DCFilterIn, 0, sizeof(DCFilterIn));
	memset(&DCFilterOut, 0, sizeof(DCFilterOut));
	memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
	V_Core_Init(&Cores[0], 0);
	V_Core_Init(&Cores[1], 1);
	/* Everything after Main is marked, so a copy that runs off its end
	 * shows up as the mark. */
	memset(iopMem, 0x33, sizeof(*iopMem));
}

static int spu_bytes_are(u32 halfword_addr, u32 bytes, u8 v)
{
	const u8* p = (const u8*)GetMemPtr(halfword_addr);
	u32 i;
	for (i = 0; i < bytes; i++)
		if (p[i] != v)
			return 0;
	return 1;
}

static void check_adma_partial(void)
{
	V_Core* c = &Cores[0];

	reset();
	c->AutoDMACtrl   = 1;
	c->InputPosWrite = 0;
	MADR(c)          = 0x1000;
	V_Core_DoDMAwrite(c, (u16*)&iopMem->Main[0x1000], 0x180);
	CHECK(c->InputDataLeft == 0, "auto-DMA of 1.5 blocks finishes with nothing left");

	reset();
	c->AutoDMACtrl   = 1;
	c->InputPosWrite = 0;
	c->InputDataLeft = 0x180;
	c->DMAPtr        = (u16*)&iopMem->Main[0x1000];
	V_Core_AutoDMAReadBuffer(c, 1);
	CHECK(c->InputDataLeft == 0, "unsplit auto-DMA of less than a block does not wrap the count");
}

static void check_adma_wrap(void)
{
	V_Core* c = &Cores[0];

	reset();
	memset(&iopMem->Main[PS2MEM_IOP_RAM - 0x100], 0x11, 0x100);
	memset(&iopMem->Main[0], 0x22, 0x200);
	c->AutoDMACtrl   = 1;
	c->InputPosWrite = 0;
	c->InputDataLeft = 0x100;
	c->DMAPtr        = (u16*)&iopMem->Main[PS2MEM_IOP_RAM - 0x100];
	V_Core_AutoDMAReadBuffer(c, 0);
	CHECK(spu_bytes_are(0x2000, 0x100, 0x11) && spu_bytes_are(0x2080, 0x100, 0x22),
		"auto-DMA from the end of IOP RAM goes on from its start");
}

/* One auto-DMA block is 0x100 halfwords into one half of the core's input
 * buffer: exactly that much is written, and the other half is untouched. */
static void check_adma_extent(void)
{
	V_Core* c = &Cores[0];

	reset();
	memset(&iopMem->Main[0x1000], 0x55, 0x800);
	c->AutoDMACtrl   = 1;
	c->InputPosWrite = 0;
	c->InputDataLeft = 0x100;
	c->DMAPtr        = (u16*)&iopMem->Main[0x1000];
	V_Core_AutoDMAReadBuffer(c, 0);
	CHECK(spu_bytes_are(0x2000, 0x200, 0x55), "a block of auto-DMA fills its half of the input buffer");
	CHECK(spu_bytes_are(0x2100, 0x600, 0x00), "and writes nothing past it");
	CHECK(c->InputDataProgress == 0x100 && c->InputDataLeft == 0, "and moves on by one block");
}

static void check_plain_wrap(void)
{
	V_Core* c = &Cores[0];
	u32 i;
	int untouched = 1;

	reset();
	memset(&iopMem->Main[PS2MEM_IOP_RAM - 0x100], 0x11, 0x100);
	memset(&iopMem->Main[0], 0x22, 0x400);
	c->TSA = 0x4000;
	V_Core_DoDMAwrite(c, (u16*)&iopMem->Main[PS2MEM_IOP_RAM - 0x100], 0x200);
	CHECK(spu_bytes_are(0x4000, 0x100, 0x11) && spu_bytes_are(0x4080, 0x200, 0x22),
		"DMA from the end of IOP RAM goes on from its start");

	reset();
	memset(GetMemPtr(0x5000), 0x44, 0x400);
	c->TSA = 0x5000;
	V_Core_DoDMAread(c, (u16*)&iopMem->Main[PS2MEM_IOP_RAM - 0x100], 0x200);
	V_Core_FinishDMAread(c);
	for (i = 0; i < sizeof(iopMem->P); i++)
		if (iopMem->P[i] != 0x33)
			untouched = 0;
	CHECK(untouched, "DMA to the end of IOP RAM writes nothing after it");
	CHECK(iopMem->Main[PS2MEM_IOP_RAM - 1] == 0x44 && iopMem->Main[0] == 0x44
		&& iopMem->Main[0x2ff] == 0x44 && iopMem->Main[0x300] == 0x33,
		"and goes on from its start");
}

static void check_reverb(void)
{
	V_Core* c = &Cores[0];
	StereoOut32 in, out;

	reset();
	c->EffectsStartA = 0x00010000;
	c->EffectsEndA   = 0x0040ffff;
	in.Left = in.Right = 0x1000;
	out = V_Core_DoReverb(c, in);
	CHECK(out.Left == 0 && out.Right == 0, "a work area that masks to empty is silent");
}

static void check_freeze(void)
{
	const s32 size = SPU2Savestate_SizeIt();
	u8* buf        = (u8*)malloc((size_t)size * 2 + 64);
	u8* first      = (u8*)malloc((size_t)size);
	int offset;

	if (!buf || !first)
	{
		CHECK(0, "freeze: allocation");
		return;
	}
	for (offset = 0; offset < 64; offset += 7)
	{
		u8* blob = buf + 64 + offset;

		reset();
		memset(buf, 0, (size_t)size * 2 + 64);
		Cores[0].DMAPtr     = (u16*)&iopMem->Main[0x1234];
		Cores[1].DMARPtr    = (u16*)&iopMem->Main[0x5678];
		DCFilterIn.Left     = 1234;
		DCFilterOut.Right   = -5678;
		_spu2mem[0x777]     = 0x2bcd;
		SPU2Savestate_FreezeIt((struct SPU2Savestate_DataBlock*)blob);
		if (offset == 0)
			memcpy(first, blob, (size_t)size);
		else
			CHECK(memcmp(first, blob, (size_t)size) == 0, "freeze: the same bytes at any alignment");

		Cores[0].DMAPtr = Cores[1].DMARPtr = NULL;
		DCFilterIn.Left = DCFilterOut.Right = 0;
		_spu2mem[0x777] = 0;
		CHECK(SPU2Savestate_ThawIt((struct SPU2Savestate_DataBlock*)blob) == 0, "thaw");
		CHECK(Cores[0].DMAPtr == (u16*)&iopMem->Main[0x1234]
			&& Cores[1].DMARPtr == (u16*)&iopMem->Main[0x5678]
			&& Cores[0].DMARPtr == NULL,
			"thaw: the DMA pointers come back into IOP RAM");
		CHECK(DCFilterIn.Left == 1234 && DCFilterOut.Right == -5678, "thaw: the output filter comes back");
		CHECK(_spu2mem[0x777] == 0x2bcd, "thaw: sample RAM comes back");
	}
	free(first);
	free(buf);
}

static void check_freeze_round_trip(void)
{
	const s32 size = SPU2Savestate_SizeIt();
	u8* a          = (u8*)malloc((size_t)size);
	u8* b          = (u8*)malloc((size_t)size);
	int v;

	if (!a || !b)
	{
		CHECK(0, "round trip: allocation");
		free(a);
		free(b);
		return;
	}
	reset();
	for (v = 0; v < SPU2_NUM_VOICES; v++)
		Cores[0].Voices[v].NextA = 0x2800 + v * 8;
	SPU2Savestate_FreezeIt((struct SPU2Savestate_DataBlock*)a);
	CHECK(SPU2Savestate_ThawIt((struct SPU2Savestate_DataBlock*)a) == 0, "round trip: thaw");
	SPU2Savestate_FreezeIt((struct SPU2Savestate_DataBlock*)b);
	CHECK(memcmp(a, b, (size_t)size) == 0, "a thawed state freezes to the same bytes");
	free(a);
	free(b);
}

static void check_cache_generation(void)
{
	PcmCacheEntry* e = &pcm_cache_data[0x4000];

	reset();
	e->Generation = pcm_cache_generation;
	pcm_cache_invalidate_all();
	CHECK(e->Generation != pcm_cache_generation, "an entry valid before is not after");

	pcm_cache_generation = 0xffffffffu;
	e->Generation        = 0xffffffffu;
	pcm_cache_data[7].Generation = 1;
	pcm_cache_invalidate_all();
	CHECK(pcm_cache_generation != 0 && e->Generation != pcm_cache_generation
		&& pcm_cache_data[7].Generation != pcm_cache_generation,
		"nor across the wrap, where every entry is cleared");
}

int main(void)
{
	iopMem = (IopVM_MemoryAllocMess*)malloc(sizeof(*iopMem));
	if (!iopMem)
		return 1;
	check_adma_partial();
	check_adma_wrap();
	check_adma_extent();
	check_plain_wrap();
	check_reverb();
	check_freeze();
	check_freeze_round_trip();
	check_cache_generation();
	free(iopMem);
	printf(failures ? "spu2 dmabounds: FAILED (%d)\n" : "spu2 dmabounds: ok\n", failures);
	return failures != 0;
}
