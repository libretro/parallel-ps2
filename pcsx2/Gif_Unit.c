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

#include <memalign.h>
#include <rthreads/rthreads.h>

#include "Common.h"
#include "../common/VectorIntrin.h"
#include "Gif_Unit.h"
#include "Hw.h"
#include "R5900.h"
#include "SaveState.h"
#include "VUmicro.h"
#include "Vif.h"
#include "Vif_Dma.h"
#include "ps2/HwInternal.h"

Gif_Unit gifUnit;
PCSX2_ALIGN(64) Gif_Path_ReadAmount gifPathReadAmount[3];

/* The GIF tag formats and the A+D registers the unit acts on, as
 * GS/GSRegs.h (C++) numbers them. */
enum
{
	GIF_FLG_PACKED  = 0,
	GIF_FLG_REGLIST = 1,
	GIF_FLG_IMAGE   = 2,
	GIF_FLG_IMAGE2  = 3
};
enum
{
	GIF_REG_A_D            = 0x0e,
	GIF_A_D_REG_BITBLTBUF  = 0x50,
	GIF_A_D_REG_TRXREG     = 0x52,
	GIF_A_D_REG_TRXDIR     = 0x53,
	GIF_A_D_REG_SIGNAL     = 0x60,
	GIF_A_D_REG_FINISH     = 0x61,
	GIF_A_D_REG_LABEL      = 0x62
};

/* Each path's buffer, and the part at its end a packet may run into
 * before it is moved back to the start. */
#define GIF_PATH_BUFFER_SIZE  (9u * 1024u * 1024u)
#define GIF_PATH_SAFE_ZONE    (1024u * 1024u + 1024u)

/* ---- GIF tags ------------------------------------------------------- */

static __ri void gif_tag_reset(Gif_Tag* t)
{
	memset(t, 0, sizeof(*t));
}

static __ri void gif_tag_packed_step(Gif_Tag* t)
{
	if (t->nLoop > 0)
	{
		t->nRegIdx++;
		if (t->nRegIdx >= t->nRegs)
		{
			t->nRegIdx = 0;
			t->nLoop--;
		}
	}
}

static __ri void gif_tag_analyze(Gif_Tag* t)
{
#if _M_SSE >= 0x200
	/* zero out bits for registers which shouldn't be tested */
	__m128i vregs = _mm_loadl_epi64((const __m128i*)t->tag.REGS);
	vregs = _mm_and_si128(vregs, _mm_srli_epi64(_mm_set1_epi32(0xFFFFFFFFu), (64 - t->nRegs * 4)));

	/* get upper nibbles, interleave with lower nibbles, clear upper bits from low nibbles */
	vregs = _mm_and_si128(_mm_unpacklo_epi8(vregs, _mm_srli_epi32(vregs, 4)), _mm_set1_epi8(0x0F));

	/* compare with GIF_REG_A_D, set hasAD if any lanes passed */
	t->hasAD = (_mm_movemask_epi8(_mm_cmpeq_epi8(vregs, _mm_set1_epi8(GIF_REG_A_D))) != 0);

	/* write out unpacked registers */
	_mm_storeu_si128((__m128i*)t->regs, vregs);
#elif (defined(_M_ARM64) || defined(__aarch64__))
	u64 REGS64;
	uint8x16_t vregs;
	uint8x16_t comp;

	/* zero out bits for registers which shouldn't be tested */
	memcpy(&REGS64, t->tag.REGS, sizeof(u64));
	REGS64 &= (0xFFFFFFFFFFFFFFFFULL >> (64 - t->nRegs * 4));
	vregs = vreinterpretq_u8_u64(vsetq_lane_u64(REGS64, vdupq_n_u64(0), 0));

	/* get upper nibbles, interleave with lower nibbles, clear upper bits from low nibbles */
	vregs = vandq_u8(vzip1q_u8(vregs, vshrq_n_u8(vregs, 4)), vdupq_n_u8(0x0F));

	/* compare with GIF_REG_A_D, set hasAD if any lanes passed */
	comp = vceqq_u8(vregs, vdupq_n_u8(GIF_REG_A_D));
	t->hasAD = vmaxvq_u8(comp) & 1;

	/* write out unpacked registers */
	vst1q_u8(t->regs, vregs);
#else
	/* Reference C implementation. */
	u32 r = t->tag.REGS[0];
	u32 i = 0;
	u32 j = pcsx2_min_u(t->nRegs, 8);
	t->hasAD = false;
	for (; i < j; i++)
	{
		t->regs[i] = r & 0xf;
		t->hasAD |= (t->regs[i] == GIF_REG_A_D);
		r >>= 4;
	}
	r = t->tag.REGS[1];
	j = t->nRegs;
	for (; i < j; i++)
	{
		t->regs[i] = r & 0xf;
		t->hasAD |= (t->regs[i] == GIF_REG_A_D);
		r >>= 4;
	}
#endif
}

