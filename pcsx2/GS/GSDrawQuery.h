/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_DRAW_QUERY_H
#define GS_DRAW_QUERY_H

/* What the renderers ask of the draw in hand, from its registers and its
 * vertex trace: the part of the texture it reads, the range of its alpha,
 * whether the alpha test can be done away with, whether it is opaque,
 * mipmapped, or anti-aliased by coverage, and a mip level's TEX0.
 * GSState fills a gs_draw_query for each question (C++: its member
 * functions of the same names). */

#include "common/Pcsx2Defs.h"
#include "GSRegs.h"
#include "GSDrawingContext.h"
#include "GSClut.h"
#include "gs_rect.h"
#include "Renderers/Common/GSVertexTrace.h"
#include "Renderers/Common/GSArray.h"

#ifdef __cplusplus
extern "C" {
#endif

/* An alpha the texture's is not known to be within (C++:
 * INVALID_ALPHA_MINMAX) */
#define GS_INVALID_ALPHA_MINMAX 500

/* The texture's edges a draw reads past */
#define GS_USES_BOUNDARY_LEFT   (1 << 0)
#define GS_USES_BOUNDARY_TOP    (1 << 1)
#define GS_USES_BOUNDARY_RIGHT  (1 << 2)
#define GS_USES_BOUNDARY_BOTTOM (1 << 3)
#define GS_USES_BOUNDARY_U      (GS_USES_BOUNDARY_LEFT | GS_USES_BOUNDARY_RIGHT)
#define GS_USES_BOUNDARY_V      (GS_USES_BOUNDARY_TOP | GS_USES_BOUNDARY_BOTTOM)

typedef struct gs_draw_query
{
	struct gs_vertex_trace* vt;
	const GIFRegPRIM* prim;
	const gs_drawing_regs* ctx;
	const GIFRegTEXA* texa;
	gs_clut* clut;
	/* the vertices and indices, how many of each */
	const void* vertex;
	const u16* index;
	u32 vertex_count;
	u32 index_count;
	/* the draw's rect, with and without the scissor */
	const gs_recti* r;
	const gs_recti* r_no_scissor;
	/* the draw's sprites were found to leave gaps */
	bool gaps_found;
	/* the hardware renderer's, not the software one's */
	bool hw;
} gs_draw_query;

typedef struct gs_tex_minmax
{
	gs_recti coverage;  /* the part of the texture read */
	u8 uses_boundary;   /* GS_USES_BOUNDARY_*: edges it reaches, whose wrap modes then matter */
} gs_tex_minmax;

gs_tex_minmax gs_draw_texture_min_max(const gs_draw_query* q, GIFRegTEX0 TEX0, GIFRegCLAMP CLAMP, bool linear,
	bool clamp_to_tsize);
/* The trace's alpha range after the texture function, the texture's
 * alpha being tex_min..tex_max (up to GS_INVALID_ALPHA_MINMAX when it is
 * not known) */
void gs_draw_calc_alpha_min_max(const gs_draw_query* q, int tex_min, int tex_max);
/* The range, worked out if it is not yet */
const struct gs_vt_alpha* gs_draw_alpha_min_max(const gs_draw_query* q);
/* The range cut to what passes the alpha test atst against aref */
void gs_draw_correct_ate_alpha_min_max(const gs_draw_query* q, u32 atst, int aref);
/* False when the test passes some pixels and fails others; else true,
 * with what it fails masked out of fm and zm */
bool gs_draw_try_alpha_test(const gs_draw_query* q, u32* fm, u32* zm);
bool gs_draw_is_opaque(const gs_draw_query* q);
bool gs_draw_is_mipmap_draw(const gs_draw_query* q);
bool gs_draw_is_coverage_alpha(const gs_draw_query* q);
/* TEX0 for a mip level: its base, width and size */
GIFRegTEX0 gs_draw_tex0_layer(const gs_draw_query* q, u32 lod);

/* GSState's NoGapsType and PRIM_OVERLAP */
#define GS_GAPS_UNKNOWN 0
#define GS_GAPS_FOUND 1
#define GS_GAPS_SPRITE_NONE 2
#define GS_GAPS_FULL_COVER 3
#define GS_OVERLAP_UNKNOWN 0
#define GS_OVERLAP_YES 1
#define GS_OVERLAP_NO 2

/* The triangles, in pairs, are axis-aligned quads (each pair sharing an
 * edge with the one before); shuffle_check moves the pairs after the
 * first back 8 pixels, for a channel shuffle's */
bool gs_draw_triangles_are_quads(const gs_draw_query* q, bool shuffle_check);
/* Whether the draw's primitives overlap (GS_OVERLAP_*): for sprites,
 * the runs of them that do not, as a count each, into drawlist (u32s;
 * room made beforehand) */
int gs_draw_primitive_overlap(const gs_draw_query* q, gs_array_t* drawlist);
/* The sprites tile the draw's rect without gaps, in rows or columns */
bool gs_draw_sprites_without_gaps(const gs_draw_query* q);
/* Whether the primitives cover the draw's rect (GS_GAPS_*) */
int gs_draw_covers_without_gaps(const gs_draw_query* q);
/* The draw state reads what it draws: a texture at its frame's or Z
 * buffer's address, sharing bits with what is written there */
bool gs_draw_reads_own_target(const gs_draw_query* q);
/* The primitive prim, from the vertex in hand (v, first) and the indices
 * of the n - 1 before it, reads only the pixels it writes: a sprite, or
 * a triangle writing alpha alone, each vertex's texel its pixel */
bool gs_draw_is_pixel_feedback_prim(const gs_draw_query* q, const void* v, u32 prim, const u32* buff, int n);

/* A quad's two corner vertices from its indices i into vertex: a
 * sprite's own two (false for other primitives: triangle quads are not
 * found yet) */
bool gs_draw_quad_corners(const gs_draw_query* q, const void* vertex, const u16* i, const void** v0,
	const void** v1);
/* The window rect (pixels, offset taken off) of the quad with corners v0
 * and v1, left, top, right, bottom, and with tex its texture coordinates
 * (texels) at those corners: in the corners' order if keep_order, else
 * as a bounding box too. Nothing for primitives other than sprites and
 * triangles; tex left alone without TME. */
void gs_draw_quad_bbox_window(const gs_draw_query* q, const void* v0, const void* v1, float xy[4], float tex[4],
	bool keep_order);
/* The quad's corners moved in to the pixels it covers, the texture
 * coordinates moved with them (tex NULL: none) */
void gs_draw_quad_rasterized_points(float xy[4], float tex[4], bool keep_order);

#ifdef __cplusplus
}
#endif

#endif
