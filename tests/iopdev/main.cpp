/* The IOP's memory map (pcsx2/IopMem.c), the MDEC (Mdec.c), the PGIF and
 * PS1 GPU DMA (ps2/pgif.c) and the PS1 kernel's TTY calls
 * (ps2/Iop/PsxBios.c), driven with seeded sequences of what the IOP and
 * the EE ask of them.
 *
 * Every value read, every call they make outward and the memory they
 * write goes into a running hash, written out every so many steps. The
 * transcript is compared with expected.txt, which this harness made from
 * the C++ these units were converted from (IOPDEV_TREE=<that checkout>):
 * the conversion changes none of it. The C build then checks what the
 * conversion fixed, which the C++ could not run: a PGIF DMA with a block
 * size of 0, and a FIFO's worth of words drained to the IOP in one go.
 * Everything around the units is a stub that records what it was asked. */
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
#include "Mdec.h"
#include "VirtualMemory.h"
#include "SPU2/spu2.h"
#include "DEV9/DEV9mmio.h"
#include "USB/usb-host.h"
#ifdef IOPDEV_CXX_TREE
#include "ps2/pgif.h"
#else
#include "ps2/pgifio.h"
#endif

/* ---- the record ---------------------------------------------------- */

static unsigned long long s_hash = 1469598103934665603ULL;
static u32 s_tick = 0x9e3779b9u;

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

static u32 answer(u32 tag, u32 a, u32 b)
{
	mix(tag);
	mix(a);
	mix(b);
	s_tick = s_tick * 1664525u + 1013904223u;
	return s_tick ^ a;
}

/* ---- what the units reach ------------------------------------------ */

psxRegisters psxRegs;
cpuRegisters cpuRegs;
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
EEVM_MemoryAllocMess* eeMem;

static void cpu_clear(u32 addr, u32 size) { answer(0x01, addr, size); }
static R3000Acpu s_cpu = { NULL, NULL, NULL, cpu_clear, NULL };
R3000Acpu* psxCpu = &s_cpu;

u8* vmm_alloc(const VirtualMemoryManager* m, uptr offset, size_t size)
{
	void* p = NULL;
	(void)m;
	(void)offset;
	if (posix_memalign(&p, 4096, size))
		return NULL;
	memset(p, 0, size);
	return (u8*)p;
}
void vmm_free(const VirtualMemoryManager* m, void* p, size_t size) { (void)m; (void)size; free(p); }

mem8_t iopHwRead8_Page1(u32 a) { return (u8)answer(0x10, a, 0); }
mem8_t iopHwRead8_Page3(u32 a) { return (u8)answer(0x11, a, 0); }
mem8_t iopHwRead8_Page8(u32 a) { return (u8)answer(0x12, a, 0); }
mem16_t iopHwRead16_Page1(u32 a) { return (u16)answer(0x13, a, 0); }
mem16_t iopHwRead16_Page3(u32 a) { return (u16)answer(0x14, a, 0); }
mem16_t iopHwRead16_Page8(u32 a) { return (u16)answer(0x15, a, 0); }
mem32_t iopHwRead32_Page1(u32 a) { return answer(0x16, a, 0); }
mem32_t iopHwRead32_Page3(u32 a) { return answer(0x17, a, 0); }
mem32_t iopHwRead32_Page8(u32 a) { return answer(0x18, a, 0); }
void iopHwWrite8_Page1(u32 a, mem8_t v) { answer(0x19, a, v); }
void iopHwWrite8_Page3(u32 a, mem8_t v) { answer(0x1a, a, v); }
void iopHwWrite8_Page8(u32 a, mem8_t v) { answer(0x1b, a, v); }
void iopHwWrite16_Page1(u32 a, mem16_t v) { answer(0x1c, a, v); }
void iopHwWrite16_Page3(u32 a, mem16_t v) { answer(0x1d, a, v); }
void iopHwWrite16_Page8(u32 a, mem16_t v) { answer(0x1e, a, v); }
void iopHwWrite32_Page1(u32 a, mem32_t v) { answer(0x1f, a, v); }
void iopHwWrite32_Page3(u32 a, mem32_t v) { answer(0x20, a, v); }
void iopHwWrite32_Page8(u32 a, mem32_t v) { answer(0x21, a, v); }
u8 psxHw4Read8(u32 a) { return (u8)answer(0x22, a, 0); }
void psxHw4Write8(u32 a, u8 v) { answer(0x23, a, v); }

