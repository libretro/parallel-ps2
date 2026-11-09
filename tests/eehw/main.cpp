/* The EE hardware register handlers and the DMAC (pcsx2/Hw.c, Dmac.c),
 * driven with seeded register accesses of every width on every page.
 *
 * The handlers are reached the way the core reaches them: the harness
 * asks hwRegisterPageHandlers and hwAssignPage0FHandlers for each page's
 * set, catching the function pointers they hand the vtlb, and calls
 * through those. The addresses are drawn mostly from the registers the
 * handlers single out (the DMAC's channel registers, its control, status
 * and enable registers, INTC, SBUS, MCH, the SIO FIFO, GIF, VIF and the
 * FIFOs), the rest anywhere on the page, the gamefixes the handlers check
 * are flipped at random, and the interrupt entry points and the MFIFO
 * write are called between accesses.
 *
 * Everything the units reach outside themselves is a stub that records
 * its arguments and answers from a sequence; every value read, every call
 * made and the register file afterwards go into a running hash, written
 * out every so many steps. The transcript is compared with expected.txt,
 * which this harness made from the C++ these units were converted from
 * (EEHW_TREE=<that checkout>): the conversion changes none of it. The C
 * build then checks what the conversion fixed, in fixes(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common.h"
#include "Config.h"
#include "R3000A.h"
#include "R5900.h"
#include "Dmac.h"
#include "Hw.h"
#include "IopHw.h"
#include "IopMem.h"
#include "Counters.h"
#include "Sif.h"
#include "SPR.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "vtlb.h"
#include "VUmicro.h"
#include "IPU/IPUdma.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#include "FW.h"
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"
#include "ps2/pgifio.h"
#include "ps2/HwInternal.h"
#ifdef EEHW_CXX_TREE
#include "Gif.h"
#include "Gif_Unit.h"
#include "IPU/IPU.h"
#include "GS.h"
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

static u32 s_rng = 0x3c6ef372u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

static u32 s_tick = 0x9e3779b9u;
static u32 answer(u32 tag, u32 a, u32 b)
{
	mix(tag);
	mix(a);
	mix(b);
	s_tick = s_tick * 1664525u + 1013904223u;
	return s_tick;
}

/* ---- what the units reach ------------------------------------------ */

alignas(16) cpuRegisters cpuRegs;
psxRegisters psxRegs;
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
alignas(4096) u8 iopHw[PS2MEM_IOP_HARDWARE];
s64 PSXCLK = 36864000;
cdvdStruct cdvd;
Counter counters[4];
_sif sif2;
static u8* s_physmem; /* what vtlb_GetPhyPtr maps: 32 MiB */

static bool s_oph, s_dmabusy, s_viffifo;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");
static void set_hacks(void)
{
	Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
	s_oph     = rnd_n(3) == 0;
	s_dmabusy = rnd_n(3) == 0;
	s_viffifo = rnd_n(3) == 0;
	c->Gamefixes.OPHFlagHack = s_oph;
	c->Gamefixes.DMABusyHack = s_dmabusy;
	c->Gamefixes.VIFFIFOHack = s_viffifo;
}

void CPU_INT(EE_EventType n, s32 ecycle) { answer(0x01, (u32)n, (u32)ecycle); }
void cpuTestINTCInts(void) { answer(0x02, 0, 0); }
void cpuTestDMACInts(void) { answer(0x03, 0, 0); }

/* The DMA starts and interrupts: each records itself, and the ones the
 * DMAC's busy hack waits on stop their channel, as finishing would. */
