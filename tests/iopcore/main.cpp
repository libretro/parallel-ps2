/* The IOP's CPU core: the interpreter (pcsx2/R3000AInterpreter.c), its
 * opcode tables (R3000AOpcodeTables.c), the event and exception logic
 * (R3000A.c) and the module-import HLE (IopBios.c), driven the way the EE
 * drives the IOP.
 *
 * Seeded random programs run block after block through the interpreter,
 * with interrupts raised and events scheduled between blocks; then the
 * HLE's host: file calls run through import stubs in IOP memory, on a
 * scratch directory. Registers, memory, every outward call and every
 * value the guest sees go into the transcript, which is compared with
 * expected.txt, made by this harness from the C++ these units were
 * converted from (IOPCORE_TREE=<that checkout>). Where the C behaves
 * differently on purpose the transcript leaves the difference out, and
 * the C build checks it on its own afterwards: what remove returns, and
 * what a failed stat or the end of a directory hands the guest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>

#include <libretro.h>
#include "Common.h"
#include "R3000A.h"
#include "R5900.h"
#include "IopMem.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopCounters.h"
#include "IopBios.h"
#include "Sif.h"
#include "Sio.h"
#include "FW.h"
#include "CDVD/CDVD.h"
#include "CDVD/Ps1CD.h"
#include "MemoryCardFile.h"
#ifdef IOPCORE_CXX_TREE
#include "Config.h"
#include "DEV9/DEV9.h"
using namespace R3000A;
#define IOMAN_RESET() R3000A::ioman::reset()
#else
#include "DEV9/DEV9irq.h"
#define IOMAN_RESET() irxIomanReset()
#endif

/* ---- the record ---------------------------------------------------- */

static unsigned long long s_hash = 1469598103934665603ULL;
static FILE* s_out;

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

static void tag(u32 t) { mix(t); }

/* ---- IOP memory: one 64 KiB page, mirrored everywhere --------------- */

#define MEMSIZE 0x10000
static u8 s_mem[MEMSIZE + 16];
static uptr s_rlut[0x10000];
const uptr* psxMemRLUT = s_rlut;

u8 iopMemRead8_slow(u32 a) { return s_mem[a & 0xffff]; }
u16 iopMemRead16_slow(u32 a) { u16 v; memcpy(&v, &s_mem[a & 0xfffe], 2); return v; }
u32 iopMemRead32_slow(u32 a) { u32 v; memcpy(&v, &s_mem[a & 0xfffc], 4); return v; }
void iopMemWrite8(u32 a, u8 v) { s_mem[a & 0xffff] = v; }
void iopMemWrite16(u32 a, u16 v) { memcpy(&s_mem[a & 0xfffe], &v, 2); }
void iopMemWrite32(u32 a, u32 v) { memcpy(&s_mem[a & 0xfffc], &v, 4); }
void iopMemReadStringBuf(char* dst, int cap, u32 mem, int maxlen)
{
	int n = 0;
	if (cap <= 0)
		return;
	while (n < cap - 1 && n < maxlen)
	{
		const char c = (char)s_mem[(mem + n) & 0xffff];
		if (!c)
			break;
		dst[n++] = c;
	}
	dst[n] = 0;
}

/* ---- the machine around the core ------------------------------------- */

static void quiet(enum retro_log_level, const char*, ...) {}
extern "C" retro_log_printf_t log_cb;
retro_log_printf_t log_cb = quiet;

psxRegisters psxRegs;
cpuRegisters cpuRegs;
alignas(4096) u8 iopHw[PS2MEM_IOP_HARDWARE];
int iopIsDelaySlot;
s32 psxNextDeltaCounter;
u32 psxNextStartCounter;
s64 PSXCLK = 36864000;
bool g_GameStarted;
retro_atomic_int_t eeEventTestIsActive = RETRO_ATOMIC_INT_INITIALIZER(0);

static void cpu_clear(u32 addr, u32 size) { tag(0x01); mix(addr); mix(size); }
static R3000Acpu s_rec = { NULL, NULL, NULL, cpu_clear, NULL };
R3000Acpu psxRec;

