/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

#ifndef GS_STATE_H
#define GS_STATE_H

#include "common/Pcsx2Defs.h"
#include <boolean.h>
#include "GSRegs.h"
#include "GSLocalMemory.h"
#include "GSClut.h"
#include "GSDrawingContext.h"
#include "GSDrawingEnvironment.h"
#include "Renderers/Common/gs_vertex.h"
#include "Renderers/Common/GSVertexTrace.h"
#include "Renderers/Common/GSArray.h"
#include "gs_vector.h"
#include "GSPCRTC.h"
#include "GSDrawQuery.h"
#include "GSTransfer.h"
#include "GSStateFile.h"
#include "GSBackend.h"
#include "Renderers/Common/GSPresent.h"

#ifdef __cplusplus
#include "GS.h"
#include "MultiISA.h"
#include "Renderers/Common/GSVertex.h"
#include "Renderers/SW/GSVertexSW.h"
#include "Renderers/HW/GSVertexHW.h"
#include "Renderers/Common/GSFunctionMap.h"
#include "Renderers/Common/GSDevice.h"
#include "GSVector.h"
#endif

/* The GS's state machine: the GIF register handlers, the vertex queue and
 * its kicks, the transfers between the EE and local memory, the flush of a
 * draw and the savestate. A C struct the GIF path (GSState.c) works on;
 * the renderers are C++ classes deriving from GSState, which wraps it and
 * names its members as they always were. Each member is typed as its own
 * language spells it (PCSX2_LANG_TYPE), laid out alike in both. */

#ifdef __cplusplus
/* The PCRTC's read circuits, with the renderers' names for them */
struct GSPCRTCRegs : gs_pcrtc
{
	typedef gs_pcrtc_display PCRTCDisplay;

	bool IsAnalogue() const { return gs_pcrtc_is_analogue(this); }
	void SetVideoMode(GSVideoMode mode) { gs_pcrtc_set_video_mode(this, static_cast<int>(mode)); }
	void EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle) { gs_pcrtc_enable_displays(this, pmode, smode2, smodetoggle); }
	void CheckSameSource() { gs_pcrtc_check_same_source(this); }
	bool FrameWrap() const { return gs_pcrtc_frame_wrap(this); }
	/* Both reads start at the same place: one read does */
	bool FrameRectMatch() const { return PCRTCSameSrc; }
	GSVector2i GetResolution() const { const gs_point r = gs_pcrtc_resolution(this); return GSVector2i(r.x, r.y); }
	GSVector4i GetFramebufferRect(int display) const
	{
		const gs_recti r = gs_pcrtc_framebuffer_rect(this, display);
		return GSVector4i(r.x, r.y, r.z, r.w);
	}
	int GetFramebufferBitDepth() const { return gs_pcrtc_framebuffer_bit_depth(this); }
	GSVector2i GetFramebufferSize(int display) const { const gs_point r = gs_pcrtc_framebuffer_size(this, display); return GSVector2i(r.x, r.y); }
	void SetRects(int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg) { gs_pcrtc_set_rects(this, display, displayReg, framebufferReg); }
	void CalculateFramebufferOffset(bool scanmask) { gs_pcrtc_calculate_framebuffer_offset(this, scanmask); }
	void RemoveFramebufferOffset(int display) { gs_pcrtc_remove_framebuffer_offset(this, display); }
	void CalculateDisplayOffset(bool scanmask) { gs_pcrtc_calculate_display_offset(this, scanmask); }
	/* A display's rects as vectors */
	GSVector4i DisplayRect(int i) const { return GS_V4I(&PCRTCDisplays[i].displayRect); }
	GSVector4i FramebufferRect(int i) const { return GS_V4I(&PCRTCDisplays[i].framebufferRect); }
};

#endif

