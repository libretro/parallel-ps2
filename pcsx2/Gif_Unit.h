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

#pragma once
#include "common/Pcsx2Defs.h"
#include <memalign.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <cstring> /* memset */

#include "../common/VectorIntrin.h"

#include "Gif.h"
#include "Vif.h"
#include "GS.h"
#include "GS/GSRegs.h"

struct GS_Packet;
extern void Gif_FinishIRQ();
extern void Gif_MTVU_KickSema(); // MTVU.cpp: notify vu1Thread.ecXGkick
extern bool Gif_HandlerAD(u8* pMem);
extern void Gif_HandlerAD_MTVU(u8* pMem);
extern void Gif_AddBlankGSPacket(u32 size, GIF_PATH path);
extern void Gif_AddCompletedGSPacket(GS_Packet& gsPack, GIF_PATH path);

struct Gif_Tag
{
	struct HW_Gif_Tag
	{
		u16 NLOOP : 15;
		u16 EOP : 1;
		u16 _dummy0 : 16;
		u32 _dummy1 : 14;
		u32 PRE : 1;
		u32 PRIM : 11;
		u32 FLG : 2;
		u32 NREG : 4;
		u32 REGS[2];
	} tag;

	u32 nLoop;    // NLOOP left to process
	u32 nRegs;    // NREG (1~16)
	u32 nRegIdx;  // Current nReg Index (packed mode processing)
	u32 len;      // Packet Length in Bytes (not including tag)
	u32 cycles;   // Time needed to process packet data in ee-cycles
	u8 regs[16];  // Regs
	bool hasAD;   // Has an A+D Write
	bool isValid; // Tag is valid

	__ri Gif_Tag() { Reset(); }
	__ri Gif_Tag(u8* pMem, bool analyze = false)
	{
		setTag(pMem, analyze);
	}

	__ri void Reset() { memset(this, 0, sizeof(*this)); }

	__ri void packedStep()
	{
		if (nLoop > 0)
		{
			nRegIdx++;
			if (nRegIdx >= nRegs)
			{
				nRegIdx = 0;
				nLoop--;
			}
		}
	}

	__ri void setTag(u8* pMem, bool analyze = false)
	{
		tag = *(HW_Gif_Tag*)pMem;
		nLoop = tag.NLOOP;
		hasAD = false;
		nRegIdx = 0;
		isValid = 1;
		len = 0; // avoid uninitialized compiler warning
		switch (tag.FLG)
		{
			case GIF_FLG_PACKED:
				nRegs = ((tag.NREG - 1) & 0xf) + 1;
				len = (nRegs * tag.NLOOP) * 16;
				cycles = len << 1; // Packed Mode takes 2 ee-cycles
				if (analyze)
					analyzeTag();
				break;
			case GIF_FLG_REGLIST:
				nRegs = ((tag.NREG - 1) & 0xf) + 1;
				len = ((nRegs * tag.NLOOP + 1) >> 1) * 16;
				cycles = len << 2; // Reg-list Mode takes 4 ee-cycles
				break;
			case GIF_FLG_IMAGE:
			case GIF_FLG_IMAGE2:
				nRegs = 0;
				len = tag.NLOOP * 16;
				cycles = len << 2; // Image Mode takes 4 ee-cycles
				tag.FLG = GIF_FLG_IMAGE;
				break;
			default:
				break;
		}
	}

	__ri void analyzeTag(void)
	{
#if _M_SSE >= 0x200
		/* SSE2 implementation */

		// zero out bits for registers which shouldn't be tested
		__m128i vregs = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(tag.REGS));
		vregs = _mm_and_si128(vregs, _mm_srli_epi64(_mm_set1_epi32(0xFFFFFFFFu), (64 - nRegs * 4)));

		// get upper nibbles, interleave with lower nibbles, clear upper bits from low nibbles
		vregs = _mm_and_si128(_mm_unpacklo_epi8(vregs, _mm_srli_epi32(vregs, 4)), _mm_set1_epi8(0x0F));

		// compare with GIF_REG_A_D, set hasAD if any lanes passed
		hasAD = (_mm_movemask_epi8(_mm_cmpeq_epi8(vregs, _mm_set1_epi8(GIF_REG_A_D))) != 0);