static __ri void gif_tag_set(Gif_Tag* t, const u8* pMem, bool analyze)
{
	memcpy(&t->tag, pMem, sizeof(t->tag));
	t->nLoop   = t->tag.NLOOP;
	t->hasAD   = false;
	t->nRegIdx = 0;
	t->isValid = 1;
	t->len     = 0; /* avoid uninitialized compiler warning */
	switch (t->tag.FLG)
	{
		case GIF_FLG_PACKED:
			t->nRegs  = ((t->tag.NREG - 1) & 0xf) + 1;
			t->len    = (t->nRegs * t->tag.NLOOP) * 16;
			t->cycles = t->len << 1; /* Packed Mode takes 2 ee-cycles */
			if (analyze)
				gif_tag_analyze(t);
			break;
		case GIF_FLG_REGLIST:
			t->nRegs  = ((t->tag.NREG - 1) & 0xf) + 1;
			t->len    = ((t->nRegs * t->tag.NLOOP + 1) >> 1) * 16;
			t->cycles = t->len << 2; /* Reg-list Mode takes 4 ee-cycles */
			break;
		case GIF_FLG_IMAGE:
		case GIF_FLG_IMAGE2:
			t->nRegs   = 0;
			t->len     = t->tag.NLOOP * 16;
			t->cycles  = t->len << 2; /* Image Mode takes 4 ee-cycles */
			t->tag.FLG = GIF_FLG_IMAGE;
			break;
		default:
			break;
	}
}

/* ---- A+D writes the GIF itself acts on ----------------------------- */

bool Gif_HandlerAD(u8* pMem)
{
	const u32 reg   = pMem[8];
	const u32* data = (const u32*)pMem;
	if (reg >= GIF_A_D_REG_BITBLTBUF && reg <= GIF_A_D_REG_TRXREG)
	{
		memcpy(&vif1.transfer_registers[reg - GIF_A_D_REG_BITBLTBUF], pMem, sizeof(u64));
	}
	else if (reg == GIF_A_D_REG_TRXDIR)
	{ /* TRXDIR */
		if ((pMem[0] & 3) == 1)
		{                /* local -> host */
			u8 bpp = 32; /* Onimusha does TRXDIR without BLTDIVIDE first, assume 32bit */
			switch (vif1.BITBLTBUF.SPSM & 7)
			{
				case 0:
					bpp = 32;
					break;
				case 1:
					bpp = 24;
					break;
				case 2:
					bpp = 16;
					break;
				case 3:
					bpp = 8;
					break;
				default: /* 4 is 4 bit but this is forbidden */
					break;
			}
			/* qwords, rounded down; any extra bits are lost
			 * games must take care to ensure transfer rectangles are exact multiples of a qword */
			vif1.GSLastDownloadSize = vif1.TRXREG.RRW * vif1.TRXREG.RRH * bpp >> 7;
		}
	}
	else if (reg == GIF_A_D_REG_SIGNAL)
	{ /* SIGNAL */
		if (gsCSRload() & GS_CSR_SIGNAL)
		{ /* Time to ignore all subsequent drawing operations. */
			if (!gifUnit.gsSIGNAL.queued)
			{
				gifUnit.gsSIGNAL.queued = true;
				gifUnit.gsSIGNAL.data[0] = data[0];
				gifUnit.gsSIGNAL.data[1] = data[1];
				return true; /* Stalling SIGNAL */
			}
		}
		else
		{
			GS_SIGID_WORD = (GS_SIGID_WORD & ~data[1]) | (data[0] & data[1]);
			if (!(GS_IMR_WORD & GS_IMR_SIGMSK))
				hwIntcIrq(INTC_GS);
			gsCSRset(GS_CSR_SIGNAL);
		}
	}
	else if (reg == GIF_A_D_REG_FINISH) /* FINISH */
	{
		gifUnit.gsFINISH.gsFINISHFired = false;
		gifUnit.gsFINISH.gsFINISHPending = true;
	}
	else if (reg == GIF_A_D_REG_LABEL) /* LABEL */
	{
		GS_LBLID_WORD = (GS_LBLID_WORD & ~data[1]) | (data[0] & data[1]);
	}
	return false;
}

void Gif_FinishIRQ(void)
{
	if (gifUnit.gsFINISH.gsFINISHPending)
	{
		gsCSRset(GS_CSR_FINISH);
		gifUnit.gsFINISH.gsFINISHPending = false;
	}
	if ((gsCSRload() & GS_CSR_FINISH) && !(GS_IMR_WORD & GS_IMR_FINISHMSK) && !gifUnit.gsFINISH.gsFINISHFired)
	{
		hwIntcIrq(INTC_GS);
		gifUnit.gsFINISH.gsFINISHFired = true;
	}
}

/* ---- the MTVU packet queue (path 1) -------------------------------- */

static void gif_mtvu_init_queue(Gif_Path_MTVU* m, bool use_queue)
{
	if (m->gsPackQueueOk)
		retro_spsc_free(&m->gsPackQueue);
	m->gsPackQueueOk = use_queue && retro_spsc_init(&m->gsPackQueue,
		MTVU_GSPACK_QUEUE_RECORDS * sizeof(GS_Packet));
}

static void gif_mtvu_reset(Gif_Path_MTVU* m)
{
	m->fakePackets = 0;
	/* Both sides are quiesced at a VM reset, which is the only caller
	 * besides construction. */
	if (m->gsPackQueueOk)
		retro_spsc_clear(&m->gsPackQueue);
#if MTVU_GSPACK_QUEUE_HIGH_WATER
	if (m->gsPackQueuePeak)
		log_cb(RETRO_LOG_INFO, "MTVU gsPackQueue peak occupancy: %u of %u records\n",
			m->gsPackQueuePeak, (u32)MTVU_GSPACK_QUEUE_RECORDS);
	m->gsPackQueuePeak = 0;
#endif
	m->fakePacket.offset     = 0;
	m->fakePacket.size       = ~0u; /* Used to indicate that its a fake packet */
	m->fakePacket.cycles     = 0;
	m->fakePacket.readAmount = 0;
}

