/* The GIF unit (pcsx2/Gif_Unit.c), driven with seeded GIF packets on all
 * three paths.
 *
 * Each scenario feeds the unit packets -- PACKED, REGLIST and IMAGE tags,
 * A+D writes to SIGNAL, FINISH, LABEL and the transfer registers among
 * them, with and without EOP -- as PATH3 DMA, PATH2 DIRECT and DIRECTHL,
 * PATH1 XGKICK and the PATH3 FIFO, whole and in parts, with the VU1
 * thread on and off. Between them it resumes, flushes, masks PATH3,
 * pauses the GIF, acknowledges SIGNALs, asks the arbitration questions,
 * sizes VU1 packets, soft- and hard-resets, and saves and reloads the
 * unit through its savestate handler. Some scenarios push enough IMAGE
 * data through one path to run it past the end of its buffer.
 *
 * The GS thread is a model: each packet the unit hands it is hashed byte
 * for byte where it lies in the path buffer, and counted as unread until
 * the GS "catches up", which WaitGS forces and which also happens at
 * random; it drains the VU1 thread's packet queue the same way, and with
 * the VU1 thread on it catches up as each VU1 program ends. The GIF
 * registers, the GS registers the unit writes, every path's state, the
 * VIF1 transfer registers, every packet and every value returned go into
 * a running hash, written out every so many scenarios. The transcript is
 * compared with expected.txt, which this harness made from the C++ the
 * unit was converted from (EEGIFUNIT_TREE=<that checkout>).
 *
 * What the GS has yet to read -- the counts, the blank packets that
 * account for a buffer's skipped end, how often the unit waits on the GS
 * -- is the bookkeeping the fixes after the conversion correct, so it is
 * not in the hash; fixes() checks it directly in the C build, and the
 * path 1 deadlock and the savestate checks the C++ crashed or hung on. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#include "Common.h"
#include "Config.h"
#include "R5900.h"
#include "Hw.h"
#include "Vif_Dma.h"
#include "SaveState.h"
#ifdef EEGIFUNIT_CXX_TREE
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"
#else
#include "Gif_Unit.h"
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

static u32 s_rng = 0x510e527fu;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static u32 rnd_n(u32 n) { return rnd() % n; }

/* ---- what the unit reaches ----------------------------------------- */

alignas(16) cpuRegisters cpuRegs;
alignas(4096) u8 eeHw[PS2MEM_HARDWARE];
alignas(16) u8 g_RealGSMem[PS2MEM_GS_REGS];
alignas(16) vifStruct vif1;

static bool s_thread_vu1, s_xgkick_hack, s_enable_vu1;
alignas(16) u8 s_emuconfig[sizeof(Pcsx2Config)];
asm(".globl EmuConfig\n.set EmuConfig, s_emuconfig");

static void set_config(void)
{
	Pcsx2Config* c = (Pcsx2Config*)s_emuconfig;
	s_enable_vu1  = rnd_n(5) != 0;
	s_thread_vu1  = s_enable_vu1 && rnd_n(3) == 0;
	s_xgkick_hack = rnd_n(4) == 0;
	c->Cpu.Recompiler.EnableVU1          = s_enable_vu1;
	c->Speedhacks.vuThread               = s_thread_vu1;
	c->Cpu.Recompiler.EnableVu1SoftFloat = false;
	c->Gamefixes.XgKickHack              = s_xgkick_hack;
}

void CPU_INT(EE_EventType n, s32 ecycle) { mix(0x01); mix((u32)n); mix((u32)ecycle); }
void hwIntcIrq(int n) { mix(0x02); mix((u32)n); }
void gifCheckPathStatus(bool calledFromGIF) { mix(0x03); mix(calledFromGIF); }

