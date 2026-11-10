/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

#include <string.h>

#include "Common.h"

#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <retro_spsc.h>

#include <libretro.h>

#include "GS.h"
#include "GS/GSEntry.h"
#include "Gif_Unit.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "ps2/HwInternal.h"
#include "WorkEventCount.h"
#include "MTGSOwner.h"
#include "PAD/PADsio.h"
#include "../libretro/audio_queue.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

/* One 16-byte ring record: a command and three words, or a command, a
 * word and a pointer, which sits where data[1..2] would. */
typedef union PacketTagType
{
	struct
	{
		u32 command;
		u32 data[3];
	} w;
	struct
	{
		u32 command;
		u32 data0;
		uptr pointer;
	} p;
} PacketTagType;

/* The command ring is a byte SPSC of 16-byte PacketTagType records: the
 * EE thread produces, the libretro (= MTGS) thread consumes, and the
 * libretro-thread writers (ResetGS, Freeze, InitAndReadFIFO on its
 * synchronous paths) only run with the EE quiesced, so the single-producer
 * contract holds serially.  The capacity is a whole number of records, so
 * a record never straddles the wrap and the framing is sizeof(PacketTagType).
 *
 * Allocated once in TryOpenGS and kept for the life of the process, which
 * is the same lifetime the static array it replaces had.  retro_spsc pads
 * its own cursors onto separate cache lines, so the alignas gymnastics the
 * old cursor pair needed live inside the queue now. */
static retro_spsc_t s_Ring;
static bool s_RingOk = false;

/* The GS's view of the privileged registers: a copy of PS2MEM_GS taken
 * on this thread at each vsync, reset and freeze, while the EE is held
 * in WaitGS. The renderers read display registers at vsync and nothing
 * else, so a copy taken then is the frame's, and the EE is free to write
 * the next frame's the moment its vsync is consumed - before the scanout,
 * the frontend's present and its pacing wait, which the EE then overlaps
 * instead of idling through. CSR stays in PS2MEM_GS, where the EE keeps
 * it atomically; the field it carries at the vsync rides in the packet. */
static PCSX2_ALIGN(64) u8 s_gs_regs[PS2MEM_GS_REGS];

static __fi void mtgs_sync_regs(void)
{
	memcpy(s_gs_regs, PS2MEM_GS, sizeof(s_gs_regs));
}

PCSX2_STATIC_ASSERT(sizeof(PacketTagType) == 16, "command ring framing is one 16-byte record per packet");

/* Reserve one record.  Backpressure on a full ring: the consumer is the
 * libretro thread, which the frontend keeps pumping through retro_run, so
 * a full ring drains and progress is guaranteed.  The old open-coded ring
 * had no full check at all - a lapped writer silently dropped an entire
 * ring of commands - so waiting is strictly safer.  Occupancy was measured
 * at ~22k records worst-case against 65536 capacity (see GS.h), so the
 * wait should never fire outside a wedged consumer.
 *
 * Returns NULL only when the ring never allocated (TryOpenGS failed and
 * logged); the caller drops the command, which is the degraded mode the
 * MTVU packet queue already uses for the same impossible allocation. */
static __fi PacketTagType* RingWriteBegin(void)
{
	void* dst;
	if (!s_RingOk)
		return NULL;
	while (retro_spsc_write_begin(&s_Ring, &dst) < sizeof(PacketTagType))
		;
	return (PacketTagType*)dst;
}

static __fi void RingWriteEnd(void)
{
	retro_spsc_write_end(&s_Ring, sizeof(PacketTagType));
}

#ifdef ENABLE_PCSX2_PROFILER
/* Which side of the handoff is waiting? Each counter is written by exactly
 * one thread -- idle by the GS/frontend thread, wait by the EE thread -- so
 * no synchronisation is needed; they are only read together at the report.
 *
 * GS idle high  => the GS is starved and the EE is the limit.
 * EE wait high  => the GS is the limit and the EE has headroom.
 * Both low      => both saturated, genuinely overlapped.
 * Both high     => neither is saturated; the frame is paced elsewhere. */
static PCSX2_ALIGN(64) u64 g_gs_idle_ticks;
static PCSX2_ALIGN(64) u64 g_ee_wait_ticks;
#endif