/* ---- the paths ----------------------------------------------------- */

static __fi void gif_pack_clear(GS_Packet* p, u32 offset)
{
	p->offset     = offset;
	p->size       = 0;
	p->cycles     = 0;
	p->readAmount = 0;
}

/* Path 1 is the VU1 thread's when it runs VU1. */
static __fi bool gif_path_is_mtvu(const Gif_Path* path)
{
	return !path->idx && emu_config_thread_vu1();
}

static __fi s32 gif_path_read_amount(Gif_Path* path)
{
	return retro_atomic_load_acquire_int(&gifPathReadAmount[path->idx].v) + path->gsPack.readAmount;
}

static __fi bool gif_path_has_data(const Gif_Path* path)
{
	return path->curOffset < path->curSize;
}

static __fi bool gif_path_done(const Gif_Path* path)
{
	return gif_path_is_mtvu(path)
		? !path->mtvu.fakePackets
		: (!gif_path_has_data(path) && (path->state == GIF_PATH_IDLE || path->state == GIF_PATH_WAIT));
}

static void gif_path_reset(Gif_Path* path, bool softReset)
{
	path->state = GIF_PATH_IDLE;
	if (softReset)
	{
		if (!gif_path_is_mtvu(path)) /* MTVU Freaks out if you try to reset it, so let's just let it transfer */
		{
			gif_tag_reset(&path->gifTag);
			gif_pack_clear(&path->gsPack, path->curOffset);
			path->curSize = path->curOffset;
		}
		return;
	}
	gif_mtvu_reset(&path->mtvu);
	path->curSize   = 0;
	path->curOffset = 0;
	retro_atomic_store_release_int(&path->readAmount, 0);
	retro_atomic_store_release_int(&gifPathReadAmount[path->idx].v, 0);
	gif_tag_reset(&path->gifTag);
	gif_pack_clear(&path->gsPack, 0);
}

void gif_path_realign(Gif_Path* path)
{
	const s32 offset    = path->curOffset - path->gsPack.size;
	const s32 sizeToAdd = path->curSize - offset;
	s32 intersect       = sizeToAdd - offset;
	/* Already at the start: nothing moves and nothing is skipped. Waiting
	 * for the GS here waits on a count that only finishing the packet
	 * clears -- on the VU1 thread, forever. */
	if (offset == 0)
		return;
	if (intersect < 0)
		intersect = 0;
	for (;;)
	{
		const s32 frontFree = offset - gif_path_read_amount(path);
		if (frontFree >= sizeToAdd - intersect)
			break;
		mtgs_wait_gs(gif_path_is_mtvu(path));
	}
	if (offset < (s32)path->buffLimit)
	{ /* Needed for correct readAmount values */
		if (gif_path_is_mtvu(path))
			path->gsPack.readAmount += path->buffLimit - offset;
		else
			Gif_AddBlankGSPacket(path->buffLimit - offset, path->idx);
	}
	if (intersect)
		memmove(path->buffer, &path->buffer[offset], path->curSize - offset);
	else
		memcpy(path->buffer, &path->buffer[offset], path->curSize - offset);
	path->curSize -= offset;
	path->curOffset = path->gsPack.size;
	path->gsPack.offset = 0;
}

/* Moves the packet to the front of the buffer, out of the way of the
 * copy. A packet whose EOP never comes outgrows the buffer: the GIF
 * streams on through it, so the part already parsed goes to the GS
 * then and only the unparsed tail is kept. */
static void gif_path_make_room(Gif_Path* path, u32 size)
{
	gif_path_realign(path);
	if (path->curSize + size <= path->buffSize || !path->gsPack.size)
		return;
	if (gif_path_is_mtvu(path))
	{
		gif_path_finish_mtvu(path, false);
		Gif_MTVU_KickSema();
	}
	else
	{
		Gif_AddCompletedGSPacket(&path->gsPack, path->idx);
		path->gsPack.offset = path->curOffset;
		path->gsPack.size   = 0;
	}
	gif_path_realign(path);
}

void gif_path_copy(Gif_Path* path, const u8* pMem, u32 size)
{
	const bool mtvu = gif_path_is_mtvu(path);

	if (path->curSize + size > path->buffSize)
		gif_path_make_room(path, size);
	for (;;)
	{
		const s32 offset  = path->curOffset - path->gsPack.size;
		const s32 readPos = offset - gif_path_read_amount(path);
		if (readPos >= 0)
			break; /* MTGS is reading in back of curOffset */
		if ((s32)path->buffLimit + readPos > (s32)path->curSize + (s32)size)
			break; /* Enough free front space */
		if (mtvu && !gif_path_pending_packets(path) && (path->gsPack.size || path->gsPack.readAmount))
		{
			/* Nothing for MTGS to pop and the current (unfinished) packet
			 * holds the space -- its data, or only the part of the buffer
			 * a realign skipped, which nothing else hands over: give it
			 * to MTGS as a PARTIAL packet so the buffer can drain (see
			 * gif_path_finish_mtvu). */
			gif_path_finish_mtvu(path, false);
			Gif_MTVU_KickSema();
			continue;
		}
		mtgs_wait_gs(mtvu); /* Let MTGS run to free up buffer space */
	}
	memcpy(&path->buffer[path->curSize], pMem, size);
	path->curSize += size;
}

