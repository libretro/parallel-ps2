/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_RASTER_H
#define GS_RASTER_H

/* The software renderer's rasterizer: a draw's points, lines, triangles
 * and sprites into the scanlines the scanline code (generated, or
 * GSDrawScanline's C++ on ARM) draws. One per drawing thread, which
 * draws the rows of its bands. */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "../../gs_rect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GSVector4's layout */
typedef union PCSX2_ALIGN(16) gs_raster_lane
{
	float F32[4];
	s32 I32[4];
	u32 U32[4];
	double F64[2];
} gs_raster_lane;

/* GSVertexSW (GSVertexSW.h) is C++'s view of this. p is x, y and a
 * double z (a sprite's: x, y, _, f), t is s, t, q and f (a sprite's
 * z), c is r, g, b, a; a scanline's pixels, left and top go in _pad. */
#ifndef __cplusplus
typedef struct PCSX2_ALIGN(32) GSVertexSW
{
	gs_raster_lane p, _pad, t, c;
} GSVertexSW;
#endif

struct GSVertexSW;
struct GSScanlineLocalData;
struct gs_offset;
struct gs_drawing_regs;
union gs_vertex;

/* A draw's rect fills (GSDrawScanline's global data): local memory, the
 * frame and depth buffers, their write masks (all ones: not written),
 * whether each is 16 bits a pixel, and FBA */
typedef struct gs_raster_fill
{
	u8* vm;
	const struct gs_offset* fbo;
	const struct gs_offset* zbo;
	u32 fm, zm;
	u8 fpsm16, zpsm16, fba;
} gs_raster_fill;

/* The scanline code's three entries (GSDrawScanline), and its rect fill */
typedef void (*gs_raster_setup_prim_fn)(const struct GSVertexSW* vertex, const u16* index,
	const struct GSVertexSW* dscan, struct GSScanlineLocalData* local);
typedef void (*gs_raster_scanline_fn)(int pixels, int left, int top, const struct GSVertexSW* scan,
	struct GSScanlineLocalData* local);
typedef void (*gs_raster_rect_fn)(const gs_raster_fill* f, const gs_recti* r, const struct GSVertexSW* v);

/* What of the draw's selector the rasterizer goes by */
#define GS_RASTER_SEL_TEX_GRID   (1u << 0) /* fb and a texture function: S, T, Q snapped */
#define GS_RASTER_SEL_FST        (1u << 1) /* (U and V, whole units, instead) */
#define GS_RASTER_SEL_Z_GRID     (1u << 2) /* zb, not a sprite, no zequal: Z snapped */
#define GS_RASTER_SEL_TRIANGLE   (1u << 3) /* sel.prim */
#define GS_RASTER_SEL_HIRES      (1u << 4) /* into the 2x memory */
#define GS_RASTER_SEL_SOLID_RECT (1u << 5)

typedef struct gs_raster_draw
{
	gs_recti scissor;
	gs_recti bbox;
	int primclass;
	const struct GSVertexSW* vertex;
	int vertex_count;
	const u16* index;
	int index_count;
	unsigned sel;
	u8 scanmsk_value;
	gs_raster_setup_prim_fn setup_prim;
	gs_raster_scanline_fn draw_scanline;
	gs_raster_scanline_fn draw_edge; /* NULL: no antialiased edges */
	const gs_raster_fill* fill;
} gs_raster_draw;

typedef struct gs_raster
{
	/* The scanlines of a primitive, before they are drawn: at most one a
	 * row of the 2x drawing, or one a column for an edge */
	struct GSVertexSW* edge;
	int edge_count;
	/* Whether a band of 1 << thread_height rows is this one's: of its
	 * own bands, or of all of them (gs_raster_draw_whole) */
	u8* scanline;
	u8* scanline_own;
	u8* scanline_all;
	int threads;
	int threads_own;
	int thread_height;

	/* The draw's */
	gs_recti scissor;
	float fscissor_x[4]; /* left, right, left, right */
	float fscissor_y[4]; /* top, bottom, top, bottom */
	unsigned sel;
	u8 scanmsk_value;
	gs_raster_setup_prim_fn setup_prim;
	gs_raster_scanline_fn draw_scanline;
	gs_raster_scanline_fn draw_edge;
	const gs_raster_fill* fill;

	/* The scanline code's data for this thread, and the grids in it
	 * the primitive's set up with: S, T, Q, Z's four floats and their
	 * reciprocals, Z's double and its reciprocal */
	struct GSScanlineLocalData* local;
	float* tgrid;
	float* tgrid_rcp;
	double* zgrid;
	gs_raster_rect_fn draw_rect; /* gs_raster_fill_rect */
} gs_raster;

/* Thread id of threads, bands of 1 << thread_height rows; local and its
 * grids, and draw_rect, are the caller's to set */
bool gs_raster_init(gs_raster* r, int id, int threads, int thread_height);
void gs_raster_free(gs_raster* r);

void gs_raster_draw_bands(gs_raster* r, const gs_raster_draw* d);
/* Every row, as one thread, while no other draws */
void gs_raster_draw_whole(gs_raster* r, const gs_raster_draw* d);

/* A solid rect, of v's colour and depth (t.w), into the frame and depth
 * buffers, swizzled */
void gs_raster_fill_rect(const gs_raster_fill* f, const gs_recti* r, const struct GSVertexSW* v);

/* count of the GS's vertices to the rasterizer's, in ctx's offset, as a
 * primclass draws them: tme and fst its texturing, q_div whether S and T
 * are divided by Q (a sprite's by the second vertex's) */
void gs_raster_convert_vertices(const struct gs_drawing_regs* ctx, struct GSVertexSW* dst, const union gs_vertex* src,
	u32 count, int primclass, int tme, int fst, int q_div);

#ifdef __cplusplus
}
#endif

#endif