/* The API, either way round. */
#ifdef EEGIFUNIT_CXX_TREE
alignas(16) u8 s_vu1thread[sizeof(VU_Thread)];
asm(".globl vu1Thread\n.set vu1Thread, s_vu1thread");
#define TRANSFER(t, m, s)    gifUnit.TransferGSPacketData((t), (m), (s))
#define EXECUTE(a, b)        gifUnit.Execute((a), (b))
#define FLUSH()              gifUnit.FlushToMTGS()
#define CHECK(a, b, c, q)    gifUnit.checkPaths((a), (b), (c), (q))
#define CAN1()               gifUnit.CanDoPath1()
#define CAN2()               gifUnit.CanDoPath2()
#define CAN2HL()             gifUnit.CanDoPath2HL()
#define CAN3()               gifUnit.CanDoPath3()
#define MASKED()             gifUnit.Path3Masked()
#define RESET(soft)          gifUnit.Reset(soft)
#define SIZE(p, m, o, s, f)  gifUnit.GetGSPacketSize((p), (m), (o), (s), (f))
#define COPY1(m, s)          gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData((m), (s), true)
#define FINISH_MTVU(final)   gifUnit.gifPath[GIF_PATH_1].FinishGSPacketMTVU(final)
#define PENDING()            gifUnit.gifPath[GIF_PATH_1].GetPendingGSPackets()
#define GETPACK()            gifUnit.gifPath[GIF_PATH_1].GetGSPacketMTVU()
#define POPPACK()            gifUnit.gifPath[GIF_PATH_1].PopGSPacketMTVU()
#define DONE(i)              gifUnit.gifPath[i].isDone()
#else
#define TRANSFER(t, m, s)    gif_unit_transfer((t), (m), (s))
#define EXECUTE(a, b)        gif_unit_execute((a), (b))
#define FLUSH()              gif_unit_flush_to_mtgs()
#define CHECK(a, b, c, q)    ((q) ? gif_unit_check_paths_queued((a), (b), (c)) : gif_unit_check_paths((a), (b), (c)))
#define CAN1()               gif_unit_can_do_path1()
#define CAN2()               gif_unit_can_do_path2()
#define CAN2HL()             gif_unit_can_do_path2hl()
#define CAN3()               gif_unit_can_do_path3()
#define MASKED()             gif_unit_path3_masked()
#define RESET(soft)          gif_unit_reset(soft)
#define SIZE(p, m, o, s, f)  gif_unit_get_packet_size((p), (m), (o), (s), (f))
#define COPY1(m, s)          gif_path_copy(&gifUnit.gifPath[GIF_PATH_1], (m), (s))
#define FINISH_MTVU(final)   gif_path_finish_mtvu(&gifUnit.gifPath[GIF_PATH_1], (final))
#define PENDING()            gif_path_pending_packets(&gifUnit.gifPath[GIF_PATH_1])
#define GETPACK()            gif_path_get_packet_mtvu(&gifUnit.gifPath[GIF_PATH_1])
#define POPPACK()            gif_path_pop_packet_mtvu(&gifUnit.gifPath[GIF_PATH_1])
#define DONE(i)              gif_path_is_done(i)
#endif

/* ---- the GS thread, modelled --------------------------------------- */

static void gs_packet(const GS_Packet* p, int path)
{
	mix(0x10);
	mix((u32)path);
	mix(p->offset);
	mix(p->size);
	mix((u32)p->cycles);
	if (p->size != ~0u)
	{
		mix_bytes(&gifUnit.gifPath[path].buffer[p->offset], p->size < 4096 ? p->size : 4096);
		retro_atomic_fetch_add_int(&gifPathReadAmount[path].v, (int)p->size);
	}
}

static int s_blanks, s_waits;

static void gs_blank(u32 size, int path)
{
	s_blanks++;
	retro_atomic_fetch_add_int(&gifPathReadAmount[path].v, (int)size);
}