/* How long retro_run waits on an empty ring before it dupes a frame. */
#define MTGS_WEDGE_MS 1000

/* See MTGSOwner.h. */
struct mtgs_frame mtgs_pending;
uint32_t mtgs_presented_mark;
int mtgs_vsync_drained;

/* The frame gate, MTGSOwner.h. granted counts the frames retro_run let
 * the EE start and taken those the EE started, so a grant that lands
 * before the EE reaches the gate is not lost; stop sends the EE out of
 * the gate to stop there. owed is set while the EE is held behind the
 * vsync retro_run last consumed: the frontend's thread only. */
static retro_eventcount_t s_gate_ec;
static int s_gate_ec_ok;
static retro_atomic_int_t s_gate_granted = RETRO_ATOMIC_INT_INITIALIZER(0);
static retro_atomic_int_t s_gate_stop    = RETRO_ATOMIC_INT_INITIALIZER(0);
static int s_gate_taken;
static int s_gate_owed;

#define MTGS_GATE_OPEN() \
	(retro_atomic_load_acquire_int(&s_gate_granted) != s_gate_taken \
	 || retro_atomic_load_acquire_int(&s_gate_stop) \
	 || !mtgs_is_open())

static void mtgs_gate_notify(void)
{
	if (s_gate_ec_ok)
		retro_eventcount_notify(&s_gate_ec);
}

/* The EE thread, behind a vsync retro_run consumed: wait for the next
 * retro_run to let it start the next frame. Sent out to stop, or with
 * the GS gone, the vsync is where it stops, as a drained one is. */
static void mtgs_gate_wait(void)
{
	while (!MTGS_GATE_OPEN())
	{
		const int key = retro_eventcount_prepare_wait(&s_gate_ec);
		if (MTGS_GATE_OPEN())
		{
			retro_eventcount_cancel_wait(&s_gate_ec);
			break;
		}
		retro_eventcount_commit_wait(&s_gate_ec, key);
	}
	if (retro_atomic_load_acquire_int(&s_gate_granted) != s_gate_taken)
		s_gate_taken++;
	else
		mtgs_vsync_drained = 1;
}

/* See GS.h. NULL unless the renderer in use installed them. */
void (*gs_hw_context_begin)(void) = NULL;
void (*gs_hw_context_end)(void)   = NULL;

/* One read of the CPU count, cached. Spinning on a single-core host only
 * steals the producer's timeslice, so the budget is 0 there. Defined here
 * rather than in the header so the header pulls in no platform code. */
s32 WorkEventCount_SpinBudget(void)
{
	static s32 budget = -1;
	if (budget < 0)
	{
#if defined(_WIN32)
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		budget = (si.dwNumberOfProcessors >= 2) ? 2000 : 0;
#else
		budget = (sysconf(_SC_NPROCESSORS_ONLN) >= 2) ? 2000 : 0;
#endif
	}
	return budget;
}

static WorkEventCount s_sem_event;
static uintptr_t s_thread;
static uintptr_t s_producer_thread;
static retro_atomic_int_t s_open_flag = RETRO_ATOMIC_INT_INITIALIZER(0);
/* The VU1 thread's eventcounts, fixed for the process: mtgs_try_open_gs
 * takes them before anything here can wait or notify on them. */
static struct retro_asym_eventcount* s_ec_xgkick;
static struct retro_asym_eventcount* s_ec_p1_progress;

bool mtgs_is_open(void)
{
	return retro_atomic_load_acquire_int(&s_open_flag) != 0;
}

void mtgs_reset_gs(bool hardware_reset)
{
	PacketTagType* tag;
	/* Clear the ring and queue the reset. Discarding pending entries requires both sides quiesced: the EE is
	 * paused across a reset, and the consumer is this very thread. */
	if (hardware_reset && s_RingOk)
		retro_spsc_clear(&s_Ring);

	tag = RingWriteBegin();
	if (tag)
	{
		tag->w.command = GS_RINGTYPE_RESET;
		tag->w.data[0] = hardware_reset ? 1 : 0;
		tag->w.data[1] = 0;
		tag->w.data[2] = 0;
		RingWriteEnd();
	}

	if (hardware_reset)
		work_eventcount_notify(&s_sem_event);
}