struct gs_state;
#ifdef __cplusplus
class GSState;
class GSTexture;
typedef GSState gs_state_self;
#define GS_STATE_V4I  GSVector4i
#define GS_STATE_OPV4I GSVector4i
#define GS_STATE_V2I  GSVector2i
#define GS_STATE_TEX  GSTexture
#define GS_STATE_VTX  GSVertex
extern "C" {
#else
/* The renderers' functions take the C++ types; C hands them the same
 * objects under those names (incomplete here), so the functions are
 * called as the type they are. */
struct GSState;
struct GSVector4i;
struct GSVector2i;
struct GSTexture;
typedef struct GSState gs_state_self;
#define GS_STATE_V4I  gs_vec4i
#define GS_STATE_OPV4I struct GSVector4i
#define GS_STATE_V2I  struct GSVector2i
#define GS_STATE_TEX  struct GSTexture
#define GS_STATE_VTX  union gs_vertex
#endif

/* What the hardware and the software renderer answer differently, as a
 * table of plain functions; the renderer itself is the first argument.
 * Each renderer fills one (GSRendererHW.cpp, GSRendererSW.cpp) and sets
 * m_ops when it is made. A NULL entry means "what the state does by
 * default", which for most of them is nothing. */
struct gs_state_ops
{
	void          (*free)(gs_state_self* gs);
	void          (*destroy)(gs_state_self* gs);
	void          (*reset)(gs_state_self* gs, bool hardware_reset);
	void          (*update_settings)(gs_state_self* gs, const EmuGSOptions* old_config);
	void          (*update_render_fixes)(gs_state_self* gs);
	void          (*vsync)(gs_state_self* gs, u32 field, bool registers_written, bool idle_frame);
	void          (*draw)(gs_state_self* gs);
	void          (*move)(gs_state_self* gs);
	void          (*purge_texture_cache)(gs_state_self* gs, bool sources, bool targets, bool hash_cache);
	void          (*readback_texture_cache)(gs_state_self* gs);
	void          (*invalidate_video_mem)(gs_state_self* gs, const GIFRegBITBLTBUF* BITBLTBUF, const GS_STATE_OPV4I* r);
	void          (*invalidate_local_mem)(gs_state_self* gs, const GIFRegBITBLTBUF* BITBLTBUF, const GS_STATE_OPV4I* r, bool clut);
	bool          (*can_upscale)(gs_state_self* gs);
	float         (*get_upscale_multiplier)(gs_state_self* gs);
	float         (*get_texture_scale_factor)(gs_state_self* gs);
	GS_STATE_TEX* (*lookup_palette_source)(gs_state_self* gs, u32 CBP, u32 CPSM, u32 CBW, GS_STATE_V2I* offset, float* scale, float* sample, const GS_STATE_V2I* size);
	GS_STATE_TEX* (*get_output)(gs_state_self* gs, int i, float* scale, int* y_offset);
	GS_STATE_TEX* (*get_feedback_output)(gs_state_self* gs, float* scale);
	/* The depth the picture in color, a texture get_output returned, was
	 * drawn with, for the VR eyes (GSVR.h): the depth buffer itself, or
	 * the bytes of its Z when *encoded, and the scale that takes those to
	 * [0, 1]. NULL, or false, where there is none. */
	bool          (*get_vr_depth)(gs_state_self* gs, GS_STATE_TEX* color, GS_STATE_TEX** depth, bool* encoded, float* z_scale);
	/* A savestate has replaced local memory: `changed` has a bit set for
	 * each page (GS_MAX_PAGES) whose contents it changed. The others hold
	 * what they held, so what a renderer made from them can stay. */
	void          (*mem_restored)(gs_state_self* gs, const u32* changed);
	/* A savestate taken in this instance (GSSnapshotSave): keep a copy
	 * of what is drawn on the GPU under the key. */
	void          (*snapshot_targets)(gs_state_self* gs, u64 key);
	/* An auto-flushed batch split into segments: whether the renderer
	 * draws it at once, each segment reading what the ones before it
	 * drew (true), or is to be handed one segment at a time (false or
	 * NULL). Asked with the vertex trace of the whole batch made. */
	bool          (*can_draw_runs)(gs_state_self* gs);
};

/* The registers a draw's flush tests for change since the draw began */
enum
{
	GS_DIRTY_REG_ALPHA,
	GS_DIRTY_REG_CLAMP,
	GS_DIRTY_REG_COLCLAMP,
	GS_DIRTY_REG_DIMX,
	GS_DIRTY_REG_DTHE,
	GS_DIRTY_REG_FBA,
	GS_DIRTY_REG_FOGCOL,
	GS_DIRTY_REG_FRAME,
	GS_DIRTY_REG_MIPTBP1,
	GS_DIRTY_REG_MIPTBP2,
	GS_DIRTY_REG_PABE,
	GS_DIRTY_REG_PRIM,
	GS_DIRTY_REG_SCANMSK,
	GS_DIRTY_REG_SCISSOR,
	GS_DIRTY_REG_TEST,
	GS_DIRTY_REG_TEX0,
	GS_DIRTY_REG_TEX1,
	GS_DIRTY_REG_TEXA,
	GS_DIRTY_REG_XYOFFSET,
	GS_DIRTY_REG_ZBUF
};

/* Why a draw was flushed, for the renderer to read while it draws */
#define GS_FLUSH_UNKNOWN          (1 << 0)
#define GS_FLUSH_RESET            (1 << 1)
#define GS_FLUSH_CONTEXTCHANGE    (1 << 2)
#define GS_FLUSH_CLUTCHANGE       (1 << 3)
#define GS_FLUSH_GSTRANSFER       (1 << 4)
#define GS_FLUSH_UPLOADDIRTYTEX   (1 << 5)
#define GS_FLUSH_UPLOADDIRTYFRAME (1 << 6)
#define GS_FLUSH_UPLOADDIRTYZBUF  (1 << 7)
#define GS_FLUSH_LOCALTOLOCALMOVE (1 << 8)
#define GS_FLUSH_DOWNLOADFIFO     (1 << 9)
#define GS_FLUSH_SAVESTATE        (1 << 10)
#define GS_FLUSH_LOADSTATE        (1 << 11)
#define GS_FLUSH_AUTOFLUSH        (1 << 12)
#define GS_FLUSH_VSYNC            (1 << 13)
#define GS_FLUSH_GSREOPEN         (1 << 14)
#define GS_FLUSH_VERTEXCOUNT      (1 << 15)

#define GS_INVALID_ALPHA_MINMAX_STATE GS_INVALID_ALPHA_MINMAX

/* The vertex queue: room for a full draw of the widest primitive and the
 * transfers the targets made from them look back on */
#define GS_STATE_VERTEX_CAPACITY    (0x10000 + 8)
#define GS_STATE_DRAW_TRANSFERS_MAX 4096
/* Where auto flush would have ended the batch, at most */
#define GS_STATE_AF_MAX_SEGMENTS    64

/* An EE transfer into local memory, kept for the targets made from it */
typedef struct gs_upload_queue
{
	GIFRegBITBLTBUF blit;
	GS_STATE_V4I rect;
	int draw;
	bool zero_clear;
} gs_upload_queue;

/* The GIF register handlers, by register */
typedef void (*gs_gif_packed_fn)(struct gs_state* s, const GIFPackedReg* RESTRICT r);
typedef void (*gs_gif_reg_fn)(struct gs_state* s, const GIFReg* RESTRICT r);
typedef void (*gs_gif_packed_run_fn)(struct gs_state* s, const GIFPackedReg* RESTRICT r, u32 size);

/* C's view of local memory as C++ wraps it: the palette after it */
typedef struct PCSX2_ALIGN(32) gs_state_clut
{
	gs_clut m_c;
} gs_state_clut;

typedef struct gs_state_mem
{
	gs_local_memory lm;
	gs_state_clut m_clut;
} gs_state_mem;


struct gs_state
{
	/* Each GIF register's handler: the vertex kicks' are swapped in by
	 * primitive and by whether the draw reads its own target
	 * (gs_state_update_vertex_kick) */
	gs_gif_packed_fn     m_fpGIFPackedRegHandlers[16];
	/* [plain, auto-flush][prim][XYZF2, XYZF3, XYZ2, XYZ3] */
	gs_gif_packed_fn     m_fpGIFPackedRegHandlerXYZ[2][8][4];
	gs_gif_reg_fn        m_fpGIFRegHandlers[256];
	gs_gif_reg_fn        m_fpGIFRegHandlerXYZ[2][8][4];
	gs_gif_packed_run_fn m_fpGIFPackedRegHandlersC[2];
	gs_gif_packed_run_fn m_fpGIFPackedRegHandlerSTQRGBAXYZF2[2][8];
	gs_gif_packed_run_fn m_fpGIFPackedRegHandlerSTQRGBAXYZ2[2][8];

	gs_transfer m_tr;

	GS_STATE_VTX m_v;
	float m_q;
	/* Hot per-vertex flags, in the alignment hole after m_q, so they
	 * share a cache line with it and the scissor cull constants */
	u32 m_dirty_gs_regs;
	bool m_scissor_invalid;
	bool m_nativeres;
	GS_STATE_V4I m_scissor_cull_min;
	GS_STATE_V4I m_scissor_cull_max;
	GS_STATE_V4I m_xyof;

	struct
	{
		GS_STATE_VTX* buff;
		u32 head, tail, next; /* head: first vertex, tail: last vertex + 1, next: last indexed + 1 */
		u32 xy_tail;
		GS_STATE_V4I xy[4];
		GS_STATE_V4I xyhead;
	} m_vertex;

	struct
	{
		u16* buff;
		u32 tail;
	} m_index;

	/* Whether the draw state samples its own frame or Z buffer, so its
	 * vertex kicks take the auto-flush handlers; re-chosen when it flips */
	bool m_autoflush_kick;
	/* The batch so far is all primitives reading only the pixels they
	 * write, and an overlap auto flush would have flushed was let through
	 * for the draw to take in place (tex-is-fb) */
	bool m_af_pixel_batch;
	bool m_af_skipped;
	/* Where auto flush would have ended the batch: an index count each,
	 * the start of a segment that reads what the ones before it drew.
	 * The batch is drawn whole when the renderer can (can_draw_runs),
	 * else one segment at a time; detection looks back to the current
	 * segment's start, over its own draw rect */
	u32 m_af_seg_start[GS_STATE_AF_MAX_SEGMENTS];
	u32 m_af_seg_count;
	GS_STATE_V4I m_af_seg_draw;
	bool m_af_seg_fresh;
	/* (where a batch is kept while it is drawn a segment at a time, made
	 * with the buffers) */
	u16* m_af_saved_index;
	GS_STATE_VTX* m_af_saved_vertex;

	PCSX2_LANG_TYPE(GSVertexTrace, struct gs_vertex_trace) m_vt;

	GIFPath m_path[4];
	const GIFRegPRIM* PRIM;
	GSPrivRegSet* m_regs;
	GSDrawingEnvironment m_env;
	GSDrawingEnvironment m_prev_env;
	const GSDrawingEnvironment* m_draw_env;
	PCSX2_LANG_TYPE(GSDrawingContext, gs_drawing_context)* m_context;

	/* The renderer takes draws reading only the pixels they write in
	 * place (tex-is-fb), so auto flush leaves them in the batch */
	bool m_pixel_feedback_ok;
	GS_STATE_V4I temp_draw_rect;
	/* Savestates restored: what a renderer made of local memory before
	 * the count changed may be stale */
	u32 m_mem_restores;
	/* A savestate is being loaded: the reset it makes is the start of
	 * that */
	bool m_state_loading;

	bool m_mipmap;
	bool m_texflush_flag;
	bool m_isPackedUV_HackFlag;
	bool m_channel_shuffle;
	u8 m_scanmask_used;
	int m_backed_up_ctx;

	/* The EE's transfers into local memory since the draws stopped
	 * preloading them, in order (gs_upload_queue), for the targets made
	 * from them: pushed, read back from the end, walked backwards and
	 * cut at the front */
	gs_array_t m_draw_transfers;

	int m_primitive_covers_without_gaps; /* GS_GAPS_* */
	GS_STATE_V4I m_r;
	GS_STATE_V4I m_r_no_scissor;

	int m_state_flush_reason; /* GS_FLUSH_* */
	int m_prim_overlap;       /* GS_OVERLAP_* */

	/* The sprites of a draw in runs that do not overlap, a count a run
	 * (u32), for the device to draw a run at a time between barriers */
	gs_array_t m_drawlist;

	/* The PCRTC's two read circuits, as GSPCRTC.c keeps them */
	PCSX2_LANG_TYPE(GSPCRTCRegs, gs_pcrtc) PCRTCDisplays;

	const struct gs_state_ops* m_ops;

	/* The merge, the VR eyes and the present at a vsync (GSPresent.c) */
	gs_present m_present;

	bool m_process_texture;
	bool m_downscale_source;

	/* Last: C++ makes it before anything else here is set (GSLocalMemory),
	 * and gs_state_init clears everything before it */
	PCSX2_LANG_TYPE(GSLocalMemory, gs_state_mem) m_mem;
};

/* The draw number and the transfer counts. Only the MTGS thread touches
 * them. */
extern int gs_state_n;
extern int gs_state_last_transfer_draw_n;
extern int gs_state_transfer_n;

/* The members as the C code reads them, in either language */
#define GS_STATE_LM(s)   ((gs_local_memory*)&(s)->m_mem)
#define GS_STATE_CLUT(s) ((gs_clut*)&(s)->m_mem.m_clut)
#define GS_STATE_VT(s)   ((struct gs_vertex_trace*)&(s)->m_vt)
#define GS_STATE_CTX(s)  ((gs_drawing_context*)(s)->m_context)

/* Everything but m_mem, which its owner has made (C++: GSLocalMemory's
 * constructor) */
void gs_state_init(struct gs_state* s);
void gs_state_deinit(struct gs_state* s);

void gs_state_reset_base(struct gs_state* s, bool hardware_reset);
void gs_state_update_settings_base(struct gs_state* s, const EmuGSOptions* old_config);
void gs_state_move_base(struct gs_state* s);
void gs_state_reset_handlers(struct gs_state* s);
void gs_state_refetch_pixel_offsets(struct gs_state* s);
void gs_state_push_draw_transfer(struct gs_state* s, const gs_upload_queue* transfer);

void gs_state_flush(struct gs_state* s, int reason);
void gs_state_flush_write(struct gs_state* s);
void gs_state_flush_prim(struct gs_state* s);
bool gs_state_test_draw_changed(struct gs_state* s);
void gs_state_check_write_overlap(struct gs_state* s, bool req_write, bool req_read);
void gs_state_update_context(struct gs_state* s);
void gs_state_update_scissor(struct gs_state* s);
void gs_state_update_vertex_kick(struct gs_state* s);
void gs_state_update_auto_flush_kick(struct gs_state* s);

/* The EE's side: a transfer into local memory, a read out of it, the GIF
 * packets and the savestate */
void gs_state_transfer_write(struct gs_state* s, const u8* mem, int len);
void gs_state_transfer_read(struct gs_state* s, u8* mem, int len);
void gs_state_init_read_fifo(struct gs_state* s, u8* mem, int len);
void gs_state_read_fifo(struct gs_state* s, u8* mem, int size);
void gs_state_read_local_memory_unsync(struct gs_state* s, u8* mem, int qwc, GIFRegBITBLTBUF BITBLTBUF,
	GIFRegTRXPOS TRXPOS, GIFRegTRXREG TRXREG);
void gs_state_soft_reset(struct gs_state* s, u32 mask);
void gs_state_transfer(struct gs_state* s, const u8* mem, u32 size);
int  gs_state_freeze(struct gs_state* s, freezeData* fd, bool sizeonly);
int  gs_state_defrost(struct gs_state* s, const freezeData* fd);

/* The trace of the draw in hand, from the registers it was drawn with */
void gs_state_vt_update(struct gs_state* s, const void* vertex, const u16* index, int v_count, int i_count,
	GS_PRIM_CLASS primclass);

/* The draw in hand's registers and trace, for GSDrawQuery.c */
static INLINE gs_draw_query gs_state_draw_query(const struct gs_state* s)
{
	gs_draw_query q;
	q.vt           = GS_STATE_VT(s);
	q.prim         = s->PRIM;
	q.ctx          = gs_ctx_regs(GS_STATE_CTX(s));
	q.texa         = &s->m_draw_env->TEXA;
	q.clut         = GS_STATE_CLUT(s);
	q.vertex       = s->m_vertex.buff;
	q.index        = s->m_index.buff;
	q.vertex_count = s->m_vertex.next;
	q.index_count  = s->m_index.tail;
	q.r            = (const gs_recti*)&s->m_r;
	q.r_no_scissor = (const gs_recti*)&s->m_r_no_scissor;
	q.gaps_found   = s->m_primitive_covers_without_gaps == GS_GAPS_FOUND;
	q.hw           = emu_gs_hardware_renderer(&GSConfig);
	return q;
}

/* Where the savestate's parts live (GSStateFile.c) */
static INLINE gs_state_parts gs_state_get_parts(struct gs_state* s)
{
	gs_state_parts p;
	p.env    = (gs_env_regs*)&s->m_env;
	p.ctx[0] = (gs_drawing_regs*)&s->m_env.CTXT[0];
	p.ctx[1] = (gs_drawing_regs*)&s->m_env.CTXT[1];
	p.v      = (union gs_vertex*)&s->m_v;
	p.tr     = &s->m_tr;
	p.vm     = GS_STATE_LM(s)->m_vm8;
	p.path   = s->m_path;
	p.q      = &s->m_q;
	return p;
}

static INLINE gs_upload_queue* gs_state_draw_transfers(struct gs_state* s)
{
	return GS_ARRAY_ITEMS(&s->m_draw_transfers, gs_upload_queue);
}

static INLINE gs_upload_queue* gs_state_last_draw_transfer(struct gs_state* s)
{
	return &gs_state_draw_transfers(s)[s->m_draw_transfers.count - 1];
}

#ifdef __cplusplus
}