/* If completed a GS packet (with EOP) then set done to true.
 * MTVU: This function only should be called on the EE thread. */
static GS_Packet gif_path_execute(Gif_Path* path, bool* done)
{
	const bool mtvu = gif_path_is_mtvu(path);

	if (path->mtvu.fakePackets)
	{ /* For MTVU mode... */
		path->mtvu.fakePackets--;
		*done = true;
		return path->mtvu.fakePacket;
	}
	for (;;)
	{
		if (!path->gifTag.isValid)
		{ /* Need new Gif Tag */
			/* We don't have enough data for a Gif Tag */
			if (path->curOffset + 16 > path->curSize)
				return path->gsPack;

			/* Move packet to start of buffer */
			if (path->curOffset > path->buffLimit)
				gif_path_realign(path);

			gif_tag_set(&path->gifTag, &path->buffer[path->curOffset], true);

			path->state = (GIF_PATH_STATE)(path->gifTag.tag.FLG + 1);
			/* We don't have enough data for a complete GS packet */
			if (!path->gifTag.hasAD && path->curOffset + 16 + path->gifTag.len > path->curSize)
			{
				path->gifTag.isValid = false; /* So next time we test again */
				return path->gsPack;
			}

			path->gsPack.size   += 16;                      /* Tag Size */
			path->curOffset     += 16;                      /* Tag Size */
			path->gsPack.cycles += 2 + path->gifTag.cycles; /* Tag + Len ee-cycles */
		}

		if (path->gifTag.hasAD)
		{ /* Only can be true if GIF_FLG_PACKED */
			bool dblSIGNAL = false;
			while (path->gifTag.nLoop && !dblSIGNAL)
			{
				if (path->curOffset + 16 > path->curSize)
					return path->gsPack; /* Exit Early */
				if (path->gifTag.regs[path->gifTag.nRegIdx & 0xf] == GIF_REG_A_D)
				{
					if (!mtvu)
						dblSIGNAL = Gif_HandlerAD(&path->buffer[path->curOffset]);
				}
				path->gsPack.size += 16; /* 1 QWC */
				path->curOffset   += 16; /* 1 QWC */
				gif_tag_packed_step(&path->gifTag);
			}
			if (dblSIGNAL && !(path->gifTag.tag.EOP && !path->gifTag.nLoop))
				return path->gsPack; /* Exit Early */
		}
		else
		{
			path->gsPack.size += path->gifTag.len; /* Data length */
			path->curOffset   += path->gifTag.len; /* Data length */
		}

		/* Reload gif tag next loop */
		path->gifTag.isValid = false;

		if (path->gifTag.tag.EOP)
		{
			const GS_Packet t = path->gsPack;
			*done = true;

			path->dmaRewind = 0;

			gif_pack_clear(&path->gsPack, path->curOffset);
			/* Path 3 Masking is timing sensitive, we need to simulate its length! (NFSU2/Outrun 2006) */

			if ((gifRegs.stat.APATH - 1) == GIF_PATH_3)
			{
				path->state = GIF_PATH_WAIT;

				if (path->curSize - path->curOffset > 0 && (gifRegs.stat.M3R || gifRegs.stat.M3P))
				{
					/* Including breaking packets early (Rewind DMA to pick up where left off)
					 * but only do this when the path is masked, else we're pointlessly slowing things down. */
					path->dmaRewind = path->curSize - path->curOffset;
					path->curSize   = path->curOffset;
				}
			}
			else
				path->state = GIF_PATH_IDLE;

			return t; /* Complete GS packet */
		}
	}
}

/* MTVU: Gets called on VU XGkicks on the MTVU thread */
static void gif_path_execute_mtvu(Gif_Path* path)
{
	/* Move packet to start of buffer */
	if (path->curOffset > path->buffLimit)
		gif_path_realign(path);
	for (;;)
	{ /* needed to be processed by pcsx2... */
		if (path->curOffset + 16 > path->curSize)
			break;
		gif_tag_set(&path->gifTag, &path->buffer[path->curOffset], true);

		if (!path->gifTag.hasAD && path->curOffset + 16 + path->gifTag.len > path->curSize)
			break;
		path->gsPack.size += 16; /* Tag Size */
		path->curOffset   += 16; /* Tag Size */

		if (path->gifTag.hasAD)
		{ /* Only can be true if GIF_FLG_PACKED */
			while (path->gifTag.nLoop)
			{
				if (path->curOffset + 16 > path->curSize)
					break; /* Exit Early */
				if (path->gifTag.regs[path->gifTag.nRegIdx & 0xf] == GIF_REG_A_D)
					Gif_HandlerAD_MTVU(&path->buffer[path->curOffset]);
				path->gsPack.size += 16; /* 1 QWC */
				path->curOffset   += 16; /* 1 QWC */
				gif_tag_packed_step(&path->gifTag);
			}
		}
		else
		{
			path->gsPack.size += path->gifTag.len; /* Data length */
			path->curOffset   += path->gifTag.len; /* Data length */
		}
		if (path->curOffset >= path->curSize)
			break;
		if (path->gifTag.tag.EOP)
			break;
	}
	path->gifTag.isValid = false;
}

