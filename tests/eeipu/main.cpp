/* The IPU's FIFOs and its two DMA channels (pcsx2/IPU/IPU_Fifo.c,
 * IPUdma.c), driven with seeded transfers in and out.
 *
 * Main memory is filled with quadwords that all read as sane source-chain
 * tags, the channels and the FIFOs are set up at random, and then the DMA
 * kicks, the interrupts, the FIFO registers, the decoder's side of the
 * FIFOs and the bit pointer's reads are called in a random order. Every
 * register, the FIFOs, the IPU's core status, the channel's saved state,
 * every call made and the memory written go into a running hash, written
 * out every so many scenarios. The transcript is compared with
 * expected.txt, which this harness made from the C++ these units were
 * converted from (EEIPU_TREE=<that checkout>): the conversion changes none
 * of it. Transfers stay clear of the end of main RAM and of addresses
 * with nothing behind them, where the C++ copied past the array or
 * through NULL; the C build checks those in fixes(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R5900.h"
#include "Dmac.h"
#include "IPU/IPU.h"
#include "IPU/IPUdma.h"
#include "ps2/HwInternal.h"

#ifdef EEIPU_CXX_TREE
#define FIFO_IN_WRITE(p, n)  ipu_fifo.in.write((p), (n))
#define FIFO_OUT_WRITE(p, n) ipu_fifo.out.write((p), (n))
#define FIFO_CLEAR()         ipu_fifo.clear()
#define FIFO_IN_CLEAR()      ipu_fifo.in.clear()
#define FIFO_INIT()          ipu_fifo.init()
#else
#define FIFO_IN_WRITE(p, n)  ipu_fifo_in_write((p), (n))
#define FIFO_OUT_WRITE(p, n) ipu_fifo_out_write((p), (n))
#define FIFO_CLEAR()         ipu_fifo_clear()
#define FIFO_IN_CLEAR()      ipu_fifo_in_clear()
#define FIFO_INIT()          ipu_fifo_init()
#endif

/* ---- the record ---------------------------------------------------- */

static unsigned long long s_hash = 1469598103934665603ULL;

static void mix(unsigned long long v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		s_hash ^= (v >> (i * 8)) & 0xff;
		s_hash *= 1099511628211ULL;
	}
}

static void mix_bytes(const void* p, size_t n)
{
	const u8* b = (const u8*)p;
	size_t i;
	for (i = 0; i < n; i++)
	{
		s_hash ^= b[i];
		s_hash *= 1099511628211ULL;
	}
}

static u32 s_rng = 0xa54ff53au;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

/* ---- what the units reach ------------------------------------------ */

alignas(16) cpuRegisters cpuRegs;
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
EEVM_MemoryAllocMess* eeMem;
alignas(16) tIPU_BP g_BP;
IPUStatus IPUCoreStatus;

void CPU_INT(EE_EventType n, s32 ecycle) { mix(0x01); mix((u32)n); mix((u32)ecycle); }
void hwDmacIrq(int n) { mix(0x02); mix((u32)n); }
void IPUProcessInterrupt(void) { mix(0x03); }

static const u8* s_load;
void SaveState_FreezeMem(SaveStateBase* s, void* data, int size)
{
	if (s->is_saving)
		mix_bytes(data, (size_t)size);
	else if (s_load)
	{
		memcpy(data, s_load, (size_t)size);
		s_load += size;
	}
}
bool SaveState_FreezeTag(SaveStateBase* s, const char* src) { (void)s; (void)src; return true; }

/* ---- the layout ---------------------------------------------------- */

static const u32 kMainA  = 0x00300000;
static const u32 kMainB  = 0x01c00000;
static const u32 kWindow = 0x10000;
static const u32 kMargin = 0x4000;

static u32 main_addr(void)
{
	return (rnd() & 1 ? kMainA : kMainB) + (rnd_n((kWindow - kMargin) >> 4) << 4);
}

/* Every quadword a sane source tag: a short count, any ID, an address in
 * main memory. */
static void fill_tags(u8* p, u32 bytes)
{
	u32 i, w[4];
	for (i = 0; i < bytes; i += 16)
	{
		w[0] = rnd_n(12) | (rnd_n(8) << 28) | (rnd_n(6) == 0 ? 0x80000000u : 0);
		w[1] = main_addr();
		w[2] = rnd();
		w[3] = rnd();
		memcpy(p + i, w, 16);
	}
}