#define CHCR_STOP(addr) (psHu32(addr) &= ~0x100u)
void dmaVIF0(void) { answer(0x10, 0, 0); }
void dmaVIF1(void) { answer(0x11, 0, 0); }
void dmaGIF(void) { answer(0x12, 0, 0); }
void dmaIPU0(void) { answer(0x13, 0, 0); }
void dmaIPU1(void) { answer(0x14, 0, 0); }
void dmaSIF0(void) { answer(0x15, 0, 0); }
void dmaSIF1(void) { answer(0x16, 0, 0); }
void dmaSIF2(void) { answer(0x17, 0, 0); }
void dmaSPR0(void) { answer(0x18, 0, 0); }
void dmaSPR1(void) { answer(0x19, 0, 0); }
void vif0Interrupt(void) { answer(0x1a, 0, 0); CHCR_STOP(D0_CHCR); }
void vif1Interrupt(void) { answer(0x1b, 0, 0); CHCR_STOP(D1_CHCR); }
void vif1VUFinish(void) { answer(0x1c, 0, 0); CHCR_STOP(D1_CHCR); }
void vu1Finish(bool add_cycles) { answer(0x1d, add_cycles, 0); }
void gifInterrupt(void) { answer(0x1e, 0, 0); CHCR_STOP(D2_CHCR); }
void SPRFROMinterrupt(void) { answer(0x1f, 0, 0); CHCR_STOP(D8_CHCR); }
void SPRTOinterrupt(void) { answer(0x20, 0, 0); CHCR_STOP(D9_CHCR); }

u16 rcntRead32(u32 mem) { return (u16)answer(0x30, mem, 0); }
bool rcntWrite32(u32 mem, mem32_t* value) { u32 a = answer(0x31, mem, *value); *value ^= a & 0xff; return (a & 3) != 0; }
u32 rcntRcount(int index) { return answer(0x32, (u32)index, 0); }
void rcntInit(void) { answer(0x33, 0, 0); }
u32 ipuRead32(u32 mem) { return answer(0x34, mem, 0); }
u64 ipuRead64(u32 mem) { u32 a = answer(0x35, mem, 0); return ((u64)a << 32) | answer(0x35, mem, 1); }
bool ipuWrite32(u32 mem, u32 value) { return (answer(0x36, mem, value) & 3) != 0; }
bool ipuWrite64(u32 mem, u64 value) { return (answer(0x37, mem, (u32)value ^ (u32)(value >> 32)) & 3) != 0; }
void ipuReset(void) { answer(0x38, 0, 0); }

static void fifo_out(u32 tag, u128* out)
{
	out->_u32[0] = answer(tag, 0, 0);
	out->_u32[1] = answer(tag, 1, 0);
	out->_u32[2] = answer(tag, 2, 0);
	out->_u32[3] = answer(tag, 3, 0);
}
void ReadFIFO_VIF1(mem128_t* out) { fifo_out(0x40, out); }
void ReadFIFO_IPUout(mem128_t* out) { fifo_out(0x41, out); }
void WriteFIFO_VIF0(const mem128_t* v) { mix(0x42); mix_bytes(v, 16); }
void WriteFIFO_VIF1(const mem128_t* v) { mix(0x43); mix_bytes(v, 16); }
void WriteFIFO_GIF(const mem128_t* v) { mix(0x44); mix_bytes(v, 16); }
void WriteFIFO_IPUin(const mem128_t* v) { mix(0x45); mix_bytes(v, 16); }

void PGIFw(int addr, u32 data) { answer(0x50, (u32)addr, data); }
u32 PGIFr(int addr) { return answer(0x51, (u32)addr, 0); }
void PGIFwQword(u32 addr, void* data) { mix(0x52); mix(addr); mix_bytes(data, 16); }
void PGIFrQword(u32 addr, void* data) { fifo_out(0x53, (u128*)data); mix(addr); }
void ReadFifoSingleWord(void) { psHu32(0x1000f3e0) = answer(0x54, 0, 0); }

void SPU2_Reset(bool psxmode) { answer(0x60, psxmode, 0); }
void setPs1CDVDSpeed(int speed) { answer(0x61, (u32)speed, 0); }
void psxReset(void) { answer(0x62, 0, 0); }
void USBreset(void) { answer(0x63, 0, 0); }
void FWreset(void) { answer(0x64, 0, 0); }
void sifReset(void) { answer(0x65, 0, 0); }
void gsReset(void) { answer(0x66, 0, 0); }
void vif0Reset(void) { answer(0x67, 0, 0); }
void vif1Reset(void) { answer(0x68, 0, 0); }