void mtgs_post_vsync_start(void)
{
	PacketTagType* tag = RingWriteBegin();
	if (tag)
	{
		tag->w.command = GS_RINGTYPE_VSYNC;
		/* The field this vsync scans out, and whether the privileged
		 * registers changed since the last one: read here, on the thread
		 * that owns them, so the consumer needs nothing from the EE's
		 * state by the time it gets to them. */
		tag->w.data[0] = (gsCSRload() & GS_CSR_FIELD) ? 0 : 1;
		tag->w.data[1] = (u32)retro_atomic_exchange_int(&s_GSRegistersWritten, 0);
		/* Where this frame's audio ends: see audio_queue.h. */
		tag->w.data[2] = retro_audio_mark();
		RingWriteEnd();
	}
	else
		mtgs_vsync_drained = 1; /* no ring: nothing to wait for a pause behind */

#ifdef ENABLE_PCSX2_PROFILER
	{
		static u64 last_wall, last_idle, last_wait;
		static unsigned long frames;
		const u64 now = __builtin_ia32_rdtsc();
		if ((++frames % 60) == 0)
		{
			const double wall = (double)(now - last_wall);
			log_cb(RETRO_LOG_INFO, "[overlap] 60 frames: GS idle %.1f%% of wall, EE blocked in WaitGS %.1f%%\n",
			        wall > 0 ? 100.0 * (double)(g_gs_idle_ticks - last_idle) / wall : 0.0,
			        wall > 0 ? 100.0 * (double)(g_ee_wait_ticks - last_wait) / wall : 0.0);
			last_wall = now; last_idle = g_gs_idle_ticks; last_wait = g_ee_wait_ticks;
		}
		else if (frames == 1)
			last_wall = now;
	}
#endif

	/* This wait is the pacing between the two threads: the EE holds here
	 * until the libretro thread has consumed this vsync packet, which is
	 * where the GS takes its copy of the registers. */
	mtgs_wait_gs(false);
	/* Consumed by retro_run: the next frame waits for the next one,
	 * and its input. */
	if (!mtgs_vsync_drained && s_gate_ec_ok)
		mtgs_gate_wait();
}

void mtgs_init_and_read_fifo(u8* mem, u32 qwc)
{
	PacketTagType* tag;
	const int mode = gs_hw_download_mode();
	if (mode >= GS_HW_DOWNLOAD_UNSYNCHRONIZED && GSIsHardwareRenderer())
	{
		if (mode == GS_HW_DOWNLOAD_UNSYNCHRONIZED)
			GSReadLocalMemoryUnsync(mem, qwc, vif1.BITBLTBUF._u64, vif1.TRXPOS._u64, vif1.TRXREG._u64);
		else
			memset(mem, 0, qwc * 16);

		return;
	}

	tag = RingWriteBegin();
	if (tag)
	{
		tag->p.command = GS_RINGTYPE_INIT_AND_READ_FIFO;
		tag->p.data0   = qwc;
		tag->p.pointer = (uptr)mem;
		RingWriteEnd();
	}
	mtgs_wait_gs(false);
}

void mtgs_try_open_gs(void)
{
	/* Whoever opens the GS owns the ring until the frontend's thread
	 * says otherwise (mtgs_claim_ring, every retro_run). There is never a
	 * moment with the GS open and nobody owning it, which is the moment
	 * the EE used to fall into. */
	s_thread = sthread_get_current_thread_id();

	if (!s_RingOk)
	{
		static bool s_ec_inited = false;
		if (!s_ec_inited)
		{
			work_eventcount_init(&s_sem_event);
			s_gate_ec_ok     = retro_eventcount_init(&s_gate_ec);
			s_ec_xgkick      = mtvu_ec_xgkick();
			s_ec_p1_progress = mtvu_ec_p1_progress();
			s_ec_inited = true;
		}
		s_RingOk = retro_spsc_init(&s_Ring,
			(size_t)MTGS_RINGBUFFERSIZE * sizeof(PacketTagType));
		if (!s_RingOk)
			log_cb(RETRO_LOG_ERROR, "MTGS: command ring allocation failed; GS commands will be dropped\n");
	}

	/* The GS opens on an empty ring and a live eventcount, as it does in
	 * a freshly loaded image. A GS closed earlier in this image left the
	 * eventcount killed by mtgs_main_loop's exit tail, and the ring with
	 * whatever was written after that tail skipped it; a frontend that
	 * retires the core with dlclose may get the same image, statics and
	 * all, on the next load. The EE is not producing while the GS is
	 * closed, so both sides are quiescent here. */
	if (s_RingOk)
		retro_spsc_clear(&s_Ring);
	work_eventcount_reset(&s_sem_event);
	retro_atomic_store_release_int(&s_gate_granted, 0);
	s_gate_taken = 0;
	s_gate_owed  = 0;

	mtgs_sync_regs();
	GS_HW_CONTEXT_BEGIN();
	gs_open_emu(s_gs_regs);
	GS_HW_CONTEXT_END();

	retro_atomic_store_release_int(&s_open_flag, 1);
}

