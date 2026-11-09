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
#include <retro_atomic.h>
#include <retro_spsc.h>

#include "Gif.h"
#include "GSmmio.h"

/* The GIF unit (Gif_Unit.c): the three paths' packet buffers, the GS
 * packets parsed out of them for the GS thread, and path arbitration.
 * The GS thread (MTGS.cpp) reads each packet in place in its path's
 * buffer; the VU1 thread (MTVU.cpp) drives path 1 when it runs VU1. */

#ifdef __cplusplus
extern "C" {
#endif

/* MTVU GS-packet queue size in GS_Packet records (must be a power of two).
 *
 * One queue exists: GIF path 1 is the only path MTVU drives, and the only
 * one that allocates it. Its old size was inherited from the upstream 8 MiB
 * MTGS ring ("based on MTGS", factor 2) - a ring that no longer exists here.
 * MTGS_RINGBUFFERSIZE (GS.h) bounds how many MTVU placeholder tags the EE
 * can have outstanding, and each drains the packet queue until a final
 * packet, so occupancy tracks the same PostVsyncStart pacing that keeps the
 * command ring at ~22k entries worst-observed. 65536 records (1 MiB) matches
 * the command ring's bound-with-margin. No measured peak exists for this
 * queue yet: build with MTVU_GSPACK_QUEUE_HIGH_WATER to record one and
 * tighten this. */
#define MTVU_GSPACK_QUEUE_RECORDS 65536

/* Define to 1 to record the packet queue's peak occupancy.  Producer-side
 * only, one compare per push; the peak is logged and rearmed at every
 * reset, so each session prints what it actually used. */
#define MTVU_GSPACK_QUEUE_HIGH_WATER 0

typedef struct HW_Gif_Tag
{
	u32 NLOOP : 15;
	u32 EOP : 1;
	u32 _dummy0 : 16;
	u32 _dummy1 : 14;
	u32 PRE : 1;
	u32 PRIM : 11;
	u32 FLG : 2;
	u32 NREG : 4;
	u32 REGS[2];
} HW_Gif_Tag;

typedef struct Gif_Tag
{
	HW_Gif_Tag tag;
	u32 nLoop;    /* NLOOP left to process */
	u32 nRegs;    /* NREG (1~16) */
	u32 nRegIdx;  /* Current nReg Index (packed mode processing) */
	u32 len;      /* Packet Length in Bytes (not including tag) */
	u32 cycles;   /* Time needed to process packet data in ee-cycles */
	u8 regs[16];  /* Regs */
	bool hasAD;   /* Has an A+D Write */
	bool isValid; /* Tag is valid */
} Gif_Tag;

/* PERF note: this struct is copied various times in the hot path. Don't add
 * new fields. */
typedef struct GS_Packet
{
	u32 offset;     /* Path buffer offset for start of packet */
	u32 size;       /* Full size of GS-Packet */
	s32 cycles;     /* EE Cycles taken to process this GS packet */
	s32 readAmount; /* Dummy read-amount data needed for proper buffer calculations */
} GS_Packet;

typedef struct GS_SIGNAL
{
	u32 data[2];
	bool queued;
} GS_SIGNAL;

typedef struct GS_FINISH
{
	bool gsFINISHFired;
	bool gsFINISHPending;
} GS_FINISH;

typedef struct Gif_Path_MTVU
{
	u32 fakePackets; /* Fake packets pending to be sent to MTGS */
	GS_Packet fakePacket;
	/* One GS_Packet per record.  The byte capacity is a whole number of
	 * packets, so a record never straddles the wrap and every avail count
	 * the queue reports is a whole number of packets - which is what lets
	 * the framing be nothing more than sizeof(GS_Packet).
	 *
	 * Only GIF path 1 ever runs through MTVU, so only path 1 allocates the
	 * queue. On the other two paths every member of this struct except
	 * fakePackets/fakePacket stays idle. */
	retro_spsc_t gsPackQueue;
	bool gsPackQueueOk;
#if MTVU_GSPACK_QUEUE_HIGH_WATER
	u32 gsPackQueuePeak; /* producer thread only */
#endif
} Gif_Path_MTVU;

/* The bytes MTGS has yet to read of each path, each count on a cache line
 * of its own. The count sat first in Gif_Path, in the line the EE's packet
 * parsing walks (curOffset, the tag, the packet), and every packet the GS
 * thread finished pulled that line from the EE, and the packet parser
 * stalled on its next load of the packet (a fifth of its samples in a
 * racing game). The field in Gif_Path stays, at zero, for the saved
 * state's layout. The array is 64-byte aligned, so each lands on a line. */
typedef struct Gif_Path_ReadAmount
{
	retro_atomic_int_t v;
	char pad[64 - sizeof(retro_atomic_int_t)];
} Gif_Path_ReadAmount;
typedef char gif_path_read_amount_is_a_cache_line[(sizeof(Gif_Path_ReadAmount) == 64) ? 1 : -1];
PCSX2_ALIGN(64) extern Gif_Path_ReadAmount gifPathReadAmount[3];

/* Frozen whole but for mtvu, which must stay last: its layout is the
 * saved state's. */
typedef struct Gif_Path
{
	retro_atomic_int_t readAmount; /* Always zero, for the saved state's layout: see gifPathReadAmount */
	u8* buffer;                    /* Path packet buffer */
	u32 buffSize;                  /* Full size of buffer */
	u32 buffLimit;                 /* Cut off limit to wrap around */
	u32 curSize;                   /* Used buffer in bytes */
	u32 curOffset;                 /* Offset of current gifTag */
	u32 dmaRewind;                 /* Used by path3 when only part of a DMA chain is used */
	Gif_Tag gifTag;                /* Current GS Primitive tag */
	GS_Packet gsPack;              /* Current GS Packet info */
	GIF_PATH idx;                  /* Gif Path Index */
	GIF_PATH_STATE state;          /* Path State */
	Gif_Path_MTVU mtvu;            /* Must be last for saved states */
} Gif_Path;

typedef struct Gif_Unit
{
	Gif_Path gifPath[3];
	GS_SIGNAL gsSIGNAL;             /* Stalling Signal */
	GS_FINISH gsFINISH;             /* Finish Signal */
	GIF_TRANSFER_TYPE lastTranType; /* Last Transfer Type */
} Gif_Unit;

extern Gif_Unit gifUnit;

/* The path buffers, allocated for a VM's life (VMManager.cpp). */
extern bool gif_unit_init(void);
extern void gif_unit_shutdown(void);

/* GS packet size in bytes of the packet(s) at pMem + offset, up to size;
 * bit 31 set when the size was cut at an EOP the XGKICK hack stops on. */
extern u32 gif_unit_get_packet_size(GIF_PATH pathIdx, u8* pMem, u32 offset, u32 size, bool flush);
/* Hand size bytes of a path's data to the unit; returns the bytes taken,
 * or for PATH3 DMA the bytes the DMA is to rewind. */
extern u32 gif_unit_transfer(GIF_TRANSFER_TYPE tranType, u8* pMem, u32 size);
/* As gif_unit_check_paths, also counting paths queued behind another. */
extern int  gif_unit_check_paths_queued(bool p1, bool p2, bool p3);
/* Send the active path's completed primitives to the GS. */
extern void gif_unit_flush_to_mtgs(void);
extern bool gif_unit_can_do_path1(void);

/* A path's buffer: data copied in at its end. */
extern void gif_path_copy(Gif_Path* path, const u8* pMem, u32 size);
/* Moves the current packet to the start of the buffer. */
extern void gif_path_realign(Gif_Path* path);

/* MTVU: the VU1 thread hands path 1's packet to MTGS, which takes it from
 * the queue (GetGSPacketMTVU), sends it, then pops it. */
extern void      gif_path_finish_mtvu(Gif_Path* path, bool final_packet);
extern GS_Packet gif_path_get_packet_mtvu(Gif_Path* path);
extern void      gif_path_pop_packet_mtvu(Gif_Path* path);
extern u32       gif_path_pending_packets(Gif_Path* path);

/* An A+D write the GIF itself acts on (SIGNAL, FINISH, LABEL, the
 * transfer registers); true when it is a SIGNAL that stalls the GIF. */
extern bool Gif_HandlerAD(u8* pMem);
extern void Gif_FinishIRQ(void);

/* MTGS.cpp: the GS thread's side. */
extern void Gif_AddBlankGSPacket(u32 size, GIF_PATH path);
extern void Gif_AddCompletedGSPacket(const GS_Packet* gsPack, GIF_PATH path);
extern void mtgs_wait_gs(bool isMTVU);

/* MTVU.cpp: the VU1 thread's side. */
extern void Gif_MTVU_KickSema(void);
extern void Gif_HandlerAD_MTVU(u8* pMem);

#ifdef __cplusplus
}
#endif