void* vtlb_GetPhyPtr(u32 paddr)
{
	/* Not recorded: how often the address is resolved is the unit's business. */
	return paddr < PS2MEM_MAIN_RAM ? s_physmem + paddr : NULL;
}

/* The handler sets the units hand the vtlb. */
typedef struct handler_set
{
	vtlbMemR8FP* r8; vtlbMemR16FP* r16; vtlbMemR32FP* r32; vtlbMemR64FP* r64; vtlbMemR128FP* r128;
	vtlbMemW8FP* w8; vtlbMemW16FP* w16; vtlbMemW32FP* w32; vtlbMemW64FP* w64; vtlbMemW128FP* w128;
} handler_set;
static handler_set s_sets[18];
static u32 s_nsets;
vtlbHandler vtlb_RegisterHandler(
	vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	handler_set* h = &s_sets[s_nsets];
	h->r8 = r8; h->r16 = r16; h->r32 = r32; h->r64 = r64; h->r128 = r128;
	h->w8 = w8; h->w16 = w16; h->w32 = w32; h->w64 = w64; h->w128 = w128;
	return s_nsets++;
}
vtlbHandler vtlb_NewHandler(void) { return s_nsets++; }
void vtlb_ReassignHandler(vtlbHandler rv,
	vtlbMemR8FP* r8, vtlbMemR16FP* r16, vtlbMemR32FP* r32, vtlbMemR64FP* r64, vtlbMemR128FP* r128,
	vtlbMemW8FP* w8, vtlbMemW16FP* w16, vtlbMemW32FP* w32, vtlbMemW64FP* w64, vtlbMemW128FP* w128)
{
	handler_set* h = &s_sets[rv];
	h->r8 = r8; h->r16 = r16; h->r32 = r32; h->r64 = r64; h->r128 = r128;
	h->w8 = w8; h->w16 = w16; h->w32 = w32; h->w64 = w64; h->w128 = w128;
}

#ifdef EEHW_CXX_TREE
/* The C++ reached the GIF and the VIFs directly. */
alignas(16) vifStruct vif0, vif1;
alignas(16) gifStruct gif;
alignas(16) u8 s_gifunit[sizeof(Gif_Unit)];
asm(".globl gifUnit\n.set gifUnit, s_gifunit");
alignas(16) u8 s_giffifo[sizeof(GIF_Fifo)];
asm(".globl gif_fifo\n.set gif_fifo, s_giffifo");
void GIF_Fifo::init() { answer(0x69, 0, 0); }
Gif_Path_ReadAmount gifPathReadAmount[3];
u8 g_RealGSMem[0x2000];
void retro_spsc_clear(struct retro_spsc* q) { (void)q; }
template <> u32 vifRead32<0>(u32 mem) { return answer(0x80, mem, 0); }
template <> u32 vifRead32<1>(u32 mem) { return answer(0x81, mem, 0); }
template <> bool vifWrite32<0>(u32 mem, u32 value) { return (answer(0x82, mem, value) & 3) != 0; }
template <> bool vifWrite32<1>(u32 mem, u32 value) { return (answer(0x83, mem, value) & 3) != 0; }
#else
u32 vif0Read32(u32 mem) { return answer(0x80, mem, 0); }
u32 vif1Read32(u32 mem) { return answer(0x81, mem, 0); }
bool vif0Write32(u32 mem, u32 value) { return (answer(0x82, mem, value) & 3) != 0; }
bool vif1Write32(u32 mem, u32 value) { return (answer(0x83, mem, value) & 3) != 0; }
bool emu_config_ophflag_hack(void) { return s_oph; }
bool emu_config_dmabusy_hack(void) { return s_dmabusy; }
bool emu_config_viffifo_hack(void) { return s_viffifo; }
void gifHwReset(void) { answer(0x6a, 0, 0); }
void gifFifoReset(void) { answer(0x69, 0, 0); }