/* The GS catches up: every packet read, path 1's queue drained. */
static void gs_catch_up(void)
{
	int i;
	while (PENDING())
	{
		const GS_Packet p = GETPACK();
		mix(0x12);
		mix(p.offset);
		mix(p.size);
		mix((u32)p.cycles);
		if (p.size)
			mix_bytes(&gifUnit.gifPath[0].buffer[p.offset], p.size < 4096 ? p.size : 4096);
		POPPACK();
	}
	for (i = 0; i < 3; i++)
		retro_atomic_store_release_int(&gifPathReadAmount[i].v, 0);
	retro_atomic_store_release_int(&gifUnit.gifPath[0].readAmount, 0);
}

static void gs_wait(bool isMTVU) { (void)isMTVU; s_waits++; gs_catch_up(); }

#ifdef EEGIFUNIT_CXX_TREE
namespace MTGS { void WaitGS(bool isMTVU) { gs_wait(isMTVU); } }
void Gif_AddCompletedGSPacket(GS_Packet& p, GIF_PATH path) { gs_packet(&p, path); }
void Gif_AddBlankGSPacket(u32 size, GIF_PATH path) { gs_blank(size, path); }
void Gif_MTVU_KickSema() { mix(0x14); }
#else
extern "C" {
void mtgs_wait_gs(bool isMTVU) { gs_wait(isMTVU); }
void Gif_AddCompletedGSPacket(const GS_Packet* p, GIF_PATH path) { gs_packet(p, path); }
void Gif_AddBlankGSPacket(u32 size, GIF_PATH path) { gs_blank(size, path); }
void Gif_MTVU_KickSema(void) { mix(0x14); }
/* The VU1 thread's half of an A+D write (MTVU.cpp): its own state, which
 * this harness does not keep. */
void Gif_HandlerAD_MTVU(u8*) { }
bool emu_config_thread_vu1(void) { return s_thread_vu1; }
bool emu_config_xgkick_hack(void) { return s_xgkick_hack; }
bool emu_config_enable_vu1(void) { return s_enable_vu1; }
}
#endif

/* A savestate in memory: saved, the unit reset, and loaded back. */
static u8* s_state;
static size_t s_state_len, s_state_cap, s_state_pos;
bool SaveState_FreezeTag(SaveStateBase* s, const char* tag)
{
	mix(0x18);
	mix_bytes(tag, strlen(tag));
	return !s->error;
}
void SaveState_FreezeMem(SaveStateBase* s, void* data, int size)
{
	if (size <= 0)
		return;
	if (s->is_saving)
	{
		if (s_state_len + (size_t)size > s_state_cap)
		{
			s_state_cap = (s_state_len + (size_t)size) * 2;
			s_state = (u8*)realloc(s_state, s_state_cap);
		}
		memcpy(s_state + s_state_len, data, (size_t)size);
		s_state_len += (size_t)size;
	}
	else
	{
		if (s_state_pos + (size_t)size > s_state_len)
		{
			s->error = true;
			return;
		}
		memcpy(data, s_state + s_state_pos, (size_t)size);
		s_state_pos += (size_t)size;
	}
}

/* ---- packets ------------------------------------------------------- */

alignas(16) static u8 s_pkt[0x180000];
/* Whether a flood's packets end at all: one that never does outgrows
 * its path's buffer. */
static bool s_flood_eop;
alignas(16) static u8 s_vumem[0x4000];

static const u8 kAD[] = { 0x60, 0x61, 0x62, 0x50, 0x51, 0x52, 0x53, 0x00, 0x4c, 0x3f };

static u32 put_tag(u8* out, u32 nloop, bool eop, u32 flg, u32 nreg, u64 regs)
{
	u32 w[4];
	w[0] = (nloop & 0x7fff) | (eop ? 0x8000u : 0) | ((rnd() & 0xffff) << 16);
	w[1] = (rnd() & 0x3fff) | ((rnd() & 0xfff) << 14) | (flg << 26) | ((nreg & 0xf) << 28);
	w[2] = (u32)regs;
	w[3] = (u32)(regs >> 32);
	memcpy(out, w, 16);
	return 16;
}