/* MTVU_GSPACKET: one ring item is one VU1 program, which may deliver
 * several queue packets: partial flushes (cycles != 0, when the worker's
 * path-1 buffer fills mid-program) and then the final one (cycles == 0).
 * Consume until the final one. */
static void mtgs_drain_mtvu_program(void)
{
	Gif_Path* path = &gifUnit.gifPath[GIF_PATH_1];
	for (;;)
	{
		GS_Packet gsPack;
		bool      final_packet;
		/* The queue's occupancy is the condition: there is no separate
		 * credit to take, and the pop below is the consume. Spin the
		 * budget first, since partial flushes and short programs post
		 * microseconds apart, then register, re-check, park. */
		if (!gif_path_pending_packets(path))
		{
			s32 spins = WorkEventCount_SpinBudget();
			while (!gif_path_pending_packets(path) && spins-- > 0)
				WORK_EVENTCOUNT_RELAX();
			while (!gif_path_pending_packets(path))
			{
				int key = retro_asym_eventcount_prepare_wait(s_ec_xgkick);
				if (gif_path_pending_packets(path))
				{
					retro_asym_eventcount_cancel_wait(s_ec_xgkick);
					break;
				}
				retro_asym_eventcount_commit_wait(s_ec_xgkick, key);
			}
		}
		gsPack = gif_path_get_packet_mtvu(path);
		if (gsPack.size)
		{
			GS_HW_CONTEXT_BEGIN();
			GSgifTransfer((u8*)&path->buffer[gsPack.offset], gsPack.size / 16);
			GS_HW_CONTEXT_END();
		}
		retro_atomic_fetch_sub_int(&gifPathReadAmount[GIF_PATH_1].v, gsPack.size + gsPack.readAmount);
		final_packet = gsPack.cycles == 0;
		/* Popped last, for mtgs_wait_gs(true); one notify per pop is what
		 * it sleeps on. */
		gif_path_pop_packet_mtvu(path);
		retro_asym_eventcount_notify(s_ec_p1_progress);
		if (final_packet)
			break;
	}
}