/* MTVU: Gets called after VU1 execution on MTVU thread (final), and from
 * gif_path_copy's would-block path (not final) to hand a PARTIAL packet to
 * MTGS. Without partial flushes a continuous VU1 microprogram (endless loop
 * streaming XGKICK, no E-bit -- GT3's arcade attract) is a deadlock: this
 * thread waits for MTGS to drain the path1 buffer while MTGS waits in
 * semaXGkick for a program end that never comes. The MTVU queue reuses
 * GS_Packet.cycles (never set on this path) as the "more packets follow for
 * this program" flag; MTGS's MTVU_GSPACKET handler consumes packets until it
 * sees a final one (cycles == 0). */
void gif_path_finish_mtvu(Gif_Path* path, bool final_packet)
{
	path->gsPack.cycles = final_packet ? 0 : 1;
	/* Counted where every space check reads it: the GS thread takes it
	 * off once it has sent the packet (MTGS.cpp, MTVU_GSPACKET). */
	retro_atomic_fetch_add_int(&gifPathReadAmount[path->idx].v, path->gsPack.size + path->gsPack.readAmount);
	/* Wait for room for a whole packet before writing any of it: a short
	 * write would put a partial record in the queue and every packet after
	 * it would be read off by one. */
	if (path->mtvu.gsPackQueueOk)
	{
		void* dst;
		while (retro_spsc_write_begin(&path->mtvu.gsPackQueue, &dst) < sizeof(GS_Packet))
			;
		memcpy(dst, &path->gsPack, sizeof(GS_Packet));
		retro_spsc_write_end(&path->mtvu.gsPackQueue, sizeof(GS_Packet));
#if MTVU_GSPACK_QUEUE_HIGH_WATER
		{
			const u32 occ = (u32)((MTVU_GSPACK_QUEUE_RECORDS * sizeof(GS_Packet)
				- retro_spsc_write_avail(&path->mtvu.gsPackQueue)) / sizeof(GS_Packet));
			if (occ > path->mtvu.gsPackQueuePeak)
				path->mtvu.gsPackQueuePeak = occ;
		}
#endif
	}

	gif_pack_clear(&path->gsPack, path->curOffset);
}

/* MTVU: Gets called by the MTGS thread */
GS_Packet gif_path_get_packet_mtvu(Gif_Path* path)
{
	const void* src;
	GS_Packet pkt;
	if (path->mtvu.gsPackQueueOk && retro_spsc_read_begin(&path->mtvu.gsPackQueue, &src) >= sizeof(GS_Packet))
	{
		memcpy(&pkt, src, sizeof(GS_Packet));
		return pkt;
	}
	gif_pack_clear(&pkt, 0); /* size 0: nothing there */
	return pkt;
}

/* MTVU: Gets called by the MTGS thread */
void gif_path_pop_packet_mtvu(Gif_Path* path)
{
	/* The span was already established by the gif_path_get_packet_mtvu
	 * that precedes every pop; skip commits it without deriving it a
	 * second time. */
	if (path->mtvu.gsPackQueueOk)
		retro_spsc_skip(&path->mtvu.gsPackQueue, sizeof(GS_Packet));
}

/* MTVU: the GS packets MTGS has yet to process */
u32 gif_path_pending_packets(Gif_Path* path)
{
	if (!path->mtvu.gsPackQueueOk)
		return 0;
	return (u32)(retro_spsc_read_avail(&path->mtvu.gsPackQueue) / sizeof(GS_Packet));
}

/* ---- the unit ------------------------------------------------------ */

bool gif_unit_init(void)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		Gif_Path* path = &gifUnit.gifPath[i];
		if (path->buffer)
			continue;
		path->idx       = (GIF_PATH)i;
		path->buffSize  = GIF_PATH_BUFFER_SIZE;
		path->buffLimit = GIF_PATH_BUFFER_SIZE - GIF_PATH_SAFE_ZONE;
		path->buffer    = (u8*)memalign_alloc(16, GIF_PATH_BUFFER_SIZE);
		if (!path->buffer)
		{
			gif_unit_shutdown();
			return false;
		}
		/* MTVU only ever drives path 1 (MTVU.cpp posts exclusively to
		 * gifPath[GIF_PATH_1]), so the packet queue exists only there. */
		gif_mtvu_init_queue(&path->mtvu, i == GIF_PATH_1);
		gif_path_reset(path, false);
	}
	gifUnit.lastTranType = GIF_TRANS_INVALID;
	return true;
}

void gif_unit_shutdown(void)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		Gif_Path* path = &gifUnit.gifPath[i];
		if (path->mtvu.gsPackQueueOk)
			retro_spsc_free(&path->mtvu.gsPackQueue);
		path->mtvu.gsPackQueueOk = false;
		memalign_free(path->buffer);
		path->buffer = NULL;
	}
}

static __fi bool gif_unit_can_do_gif(void)
{
	return gifRegs.stat.PSE == 0 && gifRegs.stat.DIR == 0 && gifUnit.gsSIGNAL.queued == 0;
}

static __fi bool gif_unit_can_do_p3_slice(void)
{
	return gifRegs.stat.IMT == 1 && gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IMAGE;
}

/* Mask stops the next packet which hasnt started from transferring */
bool gif_unit_path3_masked(void)
{
	return (gifRegs.stat.M3R || gifRegs.stat.M3P)
		&& (gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifUnit.gifPath[GIF_PATH_3].state == GIF_PATH_WAIT);
}