/* The state, with the renderers' names for what it does. Nothing here
 * but the C struct: the renderers derive from it and the GIF path, in C,
 * is handed them as a gs_state. */
class GSState : public gs_state, public GSAlignedClass<32>
{
public:
	GSState();
	~GSState();

	/* Deletes the renderer as what it really is: the destructor is not
	 * virtual, so this is how one is disposed of. */
	void Free() { if (m_ops && m_ops->free) m_ops->free(this); }

	static int GetSaveStateSize() { return static_cast<int>(gs_state_size()); }

	typedef gs_upload_queue GSUploadQueue;
	typedef ::GSPCRTCRegs GSPCRTCRegs;

	static constexpr int INVALID_ALPHA_MINMAX = GS_INVALID_ALPHA_MINMAX;
	enum { VERTEX_CAPACITY = GS_STATE_VERTEX_CAPACITY, DRAW_TRANSFERS_MAX = GS_STATE_DRAW_TRANSFERS_MAX };
	enum { AF_MAX_SEGMENTS = GS_STATE_AF_MAX_SEGMENTS };

	enum NoGapsType
	{
		Uninitialized = 0,
		GapsFound     = GS_GAPS_FOUND,
		SpriteNoGaps  = GS_GAPS_SPRITE_NONE,
		FullCover     = GS_GAPS_FULL_COVER,
	};