bool mtgs_main_loop(bool flush_all)
{
	for (;;)
	{
		size_t      span;
		const void* span_ptr;

		if (flush_all)
		{
			if (!work_eventcount_check(&s_sem_event))
				return true;
		}
		else
		{
			/* The frontend thread must never park unboundedly: a wedged
			 * producer degrades to duped frames with a live frontend,
			 * never a frozen process. A dupe puts the frame it stood in
			 * for into a later retro_run, so the bound is long enough
			 * that only a wedge reaches it: a second with nothing at all
			 * in the ring, not an EE busy recompiling or a slow build. */
#ifdef ENABLE_PCSX2_PROFILER
			const u64  t0  = __builtin_ia32_rdtsc();
			const bool got = work_eventcount_wait_timed(&s_sem_event, MTGS_WEDGE_MS);
			g_gs_idle_ticks += __builtin_ia32_rdtsc() - t0;
			if (!got)
				return false;
#else
			if (!work_eventcount_wait_timed(&s_sem_event, MTGS_WEDGE_MS))
				return false;
#endif
		}

		if (!retro_atomic_load_acquire_int(&s_open_flag))
			break;

		/* Drain in contiguous spans: read_begin acquires the head once per
		 * span, and the consumed records are committed in one read_end per
		 * span. The only cross-thread reader of the tail is the producer's
		 * full check, and the WaitGS emptiness contract lives in the work
		 * eventcount, so batching the commit changes nothing observable.
		 * The vsync early-return commits before leaving.
		 *
		 * Spans run until the queue reports empty: wakes are not 1:1 with
		 * records (a soft-reset tag is written with no notify and rides
		 * along on the next one), so leaving with buffered records would
		 * strand them. */
		while (s_RingOk && (span = retro_spsc_read_begin(&s_Ring, &span_ptr)) >= sizeof(PacketTagType))
		{
			size_t consumed = 0;
			/* The hardware context, where it has to be taken, is taken
			 * around each call into the GS and nothing else. Not around a
			 * span: a span can be a whole frame, including the waits for
			 * the VU1 worker, and for all of that the frontend's thread
			 * could not start its frame. */
			while (consumed < span)
			{
				const PacketTagType* tag = (const PacketTagType*)((const u8*)span_ptr + consumed);

				switch (tag->w.command)
				{
					case GS_RINGTYPE_GSPACKET:
						{
							Gif_Path* path = &gifUnit.gifPath[tag->w.data[2]];
							const u32 offset = tag->w.data[0];
							const u32 size   = tag->w.data[1];
							if (offset != ~0u)
							{
								GS_HW_CONTEXT_BEGIN();
								GSgifTransfer((u8*)&path->buffer[offset], size / 16);
								GS_HW_CONTEXT_END();
							}
							retro_atomic_fetch_sub_int(&gifPathReadAmount[tag->w.data[2]].v, size);
						}
						break;
					case GS_RINGTYPE_MTVU_GSPACKET:
						mtgs_drain_mtvu_program();
						break;
					case GS_RINGTYPE_VSYNC:
						{
							const u32 field             = tag->w.data[0];
							const u32 registers_written = tag->w.data[1];
							const u32 audio_mark        = tag->w.data[2];
							int       epoch;
							/* The EE is held in WaitGS behind this packet, so
							 * the registers are this frame's. The tag is not
							 * read past here. */
							mtgs_sync_regs();
							if (flush_all)
							{
								/* A drain: kept for the next retro_run, not
								 * presented. See MTGSOwner.h. */
								memcpy(mtgs_pending.regs, s_gs_regs, sizeof(mtgs_pending.regs));
								mtgs_pending.audio_mark        = audio_mark;
								mtgs_pending.field             = field;
								mtgs_pending.registers_written = registers_written;
								mtgs_pending.pending           = 1;
								mtgs_vsync_drained             = 1;
								break;
							}
							/* retro_run's frame. The EE waits behind it at
							 * the frame gate for the next retro_run's input. */
							s_gate_owed         = 1;
							mtgs_vsync_drained  = 0;
							mtgs_presented_mark = audio_mark;
							consumed += sizeof(PacketTagType);
							retro_spsc_read_end(&s_Ring, consumed);
							/* Idle at the current epoch releases WaitGS's
							 * empty-wait, including when the vsync's own
							 * notify landed after this drain began. Entries
							 * behind the vsync re-arm the work count so the
							 * next call drains them. */
							epoch = work_eventcount_epoch(&s_sem_event);
							work_eventcount_drained(&s_sem_event, epoch,
									retro_spsc_read_avail(&s_Ring) != 0);
							GS_HW_CONTEXT_BEGIN();
							GSvsync(field, registers_written != 0);
							GS_HW_CONTEXT_END();
							return true;
						}
					case GS_RINGTYPE_RESET:
						mtgs_sync_regs();
						GS_HW_CONTEXT_BEGIN();
						GSreset(tag->w.data[0] != 0);
						GS_HW_CONTEXT_END();
						break;
					case GS_RINGTYPE_INIT_AND_READ_FIFO:
						GS_HW_CONTEXT_BEGIN();
						GSInitAndReadFIFO((u8*)tag->p.pointer, tag->p.data0);
						GS_HW_CONTEXT_END();
						break;
					default:
						break;
				}

				consumed += sizeof(PacketTagType);
			}
			retro_spsc_read_end(&s_Ring, consumed);
		}
	}

	/* Unblock any threads in WaitGS in case MTGS gets cancelled while
	 * still processing work. */
	if (s_RingOk)
		retro_spsc_skip(&s_Ring, retro_spsc_read_avail(&s_Ring));
	/* Wake a mtgs_wait_gs(true) sleeper too; its loop re-checks
	 * s_open_flag and exits. */
	if (s_ec_p1_progress)
		retro_asym_eventcount_notify(s_ec_p1_progress);
	work_eventcount_kill(&s_sem_event);
	mtgs_gate_notify();
	return true;
}

