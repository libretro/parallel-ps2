/* The SIF DMA channels (pcsx2/Sif0.c, Sif1.c, sif2.c) and the EE DMAC
 * channel helpers they run on (DmacChannel.c), driven with seeded
 * transfers in both directions.
 *
 * Each scenario lays a chain out in IOP or EE memory, starts the two ends
 * of the channel and then pokes them in a random order: kicks, interrupt
 * deliveries, restarts, register changes. Every register and fifo the
 * channel owns, every call it makes outward and the memory it writes go
 * into a running hash, written out every so many scenarios. The
 * transcript is compared with expected.txt, which this harness made from
 * the C++ these units were converted from (IOPSIF_TREE=<that checkout>):
 * the conversion changes none of it. Addresses stay clear of the ends of
 * the memories, where the C++ ran a copy past the array it was in; the C
 * build checks those separately, in fixes(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R3000A.h"
#include "R5900.h"
#include "Dmac.h"
#include "IopMem.h"
#include "IopHw.h"
#include "IopDma.h"
#include "Sif.h"

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

static u32 s_rng = 0x6a09e667u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

/* ---- what the units reach ------------------------------------------ */

cpuRegisters cpuRegs;
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
alignas(4096) u8 iopHw[PS2MEM_IOP_HARDWARE];
EEVM_MemoryAllocMess* eeMem;
IopVM_MemoryAllocMess* iopMem;

static void cpu_clear(u32 addr, u32 size) { mix(0x01); mix(addr); mix(size); }
static R3000Acpu s_cpu = { NULL, NULL, NULL, cpu_clear, NULL };
R3000Acpu* psxCpu = &s_cpu;

/* What the units asked to be told about later; the driver delivers it. */
static int s_ee_pending[3];
static int s_iop_pending[3];

void CPU_INT(EE_EventType n, s32 ecycle)
{
	mix(0x02); mix((u32)n); mix((u32)ecycle);
	if (n == DMAC_SIF0) s_ee_pending[0] = 1;
	if (n == DMAC_SIF1) s_ee_pending[1] = 1;
	if (n == DMAC_SIF2) s_ee_pending[2] = 1;
}

void PSX_INT(IopEventId n, s32 ecycle)
{
	mix(0x03); mix((u32)n); mix((u32)ecycle);
	if (n == IopEvt_SIF0) s_iop_pending[0] = 1;
	if (n == IopEvt_SIF1) s_iop_pending[1] = 1;
	if (n == IopEvt_SIF2) s_iop_pending[2] = 1;
}

void hwDmacIrq(int n) { mix(0x04); mix((u32)n); }
void psxDmaInterrupt2(int n) { mix(0x05); mix((u32)n); }

/* Sif.c's savestate entry point; only fixes() runs it. */
static const u8* s_load;
void SaveState_FreezeMem(SaveStateBase* s, void* data, int size)
{
	if (!s->is_saving && s_load)
	{
		memcpy(data, s_load, (size_t)size);
		s_load += size;
	}
}
bool SaveState_FreezeTag(SaveStateBase* s, const char* src) { (void)s; (void)src; return true; }

/* ---- the layout ---------------------------------------------------- */

/* EE windows, away from every end: two in main RAM (one through a kseg
 * mirror), the scratchpad through the SPR bit and through its secret
 * physical alias, the zero sink, and a bus error. */
static const u32 kEeMainA  = 0x00100000;
static const u32 kEeMainB  = 0x01f00000;
static const u32 kEeWindow = 0x8000;

static u32 ee_dest(void)
{
	switch (rnd_n(16))
	{
		case 0: return 0x80000000u | (rnd_n(0x100) << 4);           /* SPR */
		case 1: return 0x10002000u | (rnd_n(0x80) << 4);            /* scratch alias */
		case 2: return 0x08000000u | (rnd_n(0x1000) << 4);          /* the zero sink */
		case 3: return rnd_n(4) == 0 ? 0x10008000u : kEeMainA;      /* bus error, now and then */
		case 4: return 0x20000000u | (kEeMainB + (rnd_n(0x400) << 4)); /* mirror */
		default:
			return (rnd() & 1 ? kEeMainA : kEeMainB) + (rnd_n(kEeWindow >> 4) << 4);
	}
}