static void mix_state(void)
{
	SaveStateBase s;
	mix_bytes(&eeHw[0x2000], 0x40);
	mix_bytes(&eeHw[0xb000], 0x500);
	mix_bytes(&eeHw[0xe000], 0x100);
	mix(cpuRegs.dmastall);
	mix(cpuRegs.interrupt);
	mix(cpuRegs.eCycle[4]);
	mix_bytes(&ipu_fifo, sizeof(ipu_fifo));
	mix(g_BP.IFC);
	mix(g_BP.BP);
	mix(g_BP.FP);
	mix_bytes(&IPUCoreStatus, sizeof(IPUCoreStatus));
	memset(&s, 0, sizeof(s));
	s.is_saving = true;
	ipuDmaFreeze(&s);
}

static void random_channel(DMACh* ch, bool chain)
{
	ch->chcr._u32 = (rnd_n(4) == 0 ? 0 : 0x100) | ((chain ? CHAIN_MODE : rnd_n(3)) << 2) | (rnd() & 0x81);
	if (rnd_n(4) == 0)
		ch->chcr._u32 |= (rnd_n(8) << 28) | (rnd() & 0x80000000u);
	ch->qwc  = rnd_n(3) == 0 ? 0 : rnd_n(24);
	ch->madr = main_addr();
	ch->tadr = main_addr();
}