		// write out unpacked registers
		_mm_storeu_si128(reinterpret_cast<__m128i*>(regs), vregs);
#elif (defined(_M_ARM64) || defined(__aarch64__))
		// zero out bits for registers which shouldn't be tested
		u64 REGS64;
		memcpy(&REGS64, tag.REGS, sizeof(u64));
		REGS64 &= (0xFFFFFFFFFFFFFFFFULL >> (64 - nRegs * 4));
		uint8x16_t vregs = vreinterpretq_u8_u64(vsetq_lane_u64(REGS64, vdupq_n_u64(0), 0));

		// get upper nibbles, interleave with lower nibbles, clear upper bits from low nibbles
		vregs = vandq_u8(vzip1q_u8(vregs, vshrq_n_u8(vregs, 4)), vdupq_n_u8(0x0F));

		// compare with GIF_REG_A_D, set hasAD if any lanes passed
		const uint8x16_t comp = vceqq_u8(vregs, vdupq_n_u8(GIF_REG_A_D));
		hasAD = vmaxvq_u8(comp) & 1;

		// write out unpacked registers
		vst1q_u8(regs, vregs);
#else
		// Reference C implementation.
		hasAD = false;
		u32 t = tag.REGS[0];
		u32 i = 0;
		u32 j = pcsx2_min_u(nRegs, 8);
		for (; i < j; i++)
		{
			regs[i] = t & 0xf;
			hasAD |= (regs[i] == GIF_REG_A_D);
			t >>= 4;
		}
		t = tag.REGS[1];
		j = nRegs;
		for (; i < j; i++)
		{
			regs[i] = t & 0xf;
			hasAD |= (regs[i] == GIF_REG_A_D);
			t >>= 4;
		}
#endif
	}
};

struct GS_Packet
{
	// PERF note: this struct is copied various time in hot path. Don't add
	// new field

	u32 offset;     // Path buffer offset for start of packet
	u32 size;       // Full size of GS-Packet
	s32 cycles;     // EE Cycles taken to process this GS packet
	s32 readAmount; // Dummy read-amount data needed for proper buffer calculations
	GS_Packet() { offset = 0; size = 0; cycles = 0; readAmount = 0; }
};

struct GS_SIGNAL
{
	u32 data[2];
	bool queued;
};

struct GS_FINISH
{
	bool gsFINISHFired;
	bool gsFINISHPending;
};

/* Define to 1 to record the packet queue's peak occupancy.  Producer-side
 * only, one compare per push; the peak is logged and rearmed at every
 * Reset(), so each session prints what it actually used.  Exists so
 * MTVU_GSPACK_QUEUE_RECORDS (GS.h) can be tightened from a measurement
 * instead of a guess. */
#define MTVU_GSPACK_QUEUE_HIGH_WATER 0
#if MTVU_GSPACK_QUEUE_HIGH_WATER
#endif

struct Gif_Path_MTVU
{
	u32 fakePackets; // Fake packets pending to be sent to MTGS
	GS_Packet fakePacket;
	// One GS_Packet per record.  The byte capacity is a whole number of
	// packets, so a record never straddles the wrap and every avail count
	// the queue reports is a whole number of packets - which is what lets
	// the framing be nothing more than sizeof(GS_Packet).
	//
	// Only GIF path 1 ever runs through MTVU, so only path 1 allocates
	// the queue: InitQueue(true) from Gif_Path::Init.  On the other two
	// paths every member of this struct except fakePackets/fakePacket
	// stays idle, and the queue stays a pair of null pointers.
	retro_spsc_t gsPackQueue;
	bool gsPackQueueOk;
#if MTVU_GSPACK_QUEUE_HIGH_WATER
	u32 gsPackQueuePeak; /* producer thread only */
#endif
	Gif_Path_MTVU()
	{
		/* The queue struct stays untouched until InitQueue(); every
		 * path through it - producer, consumer, clear, free - is
		 * gated on gsPackQueueOk, so nothing reads it first. */
		gsPackQueueOk = false;
#if MTVU_GSPACK_QUEUE_HIGH_WATER
		gsPackQueuePeak = 0;
#endif
		Reset();
	}
	~Gif_Path_MTVU()
	{
		if (gsPackQueueOk)
			retro_spsc_free(&gsPackQueue);
	}
	Gif_Path_MTVU(const Gif_Path_MTVU&) = delete;
	Gif_Path_MTVU& operator=(const Gif_Path_MTVU&) = delete;
	void InitQueue(bool use_queue)
	{
		if (gsPackQueueOk)
			retro_spsc_free(&gsPackQueue);
		gsPackQueueOk = use_queue && retro_spsc_init(&gsPackQueue,
			MTVU_GSPACK_QUEUE_RECORDS * sizeof(GS_Packet));
	}
	void Reset()
	{
		fakePackets = 0;
		/* Both sides are quiesced at a VM reset, which is the only
		 * caller besides construction. */
		if (gsPackQueueOk)
			retro_spsc_clear(&gsPackQueue);
#if MTVU_GSPACK_QUEUE_HIGH_WATER
		if (gsPackQueuePeak)
			log_cb(RETRO_LOG_INFO, "MTVU gsPackQueue peak occupancy: %u of %u records\n",
				gsPackQueuePeak, (u32)MTVU_GSPACK_QUEUE_RECORDS);
		gsPackQueuePeak = 0;
#endif
		fakePacket.offset     = 0;
		fakePacket.size       = ~0u; // Used to indicate that its a fake packet
		fakePacket.cycles     = 0;
		fakePacket.readAmount = 0;
	}
};

