/* The IOP's hardware register handlers (pcsx2/ps2/Iop/IopHwRead.c,
 * IopHwWrite.c): every 8-, 16- and 32-bit read and write the EE's vtlb and
 * the IOP's memory map send them, over pages 1, 3 and 8 and the generic
 * page, with the registers holding a pattern.
 *
 * Each access's result goes into a running hash with the register words
 * around it, the interrupt and DMA control registers, SIO0, SIO2, the
 * counters and every call the handlers make outward with its arguments;
 * a seeded run of mixed accesses follows the sweep. The transcript is
 * compared with expected.txt, which this harness made from the C++ the
 * handlers were converted from (IOPHW_TREE=<that checkout>): the
 * conversion changes no value read, no byte written and no call made.
 * Everything around the handlers is a stub that records what it was
 * asked and answers from a counter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "R3000A.h"
#include "IopMem.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopCounters.h"
#include "Mdec.h"
#include "Sio.h"
#include "FW.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#include "CDVD/Ps1CD.h"
#include "DEV9/DEV9mmio.h"
#include "ps2/BiosTools.h"
#ifdef IOPHW_CXX_TREE
#include "ps2/pgif.h"
using namespace IopMemory;
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

/* Every stub records its own tag and arguments and answers from here. */
static u32 answer(u32 tag, u32 a, u32 b)
{
	mix(tag);
	mix(a);
	mix(b);
	s_tick = s_tick * 1664525u + 1013904223u;
	return s_tick ^ a;
}

/* ---- what the handlers reach ---------------------------------------- */

psxRegisters psxRegs;
alignas(4096) u8 iopHw[PS2MEM_IOP_HARDWARE];
psxCounter psxCounters[NUM_COUNTERS];
Sio0 sio0;
Sio2 sio2;
bool BiosIsArcade;

u8 sio0_get_rx_data(void) { return (u8)answer(0x10, 0, 0); }
u32 sio0_get_stat(void) { return answer(0x11, 0, 0); }
void sio0_set_tx_data(u8 v) { answer(0x12, v, 0); }
void sio0_set_ctrl(u16 v) { answer(0x13, v, 0); }
u8 sio2_read(void) { return (u8)answer(0x14, 0, 0); }
void sio2_write(u8 v) { answer(0x15, v, 0); }
void sio2_set_send3(size_t i, u32 v) { answer(0x16, (u32)i, v); }
void sio2_set_ctrl(u32 v) { answer(0x17, v, 0); }

u8 DEV9read8(u32 a) { return (u8)answer(0x20, a, 0); }
u16 DEV9read16(u32 a) { return (u16)answer(0x21, a, 0); }
void DEV9write8(u32 a, u8 v) { answer(0x22, a, v); }
void DEV9write16(u32 a, u16 v) { answer(0x23, a, v); }

u8 cdrRead0(void) { return (u8)answer(0x30, 0, 0); }
u8 cdrRead1(void) { return (u8)answer(0x31, 0, 0); }
u8 cdrRead2(void) { return (u8)answer(0x32, 0, 0); }
u8 cdrRead3(void) { return (u8)answer(0x33, 0, 0); }
void cdrWrite0(u8 v) { answer(0x34, v, 0); }
void cdrWrite1(u8 v) { answer(0x35, v, 0); }
void cdrWrite2(u8 v) { answer(0x36, v, 0); }
void cdrWrite3(u8 v) { answer(0x37, v, 0); }

u8 USBread8(u32 a) { return (u8)answer(0x40, a, 0); }
u16 USBread16(u32 a) { return (u16)answer(0x41, a, 0); }
u32 USBread32(u32 a) { return answer(0x42, a, 0); }
void USBwrite8(u32 a, u8 v) { answer(0x43, a, v); }
void USBwrite16(u32 a, u16 v) { answer(0x44, a, v); }
void USBwrite32(u32 a, u32 v) { answer(0x45, a, v); }

u16 psxRcntRcount16(int i) { return (u16)answer(0x50, (u32)i, 0); }
u32 psxRcntRcount32(int i) { return answer(0x51, (u32)i, 0); }
void psxRcntWcount16(int i, u16 v) { answer(0x52, (u32)i, v); }
void psxRcntWcount32(int i, u32 v) { answer(0x53, (u32)i, v); }
void psxRcntWmode16(int i, u32 v) { answer(0x54, (u32)i, v); }
void psxRcntWmode32(int i, u32 v) { answer(0x55, (u32)i, v); }
void psxRcntWtarget16(int i, u32 v) { answer(0x56, (u32)i, v); }
void psxRcntWtarget32(int i, u32 v) { answer(0x57, (u32)i, v); }

u16 SPU2read(u32 a) { return (u16)answer(0x60, a, 0); }
void SPU2write(u32 a, u16 v) { answer(0x61, a, v); }

