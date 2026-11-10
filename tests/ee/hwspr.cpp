/*  Scratchpad DMA interleave against console captures.
 *
 *  Scores _SPR0interleave and _SPR1interleave in pcsx2/SPR.c against
 *  ps2autotests tests/dma/spr/interleave.expected, the largest capture
 *  outside the cpu tree and one nothing read.
 *
 *  Interleave mode transfers TQWC quadwords, skips SQWC, and repeats
 *  until QWC is exhausted. The capture fills the destination with a
 *  recognisable pattern beforehand -- 0xcccccccc for uploads -- and
 *  prints the whole buffer afterwards, so the untouched gaps are visible
 *  as well as the written runs. That is the whole of what these two
 *  functions decide, and the address arithmetic is what this scores.
 *
 *  The two directions differ in which side moves: SPR0 reads the
 *  scratchpad and writes main memory, SPR1 the reverse, and only one of
 *  the two addresses advances across a skip. Getting that backwards is
 *  the mistake this is here to catch.
 *
 *  SPR.cpp's other dependencies are the DMA scheduling, MFIFO and
 *  savestate paths, which a plain interleave transfer does not reach.
 *  They are stubbed; eeMem and the scratchpad are real arrays, since the
 *  transfer's whole result is what it leaves in them.
 *
 *  Both directions are scored, and only their address arithmetic. Each
 *  direction is mutation-checked on its own function, since they are
 *  separate code: for the uploads, making madr skip by SQWC alone rather
 *  than SQWC plus the transfer fails 29 rows and making sadr skip as well
 *  fails 41; for the downloads the same two changes to _SPR0interleave
 *  fail 48 and 25.
 *
 *  One mutation does not bite. Changing the TQWC == 0 default from qwc to
 *  1 leaves everything passing, because no case in the capture leaves
 *  TQWC zero on its own -- the disabled ones are driven with TQWC and
 *  SQWC both zero, where either default gives the same contiguous copy.
 *
 *  Usage: tests/ee/hwspr <path-to-dma/spr/interleave.expected>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "Config.h"
#include "Hw.h"
#include "VUmicro.h"
#include "MTVU.h"
#include "SPR.h"

u32 emu_hot; /* HotConfig.h: VU1 runs on the EE thread here */

alignas(16) cpuRegisters cpuRegs;
alignas(16) VURegs vuRegs[2];
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");

/* eeMem is a pointer to the EE's memory image; the transfer reads and
 * writes it, so it gets real storage. */
static EEVM_MemoryAllocMess s_eemem;
EEVM_MemoryAllocMess* eeMem = &s_eemem;
alignas(16) u8 eeHw[0x10000];

/* Stubs: the scheduling, interrupt, MFIFO and savestate paths a plain
 * interleave transfer never enters. */
void CPU_INT(EE_EventType, int) { }
void hwDmacIrq(int) { }
bool hwMFIFOWrite(u32, const u128*, uint) { return false; }
void hwMFIFOResume(u32) { }
bool hwDmacSrcChain(DMACh*, int) { return false; }
void hwDmacSrcTadrInc(DMACh*) { }
void _vu0FinishMicro() { }
void vucpu_execute_block(const VUmicroCpu*, int) { }
void SaveState_FreezeMem(SaveStateBase*, void*, int) { }
bool SaveState_FreezeTag(SaveStateBase*, const char*) { return true; }
void mtvu_wait_vu(void) { }
/* Chain-mode tag handling; interleave never reads a tag. */
bool dmach_transfer(DMACh*, tDMA_TAG*) { return true; }
void dmach_unsafe_transfer(DMACh*, const tDMA_TAG*) { }
static VUmicroCpu s_vu0cpu, s_vu1cpu;
const VUmicroCpu* CpuVU0 = &s_vu0cpu;
const VUmicroCpu* CpuVU1 = &s_vu1cpu;
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");


#define QW(n) ((u32)(n) * 4)