	enum REG_DIRTY
	{
		DIRTY_REG_ALPHA    = GS_DIRTY_REG_ALPHA,
		DIRTY_REG_CLAMP    = GS_DIRTY_REG_CLAMP,
		DIRTY_REG_COLCLAMP = GS_DIRTY_REG_COLCLAMP,
		DIRTY_REG_DIMX     = GS_DIRTY_REG_DIMX,
		DIRTY_REG_DTHE     = GS_DIRTY_REG_DTHE,
		DIRTY_REG_FBA      = GS_DIRTY_REG_FBA,
		DIRTY_REG_FOGCOL   = GS_DIRTY_REG_FOGCOL,
		DIRTY_REG_FRAME    = GS_DIRTY_REG_FRAME,
		DIRTY_REG_MIPTBP1  = GS_DIRTY_REG_MIPTBP1,
		DIRTY_REG_MIPTBP2  = GS_DIRTY_REG_MIPTBP2,
		DIRTY_REG_PABE     = GS_DIRTY_REG_PABE,
		DIRTY_REG_PRIM     = GS_DIRTY_REG_PRIM,
		DIRTY_REG_SCANMSK  = GS_DIRTY_REG_SCANMSK,
		DIRTY_REG_SCISSOR  = GS_DIRTY_REG_SCISSOR,
		DIRTY_REG_TEST     = GS_DIRTY_REG_TEST,
		DIRTY_REG_TEX0     = GS_DIRTY_REG_TEX0,
		DIRTY_REG_TEX1     = GS_DIRTY_REG_TEX1,
		DIRTY_REG_TEXA     = GS_DIRTY_REG_TEXA,
		DIRTY_REG_XYOFFSET = GS_DIRTY_REG_XYOFFSET,
		DIRTY_REG_ZBUF     = GS_DIRTY_REG_ZBUF
	};