/* XGkick */
bool gif_unit_can_do_path1(void)
{
	return (gifRegs.stat.APATH == 0 || gifRegs.stat.APATH == 1 || (gifRegs.stat.APATH == 3 && gif_unit_can_do_p3_slice())) && gif_unit_can_do_gif();
}

/* Direct */
bool gif_unit_can_do_path2(void)
{
	return (gifRegs.stat.APATH == 0 || gifRegs.stat.APATH == 2 || (gifRegs.stat.APATH == 3 && gif_unit_can_do_p3_slice())) && gif_unit_can_do_gif();
}

/* DirectHL */
bool gif_unit_can_do_path2hl(void)
{
	return (gifRegs.stat.APATH == 0 || gifRegs.stat.APATH == 2) && gif_unit_can_do_gif();
}

/* Gif DMA */
bool gif_unit_can_do_path3(void)
{
	return ((gifRegs.stat.APATH == 0 && !gif_unit_path3_masked()) || gifRegs.stat.APATH == 3) && gif_unit_can_do_gif();
}

static __fi int gif_unit_check_queued(bool p1, bool p2, bool p3)
{
	return ((p1 && gifRegs.stat.P1Q) << 0)
	     | ((p2 && gifRegs.stat.P2Q) << 1)
	     | ((p3 && gifRegs.stat.P3Q) << 2);
}

/* A bit set for each given path that is not finished (needs more data or
 * processing for an EOP). */
static __fi int gif_unit_paths(bool p1, bool p2, bool p3, bool checkQ)
{
	const int ret = ((p1 && !gif_path_done(&gifUnit.gifPath[GIF_PATH_1])) << 0)
	              | ((p2 && !gif_path_done(&gifUnit.gifPath[GIF_PATH_2])) << 1)
	              | ((p3 && !gif_path_done(&gifUnit.gifPath[GIF_PATH_3])) << 2);
	return ret | (checkQ ? gif_unit_check_queued(p1, p2, p3) : 0);
}

int gif_unit_check_paths(bool p1, bool p2, bool p3)
{
	return gif_unit_paths(p1, p2, p3, false);
}

int gif_unit_check_paths_queued(bool p1, bool p2, bool p3)
{
	return gif_unit_paths(p1, p2, p3, true);
}

/* Enable softReset when resetting during game emulation */
void gif_unit_reset(bool softReset)
{
	gifRegs.stat._u32 = 0;
	gifRegs.ctrl._u32 = 0;
	gifRegs.mode._u32 = 0;
	gsCSRfield(GS_CSR_FIFO, (u32)CSR_FIFO_EMPTY << GS_CSR_FIFO_SH); /* This is the GIF unit side FIFO, not DMA! */
	gifUnit.gsSIGNAL.data[0]       = 0;
	gifUnit.gsSIGNAL.data[1]       = 0;
	gifUnit.gsSIGNAL.queued        = false;
	gifUnit.gsFINISH.gsFINISHFired = false;
	gif_path_reset(&gifUnit.gifPath[0], softReset);
	gif_path_reset(&gifUnit.gifPath[1], softReset);
	gif_path_reset(&gifUnit.gifPath[2], softReset);
	if (!softReset)
		gifUnit.lastTranType = GIF_TRANS_INVALID;
	/* If the VIF has paused waiting for PATH3, recheck it after the reset has occurred (Eragon) */
	if (vif1Regs.stat.VGW)
	{
		if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
			CPU_INT(DMAC_VIF1, 1);
	}
}

u32 gif_unit_get_packet_size(GIF_PATH pathIdx, u8* pMem, u32 offset, u32 size, bool flush)
{
	const u32 memMask = pathIdx ? ~0u : 0x3fffu;
	const bool cut    = emu_config_xgkick_hack() || !emu_config_enable_vu1();
	u32 curSize       = 0;
	for (;;)
	{
		Gif_Tag gifTag;
		gif_tag_set(&gifTag, &pMem[offset & memMask], false);
		curSize += 16 + gifTag.len; /* Tag + Data Length */
		offset  += 16 + gifTag.len; /* Tag + Data Length */
		if (pathIdx == GIF_PATH_1 && curSize >= 0x4000)
			return 0; /* Bios does this... (Fixed if you delay vu1's xgkick by 103 vu cycles) */
		if (curSize >= size)
			return size;
		if (((flush && gifTag.tag.EOP) || !flush) && cut)
			return curSize | ((u32)gifTag.tag.EOP << 31);
		if (gifTag.tag.EOP)
			return curSize;
	}
}

/* Send processed GS Primitive(s) to the MTGS thread.
 * Note: Only does so if current path fully completed all of its given gs
 * primitives (but didn't upload them yet). */
void gif_unit_flush_to_mtgs(void)
{
	Gif_Path* path;
	if (!gifRegs.stat.APATH)
		return;
	path = &gifUnit.gifPath[gifRegs.stat.APATH - 1];
	if (path->gsPack.size && !path->gifTag.isValid)
	{
		Gif_AddCompletedGSPacket(&path->gsPack, (GIF_PATH)(gifRegs.stat.APATH - 1));
		path->gsPack.offset = path->curOffset;
		path->gsPack.size   = 0;
	}
}

/* Processes gif packets and performs path arbitration on EOPs or on Path 3
 * Images when IMT is set. */