void mtgs_close_gs(void)
{
	GS_HW_CONTEXT_BEGIN();
	GSclose();
	GS_HW_CONTEXT_END();
	retro_atomic_store_release_int(&s_open_flag, 0);
	mtgs_gate_notify();
}

/* The consumer commits a record only once it has processed it, and the
 * vsync before its scanout, so an empty ring is a drained one. */
static int mtgs_ring_drained(void* ctx)
{
	(void)ctx;
	return retro_spsc_read_avail(&s_Ring) == 0;
}

/* The MTVU worker: wait until MTGS has consumed a path-1 packet, or has
 * none left to consume. MTGS notifies ecP1Progress once per pop, and the
 * exit condition is the packet count moving: only that one happened
 * matters, not how many, so there is no credit to drain and a stale
 * notify cannot turn this into a spin. Progress needs MTGS to pop, and it
 * notifies at every pop, including before it parks for the next packet.
 * The command ring's cursors belong to the EE and MTGS; this never reads
 * them. */
static void mtgs_wait_p1_progress(void)
{
	Gif_Path* path          = &gifUnit.gifPath[GIF_PATH_1];
	const u32 startP1Packs  = gif_path_pending_packets(path);
	if (!startP1Packs)
		return;
	for (;;)
	{
		int key;
		if (gif_path_pending_packets(path) != startP1Packs)
		{
			/* The count derives from the queue's cursors, so on weak
			 * memory it can move before MTGS's pop-side writes are
			 * visible; pair with their release before acting on it. */
			retro_atomic_thread_fence_acquire();
			break;
		}
		if (!retro_atomic_load_acquire_int(&s_open_flag))
			break; /* MTGS cancelled; see mtgs_main_loop's exit tail */
		key = retro_asym_eventcount_prepare_wait(s_ec_p1_progress);
		if (gif_path_pending_packets(path) != startP1Packs
		 || !retro_atomic_load_acquire_int(&s_open_flag))
		{
			retro_asym_eventcount_cancel_wait(s_ec_p1_progress);
			continue;
		}
		retro_asym_eventcount_commit_wait(s_ec_p1_progress, key);
	}
}

/* Waits for the GS to empty out the entire ring. The MTVU worker's wait
 * may exit once MTGS has finished a path-1 packet. */
void mtgs_wait_gs(bool isMTVU)
{
#ifdef ENABLE_PCSX2_PROFILER
	u64 t_wait0;
#endif
	/* See MTGSOwner.h for who drains and why it is decided by who
	 * produces. A producer's wait parks until the frontend's side has
	 * drained the ring, which every retro_run does; anyone else's wait
	 * drains it here. */
	if (!isMTVU)
	{
		if (MTGS_WAIT_DRAINS(sthread_get_current_thread_id(), s_producer_thread, 0))
		{
			/* Entries may have been written without a notify (a frame
			 * with no completed GIF packets between the vsync and here);
			 * without this mtgs_main_loop(true) returns at once. */
			work_eventcount_notify(&s_sem_event);
			mtgs_main_loop(true);
			return;
		}
		if (!mtgs_is_open())
			return;
		work_eventcount_notify(&s_sem_event);
		/* Blocks until mtgs_main_loop has committed every record written
		 * so far, the vsync among them. */
		if (s_RingOk)
			work_eventcount_wait_drained(&s_sem_event, mtgs_ring_drained, NULL);
		else
			work_eventcount_wait_empty(&s_sem_event);
		return;
	}
	if (!mtgs_is_open()) /* issued on a closed GS */
		return;

#ifdef ENABLE_PCSX2_PROFILER
	t_wait0 = __builtin_ia32_rdtsc();
#endif
	work_eventcount_notify(&s_sem_event);
	mtgs_wait_p1_progress();
#ifdef ENABLE_PCSX2_PROFILER
	g_ee_wait_ticks += __builtin_ia32_rdtsc() - t_wait0;
#endif
}