u32 psxDma2GpuR(u32 a) { return answer(0x70, a, 0); }
void psxDma2GpuW(u32 a, u32 v) { answer(0x71, a, v); }
u32 psxGPUr(int a) { return answer(0x72, (u32)a, 0); }
void psxGPUw(int a, u32 v) { answer(0x73, (u32)a, v); }

u32 mdecRead0(void) { return answer(0x80, 0, 0); }
u32 mdecRead1(void) { return answer(0x81, 0, 0); }
void mdecWrite0(u32 v) { answer(0x82, v, 0); }
void mdecWrite1(u32 v) { answer(0x83, v, 0); }

u32 FWread32(u32 a) { return answer(0x90, a, 0); }
void FWwrite32(u32 a, u32 v) { answer(0x91, a, v); }

void iopTestIntc(void) { answer(0xa0, 0, 0); }
void iopIntcIrq(uint n) { answer(0xa1, n, 0); }
void psxDmaInterrupt(int n) { answer(0xa2, (u32)n, 0); }
void psxDmaInterrupt2(int n) { answer(0xa3, (u32)n, 0); }

#define DMA_STUB(n) \
	void psxDma##n(u32 m, u32 b, u32 c) { answer(0xb0 + n, m, b ^ c); }
DMA_STUB(0) DMA_STUB(1) DMA_STUB(2) DMA_STUB(3) DMA_STUB(4) DMA_STUB(6)
DMA_STUB(7) DMA_STUB(8) DMA_STUB(9) DMA_STUB(10) DMA_STUB(11) DMA_STUB(12)

/* ---- state out ------------------------------------------------------ */

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

static void mix_state(u32 addr)
{
	const u32 at = addr & 0xfff8;
	mix_bytes(&iopHw[at], 24);
	mix(psxHu32(0x1070));
	mix(psxHu32(0x1074));
	mix(psxHu32(0x1078));
	mix(HW_DMA_ICR);
	mix(HW_DMA_ICR2);
	mix(psxRegs.CP0.n.Cause);
	mix(sio0.mode);
	mix(sio0.ctrl);
	mix(sio0.baud);
	mix_bytes(sio2.send1, sizeof(sio2.send1));
	mix_bytes(sio2.send2, sizeof(sio2.send2));
	mix(sio2.recv1);
	mix(sio2.recv2);
	mix(sio2.recv3);
	mix(sio2.unknown1);
	mix(sio2.unknown2);
	mix(sio2.iStat);
	{
		int i;
		for (i = 0; i < 6; i++)
		{
			mix(psxCounters[i].mode);
			mix(psxCounters[i].target);
		}
	}
}

/* The registers hold a pattern, and the counters and SIO2 a known state;
 * the PCR enables vary with the seed so the DMA kicks run both ways. */
static void fill(u32 seed)
{
	u32 i;
	for (i = 0; i < PS2MEM_IOP_HARDWARE; i++)
		iopHw[i] = (u8)((i * 131u + seed * 7u) ^ (i >> 8));
	for (i = 0; i < 6; i++)
	{
		psxCounters[i].mode = 0x1c58 ^ seed ^ i;
		psxCounters[i].target = 0x1000012345ULL * (i + 1) + seed;
	}
	for (i = 0; i < 4; i++)
	{
		sio2.send1[i] = seed * 3 + i;
		sio2.send2[i] = seed * 5 + i;
	}
	sio2.iStat = 0xffffffffu ^ seed;
	for (i = 0; i < SIO2_SEND3_COUNT; i++)
		sio2.send3[i] = seed + i * 0x01010101u;
}

typedef u32 (*read_fn)(u32);

static u32 r8p1(u32 a) { return iopHwRead8_Page1(a); }
static u32 r8p3(u32 a) { return iopHwRead8_Page3(a); }
static u32 r8p8(u32 a) { return iopHwRead8_Page8(a); }
static u32 r16p1(u32 a) { return iopHwRead16_Page1(a); }
static u32 r16p3(u32 a) { return iopHwRead16_Page3(a); }
static u32 r16p8(u32 a) { return iopHwRead16_Page8(a); }
static u32 r32p1(u32 a) { return iopHwRead32_Page1(a); }
static u32 r32p3(u32 a) { return iopHwRead32_Page3(a); }
static u32 r32p8(u32 a) { return iopHwRead32_Page8(a); }
static u32 r8g(u32 a) { return iopHwRead8_generic(a); }
static u32 r16g(u32 a) { return iopHwRead16_generic(a); }
static u32 r32g(u32 a) { return iopHwRead32_generic(a); }