struct Gif_Path
{
	retro_atomic_int_t readAmount; // Amount of data MTGS still needs to read
	u8* buffer;                  // Path packet buffer
	u32 buffSize;                // Full size of buffer
	u32 buffLimit;               // Cut off limit to wrap around
	u32 curSize;                 // Used buffer in bytes
	u32 curOffset;               // Offset of current gifTag
	u32 dmaRewind;               // Used by path3 when only part of a DMA chain is used
	Gif_Tag gifTag;              // Current GS Primitive tag
	GS_Packet gsPack;            // Current GS Packet info
	GIF_PATH idx;                // Gif Path Index
	GIF_PATH_STATE state;        // Path State
	Gif_Path_MTVU mtvu;          // Must be last for saved states

	Gif_Path() { Reset(); }
	~Gif_Path() { memalign_free(buffer); }

	void Init(GIF_PATH _idx, u32 _buffSize, u32 _buffSafeZone)
	{
		idx = _idx;
		buffSize = _buffSize;
		buffLimit = _buffSize - _buffSafeZone;
		buffer = (u8*)memalign_alloc(16, buffSize);
		/* MTVU only ever drives path 1 (MTVU.cpp posts exclusively to
		 * gifPath[GIF_PATH_1]), so the packet queue exists only there. */
		mtvu.InitQueue(_idx == GIF_PATH_1);
		Reset();
	}

	void Reset(bool softReset = false)
	{
		state = GIF_PATH_IDLE;
		if (softReset)
		{
			if (!isMTVU()) // MTVU Freaks out if you try to reset it, so let's just let it transfer
			{
				gifTag.Reset();
				gsPack.offset     = curOffset;
				gsPack.size       = 0;
				gsPack.cycles     = 0;
				gsPack.readAmount = 0;
				curSize           = curOffset;
			}
			return;
		}
		mtvu.Reset();
		curSize = 0;
		curOffset = 0;
		retro_atomic_store_release_int(&readAmount, 0);
		gifTag.Reset();
		gsPack.offset     = 0;
		gsPack.size       = 0;
		gsPack.cycles     = 0;
		gsPack.readAmount = 0;
	}

	bool isMTVU() const { return !idx && THREAD_VU1; }
	s32 getReadAmount() { return retro_atomic_load_acquire_int(&readAmount) + gsPack.readAmount; }
	bool hasDataRemaining() const { return curOffset < curSize; }
	bool isDone() const { return isMTVU() ? !mtvu.fakePackets : (!hasDataRemaining() && (state == GIF_PATH_IDLE || state == GIF_PATH_WAIT)); }

	// Moves packet data to start of buffer
	void RealignPacket()
	{
		s32 offset = curOffset - gsPack.size;
		s32 sizeToAdd = curSize - offset;
		s32 intersect = sizeToAdd - offset;
		if (intersect < 0)
			intersect = 0;
		for (;;)
		{
			s32 frontFree = offset - getReadAmount();
			if (frontFree >= sizeToAdd - intersect)
				break;
			MTGS::WaitGS(isMTVU());
		}
		if (offset < (s32)buffLimit)
		{ // Needed for correct readAmount values
			if (isMTVU())
				gsPack.readAmount += buffLimit - offset;
			else
				Gif_AddBlankGSPacket(buffLimit - offset, idx);
		}
		if (intersect)
			memmove(buffer, &buffer[offset], curSize - offset);
		else
			memcpy(buffer, &buffer[offset], curSize - offset);
		curSize -= offset;
		curOffset = gsPack.size;
		gsPack.offset = 0;
	}