/* One or more tags into out; the bytes written. big makes a large IMAGE. */
static u32 make_packet(u8* out, u32 cap, bool big)
{
	u32 len = 0;
	const u32 tags = 1 + rnd_n(3);
	u32 t;

	for (t = 0; t < tags; t++)
	{
		const bool last = t + 1 == tags;
		const bool eop  = big ? (s_flood_eop && last && rnd_n(3) == 0) : (last ? rnd_n(4) != 0 : rnd_n(8) == 0);
		const u32 flg   = big ? 2 : rnd_n(4);
		u32 nreg        = rnd_n(17) & 0xf;
		u32 nregs       = nreg ? nreg : 16;
		u32 nloop       = big ? 0x4000 + rnd_n(0x3fff) : rnd_n(5);
		u64 regs        = 0;
		u32 body, i;

		for (i = 0; i < 16; i++)
			regs |= (u64)(rnd_n(3) == 0 ? 0xe : rnd_n(16)) << (i * 4);
		if (flg == 0)
			body = nregs * nloop * 16;
		else if (flg == 1)
			body = ((nregs * nloop + 1) >> 1) * 16;
		else
			body = nloop * 16;
		if (len + 16 + body > cap)
			break;
		len += put_tag(out + len, nloop, eop, flg, nreg, regs);
		for (i = 0; i < body; i += 4)
		{
			const u32 w = rnd();
			memcpy(out + len + i, &w, 4);
		}
		if (flg == 0)
		{
			/* The A+D quadwords name a register in their second dword. */
			u32 q;
			for (q = 0; q < nregs * nloop; q++)
			{
				if (((regs >> ((q % nregs) * 4)) & 0xf) == 0xe)
				{
					out[len + q * 16 + 8] = kAD[rnd_n(sizeof(kAD))];
					if (out[len + q * 16 + 8] == 0x53 && rnd_n(2))
						out[len + q * 16] = 1; /* TRXDIR local -> host */
				}
			}
		}
		len += body;
		if (eop)
			break;
	}
	return len;
}

/* ---- the world ----------------------------------------------------- */

static void mix_state(void)
{
	int i;
	mix_bytes(&eeHw[0x3000], 0xa0);
	mix_bytes(&g_RealGSMem[0x1000], 0x20);
	mix_bytes(&g_RealGSMem[0x1080], 8);
	for (i = 0; i < 3; i++)
	{
		const Gif_Path* p = &gifUnit.gifPath[i];
		mix(p->curSize);
		mix(p->curOffset);
		mix(p->dmaRewind);
		mix_bytes(&p->gifTag.tag, sizeof(p->gifTag.tag));
		mix(p->gifTag.nLoop);
		mix(p->gifTag.nRegs);
		mix(p->gifTag.nRegIdx);
		mix(p->gifTag.len);
		mix(p->gifTag.cycles);
		mix_bytes(p->gifTag.regs, 16);
		mix(p->gifTag.hasAD);
		mix(p->gifTag.isValid);
		mix(p->gsPack.offset);
		mix(p->gsPack.size);
		mix((u32)p->gsPack.cycles);
		mix((u32)p->idx);
		mix((u32)p->state);
		mix(p->mtvu.fakePackets);
	}
	mix(gifUnit.gsSIGNAL.data[0]);
	mix(gifUnit.gsSIGNAL.data[1]);
	mix(gifUnit.gsSIGNAL.queued);
	mix(gifUnit.gsFINISH.gsFINISHFired);
	mix(gifUnit.gsFINISH.gsFINISHPending);
	mix((u32)gifUnit.lastTranType);
	mix_bytes(vif1.transfer_registers, sizeof(vif1.transfer_registers));
	mix(vif1.GSLastDownloadSize);
	mix(cpuRegs.interrupt);
}