/* IOP windows: SIF0 and SIF2 read from the first, SIF1 writes the
 * second, the tags live in the third. */
static const u32 kIopSrc  = 0x00010000;
static const u32 kIopDst  = 0x00040000;
static const u32 kIopTags = 0x00001000;
static const u32 kIopWin  = 0x10000;

static u32 safe_word(void)
{
	/* Either side can take one as an address and the next as a count:
	 * keep both small, so no run reaches the top of IOP RAM, and never
	 * through the SPR bit, where a long run would reach the end of the
	 * scratchpad. Those are where the C++ ran over; fixes() covers them. */
	return rnd() & 0x4000fffcu;
}

static void fill(u8* p, size_t n)
{
	size_t i;
	for (i = 0; i + 4 <= n; i += 4)
	{
		const u32 v = safe_word();
		memcpy(p + i, &v, 4);
	}
}

static void put32(u8* base, u32 off, u32 v) { memcpy(base + off, &v, 4); }

static u32 ee_off(u32 addr)
{
	return addr & 0x1ffffff0u;
}

static void reset_world(void)
{
	memset(&sif0, 0, sizeof(sif0));
	memset(&sif1, 0, sizeof(sif1));
	memset(&sif2, 0, sizeof(sif2));
	memset(eeHw, 0, sizeof(eeHw));
	memset(iopHw, 0, sizeof(iopHw));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(s_ee_pending, 0, sizeof(s_ee_pending));
	memset(s_iop_pending, 0, sizeof(s_iop_pending));
	fill(&eeMem->Main[kEeMainA], kEeWindow + 0x2000);
	fill(&eeMem->Main[kEeMainB], kEeWindow + 0x2000);
	fill(eeMem->Scratch, PS2MEM_SCRATCH);
	fill(&iopMem->Main[kIopSrc], kIopWin);
	fill(&iopMem->Main[kIopDst], kIopWin);
	fill(&iopMem->Main[kIopTags], 0x8000);
	memset(eeMem->ZeroWrite, 0, 0x1000);
}

static void random_fifo(sifFifo* f, bool qw)
{
	if (rnd_n(3) != 0)
		return;
	f->readPos  = (s32)rnd_n(FIFO_SIF_W);
	f->size     = (s32)rnd_n(FIFO_SIF_W / 4 + 1) * 4;
	if (!qw && rnd_n(2))
		f->size = (s32)rnd_n(FIFO_SIF_W + 1);
	if (qw)
		f->readPos &= ~3;
	f->writePos = (f->readPos + f->size) & (FIFO_SIF_W - 1);
	fill((u8*)f->data, sizeof(f->data));
}

static void mix_state(void)
{
	mix_bytes(&sif0, sizeof(sif0));
	mix_bytes(&sif1, sizeof(sif1));
	mix_bytes(&sif2, sizeof(sif2));
	mix_bytes(&eeHw[0xc000], 0x900);
	mix_bytes(&eeHw[0xe000], 0x100);
	mix_bytes(&eeHw[0xf200], 0x200);
	mix_bytes(&iopHw[0x10a0], 0x10);
	mix_bytes(&iopHw[0x1520], 0x20);
	mix_bytes(&iopHw[0xf300], 0x10);
	mix(cpuRegs.dmastall);
}

static void mix_memory(void)
{
	mix_bytes(&eeMem->Main[kEeMainA], kEeWindow + 0x2000);
	mix_bytes(&eeMem->Main[kEeMainB], kEeWindow + 0x2000);
	mix_bytes(eeMem->Scratch, PS2MEM_SCRATCH);
	mix_bytes(eeMem->ZeroWrite, 0x1000);
	mix_bytes(&iopMem->Main[kIopDst], kIopWin);
}

static void random_dmac(void)
{
	dmacRegs.ctrl._u32  = rnd() & 0xff;
	dmacRegs.stadr._u32 = rnd() & 0x01fffff0u;
	if (rnd_n(4) == 0)
		dmacRegs.stadr._u32 = 0x01ffffff;
}

/* ---- SIF0: IOP to EE ----------------------------------------------- */