	/* Moves the packet to the front of the buffer, out of the way of the
	 * copy. A packet whose EOP never comes outgrows the buffer: the GIF
	 * streams on through it, so the part already parsed goes to the GS
	 * then and only the unparsed tail is kept. */
	__noinline void MakeRoom(u32 size)
	{
		RealignPacket();
		if (curSize + size <= buffSize || !gsPack.size)
			return;
		if (isMTVU())
		{
			FinishGSPacketMTVU(false);
			Gif_MTVU_KickSema();
		}
		else
		{
			Gif_AddCompletedGSPacket(gsPack, idx);
			gsPack.offset = curOffset;
			gsPack.size   = 0;
		}
		RealignPacket();
	}

	void CopyGSPacketData(u8* pMem, u32 size, bool aligned = false)
	{
		if (curSize + size > buffSize)
			MakeRoom(size);
		for (;;)
		{
			s32 offset = curOffset - gsPack.size;
			s32 readPos = offset - getReadAmount();
			if (readPos >= 0)
				break; // MTGS is reading in back of curOffset
			if ((s32)buffLimit + readPos > (s32)curSize + (s32)size)
				break;      // Enough free front space
			if (isMTVU() && !GetPendingGSPackets() && gsPack.size)
			{
				// Nothing for MTGS to pop and the current (unfinished) packet
				// holds the space: hand it over as a PARTIAL packet so MTGS
				// can drain the buffer (see FinishGSPacketMTVU).
				FinishGSPacketMTVU(false);
				Gif_MTVU_KickSema();
				continue;
			}
			MTGS::WaitGS(isMTVU()); // Let MTGS run to free up buffer space
		}
		memcpy(&buffer[curSize], pMem, size);
		curSize += size;
	}

	// If completed a GS packet (with EOP) then set done to true
	// MTVU: This function only should be called called on EE thread
	GS_Packet ExecuteGSPacket(bool& done)
	{
		if (mtvu.fakePackets)
		{ // For MTVU mode...
			mtvu.fakePackets--;
			done = true;
			return mtvu.fakePacket;
		}
		for (;;)
		{
			if (!gifTag.isValid)
			{ // Need new Gif Tag
				// We don't have enough data for a Gif Tag
				if (curOffset + 16 > curSize)
					return gsPack;

				// Move packet to start of buffer
				if (curOffset > buffLimit)
				{
					RealignPacket();
				}

				gifTag.setTag(&buffer[curOffset], 1);

				state = (GIF_PATH_STATE)(gifTag.tag.FLG + 1);
				// We don't have enough data for a complete GS packet
				if (!gifTag.hasAD && curOffset + 16 + gifTag.len > curSize)
				{
					gifTag.isValid = false; // So next time we test again
					return gsPack;
				}

				gsPack.size   += 16;                /* Tag Size */
				curOffset     += 16;                /* Tag Size */
				gsPack.cycles += 2 + gifTag.cycles; /* Tag + Len ee-cycles */
			}

			if (gifTag.hasAD)
			{ // Only can be true if GIF_FLG_PACKED
				bool dblSIGNAL = false;
				while (gifTag.nLoop && !dblSIGNAL)
				{
					if (curOffset + 16 > curSize)
					{
						return gsPack; // Exit Early
					}
					if (gifTag.regs[gifTag.nRegIdx & 0xf] == GIF_REG_A_D)
					{
						if (!isMTVU())
							dblSIGNAL = Gif_HandlerAD(&buffer[curOffset]);
					}
					gsPack.size += 16; /* 1 QWC */
					curOffset   += 16; /* 1 QWC */
					gifTag.packedStep();
				}
				if (dblSIGNAL && !(gifTag.tag.EOP && !gifTag.nLoop))
					return gsPack; // Exit Early
			}
			else
			{
				gsPack.size += gifTag.len; /* Data length */
				curOffset   += gifTag.len; /* Data length */
			}

			// Reload gif tag next loop
			gifTag.isValid = false;

			if (gifTag.tag.EOP)
			{
				GS_Packet t = gsPack;
				done = true;

				dmaRewind = 0;

				gsPack.offset     = 0;
				gsPack.size       = 0;
				gsPack.cycles     = 0;
				gsPack.readAmount = 0;
				gsPack.offset     = curOffset;
				//Path 3 Masking is timing sensitive, we need to simulate its length! (NFSU2/Outrun 2006)

				if ((gifRegs.stat.APATH - 1) == GIF_PATH_3)
				{
					state = GIF_PATH_WAIT;

					if (curSize - curOffset > 0 && (gifRegs.stat.M3R || gifRegs.stat.M3P))
					{
						//Including breaking packets early (Rewind DMA to pick up where left off)
						//but only do this when the path is masked, else we're pointlessly slowing things down.
						dmaRewind = curSize - curOffset;
						curSize = curOffset;
					}
				}
				else
					state = GIF_PATH_IDLE;

				return t; // Complete GS packet
			}
		}
	}