	enum GSFlushReason
	{
		UNKNOWN          = GS_FLUSH_UNKNOWN,
		RESET            = GS_FLUSH_RESET,
		CONTEXTCHANGE    = GS_FLUSH_CONTEXTCHANGE,
		CLUTCHANGE       = GS_FLUSH_CLUTCHANGE,
		GSTRANSFER       = GS_FLUSH_GSTRANSFER,
		UPLOADDIRTYTEX   = GS_FLUSH_UPLOADDIRTYTEX,
		UPLOADDIRTYFRAME = GS_FLUSH_UPLOADDIRTYFRAME,
		UPLOADDIRTYZBUF  = GS_FLUSH_UPLOADDIRTYZBUF,
		LOCALTOLOCALMOVE = GS_FLUSH_LOCALTOLOCALMOVE,
		DOWNLOADFIFO     = GS_FLUSH_DOWNLOADFIFO,
		SAVESTATE        = GS_FLUSH_SAVESTATE,
		LOADSTATE        = GS_FLUSH_LOADSTATE,
		AUTOFLUSH        = GS_FLUSH_AUTOFLUSH,
		VSYNC            = GS_FLUSH_VSYNC,
		GSREOPEN         = GS_FLUSH_GSREOPEN,
		VERTEXCOUNT      = GS_FLUSH_VERTEXCOUNT,
	};

	enum PRIM_OVERLAP
	{
		PRIM_OVERLAP_UNKNOW,
		PRIM_OVERLAP_YES = GS_OVERLAP_YES,
		PRIM_OVERLAP_NO  = GS_OVERLAP_NO
	};

	static constexpr u32 STATE_VERSION = GS_STATE_VERSION;

	/* (only the MTGS thread touches them) */
	static inline int& s_n = gs_state_n;
	static inline int& s_last_transfer_draw_n = gs_state_last_transfer_draw_n;
	static inline int& s_transfer_n = gs_state_transfer_n;