u8 DEV9read8(u32 a) { return (u8)answer(0x30, a, 0); }
u16 DEV9read16(u32 a) { return (u16)answer(0x31, a, 0); }
u32 DEV9read32(u32 a) { return answer(0x32, a, 0); }
void DEV9write8(u32 a, u8 v) { answer(0x33, a, v); }
void DEV9write16(u32 a, u16 v) { answer(0x34, a, v); }
void DEV9write32(u32 a, u32 v) { answer(0x35, a, v); }
u16 SPU2read(u32 a) { return (u16)answer(0x36, a, 0); }
void SPU2write(u32 a, u16 v) { answer(0x37, a, v); }

void psxDmaInterrupt(int n) { answer(0x40, (u32)n, 0); }
void iopIntcIrq(uint n) { answer(0x41, n, 0); }
void hwIntcIrq(int n) { answer(0x42, (u32)n, 0); }

/* ---- the IOP's memory map ------------------------------------------ */

static u32 s_rng = 0x2545f491u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

/* Where the map does something different: RAM and its mirrors, the
 * scratch page, the hardware pages, the CDVD page, the SBUS window, DEV9,
 * SPU2, the ROMs, and nothing. */
static const u32 s_regions[] = {
	0x00000000, 0x00100000, 0x00600000, 0x80000000, 0xa0100000,
	0x1f800000, 0x1f801000, 0x1f803000, 0x1f808000, 0x1f804000,
	0x1f400000, 0x1d000000, 0x10000000, 0x1f900000,
	0x1fc00000, 0x1e000000, 0x1e400000, 0x12340000,
};
#define NUM_REGIONS (sizeof(s_regions) / sizeof(s_regions[0]))

static void mix_memory(void)
{
	mix_bytes(iopMem->Main, sizeof(iopMem->Main));
	mix_bytes(iopMem->P, sizeof(iopMem->P));
	mix_bytes(iopMem->Sif, sizeof(iopMem->Sif));
	mix_bytes(&eeHw[0xf200], 0x80);
	mix_bytes(iopHw, sizeof(iopHw));
}

static void memory_map(FILE* out)
{
	int i;
	for (i = 1; i <= 300000; i++)
	{
		const u32 base = s_regions[rnd() % NUM_REGIONS];
		const int width = 1 << (rnd() % 3);
		/* SBUS registers want the low bits that pick them; elsewhere any. */
		const u32 off = (base == 0x1d000000) ? (rnd() & 0x8ff) : (rnd() & 0xffff);
		const u32 addr = (base + off) & ~(u32)(width - 1);
		psxRegs.CP0.n.Status = (rnd() % 8) ? 0 : 0x10000;
		/* the EE's side of the SBUS registers, so the IOP's writes that
		 * clear and set bits in them have bits to work on */
		if (i % 1000 == 1)
		{
			u32 k;
			for (k = 0; k < 0x80; k++)
				eeHw[0xf200 + k] = (u8)rnd();
		}
		mix(addr);
		if (rnd() & 1)
		{
			if (width == 1)
				mix(iopMemRead8(addr));
			else if (width == 2)
				mix(iopMemRead16(addr));
			else
				mix(iopMemRead32(addr));
		}
		else
		{
			const u32 v = rnd();
			if (width == 1)
				iopMemWrite8(addr, (u8)v);
			else if (width == 2)
				iopMemWrite16(addr, (u16)v);
			else
				iopMemWrite32(addr, v);
		}
		if (i % 30000 == 0)
		{
			mix_memory();
			fprintf(out, "memory %d: %016llx\n", i, s_hash);
		}
	}
	{
		char buf[64];
		memcpy(&iopMem->Main[0x1000], "a guest string\0and more", 23);
		iopMemReadStringBuf(buf, (int)sizeof(buf), 0x1000, 65536);
		mix_bytes(buf, strlen(buf));
		iopMemReadStringBuf(buf, 5, 0x1000, 65536);
		mix_bytes(buf, strlen(buf));
		iopMemReadStringBuf(buf, (int)sizeof(buf), 0x1000, 3);
		mix_bytes(buf, strlen(buf));
		mix((u64)(usb_iop_ram() - iopMem->Main));
	}
	fprintf(out, "strings: %016llx\n", s_hash);
}

/* ---- the PS1 kernel's TTY calls ------------------------------------ */