void psxHwReset(void) { tag(0x10); }
void psxBiosReset(void) { tag(0x11); }
bool psxBiosCall(void) { tag(0x12); mix(psxRegs.pc); return false; }
void psxRcntUpdate(void) { tag(0x13); psxNextStartCounter = psxRegs.cycle; psxNextDeltaCounter = 700 + (psxRegs.cycle % 300); }
void sio0_interrupt(Sio0Interrupt why) { tag(0x14); mix(why); }
void sif0Interrupt(void) { tag(0x15); }
void sif1Interrupt(void) { tag(0x16); }
void sif2Interrupt(void) { tag(0x17); }
void cdvdSectorReady(void) { tag(0x18); }
void cdvdReadInterrupt(void) { tag(0x19); }
void cdvdActionInterrupt(void) { tag(0x1a); }
void psxDMA11Interrupt(void) { tag(0x1b); }
void psxDMA12Interrupt(void) { tag(0x1c); }
void cdrInterrupt(void) { tag(0x1d); }
void cdrReadInterrupt(void) { tag(0x1e); }
void dev9Interrupt(void) { tag(0x1f); }
void usbInterrupt(void) { tag(0x20); }
void FWinterrupt(void) { tag(0x21); }
/* Pcsx2Config's constructor names the cards. */
bool FileMcd_IsMultitapSlot(uint slot) { return slot > 1; }
void FileMcd_GetDefaultName(char* out, size_t out_size, uint slot) { snprintf(out, out_size, "card%u", slot); }

/* ---- part one: programs ---------------------------------------------- */

static u32 s_rng = 0x9e3779b9u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

/* An instruction the IOP might run: any ALU, shift, multiply, divide,
 * COP0 and GTE word; loads and stores off $zero, so their address is the
 * immediate and aligned to the access; branches and jumps. Never JR or
 * JALR, whose target is a register's value and could be unaligned, and
 * never a load or store off a register for the same reason.
 *
 * The page is code up to 0xc000, then a band of jumps back into it that a
 * forward branch from the end of the code lands in, then data from 0xc400
 * that the stores write: what they write is never run as code. */
static u32 instr(void)
{
	const u32 r = rnd();
	const u32 rs = (r >> 21) & 31, rt = (r >> 16) & 31;
	switch (rnd() % 16)
	{
		case 0: case 1: case 2: /* SPECIAL, ALU/shift/muldiv/mf/mt */
		{
			static const u8 fn[] = { 0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x10, 0x11, 0x12, 0x13,
			                         0x18, 0x19, 0x1a, 0x1b, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
			                         0x26, 0x27, 0x2a, 0x2b, 0x0c, 0x0d };
			return (r & 0x03fff7c0) | fn[rnd() % sizeof(fn)];
		}
		case 3: case 4: case 5: /* immediate ALU */
			return ((8 + rnd() % 8) << 26) | (r & 0x03ffffff);
		case 6: /* loads off $zero */
		{
			static const u8 op[] = { 0x20, 0x21, 0x23, 0x24, 0x25, 0x22, 0x26 };
			const u32 o = op[rnd() % sizeof(op)];
			const u32 align = (o == 0x21 || o == 0x25) ? ~1u : (o == 0x23) ? ~3u : ~0u;
			return (o << 26) | (rt << 16) | (r & 0x7fff & align);
		}
		case 7: /* stores off $zero, into the data at 0xc400 up */
		{
			static const u8 op[] = { 0x28, 0x29, 0x2b, 0x2a, 0x2e };
			const u32 o = op[rnd() % sizeof(op)];
			const u32 align = (o == 0x29) ? ~1u : (o == 0x2b) ? ~3u : ~0u;
			return (o << 26) | (rt << 16) | ((0xc400 | (r & 0x3bff)) & align);
		}
		case 8: /* BEQ, BNE, BLEZ, BGTZ */
			return ((4 + rnd() % 4) << 26) | (rs << 21) | (rt << 16) | (r & 0xff);
		case 9: /* REGIMM: BLTZ, BGEZ, BLTZAL, BGEZAL */
		{
			static const u8 rtv[] = { 0x00, 0x01, 0x10, 0x11 };
			return (1u << 26) | (rs << 21) | ((u32)rtv[rnd() % 4] << 16) | (r & 0xff);
		}
		case 10: /* J, JAL, into the code */
			return ((2 + rnd() % 2) << 26) | (r & 0x2fff);
		case 11: /* COP0 moves and RFE */
		{
			const u32 k = rnd() % 4;
			if (k == 3)
				return 0x42000010;
			return (0x10u << 26) | ((k == 0 ? 0u : k == 1 ? 4u : 2u) << 21) | (rt << 16) | ((rnd() % 16) << 11);
		}
		case 12: /* GTE */
			return (0x12u << 26) | (r & 0x03ffffff);
		case 13: /* SYSCALL and BREAK, now and then */
			return (rnd() % 8) ? 0 : ((rnd() & 1) ? 0x0000000c : 0x0000000d);
		default:
		{
			/* anything else, but no load, store, branch or jump with an
			 * address the generator does not control */
			u32 w = r;
			while ((w >> 26) >= 0x20 || ((w >> 26) >= 1 && (w >> 26) <= 7)
			       || ((w >> 26) == 0 && ((w & 0x3f) == 0x08 || (w & 0x3f) == 0x09)))
				w = rnd();
			return w;
		}
	}
}