	/* --- the GIF path (GSState.c) --- */
	void ResetBase(bool hardware_reset) { gs_state_reset_base(this, hardware_reset); }
	void UpdateSettingsBase(const EmuGSOptions& old_config) { gs_state_update_settings_base(this, &old_config); }
	void MoveBase() { gs_state_move_base(this); }
	void VSyncBase(u32 field, bool registers_written, bool idle_frame) { gs_present_vsync(&m_present, field, registers_written, idle_frame); }
	void ResetHandlers() { gs_state_reset_handlers(this); }
	/* Both contexts' pixel offset tables again from an empty pool: when a
	 * draw finds one NULL, once nothing reads the pool's (queued draws
	 * done). */
	void RefetchPixelOffsets() { gs_state_refetch_pixel_offsets(this); }

	void Flush(GSFlushReason reason) { gs_state_flush(this, reason); }
	void FlushPrim() { gs_state_flush_prim(this); }
	bool TestDrawChanged() { return gs_state_test_draw_changed(this); }
	void FlushWrite() { gs_state_flush_write(this); }
	void CheckWriteOverlap(bool req_write, bool req_read) { gs_state_check_write_overlap(this, req_write, req_read); }
	void Write(const u8* mem, int len) { gs_state_transfer_write(this, mem, len); }
	void Read(u8* mem, int len) { gs_state_transfer_read(this, mem, len); }
	void InitReadFIFO(u8* mem, int len) { gs_state_init_read_fifo(this, mem, len); }
	void SoftReset(u32 mask) { gs_state_soft_reset(this, mask); }
	void ReadFIFO(u8* mem, int size) { gs_state_read_fifo(this, mem, size); }
	void ReadLocalMemoryUnsync(u8* mem, int qwc, GIFRegBITBLTBUF BITBLTBUF, GIFRegTRXPOS TRXPOS, GIFRegTRXREG TRXREG)
	{
		gs_state_read_local_memory_unsync(this, mem, qwc, BITBLTBUF, TRXPOS, TRXREG);
	}
	void Transfer(const u8* mem, u32 size) { gs_state_transfer(this, mem, size); }
	int Freeze(freezeData* fd, bool sizeonly) { return gs_state_freeze(this, fd, sizeonly); }
	int Defrost(const freezeData* fd) { return gs_state_defrost(this, fd); }

	void UpdateContext() { gs_state_update_context(this); }
	void UpdateScissor() { gs_state_update_scissor(this); }
	void UpdateVertexKick() { gs_state_update_vertex_kick(this); }
	void UpdateAutoFlushKick() { gs_state_update_auto_flush_kick(this); }

	/* --- the renderer's own, through m_ops --- */
	void Reset(bool hardware_reset)
	{
		if (m_ops && m_ops->reset) m_ops->reset(this, hardware_reset); else ResetBase(hardware_reset);
	}
	void UpdateSettings(const EmuGSOptions& old_config)
	{
		if (m_ops && m_ops->update_settings) m_ops->update_settings(this, &old_config); else UpdateSettingsBase(old_config);
	}

	/* --- presentation ---------------------------------------------------
	 * What used to be a class of its own between this one and the two
	 * renderers, GSRenderer: the vsync, the merge of the two read
	 * circuits, and the handful of things a renderer answers about
	 * itself. GSRenderer is another name for this class
	 * (Renderers/Common/GSRenderer.h). */
	void Destroy()           { if (m_ops && m_ops->destroy) m_ops->destroy(this); }
	void UpdateRenderFixes() { if (m_ops && m_ops->update_render_fixes) m_ops->update_render_fixes(this); }
	void PurgePool() { gs_present_purge_pool(); }
	void VSync(u32 field, bool registers_written, bool idle_frame)
	{
		if (m_ops && m_ops->vsync) m_ops->vsync(this, field, registers_written, idle_frame); else VSyncBase(field, registers_written, idle_frame);
	}
	bool  CanUpscale()            { return (m_ops && m_ops->can_upscale) ? m_ops->can_upscale(this) : false; }
	float GetUpscaleMultiplier()  { return (m_ops && m_ops->get_upscale_multiplier) ? m_ops->get_upscale_multiplier(this) : 1.0f; }
	float GetTextureScaleFactor() { return (m_ops && m_ops->get_texture_scale_factor) ? m_ops->get_texture_scale_factor(this) : 1.0f; }
	GSTexture* LookupPaletteSource(u32 CBP, u32 CPSM, u32 CBW, GSVector2i& offset, float* scale, float* sample, const GSVector2i& size)
	{
		return (m_ops && m_ops->lookup_palette_source) ? m_ops->lookup_palette_source(this, CBP, CPSM, CBW, &offset, scale, sample, &size) : nullptr;
	}
	bool IsIdleFrame() const { return gs_present_idle_frame(&m_present); }

	GSTexture* GetOutput(int i, float& scale, int& y_offset)
	{
		return (m_ops && m_ops->get_output) ? m_ops->get_output(this, i, &scale, &y_offset) : nullptr;
	}
	GSTexture* GetFeedbackOutput(float& scale)
	{
		return (m_ops && m_ops->get_feedback_output) ? m_ops->get_feedback_output(this, &scale) : nullptr;
	}