static void reset_world(void)
{
	u32 i;
	memset(eeHw, 0, sizeof(eeHw));
	memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
	memset(&cpuRegs, 0, sizeof(cpuRegs));
	memset(&vif1, 0, sizeof(vif1));
#ifdef EEGIFUNIT_CXX_TREE
	memset(s_vu1thread, 0, sizeof(s_vu1thread));
#endif
	set_config();
	RESET(false);
	gs_catch_up();
	*(u32*)&g_RealGSMem[0x1010] = rnd() & 0x7f00;
	vif1Regs.stat.VGW = rnd_n(4) == 0;
	cpuRegs.interrupt = rnd() & (1 << DMAC_VIF1);
	for (i = 0; i < sizeof(s_vumem); i += 4)
	{
		const u32 w = rnd();
		memcpy(&s_vumem[i], &w, 4);
	}
	{
		u32 off = 0;
		while (off + 0x100 < sizeof(s_vumem))
		{
			const u32 room = sizeof(s_vumem) - off;
			const u32 n    = make_packet(&s_vumem[off], room < 0x200 ? room : 0x200, false);
			off += n ? n : 16;
		}
	}
}

static const GIF_TRANSFER_TYPE kTypes[] = {
	GIF_TRANS_DMA, GIF_TRANS_DMA, GIF_TRANS_DIRECT, GIF_TRANS_DIRECTHL, GIF_TRANS_XGKICK, GIF_TRANS_FIFO,
};

static void feed(bool big)
{
	const GIF_TRANSFER_TYPE type = big ? (s_thread_vu1 ? GIF_TRANS_XGKICK : GIF_TRANS_DMA)
	                                   : kTypes[rnd_n(sizeof(kTypes) / sizeof(kTypes[0]))];
	u32 len = make_packet(s_pkt, big ? sizeof(s_pkt) : 0x2000, big);
	u32 done = 0;

	if (!len)
		return;
	if (type == GIF_TRANS_FIFO)
	{
		mix(TRANSFER(type, s_pkt, 16));
		return;
	}
	while (done < len)
	{
		/* A flood's tags go whole: a part dropped would leave the rest
		 * of the stream parsed as image data, and its tags as SIGNALs. */
		const u32 part = (big || rnd_n(3)) ? len - done : (1 + rnd_n((len - done) / 16 + 1)) * 16;
		const u32 n    = part > len - done ? len - done : part;
		u32 took;
		if (!big && type == GIF_TRANS_XGKICK && s_thread_vu1 && rnd_n(3) == 0)
		{
			/* The VU1 thread copies a kick that is not all there yet. */
			COPY1(s_pkt + done, n);
			took = n;
		}
		else
			took = TRANSFER(type, s_pkt + done, n);
		mix(took);
		done += n;
		if (!big && rnd_n(4) == 0)
			break;
	}
	/* A flood's kicks each end their VU1 program: one left running across
	 * a buffer's worth of unparsed data is the path 1 deadlock that the
	 * C++ cannot get out of (see the fix that follows this conversion). */
	if (type == GIF_TRANS_XGKICK && s_thread_vu1 && (big || rnd_n(2) != 0))
	{
		/* The VU1 program ends: its packet to MTGS, and the EE's side. */
		FINISH_MTVU(true);
		mix(TRANSFER(GIF_TRANS_MTVU, NULL, 0));
		gs_catch_up();
	}
}

/* PATH3 as one unbroken stream of IMAGE tags with no EOP, sent in pieces
 * that do not line up with the tags: the open packet outgrows the buffer
 * and goes to the GS in parts. */
enum { kStream = 12 * 1024 * 1024 };
static u8* s_stream;

static void flood_stream(void)
{
	u32 len = 0, sent = 0;

	while (len + 0x80010 < kStream)
	{
		const u32 nloop = 0x4000 + rnd_n(0x3fff);
		u32 i;
		len += put_tag(s_stream + len, nloop, false, 2, 0, 0);
		for (i = 0; i < nloop * 16; i += 4)
		{
			const u32 w = rnd();
			memcpy(s_stream + len + i, &w, 4);
		}
		len += nloop * 16;
	}
	while (sent < len)
	{
		u32 n = (1 + rnd_n(0x18000)) * 16;
		if (n > len - sent)
			n = len - sent;
		mix(TRANSFER(GIF_TRANS_DMA, s_stream + sent, n));
		sent += n;
		if (rnd_n(4) == 0)
			gs_catch_up();
		mix_state();
	}
}