static void reset_world(void)
{
	memset(eeHw, 0, sizeof(eeHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&g_BP, 0, sizeof(g_BP));
	memset(&IPUCoreStatus, 0, sizeof(IPUCoreStatus));
	fill_tags(&eeMem->Main[kMainA], kWindow);
	fill_tags(&eeMem->Main[kMainB], kWindow);
	FIFO_INIT();
	ipuDmaReset();
	random_channel(&ipu0ch, false);
	random_channel(&ipu1ch, rnd_n(2) == 0);
	dmacRegs.ctrl._u32  = rnd() & 0xff;
	dmacRegs.stadr._u32 = main_addr();
	IPUCoreStatus.DataRequested    = rnd() & 1;
	IPUCoreStatus.WaitingOnIPUFrom = rnd() & 1;
	IPUCoreStatus.WaitingOnIPUTo   = rnd() & 1;
	ipuRegs.ctrl.BUSY = rnd() & 1;
	if (rnd_n(3) == 0)
		cpuRegs.eCycle[4] = 0x9999;
}

static void step(void)
{
	alignas(16) u128 q[8];
	u32 i, n;
	int got;

	switch (rnd_n(16))
	{
		case 0: dmaIPU0(); break;
		case 1: dmaIPU1(); break;
		case 2: ipu0Interrupt(); break;
		case 3: ipu1Interrupt(); break;
		case 4: IPU0dma(); break;
		case 5: IPU1dma(); break;
		case 6:
			for (i = 0; i < 4; i++) q[0]._u32[i] = rnd();
			WriteFIFO_IPUin(&q[0]);
			break;
		case 7:
			memset(q, 0, sizeof(q));
			ReadFIFO_IPUout(&q[0]);
			mix_bytes(&q[0], 16);
			break;
		case 8:
		case 9:
			/* The decoder hands over what it made. */
			n = 1 + rnd_n(8);
			for (i = 0; i < n * 4; i++) ((u32*)q)[i] = rnd();
			mix((u32)FIFO_OUT_WRITE((u32*)q, n));
			break;
		case 10:
			/* The bit pointer takes a quadword. */
			memset(q, 0, 16);
			got = ipu_fifo_in_read(&q[0]);
			mix((u32)got);
			mix_bytes(&q[0], 16);
			break;
		case 11:
			n = 1 + rnd_n(8);
			for (i = 0; i < n * 4; i++) ((u32*)q)[i] = rnd();
			mix((u32)FIFO_IN_WRITE((u32*)q, (int)n));
			break;
		case 12:
			if (rnd_n(4) == 0) FIFO_CLEAR();
			else if (rnd_n(3) == 0) FIFO_IN_CLEAR();
			break;
		case 13:
			/* The channel restarted, as the DMAC would. */
			if (!ipu1ch.chcr.STR && rnd_n(2))
			{
				ipu1ch.chcr.STR = 1;
				if (rnd_n(2)) ipu1ch.qwc = rnd_n(12);
				dmaIPU1();
			}
			if (!ipu0ch.chcr.STR && rnd_n(2))
			{
				ipu0ch.chcr.STR = 1;
				ipu0ch.qwc  = rnd_n(12);
				ipu0ch.madr = main_addr();
				dmaIPU0();
			}
			break;
		case 14:
			IPUCoreStatus.DataRequested    = rnd() & 1;
			IPUCoreStatus.WaitingOnIPUFrom = rnd() & 1;
			IPUCoreStatus.WaitingOnIPUTo   = rnd() & 1;
			ipuRegs.ctrl.BUSY = rnd() & 1;
			cpuRegs.interrupt = rnd() & ((1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU) | (1 << IPU_PROCESS));
			break;
		default:
			if (rnd_n(2)) cpuRegs.eCycle[4] = 0x9999;
			else cpuRegs.eCycle[4] = 0;
			break;
	}
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 2000; scen++)
	{
		reset_world();
		mix_state();
		for (i = 0; i < 40; i++)
		{
			step();
			mix_state();
		}
		mix_bytes(&eeMem->Main[kMainA], kWindow);
		mix_bytes(&eeMem->Main[kMainB], kWindow);
		if ((scen + 1) % 50 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	mix_bytes(eeMem->Main, PS2MEM_MAIN_RAM);
	mix_bytes(eeMem->Scratch, PS2MEM_SCRATCH);
	fprintf(out, " all %016llx\n", s_hash);
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EEIPU_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eeipu: %s\n", what);
		s_failed = 1;
	}
}

static void fixes(void)
{
	alignas(16) u128 q[8];
	u32 i;

	/* IPU0 to an address with nothing behind it: a bus error that stops
	 * the channel. The C++ copied the FIFO through NULL. */
	reset_world();
	for (i = 0; i < 32; i++) ((u32*)q)[i] = i;
	FIFO_OUT_WRITE((u32*)q, 4);
	memset(&ipu0ch, 0, sizeof(ipu0ch));
	ipu0ch.chcr._u32 = 0x100;
	ipu0ch.qwc  = 4;
	ipu0ch.madr = 0x12000000;
	cpuRegs.interrupt = 0;
	IPU0dma();
	expect(dmacRegs.stat.BEIS && !ipu0ch.chcr.STR && ipuRegs.ctrl.OFC == 4,
		"IPU0 to a bus error stops the channel and leaves the FIFO");

	/* IPU0 at the last quadword of main RAM: one goes there, the rest
	 * waits for the next address rather than landing in the scratchpad. */
	reset_world();
	memset(eeMem->Scratch, 0x33, 64);
	FIFO_OUT_WRITE((u32*)q, 4);
	memset(&ipu0ch, 0, sizeof(ipu0ch));
	ipu0ch.chcr._u32 = 0x100;
	ipu0ch.qwc  = 4;
	ipu0ch.madr = PS2MEM_MAIN_RAM - 16;
	cpuRegs.interrupt = 0;
	IPU0dma();
	expect(eeMem->Scratch[0] == 0x33 && eeMem->Scratch[63] == 0x33, "IPU0 does not run into the scratchpad");
	expect(ipu0ch.qwc == 3 && ipu0ch.madr == PS2MEM_MAIN_RAM, "IPU0 stops at the end of main RAM");

	/* IPU1 from the last quadword of main RAM takes just that one. */
	reset_world();
	memset(&eeMem->Main[PS2MEM_MAIN_RAM - 16], 0x5a, 16);
	memset(eeMem->Scratch, 0x66, 64);
	memset(&ipu1ch, 0, sizeof(ipu1ch));
	ipu1ch.chcr._u32 = 0x100;
	ipu1ch.qwc  = 4;
	ipu1ch.madr = PS2MEM_MAIN_RAM - 16;
	IPUCoreStatus.DataRequested = true;
	dmaIPU1();
	expect(g_BP.IFC == 1 && ipu1ch.qwc == 3, "IPU1 takes only what is mapped behind MADR");

	/* A counter out of range (from a corrupt state) moves nothing. */
	reset_world();
	g_BP.IFC = 12;
	expect(FIFO_IN_WRITE((u32*)q, 2) == 0, "the input FIFO takes nothing past full");
	ipuRegs.ctrl.OFC = 12;
	expect(FIFO_OUT_WRITE((u32*)q, 2) == 0, "the output FIFO takes nothing past full");
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	if (!eeMem || argc < 2)
		return 2;
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	fclose(out);
#ifndef EEIPU_CXX_TREE
	fixes();
	free(eeMem);
	return s_failed;
#else
	free(eeMem);
	return 0;
#endif
}