	void Draw() { if (m_ops && m_ops->draw) m_ops->draw(this); }
	void PurgeTextureCache(bool sources, bool targets, bool hash_cache)
	{
		if (m_ops && m_ops->purge_texture_cache) m_ops->purge_texture_cache(this, sources, targets, hash_cache);
	}
	void ReadbackTextureCache() { if (m_ops && m_ops->readback_texture_cache) m_ops->readback_texture_cache(this); }
	void InvalidateVideoMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r)
	{
		if (m_ops && m_ops->invalidate_video_mem) m_ops->invalidate_video_mem(this, &BITBLTBUF, &r);
	}
	void InvalidateLocalMem(const GIFRegBITBLTBUF& BITBLTBUF, const GSVector4i& r, bool clut = false)
	{
		if (m_ops && m_ops->invalidate_local_mem) m_ops->invalidate_local_mem(this, &BITBLTBUF, &r, clut);
	}
	void Move() { if (m_ops && m_ops->move) m_ops->move(this); else MoveBase(); }

	/* --- the draw in hand (GSDrawQuery.c) --- */
	gs_draw_query DrawQuery() const { return gs_state_draw_query(this); }
	gs_state_parts StateParts() { return gs_state_get_parts(this); }

	bool IsPixelFeedbackPrim(u32 prim, const u32* buff, int n) const
	{
		const gs_draw_query q = DrawQuery();
		return gs_draw_is_pixel_feedback_prim(&q, &m_v, prim, buff, n);
	}
	/* The draw state reads what it draws */
	bool IsAutoFlushDraw() { const gs_draw_query q = DrawQuery(); return gs_draw_reads_own_target(&q); }

	GSVertexTrace::VertexAlpha& GetAlphaMinMax()
	{
		if (!m_vt.m_alpha.valid)
			CalcAlphaMinMax(0, INVALID_ALPHA_MINMAX);
		return m_vt.m_alpha;
	}
	struct TextureMinMaxResult
	{
		enum UsesBoundary
		{
			USES_BOUNDARY_LEFT   = GS_USES_BOUNDARY_LEFT,
			USES_BOUNDARY_TOP    = GS_USES_BOUNDARY_TOP,
			USES_BOUNDARY_RIGHT  = GS_USES_BOUNDARY_RIGHT,
			USES_BOUNDARY_BOTTOM = GS_USES_BOUNDARY_BOTTOM,
			USES_BOUNDARY_U = GS_USES_BOUNDARY_U,
			USES_BOUNDARY_V = GS_USES_BOUNDARY_V,
		};
		GSVector4i coverage; ///< Part of the texture used
		u8 uses_boundary;    ///< Whether or not the usage touches the left, top, right, or bottom edge (and therefore needs wrap modes preserved)
	};
	TextureMinMaxResult GetTextureMinMax(GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear, bool clamp_to_tsize)
	{
		const gs_draw_query q = DrawQuery();
		const gs_tex_minmax r = gs_draw_texture_min_max(&q, TEX0, CLAMP, linear, clamp_to_tsize);
		TextureMinMaxResult res;
		res.coverage      = GSVector4i(r.coverage.x, r.coverage.y, r.coverage.z, r.coverage.w);
		res.uses_boundary = r.uses_boundary;
		return res;
	}
	bool TryAlphaTest(u32& fm, u32& zm) { const gs_draw_query q = DrawQuery(); return gs_draw_try_alpha_test(&q, &fm, &zm); }
	bool IsOpaque() { const gs_draw_query q = DrawQuery(); return gs_draw_is_opaque(&q); }
	bool IsMipMapDraw() { const gs_draw_query q = DrawQuery(); return gs_draw_is_mipmap_draw(&q); }
	bool IsMipMapActive() { return m_mipmap && IsMipMapDraw(); }
	bool IsCoverageAlpha() { const gs_draw_query q = DrawQuery(); return gs_draw_is_coverage_alpha(&q); }
	void CalcAlphaMinMax(const int tex_min, const int tex_max) { const gs_draw_query q = DrawQuery(); gs_draw_calc_alpha_min_max(&q, tex_min, tex_max); }
	void CorrectATEAlphaMinMax(const u32 atst, const int aref) { const gs_draw_query q = DrawQuery(); gs_draw_correct_ate_alpha_min_max(&q, atst, aref); }

	bool GetQuadCorners(const GSVertex* v, const u16* i, const GSVertex** v0, const GSVertex** v1) const
	{
		const gs_draw_query q = DrawQuery();
		return gs_draw_quad_corners(&q, v, i, reinterpret_cast<const void**>(v0), reinterpret_cast<const void**>(v1));
	}
	void GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout) const
	{
		const gs_draw_query q = DrawQuery();
		gs_draw_quad_bbox_window(&q, &v0, &v1, xyout.v, NULL, false);
	}
	void GetQuadBBoxWindow(const GSVertex& v0, const GSVertex& v1, GSVector4& xyout, GSVector4& texout, bool keep_tex_order = true) const
	{
		const gs_draw_query q = DrawQuery();
		gs_draw_quad_bbox_window(&q, &v0, &v1, xyout.v, texout.v, keep_tex_order);
	}
	static void GetQuadRasterizedPoints(GSVector4& xy, bool keep_order = true) { gs_draw_quad_rasterized_points(xy.v, NULL, keep_order); }
	static void GetQuadRasterizedPoints(GSVector4& xy, GSVector4& tex, bool keep_order = true) { gs_draw_quad_rasterized_points(xy.v, tex.v, keep_order); }

	bool TrianglesAreQuads(bool shuffle_check = false) const
	{
		const gs_draw_query q = DrawQuery();
		return gs_draw_triangles_are_quads(&q, shuffle_check);
	}
	PRIM_OVERLAP PrimitiveOverlap()
	{
		const gs_draw_query q = DrawQuery();
		return static_cast<PRIM_OVERLAP>(gs_draw_primitive_overlap(&q, &m_drawlist));
	}
	bool SpriteDrawWithoutGaps() { const gs_draw_query q = DrawQuery(); return gs_draw_sprites_without_gaps(&q); }
	void CalculatePrimitiveCoversWithoutGaps()
	{
		const gs_draw_query q = DrawQuery();
		m_primitive_covers_without_gaps = gs_draw_covers_without_gaps(&q);
	}
	GIFRegTEX0 GetTex0Layer(u32 lod) { const gs_draw_query q = DrawQuery(); return gs_draw_tex0_layer(&q, lod); }

	/* --- the EE's transfers since the draws stopped preloading them --- */
	__fi GSUploadQueue* DrawTransfers() { return gs_state_draw_transfers(this); }
	__fi u32 DrawTransferCount() const { return static_cast<u32>(m_draw_transfers.count); }
	__fi GSUploadQueue& LastDrawTransfer() { return *gs_state_last_draw_transfer(this); }
	__fi void PopDrawTransfer() { m_draw_transfers.count--; }
	__fi void EraseDrawTransfer(u32 i) { gs_array_erase(&m_draw_transfers, i); }
	__fi void ClearDrawTransfers() { m_draw_transfers.count = 0; }
	void PushDrawTransfer(const GSUploadQueue& transfer) { gs_state_push_draw_transfer(this, &transfer); }

	GSVector4i GetTEX0Rect()
	{
		const gs_recti r = gs_transfer_tex0_rect(m_prev_env.CTXT[m_prev_env.PRIM.CTXT].Regs());
		return GSVector4i(r.x, r.y, r.z, r.w);
	}

	u8* GetRegsMem() const { return reinterpret_cast<u8*>(m_regs); }
	void SetRegsMem(u8* basemem)
	{
		m_regs         = reinterpret_cast<GSPrivRegSet*>(basemem);
		m_present.regs = m_regs;
	}

	/// Expands dither matrix, suitable for software renderer.
	static void ExpandDIMX(GSVector4i* dimx, const GIFRegDIMX DIMX);
	void ResetPCRTC() { gs_pcrtc_reset(&PCRTCDisplays, m_regs); }
	GSVideoMode GetVideoMode() { return static_cast<GSVideoMode>(gs_regs_video_mode(m_regs)); }
	bool isinterlaced() { return gs_regs_interlaced(m_regs); }
	bool isReallyInterlaced() { return gs_regs_really_interlaced(m_regs); }
	float GetTvRefreshRate() { return gs_regs_tv_refresh_rate(m_regs); }
};

