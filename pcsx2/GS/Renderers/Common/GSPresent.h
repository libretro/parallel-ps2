/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_PRESENT_H
#define GS_PRESENT_H

/* What a GSdx renderer does at a vsync, in C: the merge of the two read
 * circuits into the device's present texture, the deinterlace, the VR
 * eyes, and the present (GSPresent.c). GSState keeps one and hands it
 * itself and a table of what only the renderer answers. */

#include "common/Pcsx2Defs.h"
#include "../../GSRegs.h"
#include "../../GSPCRTC.h"
#include "../../gs_rect.h"
#include "GSTextureBase.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The most duplicate frames skipped in a row (GSExtra.h's) */
#define GS_MAX_SKIPPED_DUPLICATE_FRAMES 3

/* What the renderer answers (GSState's gs_state_ops, through gs): the
 * texture a circuit (0, 1, or both for -1) reads, its scale and the
 * rows it starts down; the feedback write's; the depth a picture was
 * drawn with, for the VR eyes; the upscale */
typedef struct gs_present_ops
{
	gs_texture* (*get_output)(void* gs, int i, float* scale, int* y_offset);
	gs_texture* (*get_feedback_output)(void* gs, float* scale);
	bool (*get_vr_depth)(void* gs, gs_texture* color, gs_texture** depth, bool* encoded, float* z_scale);
	float (*get_upscale_multiplier)(void* gs);
} gs_present_ops;

typedef struct gs_present
{
	/* The circuit the merge took the picture from, for the VR eyes: the
	 * part read (normalised), where it went (pixels of real_size) and
	 * its texture */
	gs_rectf vr_merge_src;
	gs_rectf vr_merge_dst;
	gs_texture* vr_merge_tex;

	void* gs;
	const gs_present_ops* ops;
	gs_pcrtc* pcrtc;
	const GSPrivRegSet* regs;
	/* GSState's: the frames SCANMSK was last used for, and the draw and
	 * transfer counts */
	u8* scanmask_used;
	int* draw_n;
	const int* transfer_n;

	/* The merged frame's size */
	gs_point real_size;
	/* The size of one eye in the map that stands */
	gs_point vr_eye_size;
	u32 skipped_duplicate_frames;
	/* The draw and transfer counts at the last vsync */
	int last_draw_n;
	int last_transfer_n;
	/* The last vsync made VR eyes, or would have: the renderer keeps
	 * track of depth buffers only while it does */
	bool vr_active;
	/* A view map was sent and stands until one with no views is */
	bool vr_map_set;
} gs_present;

void gs_present_init(gs_present* p, void* gs, const gs_present_ops* ops, gs_pcrtc* pcrtc, u8* scanmask_used,
	int* draw_n, const int* transfer_n);

/* The circuits merged into the device's present texture, deinterlaced;
 * false for a blank frame */
bool gs_present_merge(gs_present* p, int field);
/* A vsync: the merge, the VR eyes and the present, or a duplicate frame
 * skipped */
void gs_present_vsync(gs_present* p, u32 field, bool registers_written, bool idle_frame);
/* Nothing drawn or transferred since the last vsync */
bool gs_present_idle_frame(const gs_present* p);
/* The device's pool emptied, where that does not crash the API */
void gs_present_purge_pool(void);

#ifdef __cplusplus
}
#endif

#endif