static void lay_sif0(void)
{
	const int n = 1 + (int)rnd_n(5);
	u32 t = kIopTags + (rnd_n(0x400) << 4);
	int i;

	HW_DMA9_TADR = t;
	for (i = 0; i < n; i++, t += 16)
	{
		const u32 words = 1 + rnd_n(64);
		u32 data = kIopSrc + (rnd_n(0x3000) << 2);
		u32 ee   = (((words + 3) >> 2) + rnd_n(3) - 1) & 0xffff;
		static const u32 ids[] = { TAG_CNT, TAG_CNT, TAG_CNTS, TAG_END, 2, 3 };
		if (i == n - 1 || rnd_n(6) == 0)
			data |= 0x40000000u; /* (bit 31 would read as an SPR address) */
		ee |= ids[rnd_n(6)] << 28;
		if (rnd_n(5) == 0)
			ee |= 0x80000000u;
		put32(iopMem->Main, t, data);
		put32(iopMem->Main, t + 4, words);
		put32(iopMem->Main, t + 8, ee);
		put32(iopMem->Main, t + 12, ee_dest());
	}

	sif0ch.chcr._u32 = 0x100 | (rnd_n(4) == 0 ? NORMAL_MODE : CHAIN_MODE << 2);
	if (rnd_n(2)) sif0ch.chcr.TIE = 1;
	if (sif0ch.chcr.MOD == NORMAL_MODE)
	{
		sif0ch.qwc  = rnd_n(40);
		sif0ch.madr = ee_dest();
	}
	random_dmac();
	random_fifo(&sif0.fifo, true);
}

static void step_sif0(void)
{
	switch (rnd_n(8))
	{
		case 0: dmaSIF0(); break;
		case 1:
			sif0.iop.busy = true;
			sif0.iop.end  = false;
			SIF0Dma();
			break;
		case 2: SIF0Dma(); break;
		case 3:
			if (s_ee_pending[0]) { s_ee_pending[0] = 0; EEsif0Interrupt(); }
			break;
		case 4:
			if (s_iop_pending[0]) { s_iop_pending[0] = 0; sif0Interrupt(); }
			break;
		case 5:
			if (!sif0ch.chcr.STR && rnd_n(2))
			{
				sif0ch.chcr.STR = 1;
				dmaSIF0();
			}
			break;
		case 6:
			if (rnd_n(4) == 0) sif0ch.qwc = rnd_n(8);
			break;
		default:
			if (rnd_n(4) == 0) random_dmac();
			break;
	}
}

/* ---- SIF1: EE to IOP ----------------------------------------------- */

/* The stream the IOP reads: [its tag][payload], back to back. */
static u32 make_stream(u32* out, u32 cap_qw)
{
	u32 qw = 0;
	while (qw + 2 < cap_qw)
	{
		const u32 words = (1 + rnd_n(16)) * 4;
		u32 data = kIopDst + (rnd_n(0x3000) << 2);
		u32 i;
		if (qw + 1 + words / 4 > cap_qw)
			break;
		if (rnd_n(4) == 0)
			data |= rnd() & 1 ? 0x80000000u : 0x40000000u;
		out[qw * 4 + 0] = data;
		out[qw * 4 + 1] = words | (rnd_n(4) == 0 ? rnd_n(4) : 0);
		out[qw * 4 + 2] = safe_word();
		out[qw * 4 + 3] = safe_word();
		qw++;
		for (i = 0; i < words; i++)
			out[qw * 4 + i] = safe_word();
		qw += words / 4;
		if (rnd_n(3) == 0)
			break;
	}
	return qw;
}