static void mix_cpu(void)
{
	mix_bytes(psxRegs.GPR.r, sizeof(psxRegs.GPR.r));
	mix_bytes(psxRegs.CP0.r, sizeof(psxRegs.CP0.r));
	mix_bytes(psxRegs.CP2D.r, sizeof(psxRegs.CP2D.r));
	mix_bytes(psxRegs.CP2C.r, sizeof(psxRegs.CP2C.r));
	mix(psxRegs.pc);
	mix(psxRegs.code);
	mix(psxRegs.cycle);
	mix(psxRegs.interrupt);
	mix(psxRegs.iopNextEventCycle);
	mix((u32)psxRegs.iopCycleEE);
	mix(psxRegs.iopBreak);
	mix(cpuRegs.nextEventCycle);
	mix(iopEventAction);
	mix(psxHu32(0x1070));
}

static void programs(void)
{
	static const IopEventId events[] = { IopEvt_SIF0, IopEvt_SIF1, IopEvt_SIF2, IopEvt_SIO,
		IopEvt_CdvdSectorReady, IopEvt_CdvdRead, IopEvt_Cdvd, IopEvt_Dma11, IopEvt_Dma12,
		IopEvt_Cdrom, IopEvt_CdromRead, IopEvt_DEV9, IopEvt_USB, IopEvt_FW };
	int p, b;
	u32 i;

	psxCpu = &psxInt;
	for (p = 0; p < 400; p++)
	{
		for (i = 0; i < MEMSIZE; i += 4)
		{
			const u32 w = (i < 0xc000) ? instr() : (i < 0xc400) ? 0x08000100u : rnd();
			memcpy(&s_mem[i], &w, 4);
		}
		/* The exception vectors jump away at once: a SYSCALL or BREAK
		 * there would take the exception again with no branch between,
		 * and the interpreter runs to its next taken branch. */
		{
			const u32 j0 = (2u << 26) | ((rnd() & 0x2ff0) | 0x400), j1 = (2u << 26) | ((rnd() & 0x2ff0) | 0x800);
			const u32 nop = 0;
			memcpy(&s_mem[0x80], &j0, 4);
			memcpy(&s_mem[0x84], &nop, 4);
			memcpy(&s_mem[0x180], &j1, 4);
			memcpy(&s_mem[0x184], &nop, 4);
		}
		memset(iopHw, 0, sizeof(iopHw));
		psxReset();
		psxRegs.pc = 0x80000000u | ((rnd() % 0xc000) & ~3u);
		psxRegs.CP0.n.Status = (rnd() & 1) ? 0x00400401 : 0x00000401;
		for (i = 1; i < 32; i++)
			psxRegs.GPR.r[i] = rnd();
		psxNextStartCounter = psxRegs.cycle;
		psxNextDeltaCounter = 1000;
		cpuRegs.cycle = 0;
		cpuRegs.nextEventCycle = 0x7fffffff;
		for (b = 0; b < 40; b++)
		{
			const u32 r = rnd();
			retro_atomic_store_release_int(&eeEventTestIsActive, (int)(r & 1));
			if (r & 2)
				PSX_INT(events[(r >> 8) % (sizeof(events) / sizeof(events[0]))], (s32)((r >> 16) % 600));
			if ((r & 0x1c) == 0x1c)
			{
				psxHu32(0x1070) |= 1u << ((r >> 5) % 26);
				psxHu32(0x1074) = rnd();
				psxHu32(0x1078) = rnd() & 1;
				iopTestIntc();
				mix(psxRegs.iopNextEventCycle);
				mix(cpuRegs.nextEventCycle);
			}
			if ((r & 0x60) == 0x60)
				psxHu32(HW_ICFG) ^= 8;
			mix(psxRemainingCycles(events[(r >> 12) % (sizeof(events) / sizeof(events[0]))]));
			mix((u32)psxInt.ExecuteBlock(64 + (s32)(r % 3000)));
			mix_cpu();
		}
		if ((p + 1) % 40 == 0)
		{
			mix_bytes(s_mem, MEMSIZE);
			fprintf(s_out, "programs %d: %016llx\n", p + 1, s_hash);
		}
	}
	psxSetNextBranchDelta(5);
	mix(psxRegs.iopNextEventCycle);
	fprintf(s_out, "programs: %016llx\n", s_hash);
}