int gif_unit_execute(bool isPath3, bool isResume)
{
	bool didPath3;
	bool path3Check;
	int curPath;

	(void)isResume;
	if (!gif_unit_can_do_gif())
		return 0;
	didPath3   = false;
	path3Check = isPath3;
	curPath    = gifRegs.stat.APATH > 0 ? gifRegs.stat.APATH - 1 : 0; /* Init to zero if no path is already set. */
	gifUnit.gifPath[2].dmaRewind = 0;
	gifRegs.stat.OPH = 1;

	for (;;)
	{
		if (gifRegs.stat.APATH)
		{ /* Some Transfer is happening */
			Gif_Path* path = &gifUnit.gifPath[gifRegs.stat.APATH - 1];
			bool done      = false;
			GS_Packet gsPack = gif_path_execute(path, &done);
			if (!done)
			{
				if (gifRegs.stat.APATH == 3 && gif_unit_can_do_p3_slice() && !gifUnit.gsSIGNAL.queued)
				{
					if (!didPath3 && gif_unit_check_paths(1, 1, 0))
					{ /* Path3 slicing */
						didPath3 = true;
						gifRegs.stat.APATH = 0;
						gifRegs.stat.IP3 = 1;
						if (gsPack.size > 16)
						{ /* Packet had other tags which we already processed */
							const u32 subOffset = path->gifTag.isValid ? 16 : 0; /* if isValid, image-primitive not finished */
							gsPack.size -= subOffset;                     /* Remove the image-tag (should be last thing read) */
							Gif_AddCompletedGSPacket(&gsPack, GIF_PATH_3); /* Consider current packet complete */

							path->curOffset -= subOffset;                 /* Start the next GS packet at the image-tag */

							/* Reset gs packet info */
							gif_pack_clear(&path->gsPack, path->curOffset); /* Set to image-tag */

							path->gifTag.isValid = false; /* Reload tag next execute */
						}
						continue;
					}
				}
				break; /* Not finished with GS packet */
			}
			if (gifUnit.gifPath[curPath].state == GIF_PATH_WAIT || gifUnit.gifPath[curPath].state == GIF_PATH_IDLE)
				Gif_AddCompletedGSPacket(&gsPack, (GIF_PATH)(gifRegs.stat.APATH - 1));
		}
		if (!gifUnit.gsSIGNAL.queued && !gif_path_done(&gifUnit.gifPath[0]))
		{
			gifRegs.stat.APATH = 1;
			gifRegs.stat.P1Q = 0;
			curPath = 0;
		}
		else if (!gifUnit.gsSIGNAL.queued && !gif_path_done(&gifUnit.gifPath[1]))
		{
			gifRegs.stat.APATH = 2;
			gifRegs.stat.P2Q = 0;
			curPath = 1;
		}
		else if (!gifUnit.gsSIGNAL.queued && !gif_path_done(&gifUnit.gifPath[2]) && !gif_unit_path3_masked())
		{
			gifRegs.stat.APATH = 3;
			gifRegs.stat.P3Q = 0;
			gifRegs.stat.IP3 = 0;
			curPath = 2;
			path3Check = true;
		}
		else
		{
			/* If PATH3 was stalled due to another transfer but the DMA ended, it'll never check this
			 * So lets quickly check if it's currently set to path3 */
			if (gifRegs.stat.APATH == 3 || path3Check)
				gifCheckPathStatus(true);
			else
			{
				if (vif1Regs.stat.VGW)
				{
					/* Check if VIF is in a cycle or is currently "idle" waiting for GIF to come back. */
					if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
						CPU_INT(DMAC_VIF1, 1);
				}

				gifRegs.stat.APATH = 0;
				gifRegs.stat.OPH = 0;
			}

			break;
		}
	}
	/* Some loaders/Refresh Rate selectors and things dont issue "End of Packet" commands
	 * So we look and see if the end of the last tag is all there, if so, stick it in the buffer for the GS :)
	 * (Invisible Screens on Terminator 3 and Growlanser 2/3) */
	if (gifUnit.gifPath[curPath].curOffset == gifUnit.gifPath[curPath].curSize)
		gif_unit_flush_to_mtgs();

	Gif_FinishIRQ();
	if (!gif_unit_check_paths_queued(gifRegs.stat.APATH != 1, gifRegs.stat.APATH != 2, gifRegs.stat.APATH != 3))
		Gif_FinishIRQ();

	/* Path3 can rewind the DMA, so we send back the amount we go back! */
	if (isPath3)
		return gifUnit.gifPath[2].dmaRewind;
	return 0;
}

u32 gif_unit_transfer(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size)
{
	if (emu_config_thread_vu1())
	{
		Gif_Path* path1 = &gifUnit.gifPath[GIF_PATH_1];
		if (tranType == GIF_TRANS_XGKICK)
		{ /* This is on the MTVU thread */
			gif_path_copy(path1, pMem, size);
			gif_path_execute_mtvu(path1);
			return size;
		}
		if (tranType == GIF_TRANS_MTVU)
		{ /* This is on the EE thread */
			path1->mtvu.fakePackets++;
			if (gif_unit_can_do_gif())
				gif_unit_execute(false, true);
			return 0;
		}
	}

	if (size == 0)
		return 0;
	gifUnit.lastTranType = tranType;

	if (tranType == GIF_TRANS_DMA)
	{
		if (!gif_unit_can_do_path3())
		{
			if (!gif_unit_path3_masked())
				gifRegs.stat.P3Q = 1;
			return 0;
		} /* DMA Stall */
	}
	if (tranType == GIF_TRANS_XGKICK)
	{
		/* We always buffer path1 packets */
		if (!gif_unit_can_do_path1())
			gifRegs.stat.P1Q = 1;
	}
	if (tranType == GIF_TRANS_DIRECT)
	{
		if (!gif_unit_can_do_path2())
		{
			gifRegs.stat.P2Q = 1;
			return 0;
		} /* Direct Stall */
	}
	if (tranType == GIF_TRANS_DIRECTHL)
	{
		if (!gif_unit_can_do_path2hl())
		{
			gifRegs.stat.P2Q = 1;
			return 0;
		} /* DirectHL Stall */
	}

	gif_path_copy(&gifUnit.gifPath[tranType & 3], pMem, size);
	size -= gif_unit_execute(tranType == GIF_TRANS_DMA, false);
	return size;
}