	// MTVU: Gets called on VU XGkicks on MTVU thread
	void ExecuteGSPacketMTVU()
	{
		// Move packet to start of buffer
		if (curOffset > buffLimit)
		{
			RealignPacket();
		}
		for (;;)
		{ // needed to be processed by pcsx2...
			if (curOffset + 16 > curSize)
				break;
			gifTag.setTag(&buffer[curOffset], 1);

			if (!gifTag.hasAD && curOffset + 16 + gifTag.len > curSize)
				break;
			gsPack.size += 16; /* Tag Size */
			curOffset   += 16; /* Tag Size */

			if (gifTag.hasAD)
			{ // Only can be true if GIF_FLG_PACKED
				while (gifTag.nLoop)
				{
					if (curOffset + 16 > curSize)
						break; // Exit Early
					if (gifTag.regs[gifTag.nRegIdx & 0xf] == GIF_REG_A_D)
					{
						Gif_HandlerAD_MTVU(&buffer[curOffset]);
					}
					gsPack.size += 16; /* 1 QWC */
					curOffset   += 16; /* 1 QWC */
					gifTag.packedStep();
				}
			}
			else
			{
				gsPack.size += gifTag.len; /* Data length */
				curOffset   += gifTag.len; /* Data length */
			}
			if (curOffset >= curSize)
				break;
			if (gifTag.tag.EOP)
				break;
		}
		gifTag.isValid = false;
	}

	// MTVU: Gets called after VU1 execution on MTVU thread (final=true), and
	// from CopyGSPacketData's would-block path (final=false) to hand a PARTIAL
	// packet to MTGS. Without partial flushes a continuous VU1 microprogram
	// (endless loop streaming XGKICK, no E-bit -- GT3's arcade attract) is a
	// deadlock: this thread waits for MTGS to drain the path1 buffer while
	// MTGS waits in semaXGkick for a program end that never comes. The MTVU
	// queue reuses GS_Packet.cycles (never set on this path) as the "more
	// packets follow for this program" flag; MTGS's MTVU_GSPACKET handler
	// consumes packets until it sees a final one (cycles == 0).
	void FinishGSPacketMTVU(bool final = true)
	{
		gsPack.cycles = final ? 0 : 1;
		// Performance note: fetch_add atomic operation might create some stall for atomic
		// operation in gsPack.push
		retro_atomic_fetch_add_int(&readAmount, gsPack.size + gsPack.readAmount);
		/* Wait for room for a whole packet before writing any of it:
		 * a short write would put a partial record in the queue and
		 * every packet after it would be read off by one. */
		if (mtvu.gsPackQueueOk)
		{
			void* dst;
			while (retro_spsc_write_begin(&mtvu.gsPackQueue, &dst) < sizeof(GS_Packet))
				;
			memcpy(dst, &gsPack, sizeof(GS_Packet));
			retro_spsc_write_end(&mtvu.gsPackQueue, sizeof(GS_Packet));
#if MTVU_GSPACK_QUEUE_HIGH_WATER
			{
				const u32 occ = (u32)((MTVU_GSPACK_QUEUE_RECORDS * sizeof(GS_Packet)
					- retro_spsc_write_avail(&mtvu.gsPackQueue)) / sizeof(GS_Packet));
				if (occ > mtvu.gsPackQueuePeak)
					mtvu.gsPackQueuePeak = occ;
			}
#endif
		}

		gsPack.offset     = curOffset;
		gsPack.size       = 0;
		gsPack.cycles     = 0;
		gsPack.readAmount = 0;
	}