/* What Gif.cpp now does for the GIF's own registers; the C++ did it in
 * Hw.cpp and Dmac.cpp, so the harness carries the same steps for the C
 * build to call. GIF_MODE's kick checks GIF DMA and FIFO state the stub
 * world does not have; both are zero here, as in the C++ run. */
#define GIF_STAT_W (*(u32*)&eeHw[0x3020])
u32 gifRead32(u32 mem)
{
	if (mem == GIF_STAT && s_oph)
	{
		static unsigned counter = 1;
		if (++counter == 8)
			counter = 2;
		return (GIF_STAT_W & ~(7 << 9)) | ((counter & 1) ? (counter << 9) : 0);
	}
	return psHu32(mem);
}
bool gifWrite32(u32 mem, u32 value)
{
	u32* ctrl = (u32*)&eeHw[0x3000];
	u32* mode = (u32*)&eeHw[0x3010];
	switch (mem)
	{
		case GIF_CTRL:
			*ctrl = value & 9;
			if (*ctrl & 1)
				answer(0x6b, 1, 0); /* gifUnit.Reset(true) */
			GIF_STAT_W = (GIF_STAT_W & ~(1u << 3)) | (*ctrl & (1u << 3));
			return false;
		case GIF_MODE:
			*mode = value;
			if ((GIF_STAT_W & 1) && !(*mode & 1) && (psHu32(D2_CHCR) & 0x100))
				CPU_INT(DMAC_GIF, 8);
			GIF_STAT_W = (GIF_STAT_W & ~1u) | (*mode & 1);
			GIF_STAT_W = (GIF_STAT_W & ~(1u << 2)) | (*mode & (1u << 2));
			return false;
		default:
			return true;
	}
}
#endif

/* ---- the drive ----------------------------------------------------- */

static const u32 kRegs[] = {
	INTC_STAT, INTC_MASK, SIO_TXFIFO, SIO_ISR, SBUS_F200, SBUS_F220, SBUS_F230, SBUS_F240, SBUS_F260,
	MCH_RICM, MCH_DRD, 0x1000f410, DMAC_CTRL, DMAC_STAT, DMAC_PCR, DMAC_SQWC, DMAC_RBSR, DMAC_RBOR,
	DMAC_STADR, DMAC_ENABLER, DMAC_ENABLEW, DMAC_FAKESTAT,
	D0_CHCR, D1_CHCR, D2_CHCR, D3_CHCR, D4_CHCR, D5_CHCR, D6_CHCR, D7_CHCR, D8_CHCR, D9_CHCR,
	D0_QWC, D1_QWC, D2_QWC, D3_QWC, D4_QWC, D5_QWC, D6_QWC, D7_QWC, D8_QWC, D9_QWC,
	D1_CHCR + 0x10, fromSPR_MADR, toSPR_MADR, fromSPR_SADR, toSPR_SADR,
	0x10003000, 0x10003010, 0x10003020, 0x10003800, 0x10003c00, 0x10003c10,
	0x10004000, 0x10005000, 0x10006000, 0x10007000, 0x10007010,
	0x10000000, 0x10000800, 0x10002000, 0x10002010,
	0x1000f300, 0x1000f310, 0x1000f380, 0x1000f3c0, 0x1000f3e0,
};

static u32 pick_addr(void)
{
	u32 a;
	if (rnd_n(4) != 0)
		a = kRegs[rnd_n(sizeof(kRegs) / sizeof(kRegs[0]))];
	else
		a = 0x10000000u | (rnd_n(16) << 12) | (rnd_n(0x1000));
	return a;
}

static void mix_state(void)
{
	mix_bytes(eeHw, sizeof(eeHw));
	mix(cpuRegs.interrupt);
	mix(cpuRegs.dmastall);
	mix(cpuRegs.cycle);
	mix(psxRegs.cycle);
	mix((u64)PSXCLK);
	mix(counters[0].hold);
	mix(counters[1].hold);
	mix_bytes(&iopHw[0x1000], 0x100);
	mix_bytes(&iopHw[0x1400], 0x100);
}

static handler_set* page_set(u32 addr)
{
	return &s_sets[(addr >> 12) & 0xf];
}