/* ---- savestates ---------------------------------------------------- */

bool gifPathFreeze(SaveStateBase* s, u32 pathIdx)
{
	Gif_Path* path = &gifUnit.gifPath[pathIdx];
	u8* bufferPtr;
	u32 realBuffSize, realBuffLimit;

	if (!gif_path_is_mtvu(path)) /* FixMe: savestate freeze bug (Gust games) with MTVU enabled */
	{
		if (SaveState_IsSaving(s)) /* Move all the buffered data to the start of buffer */
			gif_path_realign(path); /* May add readAmount which we need to clear on load */
	}
	/* The struct is frozen whole, so the buffer pointer and the two sizes
	 * that describe the allocation all come back from the state. The
	 * pointer was already being kept -- these are the same thing: buffSize
	 * and buffLimit are what every bound check in Gif_Path is expressed
	 * against, so taking them from the file means the file sets its own
	 * limits against a buffer that is whatever init allocated. */
	bufferPtr     = path->buffer;
	realBuffSize  = path->buffSize;
	realBuffLimit = path->buffLimit;
	SaveState_Freeze(s, path->mtvu.fakePackets);
	SaveState_FreezeMem(s, path, sizeof(*path) - sizeof(path->mtvu));
	path->buffer    = bufferPtr;
	path->buffSize  = realBuffSize;
	path->buffLimit = realBuffLimit;
	/* And curSize is the length of the copy that follows, read out of the
	 * struct the line above just overwrote. Clamped on the way in only --
	 * on the way out these are live values and already consistent. */
	if (SaveState_IsLoading(s))
	{
		/* A path whose packet does not sit where its sizes say -- more
		 * buffered than the buffer holds, the tag past the data, a packet
		 * that starts before the buffer -- would realign from outside it,
		 * and the bytes after it in the state no longer line up: the path
		 * starts empty and the load fails. */
		if (path->curSize > realBuffSize || path->curOffset > path->curSize
		 || path->gsPack.size > path->curOffset
		 || path->gsPack.offset != path->curOffset - path->gsPack.size)
		{
			path->curSize   = 0;
			path->curOffset = 0;
			gif_tag_reset(&path->gifTag);
			gif_pack_clear(&path->gsPack, 0);
			path->state = GIF_PATH_IDLE;
			s->error    = true;
			return false;
		}
	}
	SaveState_FreezeMem(s, bufferPtr, path->curSize);
	if (!SaveState_IsSaving(s))
	{
		retro_atomic_store_release_int(&path->readAmount, 0);
		retro_atomic_store_release_int(&gifPathReadAmount[pathIdx].v, 0);
		path->gsPack.readAmount = 0;
	}
	return SaveState_IsOkay(s);
}

bool gifFreeze(SaveStateBase* s)
{
	bool mtvuMode = emu_config_thread_vu1();
	mtgs_wait_gs(false);
	if (!(SaveState_FreezeTag(s, "Gif Unit")))
		return false;
	SaveState_Freeze(s, mtvuMode);
	SaveState_Freeze(s, gifRegs.stat);
	SaveState_Freeze(s, gifUnit.gsSIGNAL);
	SaveState_Freeze(s, gifUnit.gsFINISH);
	SaveState_Freeze(s, gifUnit.lastTranType);
	/* Every other Freeze reports the cursor's state; this one returned true
	 * whatever happened, so a state that failed inside a path was carried
	 * on from as though it had loaded. */
	if (!gifPathFreeze(s, GIF_PATH_1))
		return false;
	if (!gifPathFreeze(s, GIF_PATH_2))
		return false;
	if (!gifPathFreeze(s, GIF_PATH_3))
		return false;
	return true;
}

/* ---- the unit as C asks after it (Gif_hw.h) ------------------------- */

u32 gif_unit_transfer_dma(u8* pMem, u32 size) { return gif_unit_transfer(GIF_TRANS_DMA, pMem, size); }
u32 gif_unit_transfer_direct(u8* pMem, u32 size, bool hl) { return gif_unit_transfer(hl ? GIF_TRANS_DIRECTHL : GIF_TRANS_DIRECT, pMem, size); }
bool gif_unit_signal_queued(void) { return gifUnit.gsSIGNAL.queued != 0; }
bool gif_path_is_done(int path) { return gif_path_done(&gifUnit.gifPath[path]); }
bool gif_path_is_idle(int path) { return gifUnit.gifPath[path].state == GIF_PATH_IDLE; }
int gif_path_state(int path) { return (int)gifUnit.gifPath[path].state; }
void gif_path_set_state(int path, int state) { gifUnit.gifPath[path].state = (GIF_PATH_STATE)state; }