	// MTVU: Gets called by MTGS thread
	GS_Packet GetGSPacketMTVU()
	{
		// FIXME is the error path useful ?
		const void* src;
		if (mtvu.gsPackQueueOk && retro_spsc_read_begin(&mtvu.gsPackQueue, &src) >= sizeof(GS_Packet))
		{
			GS_Packet pkt;
			memcpy(&pkt, src, sizeof(GS_Packet));
			return pkt;
		}
		return GS_Packet(); // gsPack.size will be 0
	}

	// MTVU: Gets called by MTGS thread
	void PopGSPacketMTVU()
	{
		/* The span was already established by the GetGSPacketMTVU
		 * that precedes every pop; skip commits it without deriving
		 * it a second time. */
		if (mtvu.gsPackQueueOk)
			retro_spsc_skip(&mtvu.gsPackQueue, sizeof(GS_Packet));
	}

	// MTVU: Returns the amount of pending
	// GS Packets that MTGS hasn't yet processed
	u32 GetPendingGSPackets()
	{
		if (!mtvu.gsPackQueueOk)
			return 0;
		return (u32)(retro_spsc_read_avail(&mtvu.gsPackQueue) / sizeof(GS_Packet));
	}
};

struct Gif_Unit
{
	Gif_Path gifPath[3];
	GS_SIGNAL gsSIGNAL; // Stalling Signal
	GS_FINISH gsFINISH; // Finish Signal
	tGIF_STAT& stat;
	GIF_TRANSFER_TYPE lastTranType; // Last Transfer Type

	Gif_Unit()
		: gsSIGNAL()
		, gsFINISH()
		, stat(gifRegs.stat)
		, lastTranType(GIF_TRANS_INVALID)
	{
		gifPath[0].Init(GIF_PATH_1, _1mb * 9, _1mb + _1kb);
		gifPath[1].Init(GIF_PATH_2, _1mb * 9, _1mb + _1kb);
		gifPath[2].Init(GIF_PATH_3, _1mb * 9, _1mb + _1kb);
	}

	// Enable softReset when resetting during game emulation
	void Reset(bool softReset = false)
	{
		gifRegs.stat._u32      = 0;
		gifRegs.ctrl._u32      = 0;
		gifRegs.mode._u32      = 0;
		gsCSRfield(GS_CSR_FIFO, (u32)CSR_FIFO_EMPTY << GS_CSR_FIFO_SH); // This is the GIF unit side FIFO, not DMA!
		gsSIGNAL.data[0]       = 0;
		gsSIGNAL.data[1]       = 0;
		gsSIGNAL.queued        = false;
		gsFINISH.gsFINISHFired = false;
		gifPath[0].Reset(softReset);
		gifPath[1].Reset(softReset);
		gifPath[2].Reset(softReset);
		if (!softReset)
			lastTranType = GIF_TRANS_INVALID;
		//If the VIF has paused waiting for PATH3, recheck it after the reset has occurred (Eragon)
		if (vif1Regs.stat.VGW)
		{
			if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
				CPU_INT(DMAC_VIF1, 1);
		}
	}

	// Returns GS Packet Size in bytes
	u32 GetGSPacketSize(GIF_PATH pathIdx, u8* pMem, u32 offset = 0, u32 size = ~0u, bool flush = false)
	{
		u32 memMask = pathIdx ? ~0u : 0x3fffu;
		u32 curSize = 0;
		for (;;)
		{
			Gif_Tag gifTag(&pMem[offset & memMask]);
			curSize     += 16 + gifTag.len; /* Tag + Data Length */
			offset      += 16 + gifTag.len; /* Tag + Data Length */
			if (pathIdx == GIF_PATH_1 && curSize >= 0x4000)
				return 0; // Bios does this... (Fixed if you delay vu1's xgkick by 103 vu cycles)
			if (curSize >= size)
				return size;
			if(((flush && gifTag.tag.EOP) || !flush) && (CHECK_XGKICKHACK || !EmuConfig.Cpu.Recompiler.EnableVU1))
				return curSize | ((u32)gifTag.tag.EOP << 31);
			if (gifTag.tag.EOP )
				return curSize;
		}
	}