static void lay_sif1(void)
{
	u32 stream[4 * 400];
	const u32 total = make_stream(stream, 400);
	u32 tag  = kEeMainA + (rnd_n(0x100) << 4);
	u32 data = kEeMainB + (rnd_n(0x100) << 4);
	u32 next = kEeMainA + 0x4000 + (rnd_n(0x80) << 4);
	u32 done = 0;

	sif1ch.tadr = tag;
	while (done < total)
	{
		u32 q = 1 + rnd_n(12);
		u32 id, w0;
		const bool last = done + q >= total;
		if (last) q = total - done;
		id = rnd_n(6);
		/* CNT/NEXT/END carry their data after the tag, REF/REFS/REFE
		 * point at it; END and REFE stop the chain. */
		static const u32 mid[]  = { TAG_CNT, TAG_NEXT, TAG_REF, TAG_REFS, TAG_CNT, TAG_REF };
		id = last ? (rnd() & 1 ? TAG_END : TAG_REFE) : mid[id];
		w0 = q | (id << 28) | (rnd_n(8) == 0 ? 0x80000000u : 0);
		put32(eeMem->Main, ee_off(tag), w0);
		put32(eeMem->Main, ee_off(tag) + 8, safe_word());
		put32(eeMem->Main, ee_off(tag) + 12, safe_word());
		if (id == TAG_CNT || id == TAG_NEXT || id == TAG_END)
		{
			memcpy(&eeMem->Main[ee_off(tag) + 16], &stream[done * 4], q * 16);
			if (id == TAG_NEXT)
			{
				put32(eeMem->Main, ee_off(tag) + 4, next);
				tag  = next;
				next += 0x400;
			}
			else
			{
				put32(eeMem->Main, ee_off(tag) + 4, safe_word());
				tag += 16 + q * 16;
			}
		}
		else
		{
			memcpy(&eeMem->Main[ee_off(data)], &stream[done * 4], q * 16);
			put32(eeMem->Main, ee_off(tag) + 4, data);
			data += q * 16;
			tag  += 16;
		}
		done += q;
	}

	sif1ch.chcr._u32 = 0x101 | (rnd_n(5) == 0 ? NORMAL_MODE : CHAIN_MODE << 2);
	if (rnd_n(2)) sif1ch.chcr.TIE = 1;
	if (sif1ch.chcr.MOD == NORMAL_MODE || rnd_n(6) == 0)
	{
		sif1ch.qwc  = rnd_n(30);
		sif1ch.madr = kEeMainB + (rnd_n(0x100) << 4);
	}
	random_dmac();
	if (rnd_n(3) == 0)
		dmacRegs.ctrl.STD = STD_SIF1;
	random_fifo(&sif1.fifo, true);
}

static void step_sif1(void)
{
	switch (rnd_n(8))
	{
		case 0: dmaSIF1(); break;
		case 1:
			sif1.iop.busy = true;
			sif1.iop.end  = false;
			SIF1Dma();
			break;
		case 2: SIF1Dma(); break;
		case 3:
			if (s_ee_pending[1]) { s_ee_pending[1] = 0; EEsif1Interrupt(); }
			break;
		case 4:
			if (s_iop_pending[1]) { s_iop_pending[1] = 0; sif1Interrupt(); }
			break;
		case 5:
			/* Lift a stall. */
			dmacRegs.stadr._u32 = 0x01fffff0u;
			SIF1Dma();
			break;
		case 6:
			if (!sif1ch.chcr.STR && rnd_n(2))
			{
				sif1ch.chcr.STR = 1;
				dmaSIF1();
			}
			break;
		default:
			if (rnd_n(4) == 0) random_dmac();
			break;
	}
}

/* ---- SIF2: IOP to EE, PS1 mode ------------------------------------- */

static void lay_sif2(void)
{
	/* The EE reads its tags out of the same words the IOP sends. */
	u32 a = kIopSrc + (rnd_n(0x2000) << 4);
	u32 i;

	HW_DMA2_MADR = a;
	for (i = 0; i < 24; i++, a += 16)
	{
		static const u32 ids[] = { TAG_CNT, TAG_CNTS, TAG_END, TAG_CNT };
		u32 w0 = (rnd_n(4)) | (ids[rnd_n(4)] << 28) | (rnd_n(6) == 0 ? 0x80000000u : 0);
		put32(iopMem->Main, a, w0);
		put32(iopMem->Main, a + 4, ee_dest());
	}
	psxHu16(0x10a4) = (u16)rnd_n(9);
	psxHu16(0x10a6) = (u16)rnd_n(9);
	sif2data = (s32)safe_word();

	sif2dma.chcr._u32 = 0x100 | (rnd_n(4) == 0 ? NORMAL_MODE : CHAIN_MODE << 2);
	if (rnd_n(2)) sif2dma.chcr.TIE = 1;
	if (sif2dma.chcr.MOD == NORMAL_MODE)
	{
		sif2dma.qwc  = rnd_n(20);
		sif2dma.madr = ee_dest();
	}
	random_fifo(&sif2.fifo, false);
}