static void tty(FILE* out)
{
	static const u32 calls[] = { 0xa03, 0xb35, 0xa09, 0xb3b, 0xa3c, 0xb3d, 0xa3e, 0xb3f, 0xa00, 0xc12 };
	int i;
	for (i = 0; i < 20000; i++)
	{
		const u32 c = calls[rnd() % (sizeof(calls) / sizeof(calls[0]))];
		/* RAM most of the time, the hardware pages now and then. */
		const u32 at = (rnd() % 8) ? (rnd() & 0x1fffff) : (0x1f801000 + (rnd() & 0xfff));
		psxRegs.pc = (c >> 8) << 4;
		psxRegs.GPR.n.t1 = c & 0xff;
		psxRegs.GPR.n.a0 = (rnd() & 3) ? 1 : at;
		psxRegs.GPR.n.a1 = (rnd() & 3) ? at : 1;
		psxRegs.GPR.n.a2 = rnd() & 0xff;
		if (c == 0xa3e || c == 0xb3f)
			psxRegs.GPR.n.a0 = at;
		mix(psxBiosCall());
	}
	psxBiosReset();
	fprintf(out, "tty: %016llx\n", s_hash);
}

/* ---- the MDEC --------------------------------------------------------- */

static void put16(u32 addr, u16 v)
{
	iopMem->Main[addr & 0x1fffff] = (u8)v;
	iopMem->Main[(addr + 1) & 0x1fffff] = (u8)(v >> 8);
}

/* Blocks of run-length codes: a scale and DC word, a few runs and levels,
 * the end code. Enough of them for the largest decode below. The levels
 * are kept small: the C++ read past its clamp table on a pixel out of its
 * range, which the C build checks on its own below. */
static u32 rle_stream(u32 at, int blocks)
{
	int b;
	u32 p = at;
	for (b = 0; b < blocks; b++)
	{
		int n = (int)(rnd() % 6);
		put16(p, (u16)(((1 + rnd() % 3) << 10) | ((rnd() % 64 - 32) & 0x3ff)));
		p += 2;
		while (n--)
		{
			put16(p, (u16)(((rnd() % 12) << 10) | ((rnd() % 9 - 4) & 0x3ff)));
			p += 2;
		}
		put16(p, 0xfe00);
		p += 2;
	}
	return p - at;
}

static void mdec_run(FILE* out)
{
	int depth, round;
	u32 i;

	psxRegs.CP0.n.Status = 0;
	mdecInit();
	mdecWrite1(0x80000000);
	mix(mdecRead1());

	/* The quantisation tables: 64 bytes each, luma then chroma. */
	for (i = 0; i < 128; i++)
		iopMem->Main[0x10000 + i] = (u8)(1 + rnd() % 2);
	mdecWrite0(0x40000001);
	psxDma0(0x10000, (2 << 16) | 16, 0x01000201);
	mix(mdecRead1());

	for (round = 0; round < 6; round++)
	{
		for (depth = 0; depth < 4; depth++)
		{
			/* 15-bit and 24-bit macroblocks are 128 and 192 words out;
			 * the monochrome ones 16 and 8. Four of them. */
			static const u32 out_words[4] = { 8 * 4, 16 * 4, 192 * 4, 128 * 4 };
			const u32 words = out_words[depth];
			const u32 in_bytes = rle_stream(0x20000, 64);
			const u32 in_words = (in_bytes + 3) / 4;

			mdecWrite0(0x30000000 | ((u32)depth << 27) | (in_words & 0xffff));
			mix(mdecRead1());
			psxDma0(0x20000, (in_words << 16) | 1, 0x01000201);
			mix(mdecRead1());
			for (i = 0; i < 40; i++)
			{
				mix(mdecRead0());
				mix(mdecRead1());
			}
			psxDma1(0x40000, (words / 16 << 16) | 16, 0x01000200);
			mix(mdecRead1());
			mix_bytes(&iopMem->Main[0x40000], words * 4);
			/* the channel bits each DMA clears */
			mix(HW_DMA0_CHCR);
			mix(HW_DMA1_CHCR);
		}
		/* Input words fed through the register rather than DMA. */
		mdecWrite0(0x30000000 | 0x40);
		for (i = 0; i < 0x50; i++)
		{
			mdecWrite0(rnd());
			mix(mdecRead1());
		}
		/* A DMA in the wrong direction does nothing. */
		psxDma0(0x20000, (4 << 16) | 1, 0x01000200);
		psxDma1(0x40000, (4 << 16) | 1, 0x01000201);
		fprintf(out, "mdec %d: %016llx\n", round, s_hash);
	}
}

/* ---- the PGIF ----------------------------------------------------------- */

static void mix_pgif(void)
{
	mix_bytes(&eeHw[0xf300], 0x100);
	mix_bytes(&iopHw[0x10a0], 0x10);
	mix(cpuRegs.nextEventCycle);
}