static void access(void)
{
	const u32 addr = pick_addr();
	handler_set* h = page_set(addr);
	const u32 kind = rnd_n(10);
	alignas(16) u128 v;
	u64 v64;

	/* GIF_CTRL's reset bit reset the GIF unit inside the C++ handler; the
	 * harness has no unit, so that one bit is left clear. */
	if (kind >= 5 && (addr & ~0xfu) == GIF_CTRL)
	{
		h->w32(GIF_CTRL, rnd() & 0x8);
		return;
	}

	switch (kind)
	{
		case 0: mix(h->r8(addr)); break;
		case 1: mix(h->r16(addr & ~1u)); break;
		case 2: mix(h->r32(addr & ~3u)); mix(cpuRegs.cycle); break;
		case 3: mix(h->r64(addr & ~7u)); break;
		case 4:
		{
			r128 r = h->r128(addr & ~15u);
			r128_store(&v, r);
			mix_bytes(&v, 16);
			break;
		}
		case 5: h->w8(addr, (u8)rnd()); break;
		case 6: h->w16(addr & ~1u, (u16)rnd()); break;
		case 7:
		case 8:
			h->w32(addr & ~3u, rnd_n(3) == 0 ? rnd() : rnd() & 0x1ff);
			break;
		default:
			if (rnd_n(2))
			{
				v64 = ((u64)rnd() << 32) | rnd();
				h->w64(addr & ~7u, v64);
			}
			else
			{
				v._u32[0] = rnd(); v._u32[1] = rnd(); v._u32[2] = rnd(); v._u32[3] = rnd();
#if PCSX2_MINGW_R128_BY_PTR
				{
					r128 r = r128_load(&v);
					h->w128(addr & ~15u, &r);
				}
#else
				h->w128(addr & ~15u, r128_load(&v));
#endif
			}
			break;
	}
}

static void step(void)
{
	alignas(16) u128 data[8];
	u32 i;

	switch (rnd_n(16))
	{
		case 0: mix(intcInterrupt()); break;
		case 1: mix(dmacInterrupt()); break;
		case 2: hwIntcIrq((int)rnd_n(15)); break;
		case 3: hwDmacIrq((int)rnd_n(16)); break;
		case 4:
			/* The MFIFO ring inside main memory, a short burst into it. */
			for (i = 0; i < 8; i++)
			{
				data[i]._u32[0] = rnd(); data[i]._u32[1] = rnd();
				data[i]._u32[2] = rnd(); data[i]._u32[3] = rnd();
			}
			psHu32(DMAC_RBOR) = (rnd_n(0x1000) << 12) | 0x100000;
			psHu32(DMAC_RBSR) = (rnd_n(0x40) << 4) | 0xf;
			mix(hwMFIFOWrite(rnd(), data, rnd_n(9)));
			mix_bytes(s_physmem + (psHu32(DMAC_RBOR) & 0x1fffff00), 0x500);
			break;
		case 5: set_hacks(); break;
		case 6:
			/* The VIF1 status VEW bit the busy hack looks at. */
			psHu32(0x10003c00) ^= rnd_n(2) << 6;
			break;
		case 7:
			/* The clocks the INTC fast-forward and the PS1 switch read. */
			cpuRegs.cycle          = rnd_n(2000);
			cpuRegs.nextEventCycle = rnd_n(2000);
			cpuRegs.lastEventCycle = rnd_n(2000);
			psxRegs.cycle          = rnd();
			break;
		case 8:
		{
			/* An RDRAM command for MCH_DRD to answer. */
			static const u32 cmds[] = { 0x21, 0x23, 0x24, 0x40, 0x11 };
			s_sets[15].w32(MCH_RICM, (cmds[rnd_n(5)] << 16) | (rnd_n(2) << 6) | rnd_n(32));
			if (rnd_n(2))
				psHu32(0xf440) = rnd() & 0x80;
			break;
		}
		default:
			access();
			break;
	}
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 400; scen++)
	{
		memset(eeHw, 0, sizeof(eeHw));
		memset(&cpuRegs, 0, sizeof(cpuRegs));
		memset(iopHw, 0, sizeof(iopHw));
		psHu32(DMAC_ENABLEW) = 0x1201;
		psHu32(DMAC_ENABLER) = 0x1201;
		set_hacks();
		for (i = 0; i < 400; i++)
			step();
		mix_state();
		if ((scen + 1) % 10 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
}

/* ---- what the conversion fixed ------------------------------------- */

#ifndef EEHW_CXX_TREE
static int s_failed;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eehw: %s\n", what);
		s_failed = 1;
	}
}