/* ---- part two: the HLE's host: files ---------------------------------- */

static char s_dir[512];

/* An import table at 0x8000 naming module @name, and the stub pc whose
 * hook resolves through it. */
static u32 module(const char* name)
{
	static const u32 magic = 0x41e00000;
	char padded[8];
	memset(&s_mem[0x8000], 0, 0x40);
	memcpy(&s_mem[0x8000], &magic, 4);
	memset(padded, 0, sizeof(padded));
	memcpy(padded, name, strlen(name));
	memcpy(&s_mem[0x8000 + 12], padded, 8);
	return 0x8020;
}

static void guest_string(u32 at, const char* s)
{
	memcpy(&s_mem[at], s, strlen(s) + 1);
}

/* One call through the stub: what it returned, what the guest got in v0
 * and where it goes back to. show: bit 0 the return, bit 1 v0 -- each
 * left out where the C answers differently on purpose. */
#define SHOW_ALL 3
#define SHOW_V0  2
#define SHOW_RET 1
static int call(const char* what, const char* mod, u16 index, u32 arg0, u32 arg1, u32 arg2, int show)
{
	int ret;
	const u32 stub = module(mod);
	psxRegs.GPR.n.a0 = arg0;
	psxRegs.GPR.n.a1 = arg1;
	psxRegs.GPR.n.a2 = arg2;
	psxRegs.GPR.n.v0 = 0xdeadbeef;
	psxRegs.GPR.n.ra = 0x1234;
	psxRegs.pc = stub;
	ret = irxImportExecCached(stub, index);
	fprintf(s_out, "%s: ret %d v0 %d pc %x\n", what, (show & SHOW_RET) ? ret : -1,
		(show & SHOW_V0) ? (s32)psxRegs.GPR.n.v0 : -1, psxRegs.pc);
	return ret;
}

/* A stat as the guest got it. Left out: ctime, which is the moment of the
 * run's last change, and the bytes the HLE never writes -- attr, each
 * time's first and last byte, the iomanX private words -- which the C++
 * left as whatever its stack held. atime and mtime are set to a fixed time
 * before every stat. */
static void show_stat(const char* what, u32 at, bool x, bool valid)
{
	u8 st[64];
	const size_t n = x ? 64 : 40;
	size_t i;
	memcpy(st, &s_mem[at], n);
	if (!valid)
	{
		fprintf(s_out, "%s: (not shown)\n", what);
		return;
	}
	memset(st + 4, 0, 4);   /* attr */
	memset(st + 12, 0, 8);  /* ctime */
	st[20] = st[27] = 0;    /* atime's unwritten ends */
	st[28] = st[35] = 0;    /* mtime's */
	if (x)
		memset(st + 40, 0, 24);
	fprintf(s_out, "%s:", what);
	for (i = 0; i < n; i++)
		fprintf(s_out, "%s%02x", (i % 4) ? "" : " ", st[i]);
	fputc('\n', s_out);
}