static void freeze_round_trip(void)
{
	SaveStateBase st;
	memset(&st, 0, sizeof(st));
	st.is_saving = true;
	s_state_len = 0;
	mix(gifFreeze(&st));
	RESET(false);
	memset(&st, 0, sizeof(st));
	st.is_saving = false;
	s_state_pos = 0;
	mix(gifFreeze(&st));
	mix(s_state_len);
}

static void step(void)
{
	switch (rnd_n(24))
	{
		case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
			feed(false);
			break;
		case 8:  mix(EXECUTE(rnd() & 1, true)); break;
		case 9:  FLUSH(); break;
		case 10: mix(CHECK(rnd() & 1, rnd() & 1, rnd() & 1, rnd() & 1)); break;
		case 11:
			mix(CAN1()); mix(CAN2()); mix(CAN2HL()); mix(CAN3()); mix(MASKED());
			mix(DONE(0)); mix(DONE(1)); mix(DONE(2));
			break;
		case 12: RESET(rnd_n(4) != 0); break;
		case 13:
		{
			const u32 off = rnd() & 0x3ff0;
			const u32 size = SIZE(GIF_PATH_1, s_vumem, off, rnd_n(2) ? ~0u : rnd_n(0x2000), rnd() & 1);
			mix(size);
			/* A kick the size came back 0 for still reaches the unit. */
			if (!(size & 0x7fffffffu) && rnd_n(2))
				mix(TRANSFER(GIF_TRANS_XGKICK, s_vumem, 0));
			break;
		}
		case 14:
			/* PATH3 masked or not, the GIF paused, the image mode. */
			gifRegs.stat.M3R = rnd_n(6) == 0;
			gifRegs.stat.M3P = rnd_n(6) == 0;
			gifRegs.stat.IMT = rnd() & 1;
			gifRegs.stat.PSE = rnd_n(10) == 0;
			gifRegs.stat.DIR = rnd_n(12) == 0;
			break;
		case 15:
			/* The SIGNAL acknowledged, as a CSR write does (GS.cpp). */
			if (rnd() & 1)
				*(u32*)&g_RealGSMem[0x1000] |= 1;
			else
			{
				*(u32*)&g_RealGSMem[0x1000] &= ~1u;
				gifUnit.gsSIGNAL.queued = false;
				mix(EXECUTE(false, true));
			}
			break;
		case 16: Gif_FinishIRQ(); break;
		case 17:
			if (!PENDING())
			{
				const GS_Packet p = GETPACK(); /* nothing there */
				mix(p.size);
			}
			gs_catch_up();
			break;
		case 18:
			if (rnd_n(4) == 0)
				freeze_round_trip();
			break;
		case 19:
		{
			alignas(16) u8 ad[16];
			u32 i;
			for (i = 0; i < 16; i += 4)
			{
				const u32 w = rnd();
				memcpy(&ad[i], &w, 4);
			}
			ad[8] = kAD[rnd_n(sizeof(kAD))];
			mix(Gif_HandlerAD(ad));
			break;
		}
		default:
			mix(EXECUTE(false, rnd() & 1));
			break;
	}
}