static void fixes(void)
{
	alignas(16) u128 data[2];
	u32 tail[4];

	/* An MFIFO ring that runs past the end of main RAM takes nothing: the
	 * C++ checked only its base and wrote on past the mapping. */
	memset(eeHw, 0, sizeof(eeHw));
	memset(data, 0x77, sizeof(data));
	memset(s_physmem + PS2MEM_MAIN_RAM - 16, 0x11, 16);
	psHu32(DMAC_RBOR) = PS2MEM_MAIN_RAM - 0x100;
	psHu32(DMAC_RBSR) = 0x3ff0;
	expect(!hwMFIFOWrite(PS2MEM_MAIN_RAM - 0x10, data, 2), "an MFIFO ring past main RAM is refused");
	memcpy(tail, s_physmem + PS2MEM_MAIN_RAM - 16, 16);
	expect(tail[0] == 0x11111111u, "an MFIFO ring past main RAM is left alone");

	/* One inside it still takes the burst, wrapping at its end. */
	psHu32(DMAC_RBOR) = 0x200000;
	psHu32(DMAC_RBSR) = 0xf0;
	expect(hwMFIFOWrite(0x2000f0, data, 2), "an MFIFO ring in main RAM takes a burst");
	expect(s_physmem[0x2000f0] == 0x77 && s_physmem[0x200000] == 0x77 && s_physmem[0x200010] != 0x77,
		"an MFIFO burst wraps at the ring's end");

	/* hwReset resets every unit, in the order the C++ did. */
	{
		const unsigned long long before = s_hash;
		unsigned long long expect_hash;
		s_hash = 1;
		answer(0x60, false, 0); answer(0x65, 0, 0); answer(0x66, 0, 0); answer(0x6a, 0, 0);
		answer(0x38, 0, 0); answer(0x67, 0, 0); answer(0x68, 0, 0); answer(0x69, 0, 0);
		answer(0x33, 0, 0); answer(0x63, 0, 0); answer(0x64, 0, 0);
		expect_hash = s_hash;
		s_hash = 1;
		hwReset();
		expect(s_hash == expect_hash, "hwReset resets the units in order");
		expect(psHu32(DMAC_ENABLER) == 0x1201 && psHu32(SBUS_F260) == 0x1D000060, "hwReset seeds the registers");
		s_hash = before;
	}
}
#endif

int main(int argc, char** argv)
{
	FILE* out;
	u32 p;

	s_physmem = (u8*)calloc(1, PS2MEM_MAIN_RAM);
	if (!s_physmem || argc < 2)
		return 2;
	for (p = 0; p < 15; p++)
		hwRegisterPageHandlers(p);
	s_nsets = 15;
	hwRegisterPageHandlers(15);
	hwAssignPage0FHandlers(15, false);
	out = fopen(argv[1], "w");
	if (!out)
		return 2;
	run(out);
	/* The page 0xf set with the INTC_STAT fast-forward, briefly. */
	hwAssignPage0FHandlers(15, true);
	cpuRegs.nextEventCycle = 1000;
	cpuRegs.lastEventCycle = 0;
	cpuRegs.cycle = 100;
	mix(s_sets[15].r32(INTC_STAT));
	mix(s_sets[15].r16(INTC_STAT));
	mix(cpuRegs.cycle);
	fprintf(out, "intc %016llx\n", s_hash);
	fclose(out);
#ifndef EEHW_CXX_TREE
	fixes();
	free(s_physmem);
	return s_failed;
#else
	free(s_physmem);
	return 0;
#endif
}