static void fix_times(const char* rel)
{
	char path[1024];
	struct utimbuf t;
	snprintf(path, sizeof(path), "%s/%s", s_dir, rel);
	t.actime = 1000000000;
	t.modtime = 1100000000;
	utime(path, &t);
}

static void hle(void)
{
	int i, fd, dir;
	char elf[600];
	const u32 path = 0x9000, buf = 0x2000;

	snprintf(elf, sizeof(elf), "%s/game.elf", s_dir);
	Hle_SetElfPath(elf);
	g_GameStarted = false;
	IOMAN_RESET();

	guest_string(path, "host:sub");
	call("mkdir host:sub", "ioman", 11, path, 0, 0, SHOW_ALL);
	call("mkdir host:sub again", "iomanx", 11, path, 0, 0, SHOW_ALL);

	/* a file written in one call larger than the C's chunk */
	for (i = 0; i < 0x6000; i++)
		s_mem[buf + i] = (u8)(i * 7 + 3);
	guest_string(path, "host:sub/f.bin");
	fd = (s32)(call("open f.bin for writing", "ioman", 4, path, IOP_O_WRONLY | IOP_O_CREAT | IOP_O_TRUNC, 0, SHOW_ALL), psxRegs.GPR.n.v0);
	call("write 0x6000", "ioman", 7, (u32)fd, buf, 0x6000, SHOW_ALL);
	for (i = 0; i < 3; i++)
		call("write 0x6000 again", "ioman", 7, (u32)fd, buf, 0x6000, SHOW_ALL);
	call("close", "ioman", 5, (u32)fd, 0, 0, SHOW_ALL);
	call("close it again", "ioman", 5, (u32)fd, 0, 0, SHOW_ALL);

	fd = (s32)(call("open f.bin to read", "iomanx", 4, path, IOP_O_RDONLY, 0, SHOW_ALL), psxRegs.GPR.n.v0);
	call("lseek end", "ioman", 8, (u32)fd, 0, IOP_SEEK_END, SHOW_RET);
	call("lseek 0x5ff0", "ioman", 8, (u32)fd, 0x5ff0, IOP_SEEK_SET, SHOW_RET);
	call("lseek bad whence", "ioman", 8, (u32)fd, 0, 9, SHOW_ALL);
	memset(&s_mem[buf], 0, 0x6000);
	call("read 0x40", "ioman", 6, (u32)fd, buf, 0x40, SHOW_ALL);
	mix_bytes(&s_mem[buf], 0x40);
	call("read 0x5000", "ioman", 6, (u32)fd, buf, 0x5000, SHOW_ALL);
	mix_bytes(&s_mem[buf], 0x5000);
	call("read past the end", "ioman", 6, (u32)fd, buf, 0x6000 * 4, SHOW_ALL);
	mix_bytes(&s_mem[buf], 0x6000);
	call("close", "ioman", 5, (u32)fd, 0, 0, SHOW_ALL);

	guest_string(path, "host:sub/f.bin");
	call("open f.bin to append", "ioman", 4, path, IOP_O_WRONLY | IOP_O_APPEND, 0, SHOW_ALL);
	fd = (s32)psxRegs.GPR.n.v0;
	call("write 3", "ioman", 7, (u32)fd, buf, 3, SHOW_ALL);
	call("lseek cur", "ioman", 8, (u32)fd, 0, IOP_SEEK_CUR, SHOW_RET);
	call("close", "ioman", 5, (u32)fd, 0, 0, SHOW_ALL);
	call("write to stdout", "ioman", 7, 1, path, 5, SHOW_ALL);

	fix_times("sub/f.bin");
	fix_times("sub");
	memset(&s_mem[buf], 0, 0x100);
	guest_string(path, "host:sub/f.bin");
	call("getstat f.bin", "ioman", 16, path, buf, 0, SHOW_ALL);
	show_stat("  stat", buf, false, SHOW_ALL);
	call("getstatx f.bin", "iomanx", 16, path, buf, 0, SHOW_ALL);
	show_stat("  statx", buf, true, SHOW_ALL);
	guest_string(path, "host:sub");
	call("getstat sub", "ioman", 16, path, buf, 0, SHOW_ALL);
	show_stat("  stat", buf, false, SHOW_ALL);
	guest_string(path, "host:nothing");
	call("getstat a missing file", "ioman", 16, path, buf, 0, SHOW_ALL);
	show_stat("  stat", buf, false, false);

	guest_string(path, "host:sub");
	dir = (s32)(call("dopen sub", "ioman", 13, path, 0, 0, SHOW_ALL), psxRegs.GPR.n.v0);
	for (i = 0; i < 2; i++)
	{
		const bool x = i == 1;
		call(x ? "dreadx" : "dread", x ? "iomanx" : "ioman", 15, (u32)dir, buf, 0, SHOW_ALL);
		if ((s32)psxRegs.GPR.n.v0 == 1)
		{
			show_stat("  entry stat", buf, x, SHOW_ALL);
			fprintf(s_out, "  entry name %s\n", (const char*)&s_mem[buf + (x ? 64 : 40)]);
		}
	}
	call("dread at the end", "ioman", 15, (u32)dir, buf, 0, SHOW_ALL);
	call("dclose", "ioman", 14, (u32)dir, 0, 0, SHOW_ALL);
	call("dclose again", "ioman", 14, (u32)dir, 0, 0, SHOW_ALL);
	call("close a dir fd as a file", "ioman", 5, (u32)dir, 0, 0, SHOW_ALL);

	/* outside the elf's directory, and the root itself */
	guest_string(path, "host:../escape.bin");
	call("open outside", "ioman", 4, path, IOP_O_WRONLY | IOP_O_CREAT, 0, SHOW_ALL);
	guest_string(path, "host:");
	call("open the root as a file", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);
	guest_string(path, "host3:.");
	call("dopen the root", "ioman", 13, path, 0, 0, SHOW_ALL);
	call("dclose it", "ioman", 14, (u32)psxRegs.GPR.n.v0, 0, 0, SHOW_ALL);
	guest_string(path, "host:sub\\f.bin");
	call("open with a backslash", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);
	call("close it", "ioman", 5, (u32)psxRegs.GPR.n.v0, 0, 0, SHOW_ALL);
	guest_string(path, "cdrom0:\\SYSTEM.CNF");
	call("not host:", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);

	/* every fd taken */
	guest_string(path, "host:sub/f.bin");
	for (i = 0; i < 0x101; i++)
	{
		psxRegs.GPR.n.a0 = path;
		psxRegs.GPR.n.a1 = IOP_O_RDONLY;
		irxImportExecCached(module("ioman"), 4);
		mix(psxRegs.GPR.n.v0);
	}
	fprintf(s_out, "fds: last v0 %d\n", (s32)psxRegs.GPR.n.v0);
	IOMAN_RESET();
	call("open after reset", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);
	IOMAN_RESET();

	/* host: closed to a running game */
	g_GameStarted = true;
	call("open while a game runs", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);
	g_GameStarted = false;

	guest_string(path, "host:sub");
	call("rmdir a full directory", "ioman", 12, path, 0, 0, SHOW_ALL);
	call("remove a directory", "ioman", 10, path, 0, 0, SHOW_V0);
	guest_string(path, "host:sub/f.bin");
	call("rmdir a file", "ioman", 12, path, 0, 0, SHOW_ALL);
	call("remove f.bin", "ioman", 10, path, 0, 0, SHOW_V0);
	call("remove it again", "ioman", 10, path, 0, 0, SHOW_V0);
	guest_string(path, "host:sub");
	call("rmdir sub", "ioman", 12, path, 0, 0, SHOW_ALL);

	/* the other modules */
	psxRegs.GPR.n.sp = 0x7000;
	psxRegs.GPR.n.a3 = 0x33333333;
	call("Kprintf", "sysmem", 14, 0x11111111, 0x22222222, 0x44444444, SHOW_ALL);
	mix_bytes(&s_mem[0x7000], 16);
	call("RegisterLibraryEntries", "loadcore", 6, 0, 0, 0, SHOW_ALL);
	call("RegisterIntrHandler", "intrman", 4, 0, 0, 0, SHOW_ALL);
	call("sceSifRegisterRpc", "sifcmd", 17, 0, 0, 0, SHOW_ALL);
	call("an export nothing handles", "ioman", 99, 0, 0, 0, SHOW_ALL);
	mix(irxImportExec(0, 4));
	mix(irxImportExec(0x8000, 99));
	{
		u32 n0, n1;
		module("iomanx");
		irxImportName(0x8000, &n0, &n1);
		mix(n0);
		mix(n1);
		mix(irxImportTableAddr(0x8020));
		mix(irxImportTableAddr(0x4000));
	}
	Hle_ClearElfPath();
	guest_string(path, "host:anything");
	call("open with no elf path", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);
	fprintf(s_out, "hle: %016llx\n", s_hash);
}