	// Specify the transfer type you are initiating
	// The return value is the amount of data (in bytes) that was processed
	// If transfer cannot take place at this moment the return value is 0
	u32 TransferGSPacketData(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size, bool aligned = false)
	{

		if (THREAD_VU1)
		{
			Gif_Path& path1 = gifPath[GIF_PATH_1];
			if (tranType == GIF_TRANS_XGKICK)
			{ // This is on the MTVU thread
				path1.CopyGSPacketData(pMem, size, aligned);
				path1.ExecuteGSPacketMTVU();
				return size;
			}
			if (tranType == GIF_TRANS_MTVU)
			{ // This is on the EE thread
				path1.mtvu.fakePackets++;
				if (CanDoGif())
					Execute(false, true);
				return 0;
			}
		}

		if (size == 0)
			return 0;
		lastTranType = tranType;

		if (tranType == GIF_TRANS_DMA)
		{
			if (!CanDoPath3())
			{
				if (!Path3Masked())
					stat.P3Q = 1;
				return 0;
			} // DMA Stall
		}
		if (tranType == GIF_TRANS_XGKICK)
		{
			// We always buffer path1 packets
			if (!CanDoPath1())
				stat.P1Q = 1;
		}
		if (tranType == GIF_TRANS_DIRECT)
		{
			if (!CanDoPath2())
			{
				stat.P2Q = 1;
				return 0;
			} // Direct Stall
		}
		if (tranType == GIF_TRANS_DIRECTHL)
		{
			if (!CanDoPath2HL())
			{
				stat.P2Q = 1;
				return 0;
			} // DirectHL Stall
		}

		gifPath[tranType & 3].CopyGSPacketData(pMem, size, aligned);
		size -= Execute(tranType == GIF_TRANS_DMA, false);
		return size;
	}

	__fi int checkQueued(bool p1, bool p2, bool p3)
	{
		return ((p1 && stat.P1Q) << 0)
		     | ((p2 && stat.P2Q) << 1)
		     | ((p3 && stat.P3Q) << 2);
	}

	// Checks path activity for the given paths
	// Returns an int with a bit enabled if the corresponding
	// path is not finished (needs more data/processing for an EOP)
	__fi int checkPaths(bool p1, bool p2, bool p3, bool checkQ = false)
	{
		int ret = ((p1 && !gifPath[GIF_PATH_1].isDone()) << 0)
		        | ((p2 && !gifPath[GIF_PATH_2].isDone()) << 1)
			| ((p3 && !gifPath[GIF_PATH_3].isDone()) << 2);
		return ret | (checkQ ? checkQueued(p1, p2, p3) : 0);
	}

	// Send processed GS Primitive(s) to the MTGS thread
	// Note: Only does so if current path fully completed all
	// of its given gs primitives (but didn't upload them yet)
	void FlushToMTGS()
	{
		if (!stat.APATH)
			return;
		Gif_Path& path = gifPath[stat.APATH - 1];
		if (path.gsPack.size && !path.gifTag.isValid)
		{
			Gif_AddCompletedGSPacket(path.gsPack, (GIF_PATH)(stat.APATH - 1));
			path.gsPack.offset = path.curOffset;
			path.gsPack.size = 0;
		}
	}