inline GSState::GSState()
{
	gs_state_init(this);
}

inline GSState::~GSState()
{
	gs_state_deinit(this);
}

inline void GSVertexTrace::Update(const void* vertex, const u16* index, int v_count, int i_count, GS_PRIM_CLASS primclass)
{
	gs_state_vt_update(static_cast<gs_state*>(const_cast<void*>(m_state)), vertex, index, v_count, i_count, primclass);
}

static_assert(static_cast<int>(GSVideoMode::HDTV_1080I) == GS_VMODE_HDTV_1080I && static_cast<int>(GSVideoMode::SDTV_480P) == GS_VMODE_SDTV_480P
	&& static_cast<int>(GSVideoMode::PAL) == GS_VMODE_PAL, "GSVideoMode is GS_VMODE_*");

static_assert(MAX_SKIPPED_DUPLICATE_FRAMES == GS_MAX_SKIPPED_DUPLICATE_FRAMES, "GSPresent.c skips as many");
static_assert(offsetof(GSDrawingEnvironment, TRXREG) == offsetof(gs_env_regs, TRXREG)
	&& offsetof(GSDrawingEnvironment, PRMODECONT) == offsetof(gs_env_regs, PRMODECONT), "gs_env_regs is GSDrawingEnvironment's start");
static_assert(sizeof(GIFRegRGBAQ) == 8 && sizeof(GIFRegST) == 8 && sizeof(GIFRegXYZ) == 8 && sizeof(GIFReg) == 8,
	"the state's register sizes");
/* (GSLocalMemory has two bases, so offsetof on it is only conditionally
 * supported; every compiler that builds the core supports it) */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(sizeof(GSLocalMemory) == sizeof(gs_state_mem) && offsetof(GSLocalMemory, m_clut) == offsetof(gs_state_mem, m_clut)
	&& sizeof(GSClut) == sizeof(gs_state_clut), "C's view of local memory");
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static_assert(sizeof(GSVertexTrace) == sizeof(gs_vertex_trace), "GSVertexTrace adds no members");
static_assert(sizeof(GSPCRTCRegs) == sizeof(gs_pcrtc), "GSPCRTCRegs adds no members");

// We put this in the header because of Multi-ISA.
inline void GSState::ExpandDIMX(GSVector4i* dimx, const GIFRegDIMX DIMX)
{
	dimx[1] = GSVector4i(DIMX.DM00, 0, DIMX.DM01, 0, DIMX.DM02, 0, DIMX.DM03, 0);
	dimx[0] = dimx[1].xxzzlh();
	dimx[3] = GSVector4i(DIMX.DM10, 0, DIMX.DM11, 0, DIMX.DM12, 0, DIMX.DM13, 0);
	dimx[2] = dimx[3].xxzzlh();
	dimx[5] = GSVector4i(DIMX.DM20, 0, DIMX.DM21, 0, DIMX.DM22, 0, DIMX.DM23, 0);
	dimx[4] = dimx[5].xxzzlh();
	dimx[7] = GSVector4i(DIMX.DM30, 0, DIMX.DM31, 0, DIMX.DM32, 0, DIMX.DM33, 0);
	dimx[6] = dimx[7].xxzzlh();
}

#endif /* __cplusplus */

#undef GS_STATE_V2I
#undef GS_STATE_TEX

#endif