/* What the C does differently on purpose. */
static int fixes(void)
{
#ifdef IOPCORE_CXX_TREE
	return 0;
#else
	int failures = 0;
	const u32 path = 0x9000, buf = 0x2000;
	char elf[600];

	snprintf(elf, sizeof(elf), "%s/game.elf", s_dir);
	Hle_SetElfPath(elf);

	/* O_CREAT without O_TRUNC creates a file that is not there, and keeps
	 * one that is */
	guest_string(path, "host:gone.bin");
	call("open gone.bin", "ioman", 4, path, IOP_O_WRONLY | IOP_O_CREAT, 0, SHOW_ALL);
	{
		const u32 f = psxRegs.GPR.n.v0;
		int ok = (s32)f >= 0x100;
		call("write 5", "ioman", 7, f, 0x100, 5, SHOW_ALL);
		call("close", "ioman", 5, f, 0, 0, SHOW_ALL);
		call("open gone.bin again", "ioman", 4, path, IOP_O_WRONLY | IOP_O_CREAT, 0, SHOW_ALL);
		call("lseek end", "ioman", 8, psxRegs.GPR.n.v0, 0, IOP_SEEK_END, SHOW_ALL);
		ok &= psxRegs.GPR.n.v0 == 5;
		call("close", "ioman", 5, f, 0, 0, SHOW_ALL);
		if (!ok)
		{
			printf("FAIL: O_CREAT creates a missing file and keeps one there\n");
			failures++;
		}
		else
			printf("ok  : O_CREAT creates a missing file and keeps one there\n");
	}

	/* remove answers handled, as mkdir and rmdir do */
	if (call("remove gone.bin", "ioman", 10, path, 0, 0, SHOW_ALL) != 1 || psxRegs.GPR.n.v0 != 0)
	{
		printf("FAIL: remove answers handled\n");
		failures++;
	}
	else
		printf("ok  : remove answers handled\n");

	/* a failed stat and the end of a directory hand the guest zeros */
	memset(&s_mem[buf], 0xa5, 0x200);
	guest_string(path, "host:missing");
	call("getstat missing", "ioman", 16, path, buf, 0, SHOW_ALL);
	{
		int k, bad = 0;
		for (k = 0; k < 40; k++)
			bad |= s_mem[buf + k];
		if (bad)
		{
			printf("FAIL: a failed stat hands the guest zeros\n");
			failures++;
		}
		else
			printf("ok  : a failed stat hands the guest zeros\n");
	}
	guest_string(path, "host:");
	call("dopen the root", "ioman", 13, path, 0, 0, SHOW_ALL);
	{
		const u32 d = psxRegs.GPR.n.v0;
		int k, bad = 0;
		while (call("dread", "ioman", 15, d, buf, 0, SHOW_ALL) && psxRegs.GPR.n.v0 == 1)
			;
		memset(&s_mem[buf], 0xa5, 0x200);
		call("dread at the end", "ioman", 15, d, buf, 0, SHOW_ALL);
		for (k = 0; k < (int)(40 + 256 + 4); k++)
			bad |= s_mem[buf + k];
		if (bad)
		{
			printf("FAIL: the end of a directory hands the guest zeros\n");
			failures++;
		}
		else
			printf("ok  : the end of a directory hands the guest zeros\n");
		call("dclose", "ioman", 14, d, 0, 0, SHOW_ALL);
	}

	/* lseek answers the new position */
	guest_string(path, "host:pos.bin");
	call("open pos.bin", "ioman", 4, path, IOP_O_WRONLY | IOP_O_CREAT | IOP_O_TRUNC, 0, SHOW_ALL);
	{
		const u32 f = psxRegs.GPR.n.v0;
		int ok = 1;
		call("write 0x300", "ioman", 7, f, 0x100, 0x300, SHOW_ALL);
		call("lseek 0x10", "ioman", 8, f, 0x10, IOP_SEEK_SET, SHOW_ALL);
		ok &= psxRegs.GPR.n.v0 == 0x10;
		call("lseek +0x20", "ioman", 8, f, 0x20, IOP_SEEK_CUR, SHOW_ALL);
		ok &= psxRegs.GPR.n.v0 == 0x30;
		call("lseek end", "ioman", 8, f, 0, IOP_SEEK_END, SHOW_ALL);
		ok &= psxRegs.GPR.n.v0 == 0x300;
		call("lseek bad", "ioman", 8, f, 0, 7, SHOW_ALL);
		ok &= (s32)psxRegs.GPR.n.v0 == -IOP_EIO;
		call("close", "ioman", 5, f, 0, 0, SHOW_ALL);
		call("remove pos.bin", "ioman", 10, path, 0, 0, SHOW_ALL);
		if (!ok)
		{
			printf("FAIL: lseek answers the new position\n");
			failures++;
		}
		else
			printf("ok  : lseek answers the new position\n");
	}

	/* a read larger than any buffer the HLE keeps reads what the file
	 * has, with no allocation of the size asked for */
	guest_string(path, "host:big.bin");
	call("open big.bin", "ioman", 4, path, IOP_O_WRONLY | IOP_O_CREAT | IOP_O_TRUNC, 0, SHOW_ALL);
	{
		const u32 f = psxRegs.GPR.n.v0;
		call("write 0x4000", "ioman", 7, f, 0x100, 0x4000, SHOW_ALL);
		call("close", "ioman", 5, f, 0, 0, SHOW_ALL);
	}
	call("open big.bin", "ioman", 4, path, IOP_O_RDONLY, 0, SHOW_ALL);
	{
		const u32 f = psxRegs.GPR.n.v0;
		call("read 4 GiB less one", "ioman", 6, f, 0x100, 0xffffffffu, SHOW_ALL);
		if (psxRegs.GPR.n.v0 != 0x4000)
		{
			printf("FAIL: a read of nearly 4 GiB reads the file\n");
			failures++;
		}
		else
			printf("ok  : a read of nearly 4 GiB reads the file\n");
		call("close", "ioman", 5, f, 0, 0, SHOW_ALL);
	}
	call("remove big.bin", "ioman", 10, path, 0, 0, SHOW_ALL);
	return failures;
#endif
}

int main(int argc, char** argv)
{
	unsigned i;
	int failures;
	if (argc < 3)
	{
		fprintf(stderr, "usage: %s <scratch dir> <transcript out>\n", argv[0]);
		return 2;
	}
	snprintf(s_dir, sizeof(s_dir), "%s", argv[1]);
	s_out = fopen(argv[2], "w");
	if (!s_out)
		return 2;
	for (i = 0; i < 0x10000; i++)
		s_rlut[i] = (i == 0x1f80 || i == 0x1d00) ? 0 : (uptr)s_mem;
	psxRec = s_rec;

	programs();
	hle();
	fclose(s_out);
	/* what the C alone checks stays out of the transcript */
	s_out = tmpfile();
	failures = fixes();
	if (s_out)
		fclose(s_out);
	return failures != 0;
}