	// Processes gif packets and performs path arbitration
	// on EOPs or on Path 3 Images when IMT is set.
	int Execute(bool isPath3, bool isResume)
	{
		if (!CanDoGif())
			return 0;
		bool didPath3 = false;
		bool path3Check = isPath3;
		int curPath = stat.APATH > 0 ? stat.APATH - 1 : 0; //Init to zero if no path is already set.
		gifPath[2].dmaRewind = 0;
		stat.OPH = 1;

		for (;;)
		{
			if (stat.APATH)
			{ // Some Transfer is happening
				Gif_Path& path = gifPath[stat.APATH - 1];
				bool done = false;
				GS_Packet gsPack = path.ExecuteGSPacket(done);
				if (!done)
				{
					if (stat.APATH == 3 && CanDoP3Slice() && !gsSIGNAL.queued)
					{
						if (!didPath3 && /*!Path3Masked() &&*/ checkPaths(1, 1, 0))
						{ // Path3 slicing
							didPath3 = true;
							stat.APATH = 0;
							stat.IP3 = 1;
							if (gsPack.size > 16)
							{                                                 // Packet had other tags which we already processed
								u32 subOffset = path.gifTag.isValid ? 16 : 0; // if isValid, image-primitive not finished
								gsPack.size           -= subOffset;           // Remove the image-tag (should be last thing read)
								Gif_AddCompletedGSPacket(gsPack, GIF_PATH_3);     // Consider current packet complete
													      
								path.curOffset        -= subOffset;           // Start the next GS packet at the image-tag
													      
								// Reset gs packet info
								path.gsPack.offset     = path.curOffset;      // Set to image-tag
								path.gsPack.size       = 0;
								path.gsPack.cycles     = 0;
								path.gsPack.readAmount = 0;

								path.gifTag.isValid    = false;              // Reload tag next ExecuteGSPacket()
							}
							continue;
						}
					}
					break; // Not finished with GS packet
				}
				if (gifPath[curPath].state == GIF_PATH_WAIT || gifPath[curPath].state == GIF_PATH_IDLE)
				{
					Gif_AddCompletedGSPacket(gsPack, (GIF_PATH)(stat.APATH - 1));
				}
			}
			if (!gsSIGNAL.queued && !gifPath[0].isDone())
			{
				stat.APATH = 1;
				stat.P1Q = 0;
				curPath = 0;
			}
			else if (!gsSIGNAL.queued && !gifPath[1].isDone())
			{
				stat.APATH = 2;
				stat.P2Q = 0;
				curPath = 1;
			}
			else if (!gsSIGNAL.queued && !gifPath[2].isDone() && !Path3Masked())
			{
				stat.APATH = 3;
				stat.P3Q = 0;
				stat.IP3 = 0;
				curPath = 2;
				path3Check = true;
			}
			else
			{
				// If PATH3 was stalled due to another transfer but the DMA ended, it'll never check this
				// So lets quickly check if it's currently set to path3
				if (stat.APATH == 3 || path3Check)
					gifCheckPathStatus(true);
				else
				{
					if (vif1Regs.stat.VGW)
					{
						// Check if VIF is in a cycle or is currently "idle" waiting for GIF to come back.
						if (!(cpuRegs.interrupt & (1 << DMAC_VIF1)))
							CPU_INT(DMAC_VIF1, 1);
					}

					stat.APATH = 0;
					stat.OPH = 0;
				}

				break;
			}
		}
		//Some loaders/Refresh Rate selectors and things dont issue "End of Packet" commands
		//So we look and see if the end of the last tag is all there, if so, stick it in the buffer for the GS :)
		//(Invisible Screens on Terminator 3 and Growlanser 2/3)
		if (gifPath[curPath].curOffset == gifPath[curPath].curSize)
			FlushToMTGS();

		Gif_FinishIRQ();
		if(!checkPaths(stat.APATH != 1, stat.APATH != 2, stat.APATH != 3, true))
			Gif_FinishIRQ();

		//Path3 can rewind the DMA, so we send back the amount we go back!
		if (isPath3)
			return gifPath[2].dmaRewind;
		return 0;
	}

	// XGkick
	bool CanDoPath1() const
	{
		return (stat.APATH == 0 || stat.APATH == 1 || (stat.APATH == 3 && CanDoP3Slice())) && CanDoGif();
	}
	// Direct
	bool CanDoPath2() const
	{
		return (stat.APATH == 0 || stat.APATH == 2 || (stat.APATH == 3 && CanDoP3Slice())) && CanDoGif();
	}
	// DirectHL
	bool CanDoPath2HL() const
	{
		return (stat.APATH == 0 || stat.APATH == 2) && CanDoGif();
	}
	// Gif DMA
	bool CanDoPath3() const
	{
		return ((stat.APATH == 0 && !Path3Masked()) || stat.APATH == 3) && CanDoGif();
	}

	bool CanDoP3Slice() const { return stat.IMT == 1 && gifPath[GIF_PATH_3].state == GIF_PATH_IMAGE; }
	bool CanDoGif() const { return stat.PSE == 0 && stat.DIR == 0 && gsSIGNAL.queued == 0; }
	//Mask stops the next packet which hasnt started from transferring
	bool Path3Masked() const { return ((stat.M3R || stat.M3P) && (gifPath[GIF_PATH_3].state == GIF_PATH_IDLE || gifPath[GIF_PATH_3].state == GIF_PATH_WAIT)); }
};

extern Gif_Unit gifUnit;