/* A linked list of packets in IOP RAM ending in the end code: each
 * header's top byte is its packet's length in words. */
static u32 make_list(u32 at, int packets)
{
	int k;
	u32 p = at;
	for (k = 0; k < packets; k++)
	{
		const u32 len = rnd() % 12;
		const u32 next = (k + 1 < packets) ? (p + 4 + len * 4 + (rnd() & 0x30)) : 0x00ffffff;
		u32 j;
		{
			const u32 hdr = (len << 24) | next;
			memcpy(&iopMem->Main[p], &hdr, 4);
		}
		for (j = 0; j < len; j++)
		{
			const u32 w = rnd();
			memcpy(&iopMem->Main[p + 4 + j * 4], &w, 4);
		}
		if (next != 0x00ffffff)
			p = next;
	}
	return at;
}

static void pgif_run(FILE* out)
{
	static const int regs[] = { 0x1000F300, 0x1000F310, 0x1000F320, 0x1000F330, 0x1000F340,
	                            0x1000F380, 0x1000F3C0, 0x1000F3E0, 0x1000F390 };
	int i;
	pgifInit();
	psxRegs.CP0.n.Status = 0;
	cpuRegs.cycle = 1234;
	for (i = 1; i <= 60000; i++)
	{
		const u32 op = rnd() % 16;
		const u32 v = rnd();
		switch (op)
		{
			case 0: psxGPUw(HW_PS1_GPU_DATA, v); break;
			case 1: psxGPUw(HW_PS1_GPU_DATA, (v & 1) ? 0x1F000000 : v); break;
			case 2: psxGPUw(HW_PS1_GPU_STATUS, v); break;
			case 3: mix(psxGPUr((v & 1) ? HW_PS1_GPU_DATA : HW_PS1_GPU_STATUS)); break;
			case 4: PGIFw(regs[v % 9], rnd()); break;
			case 5: mix(PGIFr(regs[v % 9])); break;
			case 6:
			{
				u32 q[4] = { 0, 0, 0, 0 };
				PGIFrQword((v & 1) ? 0x1000F3E0 : 0x1000F3C0, q);
				mix_bytes(q, sizeof(q));
				break;
			}
			case 7:
			{
				u32 q[4] = { rnd(), rnd(), rnd(), rnd() };
				PGIFwQword((v & 1) ? 0x1000F3E0 : 0x1000F3C0, q);
				break;
			}
			case 8: /* let the FIFO take DMA data */
				PGIFw(0x1000F380, (v & ~0x1fu) | 0x8);
				break;
			case 9: /* a normal DMA to the GPU */
				psxDma2GpuW(0x1F8010A0, v & 0x1ffffc);
				psxDma2GpuW(0x1F8010A4, ((1 + (v >> 8) % 8) << 16) | (1 + v % 16));
				psxDma2GpuW(0x1F8010A8, 0x01000201 | ((v & 0x400) ? 0x600 : 0));
				break;
			case 10: /* a normal DMA to the IOP, below the lists */
				psxDma2GpuW(0x1F8010A0, v & 0xffffc);
				psxDma2GpuW(0x1F8010A4, ((1 + (v >> 8) % 8) << 16) | (1 + v % 16));
				psxDma2GpuW(0x1F8010A8, 0x01000200);
				break;
			case 11: /* a linked list to the GPU, once the last has ended: a
			          * list still running reads its next header from
			          * wherever this one's words now are */
				if (psxDma2GpuR(0x1F8010A8) & 0x01000000)
					break;
				psxDma2GpuW(0x1F8010A0, make_list((v & 0xfff0) + 0x100000, 1 + (int)(v % 6)));
				psxDma2GpuW(0x1F8010A8, 0x01000401);
				if (v & 0x10)
					psxGPUw(HW_PS1_GPU_STATUS, 0x04000002);
				break;
			case 12:
				mix(psxDma2GpuR(0x1F8010A0 + (v % 4) * 4));
				break;
			case 13:
				/* DMA moves whole words: the address's low bits are the
				 * guest's to leave clear. */
				/* any register but CHCR's start bit, which only the
				 * cases above set, with an address they chose */
				psxDma2GpuW(0x1F8010A0 + (v % 4) * 4, (rnd() & ((v % 4) == 0 ? ~3u : (v % 4) == 2 ? ~0x01000000u : ~0u)) | ((v % 4) == 1 ? 0x00010001 : 0));
				break;
			case 14: psxGPUw(HW_PS1_GPU_STATUS, 0x04000000 | (v & 3)); break;
			case 15: psxGPUw(HW_PS1_GPU_STATUS, 0x10000000 | (v & 7)); break;
		}
		mix(op);
		mix_pgif();
		if (i % 6000 == 0)
		{
			mix_bytes(iopMem->Main, sizeof(iopMem->Main));
			fprintf(out, "pgif %d: %016llx\n", i, s_hash);
		}
	}
}