static void run(FILE* out)
{
	int scen, i;

	for (scen = 0; scen < 600; scen++)
	{
		reset_world();
		mix_state();
		if (scen % 12 == 7)
		{
			/* Past the end of a path's buffer and back: long IMAGE runs,
			 * mostly or entirely without an EOP, the GS lagging. */
			s_flood_eop = (rnd() & 1) || s_thread_vu1;
			if (!s_flood_eop)
				flood_stream();
			else
			{
				for (i = 0; i < 40; i++)
				{
					feed(true);
					if (rnd_n(4) == 0)
						gs_catch_up();
					mix_state();
				}
			}
		}
		for (i = 0; i < 80; i++)
		{
			step();
			mix_state();
		}
		if ((scen + 1) % 20 == 0)
			fprintf(out, "%4d %016llx\n", scen + 1, s_hash);
	}
	fprintf(out, " all %016llx\n", s_hash);
}

/* ---- what the fixes after the conversion changed ------------------- */

#ifndef EEGIFUNIT_CXX_TREE
static int s_failed;
static const char* s_watching;

static void expect(bool ok, const char* what)
{
	if (!ok)
	{
		fprintf(stderr, "eegifunit: %s\n", what);
		s_failed = 1;
	}
}

/* The checks below hung the C++; a hang here fails rather than stalls.
 * Windows has no alarm: build.sh's timeout is the watchdog there. */
#ifndef _WIN32
static void watchdog(int sig)
{
	(void)sig;
	fprintf(stderr, "eegifunit: hung: %s\n", s_watching);
	_exit(1);
}

static void watch(const char* what, unsigned seconds)
{
	s_watching = what;
	alarm(seconds);
}
#define watch_off() alarm(0)
#else
static void watch(const char* what, unsigned seconds) { s_watching = what; (void)seconds; }
#define watch_off() ((void)0)
#endif

static void gif_pack_clear_for_test(GS_Packet* p)
{
	p->offset = p->size = 0;
	p->cycles = p->readAmount = 0;
}

static void fresh_vu1_thread(void)
{
	reset_world();
	s_thread_vu1 = true;
	gif_unit_reset(false);
	gs_catch_up();
}

static u32 eop_packet(u8* out)
{
	u32 i;
	put_tag(out, 4, true, 2, 0, 0); /* IMAGE, 4 quadwords, EOP */
	for (i = 16; i < 80; i++)
		out[i] = (u8)rnd();
	return 80;
}