static void w8p1(u32 a, u32 v) { iopHwWrite8_Page1(a, (u8)v); }
static void w8p3(u32 a, u32 v) { iopHwWrite8_Page3(a, (u8)v); }
static void w8p8(u32 a, u32 v) { iopHwWrite8_Page8(a, (u8)v); }
static void w16p1(u32 a, u32 v) { iopHwWrite16_Page1(a, (u16)v); }
static void w16p3(u32 a, u32 v) { iopHwWrite16_Page3(a, (u16)v); }
static void w16p8(u32 a, u32 v) { iopHwWrite16_Page8(a, (u16)v); }
static void w32p1(u32 a, u32 v) { iopHwWrite32_Page1(a, v); }
static void w32p3(u32 a, u32 v) { iopHwWrite32_Page3(a, v); }
static void w32p8(u32 a, u32 v) { iopHwWrite32_Page8(a, v); }
static void w8g(u32 a, u32 v) { iopHwWrite8_generic(a, (u8)v); }
static void w16g(u32 a, u32 v) { iopHwWrite16_generic(a, (u16)v); }
static void w32g(u32 a, u32 v) { iopHwWrite32_generic(a, v); }

typedef void (*write_fn)(u32, u32);

struct handler
{
	const char* name;
	u32 base;  /* the page the handler serves */
	int width; /* bytes */
	read_fn r;
	write_fn w;
};

/* The generic handlers serve whatever page they are mapped over; 0x2000
 * stands in, below the last word so a 32-bit access stays in the block. */
static const struct handler s_handlers[] = {
	{ "8 page1", 0x1f801000, 1, r8p1, w8p1 },
	{ "8 page3", 0x1f803000, 1, r8p3, w8p3 },
	{ "8 page8", 0x1f808000, 1, r8p8, w8p8 },
	{ "16 page1", 0x1f801000, 2, r16p1, w16p1 },
	{ "16 page3", 0x1f803000, 2, r16p3, w16p3 },
	{ "16 page8", 0x1f808000, 2, r16p8, w16p8 },
	{ "32 page1", 0x1f801000, 4, r32p1, w32p1 },
	{ "32 page3", 0x1f803000, 4, r32p3, w32p3 },
	{ "32 page8", 0x1f808000, 4, r32p8, w32p8 },
	{ "8 generic", 0x1f802000, 1, r8g, w8g },
	{ "16 generic", 0x1f802000, 2, r16g, w16g },
	{ "32 generic", 0x1f802000, 4, r32g, w32g },
};
#define NUM_HANDLERS (sizeof(s_handlers) / sizeof(s_handlers[0]))

/* Values that hit the special cases: all ones (the IREG ack), zero, the
 * ICR enables and flags, and a pattern. */
static const u32 s_values[] = { 0xffffffffu, 0, 0x00ff8000u, 0x7f7f0000u, 0x81000201u, 0x12345678u };
#define NUM_VALUES (sizeof(s_values) / sizeof(s_values[0]))

static u32 s_rng = 0x2545f491u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

int main(int argc, char** argv)
{
	FILE* out;
	unsigned h, v;
	u32 off;
	int arcade, i;
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <transcript out>\n", argv[0]);
		return 2;
	}
	out = fopen(argv[1], "w");
	if (!out)
		return 2;

	/* Every register of every page, read then written with each value,
	 * the page refilled between values. Page 3 once each way for the
	 * arcade board's TOOL registers. */
	for (arcade = 0; arcade < 2; arcade++)
	{
		BiosIsArcade = arcade != 0;
		for (h = 0; h < NUM_HANDLERS; h++)
		{
			const struct handler* hd = &s_handlers[h];
			for (v = 0; v < NUM_VALUES; v++)
			{
				fill(h * 16 + v + arcade);
				for (off = 0; off < 0x1000; off += (u32)hd->width)
				{
					const u32 addr = hd->base + off;
					mix(addr);
					mix(hd->r(addr));
					mix_state(addr);
					hd->w(addr, s_values[v] ^ (off * 0x00010001u * (v == 5)));
					mix_state(addr);
				}
			}
			fprintf(out, "%s%s: %016llx\n", hd->name, arcade ? " arcade" : "", s_hash);
		}
	}
	mix_bytes(iopHw, sizeof(iopHw));
	fprintf(out, "sweep: %016llx\n", s_hash);

	/* A seeded mix: any handler, any of its registers, any value, without
	 * a refill, so each access sees what the ones before it left. */
	fill(0xabc);
	for (i = 1; i <= 200000; i++)
	{
		const struct handler* hd = &s_handlers[rnd() % NUM_HANDLERS];
		const u32 addr = hd->base + ((rnd() & 0xfff) & ~(u32)(hd->width - 1));
		BiosIsArcade = (rnd() & 1) != 0;
		mix(addr);
		if (rnd() & 1)
			mix(hd->r(addr));
		else
			hd->w(addr, (rnd() & 3) ? rnd() : s_values[rnd() % NUM_VALUES]);
		mix_state(addr);
		if (i % 20000 == 0)
			fprintf(out, "mix %d: %016llx\n", i, s_hash);
	}
	mix_bytes(iopHw, sizeof(iopHw));
	mix_bytes(psxCounters, sizeof(psxCounters));
	fprintf(out, "end: %016llx\n", s_hash);
	fclose(out);
	return 0;
}