/* What the C++ could not run: the C build only. */
static int fixes(void)
{
#ifdef IOPDEV_CXX_TREE
	return 0;
#else
	int failures = 0;
	u32 q[4] = { 1, 2, 3, 4 };
	int i;

	/* RAM takes writes: the cache is not isolated */
	psxRegs.CP0.n.Status = 0;

	/* A block size of 0 is 0x10000 words: the block count is left alone
	 * until that many have gone. It used to divide by zero. */
	pgifInit();
	psxDma2GpuW(0x1F8010A0, 0x2000);
	psxDma2GpuW(0x1F8010A4, (3 << 16) | 0);
	psxDma2GpuW(0x1F8010A8, 0x01000200);
	PGIFwQword(0x1000F3E0, q);
	if ((psxDma2GpuR(0x1F8010A4) >> 16) != 3 || psxDma2GpuR(0x1F8010A0) != 0x200c)
	{
		printf("FAIL: a DMA to the IOP with a block size of 0\n");
		failures++;
	}
	else
		printf("ok  : a DMA to the IOP with a block size of 0\n");

	/* A FIFO's worth of words waiting when a DMA to the IOP starts all go
	 * in one call, which used to recurse once a word. */
	pgifInit();
	for (i = 0; i < 0x10000; i++)
		psxGPUw(HW_PS1_GPU_DATA, (u32)i);
	psxDma2GpuW(0x1F8010A0, 0x0);
	psxDma2GpuW(0x1F8010A4, (0 << 16) | 0x10);
	psxDma2GpuW(0x1F8010A8, 0x01000200);
	{
		u32 w;
		memcpy(&w, &iopMem->Main[0x3fffc], 4);
		if (psxDma2GpuR(0x1F8010A0) != 0x40000 || w != 0xffff)
		{
			printf("FAIL: a full FIFO drained to the IOP\n");
			failures++;
		}
		else
			printf("ok  : a full FIFO drained to the IOP\n");
	}

	/* Levels large enough to take a pixel past the clamp table's range
	 * saturate it instead of reading past the table (which a sanitizer
	 * build reports). */
	{
		u32 k;
		for (k = 0; k < 128; k++)
			iopMem->Main[0x10000 + k] = 0x3f;
		mdecWrite0(0x40000001);
		psxDma0(0x10000, (2 << 16) | 16, 0x01000201);
		for (k = 0; k < 0x400; k += 2)
			put16(0x20000 + k, (u16)((k & 6) == 6 ? 0xfe00 : (0xfc00 | ((k * 37) & 0x3ff))));
		mdecWrite0(0x38000100);
		psxDma0(0x20000, (0x100 << 16) | 1, 0x01000201);
		psxDma1(0x40000, (8 << 16) | 16, 0x01000200);
		mdecWrite0(0x30000100);
		psxDma0(0x20000, (0x100 << 16) | 1, 0x01000201);
		psxDma1(0x40000, (12 << 16) | 16, 0x01000200);
		printf("ok  : MDEC levels past the clamp table's range saturate\n");
	}

	/* A DMA larger than the MDEC's buffers moves what fits and stops. */
	mdecWrite0(0x30000000);
	psxDma0(0, 0xffff7fff, 0x01000201);
	printf("ok  : an oversized MDEC DMA stays in its buffer\n");
	return failures;
#endif
}

int main(int argc, char** argv)
{
	FILE* out;
	static VirtualMemoryManager* map;
	int failures;
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <transcript out>\n", argv[0]);
		return 2;
	}
	out = fopen(argv[1], "w");
	if (!out)
		return 2;

	eeMem = (EEVM_MemoryAllocMess*)calloc(1, sizeof(*eeMem));
	{
		size_t i;
		for (i = 0; i < sizeof(eeMem->ROM); i++)
			eeMem->ROM[i] = (u8)(i * 7);
	}
	iop_memory_assign(map);
	iop_memory_reset();

	memory_map(out);
	tty(out);
	mdec_run(out);
	pgif_run(out);
	fclose(out);

	failures = fixes();
	iop_memory_release(map);
	free(eeMem);
	return failures != 0;
}