static void fixes(void)
{
	Gif_Path* p1 = &gifUnit.gifPath[GIF_PATH_1];
#ifndef _WIN32
	signal(SIGALRM, watchdog);
#endif

	/* A VU1 program's packet, handed to the GS, counts as unread where
	 * the space checks look until the GS has sent it. */
	fresh_vu1_thread();
	{
		const u32 n = eop_packet(s_pkt);
		gif_unit_transfer(GIF_TRANS_XGKICK, s_pkt, n);
		gif_path_finish_mtvu(p1, true);
		expect(retro_atomic_load_acquire_int(&gifPathReadAmount[GIF_PATH_1].v) == (int)n,
		       "a VU1 program's packet counts as unread for the space checks");
		expect(retro_atomic_load_acquire_int(&p1->readAmount) == 0,
		       "nothing counts in the path's own, unread, field");
	}

	/* Path 1 on the VU1 thread with a realign's skipped end pending and no
	 * packet data: the copy hands the skip to the GS instead of waiting on
	 * a count nothing else would clear. */
	fresh_vu1_thread();
	p1->curSize   = 5u << 20;
	p1->curOffset = 0;
	gif_pack_clear_for_test(&p1->gsPack);
	p1->gsPack.readAmount = (s32)(p1->buffLimit - (4u << 20));
	watch("path 1 copy with only a skip pending", 20);
	gif_path_copy(p1, s_pkt, 300u << 10);
	watch_off();
	expect(p1->curSize == (5u << 20) + (300u << 10), "the copy lands");
	expect(p1->gsPack.readAmount == 0, "the pending skip went to the GS");

	/* A realign of a packet already at the start returns: there is
	 * nothing to move, and the count it would wait on only goes down when
	 * the packet is finished. */
	fresh_vu1_thread();
	p1->curSize   = 6u << 20;
	p1->curOffset = 6u << 20;
	p1->gsPack.offset = 0;
	p1->gsPack.size   = 6u << 20;
	p1->gsPack.readAmount = 2u << 20;
	watch("realign of a packet at the start", 20);
	gif_path_realign(p1);
	watch_off();
	expect(p1->curOffset == (6u << 20) && p1->gsPack.readAmount == (2 << 20),
	       "a packet at the start is left as it is");

	/* A VU1 program that streams one unending packet through path 1,
	 * copied in parts ahead of the parsing, outgrowing the buffer: the
	 * C++ deadlocked on it. */
	fresh_vu1_thread();
	watch("an unending VU1 stream on path 1", 120);
	{
		u32 len = 0, sent = 0;
		while (len + 0x80010 < kStream)
		{
			const u32 nloop = 0x4000 + rnd_n(0x3fff);
			u32 i;
			len += put_tag(s_stream + len, nloop, false, 2, 0, 0);
			for (i = 0; i < nloop * 16; i += 4)
			{
				const u32 w = rnd();
				memcpy(s_stream + len + i, &w, 4);
			}
			len += nloop * 16;
		}
		while (sent < len)
		{
			u32 n = (1 + rnd_n(0x18000)) * 16;
			if (n > len - sent)
				n = len - sent;
			if (rnd_n(3) == 0)
				gif_path_copy(p1, s_stream + sent, n);
			else
				gif_unit_transfer(GIF_TRANS_XGKICK, s_stream + sent, n);
			sent += n;
			if (rnd_n(4) == 0)
				gs_catch_up();
		}
	}
	watch_off();

	/* Savestates whose path does not add up: the load fails and the path
	 * is left empty, instead of realigning from outside the buffer. */
	{
		static const int kField[] = { 0, 1, 2, 3 };
		int f;
		for (f = 0; f < 4; f++)
		{
			SaveStateBase st;
			const size_t at = 1 + 4 + sizeof(GS_SIGNAL) + sizeof(GS_FINISH) + 4 + 4;
			size_t field;
			u32 bad;
			bool ok;
			fresh_vu1_thread();
			s_thread_vu1 = false;
			gif_unit_transfer(GIF_TRANS_XGKICK, s_pkt, eop_packet(s_pkt));
			memset(&st, 0, sizeof(st));
			st.is_saving = true;
			s_state_len = 0;
			gifFreeze(&st);
			switch (kField[f])
			{
				case 0:  field = offsetof(Gif_Path, curSize);       bad = 0xfffff000u; break;
				case 1:  field = offsetof(Gif_Path, curOffset);     bad = 0x00100000u; break;
				case 2:  field = offsetof(Gif_Path, gsPack.size);   bad = 0x00010000u; break;
				default: field = offsetof(Gif_Path, gsPack.offset); bad = 0x00000010u; break;
			}
			memcpy(s_state + at + field, &bad, 4);
			gif_unit_reset(false);
			memset(&st, 0, sizeof(st));
			s_state_pos = 0;
			ok = gifFreeze(&st);
			expect(!ok, "a savestate whose path does not add up fails to load");
			expect(p1->curSize == 0 && p1->curOffset == 0 && p1->gsPack.size == 0,
			       "the path it failed on is left empty");
			gif_path_realign(p1);
		}
	}
}
#endif

int main(int argc, char** argv)
{
	FILE* out;

	if (argc < 2)
		return 2;
#ifndef EEGIFUNIT_CXX_TREE
	if (!gif_unit_init())
		return 2;
#endif
	s_stream = (u8*)malloc(kStream);
	out = fopen(argv[1], "w");
	if (!out || !s_stream)
		return 2;
	run(out);
	fclose(out);
#ifndef EEGIFUNIT_CXX_TREE
	fixes();
	gif_unit_shutdown();
#endif
	free(s_stream);
	free(s_state);
#ifndef EEGIFUNIT_CXX_TREE
	return s_failed;
#else
	return 0;
#endif
}