static void step_sif2(void)
{
	switch (rnd_n(7))
	{
		case 0: dmaSIF2(); break;
		case 1:
			sif2.iop.busy = true;
			sif2.iop.end  = false;
			sif2Interrupt();
			break;
		case 2: sif2Interrupt(); break;
		case 3:
			if (sif2.fifo.size > 0)
				ReadFifoSingleWord();
			break;
		case 4:
			if (s_iop_pending[2]) { s_iop_pending[2] = 0; sif2Interrupt(); }
			break;
		case 5:
			if (!sif2dma.chcr.STR && rnd_n(2))
			{
				sif2dma.chcr.STR = 1;
				dmaSIF2();
			}
			break;
		default:
			psxHu16(0x10a4) = (u16)rnd_n(9);
			break;
	}
}

/* ---- the tag walks on their own ------------------------------------ */

/* The source-chain walks, with and without the address stack, on random
 * channels and every tag ID: the SIF only reaches some of them. */
static void walks(FILE* out)
{
	DMACh ch;
	int i;

	for (i = 0; i < 20000; i++)
	{
		const int id = (int)rnd_n(8);
		memset(&ch, 0, sizeof(ch));
		ch.chcr._u32 = rnd();
		ch.madr      = rnd();
		ch.tadr      = rnd();
		ch.qwc       = rnd() & 0xffff;
		ch.asr0      = rnd();
		ch.asr1      = rnd();
		switch (i % 3)
		{
			case 0: mix(hwDmacSrcChainWithStack(&ch, id)); break;
			case 1: mix(hwDmacSrcChain(&ch, id)); break;
			default: hwDmacSrcTadrInc(&ch); break;
		}
		mix_bytes(&ch, sizeof(ch));
	}
	fprintf(out, "walks %016llx\n", s_hash);
}

/* ---- the run ------------------------------------------------------- */

static void run(FILE* out)
{
	int chan, scen, step;
	static const char* names[] = { "sif0", "sif1", "sif2" };

	for (chan = 0; chan < 3; chan++)
	{
		for (scen = 0; scen < 1200; scen++)
		{
			reset_world();
			if (chan == 0) lay_sif0();
			if (chan == 1) lay_sif1();
			if (chan == 2) lay_sif2();
			mix_state();
			for (step = 0; step < 40; step++)
			{
				if (chan == 0) step_sif0();
				if (chan == 1) step_sif1();
				if (chan == 2) step_sif2();
				mix_state();
			}
			mix_memory();
			if ((scen + 1) % 40 == 0)
				fprintf(out, "%s %4d %016llx\n", names[chan], scen + 1, s_hash);
		}
		/* Whatever a channel wrote outside the windows, too. */
		mix_bytes(eeMem->Main, PS2MEM_MAIN_RAM);
		mix_bytes(eeMem->Scratch, PS2MEM_SCRATCH);
		mix_bytes(iopMem->Main, PS2MEM_IOP_RAM);
		mix_bytes(iopMem->P, sizeof(iopMem->P));
		fprintf(out, "%s  all %016llx\n", names[chan], s_hash);
	}
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef IOPSIF_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "iopsif: %s\n", what);
		s_failed = 1;
	}
}