void mtgs_wait_for_close(void)
{
	/* Kick the thread if it's sleeping. */
	work_eventcount_notify(&s_sem_event);

	/* Ownership of the ring goes back to nobody; the GS stays open until
	 * mtgs_close_gs(), and a wait in between pumps the ring itself. */
	s_thread = 0;
}

/* The frontend's thread, first thing in every retro_run. s_thread is
 * what the vsync and blank-packet tests compare against -- "is this the
 * thread that renders" -- and no longer decides who drains a wait. */
void mtgs_claim_ring(void)
{
	s_thread = sthread_get_current_thread_id();
}

/* The frontend's thread, retro_run, after polling: the EE held behind
 * the last vsync starts the next frame, with this input. */
int mtgs_frame_gate_open(void)
{
	if (!s_gate_owed)
		return 0;
	s_gate_owed = 0;
	pad_input_latch();
	retro_atomic_fetch_add_int(&s_gate_granted, 1);
	mtgs_gate_notify();
	return 1;
}

/* The frontend's thread: on, an EE at the gate leaves it to stop there;
 * off before the EE is resumed. A stopped EE took no frame, so none is
 * owed to it. */
void mtgs_frame_gate_stop(int on)
{
	retro_atomic_store_release_int(&s_gate_stop, on);
	if (on)
	{
		s_gate_owed = 0;
		mtgs_gate_notify();
	}
}

/* The frontend's thread, retro_run, with the EE paused: the registers
 * are the drained vsync's, whatever has been loaded or written since. */
int mtgs_present_pending(void)
{
	if (!mtgs_pending.pending || !retro_atomic_load_acquire_int(&s_open_flag))
		return 0;
	mtgs_pending.pending = 0;
	memcpy(s_gs_regs, mtgs_pending.regs, sizeof(s_gs_regs));
	GS_HW_CONTEXT_BEGIN();
	GSvsync(mtgs_pending.field, mtgs_pending.registers_written != 0);
	GS_HW_CONTEXT_END();
	return 1;
}

/* The EE thread, as it starts and as it ends. See MTGSOwner.h. */
void mtgs_set_producer_thread(int on)
{
	s_producer_thread = on ? sthread_get_current_thread_id() : 0;
}

void mtgs_apply_settings(void)
{
	gs_update_config_emu();
	/* With unsynchronized downloads the EE reads GS memory without the
	 * ring, so it must not run while the renderer is being reopened. */
	if (gs_hw_download_mode() == GS_HW_DOWNLOAD_UNSYNCHRONIZED)
		mtgs_wait_gs(false);
}

/* Adds a finished GS packet to the ring. */
void Gif_AddCompletedGSPacket(const GS_Packet* gsPack, GIF_PATH _path)
{
	PacketTagType* tag = RingWriteBegin();
	if (!tag)
		return;
	if (gsPack->size == ~0u)
	{
		/* MTVU mode: the worker completes a real packet later. */
		tag->w.command = GS_RINGTYPE_MTVU_GSPACKET;
		tag->w.data[0] = 0;
		tag->w.data[1] = 0;
	}
	else
	{
		tag->w.command = GS_RINGTYPE_GSPACKET;
		tag->w.data[0] = gsPack->offset;
		tag->w.data[1] = gsPack->size;

		retro_atomic_fetch_add_int(&gifPathReadAmount[_path].v, gsPack->size);
	}
	tag->w.data[2] = (u32)_path;
	RingWriteEnd();
	work_eventcount_notify(&s_sem_event);
}

void Gif_AddBlankGSPacket(u32 _size, GIF_PATH _path)
{
	PacketTagType* tag;
	/* On the thread that renders there is no concurrent GS thread to
	 * observe readAmount between the add here and the subtract in
	 * mtgs_main_loop, so the entry is skipped: ~88% of them. */
	if (sthread_get_current_thread_id() == s_thread)
		return;

	retro_atomic_fetch_add_int(&gifPathReadAmount[_path].v, _size);
	tag = RingWriteBegin();
	if (!tag)
		return;

	tag->w.command = GS_RINGTYPE_GSPACKET;
	tag->w.data[0] = ~0u;
	tag->w.data[1] = _size;
	tag->w.data[2] = (u32)_path;

	RingWriteEnd();
	work_eventcount_notify(&s_sem_event);
}