/* Run one upload -- main memory into the scratchpad, SPR1 -- with the
 * capture's parameters, and return the scratchpad's contents. */
static void run_upload(u16 skip, u16 transfer, u16 qwc, u32* out, int outqw)
{
	u32* src = (u32*)eeMem->Main;
	/* Main memory is large; only the first pages are touched here. */
	u32* spr = (u32*)eeMem->Scratch;
	int i;

	/* Source pattern, exactly as interleave.cpp builds it: word i is its
	 * own low byte repeated, with the top byte set past word 256 so a
	 * transfer that runs off the end is visible. Filling it per quadword
	 * instead scores 173 of 240 and looks like a transfer-length bug. */
	for (i = 0; i < 0x1000; i++)
	{
		const u32 c = (u32)i & 0xFFu;
		src[i] = c | (c << 8) | (c << 16) | (c << 24);
		if (i > 256) src[i] |= 0xFF000000u;
	}
	/* Destination prefill, so untouched gaps are visible. */
	for (i = 0; i < 0x400; i++) spr[i] = 0xCCCCCCCCu;

	memset(&spr1ch, 0, sizeof(spr1ch));
	spr1ch.qwc  = qwc;
	spr1ch.madr = 0;
	spr1ch.sadr = 0;
	dmacRegs.sqwc.SQWC = skip;
	dmacRegs.sqwc.TQWC = transfer;
	dmacRegs.ctrl.MFD = 0;

	_SPR1interleave();

	for (i = 0; i < outqw * 4 && i < 0x400; i++) out[i] = spr[i];
}

/* The other direction: SPR0 reads the scratchpad and writes main memory,
 * so the skip lands on madr there too but the roles of the two buffers
 * swap. The test prefills main memory with 0xcccccccc and prints it. */
static void run_download(u16 skip, u16 transfer, u16 qwc, u32* out, int outw)
{
	u32* dst = (u32*)eeMem->Main;
	u32* spr = (u32*)eeMem->Scratch;
	int i;

	for (i = 0; i < 0x1000; i++)
	{
		const u32 c = (u32)i & 0xFFu;
		spr[i] = c | (c << 8) | (c << 16) | (c << 24);
		if (i > 256) spr[i] |= 0xFF000000u;
	}
	for (i = 0; i < 0x1000; i++) dst[i] = 0xCCCCCCCCu;

	memset(&spr0ch, 0, sizeof(spr0ch));
	spr0ch.qwc  = qwc;
	spr0ch.madr = 0;
	spr0ch.sadr = 0;
	dmacRegs.sqwc.SQWC = skip;
	dmacRegs.sqwc.TQWC = transfer;
	dmacRegs.ctrl.MFD = 0;

	_SPR0interleave();

	for (i = 0; i < outw && i < 0x1000; i++) out[i] = dst[i];
}

/* tests/dma/dmac/tagintr.expected is not scored here, and the attempt is
 * worth recording. It builds a three-tag source chain -- REF, REF, END --
 * and runs it four times with the channel's TIE bit and the first tag's
 * IRQ bit each on and off. Only when both are set does the transfer stop
 * early: TADR advances 0x10 in that case and 0x20 in the other three.
 * That is one condition, spr1ch.chcr.TIE && ptag->IRQ in _SPR1chain, and
 * it reads correctly.
 *
 * Driving it needs the real hwDmacSrcChain, which is what advances TADR.
 * This harness stubs that function -- the interleave path never calls it
 * -- and with the stub in place all four cases report TADR unmoved, so
 * the test would be measuring the stub. Linking Hw.cpp for the real one
 * pulls in about sixty further symbols, the whole hardware register
 * dispatch, which is disproportionate for four cases and would put most
 * of the emulator behind a test of one boolean.
 */