static void fixes(void)
{
	u32 got[8];
	u32 i;

	/* SIF1 writing across the top of IOP RAM wraps to the bottom; the
	 * C++ wrote on into the memory after it. */
	reset_world();
	memset(iopMem->P, 0x5a, 64);
	for (i = 0; i < 8; i++)
		sif1.fifo.data[i] = 0x1000 + i;
	sif1.fifo.size     = 8;
	sif1.fifo.writePos = 8;
	sif1.iop.counter   = 8;
	sif1.iop.busy      = true;
	HW_DMA10_MADR      = 0x1ffff0;
	SIF1Dma();
	memcpy(got, &iopMem->Main[0x1ffff0], 16);
	memcpy(got + 4, &iopMem->Main[0], 16);
	for (i = 0; i < 8; i++)
		expect(got[i] == 0x1000 + i, "SIF1 wraps at the top of IOP RAM");
	expect(iopMem->P[0] == 0x5a && iopMem->P[63] == 0x5a, "SIF1 stays inside IOP RAM");

	/* SIF0's IOP tag, the EE tag behind it and its payload all wrap the
	 * same way: the tag sits in the last 8 bytes, the EE tag in the first
	 * 16, and the payload starts in the last word. */
	reset_world();
	put32(iopMem->Main, 0x1ffff8, 0x80000000u | 0x1ffffc); /* data: end, at the top */
	put32(iopMem->Main, 0x1ffffc, 2);                       /* words */
	put32(iopMem->Main, 0x000000, 0x70000001u);             /* EE tag: END, 1 QW */
	put32(iopMem->Main, 0x000004, kEeMainA);
	put32(iopMem->Main, 0x000008, 0x88);
	put32(iopMem->Main, 0x00000c, 0x99);
	HW_DMA9_TADR  = 0x1ffff8;
	sif0.iop.busy = true;
	SIF0Dma();
	/* (The end of the packet clears the tag's data word again.) */
	expect(sif0.iop.data.words == 2 && HW_DMA9_MADR == 0x200004 && HW_DMA9_TADR == 0x200008,
		"SIF0 reads an IOP tag across the top of IOP RAM");
	expect(sif0.fifo.data[0] == 0x70000001u && sif0.fifo.data[1] == kEeMainA
		&& sif0.fifo.data[2] == 0x88 && sif0.fifo.data[3] == 0x99,
		"SIF0 sends the EE tag from across the top of IOP RAM");
	expect(sif0.fifo.data[4] == 2 && sif0.fifo.data[5] == 0x70000001u,
		"SIF0 reads its payload across the top of IOP RAM");

	/* An EE write running off the end of main RAM goes where the next
	 * address goes (the zero sink), not into the scratchpad behind it. */
	reset_world();
	memset(eeMem->Scratch, 0x33, 32);
	for (i = 0; i < 8; i++)
		sif0.fifo.data[i] = 0x3000 + i;
	sif0.fifo.size     = 8;
	sif0.fifo.writePos = 8;
	sif0ch.chcr._u32   = 0x100;
	sif0ch.qwc         = 2;
	sif0ch.madr        = 0x01fffff0;
	sif0.ee.busy       = true;
	SIF0Dma();
	memcpy(got, &eeMem->Main[0x01fffff0], 16);
	expect(got[0] == 0x3000 && got[3] == 0x3003, "SIF0 writes the last QW of main RAM");
	expect(eeMem->Scratch[0] == 0x33 && eeMem->Scratch[31] == 0x33, "SIF0 does not run into the scratchpad");
	expect(sif0ch.qwc == 0 && sif0ch.madr == 0x02000010, "SIF0 moves both QWs");

	/* Through the SPR bit the scratchpad wraps. */
	reset_world();
	memset(eeMem->ROM, 0x44, 32);
	for (i = 0; i < 8; i++)
		sif0.fifo.data[i] = 0x4000 + i;
	sif0.fifo.size     = 8;
	sif0.fifo.writePos = 8;
	sif0ch.chcr._u32   = 0x100;
	sif0ch.qwc         = 2;
	sif0ch.madr        = 0x80003ff0u;
	sif0.ee.busy       = true;
	SIF0Dma();
	memcpy(got, &eeMem->Scratch[0x3ff0], 16);
	memcpy(got + 4, &eeMem->Scratch[0], 16);
	expect(got[0] == 0x4000 && got[4] == 0x4004 && got[7] == 0x4007, "SIF0 wraps the scratchpad");
	expect(eeMem->ROM[0] == 0x44 && eeMem->ROM[31] == 0x44, "SIF0 stays inside the scratchpad");

	/* The scratchpad's physical alias ends in a bus error part way. */
	reset_world();
	for (i = 0; i < 8; i++)
		sif0.fifo.data[i] = 0x5000 + i;
	sif0.fifo.size     = 8;
	sif0.fifo.writePos = 8;
	sif0ch.chcr._u32   = 0x100;
	sif0ch.qwc         = 2;
	sif0ch.madr        = 0x10003ff0u;
	sif0.ee.busy       = true;
	SIF0Dma();
	memcpy(got, &eeMem->Scratch[0x3ff0], 16);
	expect(got[0] == 0x5000 && got[3] == 0x5003, "SIF0 writes up to the bus error");
	expect(sif0ch.madr == 0x10004000u && sif0ch.qwc == 1, "SIF0 stops at the bus error");
	expect(dmacRegs.stat.BEIS && !sif0ch.chcr.STR, "SIF0 raises the bus error");
	expect(sif0.fifo.size == 4, "SIF0 leaves what it could not write in the fifo");

	/* SIF1 reading from the end of main RAM reads on from the zero source. */
	reset_world();
	memset(eeMem->ZeroRead, 0, 32);
	memset(&eeMem->Main[0x01fffff0], 0x66, 16);
	memset(eeMem->Scratch, 0x77, 16);
	sif1ch.chcr._u32 = 0x100;
	sif1ch.qwc       = 2;
	sif1ch.madr      = 0x01fffff0;
	sif1.ee.busy     = true;
	SIF1Dma();
	expect(sif1.fifo.size == 8, "SIF1 reads both QWs");
	expect(sif1.fifo.data[0] == 0x66666666u && sif1.fifo.data[4] == 0, "SIF1 does not read the scratchpad past main RAM");

	/* With a TTE tag's two words in the SIF1 fifo, 2 words free and the
	 * IOP not draining it, the EE has nothing it can move: the C++ went
	 * round its loop for ever. */
	reset_world();
	sif1.fifo.size     = FIFO_SIF_W - 2;
	sif1.fifo.writePos = FIFO_SIF_W - 2;
	sif1ch.chcr._u32   = 0x101;
	sif1ch.qwc         = 1;
	sif1ch.madr        = kEeMainA;
	sif1.ee.busy       = true;
	SIF1Dma();
	expect(sif1.fifo.size == FIFO_SIF_W - 2 && sif1ch.qwc == 1, "SIF1 waits for a whole QW of room");
	sif1.fifo.data[sif1.fifo.readPos] = kIopDst;
	sif1.fifo.data[sif1.fifo.readPos + 1] = 64;
	sif1.iop.busy = true;
	SIF1Dma();
	expect(sif1ch.qwc == 0, "SIF1 goes on once the IOP drains the fifo");

	/* Reading the SIF2 register with nothing in the fifo leaves both. */
	reset_world();
	psHu32(0x1000f3e0) = 0x12345678;
	ReadFifoSingleWord();
	expect(sif2.fifo.size == 0 && sif2.fifo.readPos == 0, "an empty SIF2 fifo stays empty");
	expect(psHu32(0x1000f3e0) == 0x12345678, "the SIF2 register keeps its last word");

	/* A loaded state's fifo comes back in range. */
	{
		_sif bad[2];
		SaveStateBase s;
		memset(bad, 0, sizeof(bad));
		bad[0].fifo.readPos  = 1000;
		bad[0].fifo.writePos = -3;
		bad[0].fifo.size     = 4096;
		bad[1].fifo.size     = -7;
		bad[1].fifo.readPos  = 4;
		bad[1].fifo.writePos = 12;
		memset(&s, 0, sizeof(s));
		s.is_saving = false;
		s_load = (const u8*)bad;
		sifFreeze(&s);
		s_load = NULL;
		expect(sif0.fifo.readPos >= 0 && sif0.fifo.readPos < FIFO_SIF_W
			&& sif0.fifo.writePos >= 0 && sif0.fifo.writePos < FIFO_SIF_W
			&& sif0.fifo.size >= 0 && sif0.fifo.size <= FIFO_SIF_W, "a loaded SIF0 fifo is in range");
		expect(sif1.fifo.size == 8, "a loaded SIF1 fifo's size comes from its positions");
	}
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	eeMem  = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	iopMem = (IopVM_MemoryAllocMess*)calloc(1, sizeof(*iopMem));
	if (!eeMem || !iopMem || argc < 2)
		return 2;
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	walks(out);
	fclose(out);
#ifndef IOPSIF_CXX_TREE
	fixes();
	free(eeMem);
	free(iopMem);
	return s_failed;
#else
	free(eeMem);
	free(iopMem);
	return 0;
#endif
}
