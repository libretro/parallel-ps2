/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_TRANSFER_H
#define GS_TRANSFER_H

/* The EE's image transfers into and out of local memory, and the moves
 * within it (BITBLTBUF, TRXPOS, TRXREG, TRXDIR): the transfer in progress
 * and its buffer, the flush a transfer forces on the draw queued before
 * it, and the local-to-local copy. GSState drives them (C++: its m_tr is
 * a gs_transfer). */

#include "common/Pcsx2Defs.h"
#include "GSRegs.h"
#include "GSDrawingContext.h"
#include "GSLocalMemory.h"
#include "gs_rect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The buffer a transfer goes through: all of local memory */
#define GS_TRANSFER_BUFFER_SIZE (1024 * 1024 * 4)

typedef struct gs_transfer
{
	int x, y;               /* where in the image the next pixel goes */
	int start, end, total;  /* bytes written out, taken in, of the image */
	u8* buff;               /* GS_TRANSFER_BUFFER_SIZE bytes, or NULL */
	GIFRegBITBLTBUF m_blit;
	bool write;             /* to local memory */
} gs_transfer;

/* A transfer starting at tx, ty, with nothing taken in */
void gs_transfer_init(gs_transfer* t, int tx, int ty, const GIFRegBITBLTBUF* blit, bool write);
/* The image tw x th of bpp bits sized (once), and len cut to what is left
 * of it; false when nothing is */
bool gs_transfer_update(gs_transfer* t, int tw, int th, int bpp, int* len);
/* len bytes of buff from *end out, *end moved past them: the buffer read
 * round from its start where it ends */
void gs_transfer_read_out(const u8* buff, int* end, u8* out, int len);

/* The part of a texture the draw before a transfer may read, from its
 * context's TEX0 and CLAMP */
gs_recti gs_transfer_tex0_rect(const gs_drawing_regs* ctx);

/* What a transfer (written to local memory, read from it, or both for a
 * move) needs flushed of the draw queued before it, as the flush reason
 * (GSState::GSFlushReason) or 0 */
typedef struct gs_transfer_overlap
{
	const GIFRegBITBLTBUF* blit;
	const GIFRegTRXPOS* pos;
	const GIFRegTRXREG* reg;
	/* the queued draw: its context, whether it textures, its rect */
	const gs_drawing_regs* prev_ctx;
	bool prev_tme;
	const gs_recti* draw_rect;
	/* its vertices and indices (index_count 0: none queued), and FST and
	 * the texture's size the coordinates are read with */
	const void* vertex;
	const u16* index;
	u32 index_count;
	bool fst;
	u32 tw, th;
} gs_transfer_overlap;

#define GS_FLUSH_UPLOADDIRTYTEX (1 << 5)
#define GS_FLUSH_UPLOADDIRTYFRAME (1 << 6)
#define GS_FLUSH_UPLOADDIRTYZBUF (1 << 7)
#define GS_FLUSH_LOCALTOLOCALMOVE (1 << 8)
#define GS_FLUSH_DOWNLOADFIFO (1 << 9)

int gs_transfer_overlap_flush(const gs_transfer_overlap* o, bool req_write, bool req_read);

/* The local-to-local move of BITBLTBUF, TRXPOS, TRXREG */
void gs_local_memory_move(gs_local_memory* mem, const GIFRegBITBLTBUF* blit, const GIFRegTRXPOS* pos,
	const GIFRegTRXREG* reg);

#ifdef __cplusplus
}
#endif

#endif