int main(int argc, char** argv)
{
	const char* path = (argc > 1) ? argv[1] : "interleave.expected";
	FILE* f = fopen(path, "r");
	char buf[512];
	int pass = 0, cases = 0, shown = 0;
	u32 got[0x400];
	int have = 0;
	int active = 0;
	int cur = -1;

	/* The capture's headings are free-form labels, not parameters -- the
	 * real skip, transfer and qwc live in the printTestInterleaveTo calls
	 * in interleave.cpp, and two of the labels disagree with the values
	 * beside them ("skip 0" is passed skip 0 transfer 1, "transfer 1,
	 * skip 14" is passed skip 14). So the parameters are taken from the
	 * source rather than parsed out of the title, matched by title. */
	static const struct { const char* title; u16 skip, transfer, qwc; int enabled; }
	kUp[] = {
		{ "(disabled, count = 3)",            1,   1,   3,   0 },
		{ "(skip 0, count = 3)",              0,   1,   3,   1 },
		{ "(skip 1, count = 3)",              1,   1,   3,   1 },
		{ "(skip 2, count = 3)",              2,   1,   3,   1 },
		{ "(skip 3, count = 3)",              3,   1,   3,   1 },
		{ "(transfer 1, count 3)",            1,   1,   3,   1 },
		{ "(transfer 2, count 4)",            1,   2,   4,   1 },
		{ "(transfer 3, count 6)",            1,   3,   6,   1 },
		{ "(transfer 2, skip 2, count 6)",    2,   2,   6,   1 },
		{ "(transfer 3, skip 3, count 9)",    3,   3,   9,   1 },
		{ "(transfer 1, skip 14, count 2)",  14,   1,   2,   1 },
		{ "(transfer 1, skip 128, count 2)",128,   1,   2,   1 },
		{ "(transfer 1, skip 256, count 2)",256,   1,   2,   1 },
		{ "(transfer 1, skip 257, count 2)",257,   1,   2,   1 },
		{ "(transfer 128, skip 1, count 128)",1, 128, 128,   1 },
	};
	const int nup = (int)(sizeof(kUp)/sizeof(kUp[0]));

	/* The download half, from the printTestInterleaveFrom calls. Same
	 * parameters as the uploads apart from the labels. */
	static const struct { const char* title; u16 skip, transfer, qwc; int enabled; }
	kDown[] = {
		{ "(disabled, count 3)",              1,   1,   3,   0 },
		{ "(skip 0, count 3)",                0,   1,   3,   1 },
		{ "(skip 1, count 3)",                1,   1,   3,   1 },
		{ "(skip 2, count 3)",                2,   1,   3,   1 },
		{ "(skip 3, count 3)",                3,   1,   3,   1 },
		{ "(transfer 1, count 3)",            1,   1,   3,   1 },
		{ "(transfer 2, count 4)",            1,   2,   4,   1 },
		{ "(transfer 3, count 6)",            1,   3,   6,   1 },
		{ "(transfer 2, skip 2, count 6)",    2,   2,   6,   1 },
		{ "(transfer 3, skip 3, count 9)",    3,   3,   9,   1 },
		{ "(transfer 1, skip 14, count 2)",  14,   1,   2,   1 },
		{ "(transfer 1, skip 128, count 2)",128,   1,   2,   1 },
		{ "(transfer 1, skip 256, count 2)",256,   1,   2,   1 },
		{ "(transfer 1, skip 257, count 2)",257,   1,   2,   1 },
	};
	const int ndown = (int)(sizeof(kDown)/sizeof(kDown[0]));
	int is_down = 0;

	if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }

	while (fgets(buf, sizeof(buf), f))
	{
		unsigned idx, w0, w1, w2, w3;
		char* p;

		/* "Interleave upload (enabled, skip = 1, transfer = 2, count = 6):" */
		if (!strncmp(buf, "Interleave upload ", 18)
		 || !strncmp(buf, "Interleave download ", 20))
		{
			const int down = (buf[11] == 'd');
			int i;
			active = 0;
			cur = -1;
			is_down = down;
			if (down)
			{
				for (i = 0; i < ndown; i++)
					if (strstr(buf, kDown[i].title)) { cur = i; break; }
				if (cur >= 0)
				{
					active = 1; have = 0;
					run_download(kDown[cur].enabled ? kDown[cur].skip : 0,
					             kDown[cur].enabled ? kDown[cur].transfer : 0,
					             kDown[cur].qwc, got, 0x400);
				}
			}
			else
			{
				for (i = 0; i < nup; i++)
					if (strstr(buf, kUp[i].title)) { cur = i; break; }
				if (cur >= 0)
				{
					active = 1; have = 0;
					/* A disabled transfer is a plain copy: no skipping, so
					 * the whole count lands contiguously. SPR.cpp models
					 * that as TQWC = 0, which the interleave turns into
					 * qwc. */
					run_upload(kUp[cur].enabled ? kUp[cur].skip : 0,
					           kUp[cur].enabled ? kUp[cur].transfer : 0,
					           kUp[cur].qwc, got, 0x100);
				}
			}
			continue;
		}
		if (!active) continue;

		p = strstr(buf, "): ");
		if (!p) p = strstr(buf, " %02x: ");
		p = strrchr(buf, ':');
		if (!p) continue;
		{
			/* "  Upload <title> 04: 04040404 ..." -- the index is the two
			 * hex digits before the colon. */
			char* q = p;
			while (q > buf && q[-1] != ' ') q--;
			if (sscanf(q, "%x", &idx) != 1) continue;
			if (sscanf(p + 1, " %x %x %x %x", &w0, &w1, &w2, &w3) != 4) continue;
		}
		/* The row label is a word index, not a quadword one: the test
		 * prints buf[i..i+3] stepping i by four, so "04" means words 4
		 * through 7. Scaling it again reads four times too far in and
		 * scores 188 of 240 -- which looks like a short transfer rather
		 * than a misread label. */
		if (idx + 3 >= 0x400) continue;
		(void)is_down;

		cases++;
		if (got[idx + 0] == w0 && got[idx + 1] == w1
		 && got[idx + 2] == w2 && got[idx + 3] == w3) pass++;
		else if (shown++ < 6)
			printf("  skip %u transfer %u qwc %u, row %02x:"
			       " console %08x %08x %08x %08x  ours %08x %08x %08x %08x\n",
			       (unsigned)(is_down ? kDown[cur].skip : kUp[cur].skip),
			       (unsigned)(is_down ? kDown[cur].transfer : kUp[cur].transfer),
			       (unsigned)(is_down ? kDown[cur].qwc : kUp[cur].qwc),
			       idx, w0, w1, w2, w3,
			       got[idx + 0], got[idx + 1], got[idx + 2], got[idx + 3]);
		have++;
	}
	fclose(f);

	if (cases == 0)
	{ printf("hwspr: parsed no cases; the capture's line shape changed\n");
	  return 1; }

	/* tests/dma/spr/normal.expected is about the registers rather than the
	 * transfer, and it is not scored here. Its two claims are that SADR
	 * keeps bits 4 to 13 -- the scratchpad is 16K and the address must be
	 * quadword aligned, so its 0x81234028 reads back as 0x20 -- and that
	 * MADR drops bit 31, which is why its harness reports the transfer
	 * "did not accept MADR: 80000000". Dmac.cpp's write path applies
	 * 0x3FF0 and 0x7FFFFFFF respectively, which is checked by reading it.
	 *
	 * Not scored because it cannot be, cheaply: reaching that path needs
	 * the hardware register file and its dispatch, and a test that
	 * restated the two masks here would be comparing constants I copied
	 * out of the source against constants I copied out of the capture --
	 * arithmetic, not the emulator. An inspection recorded as an
	 * inspection is worth more than a check that cannot fail.
	 */

	printf("hwspr: %d/%d scratchpad interleave rows match the console\n",
	       pass, cases);

	return pass != cases;
}
