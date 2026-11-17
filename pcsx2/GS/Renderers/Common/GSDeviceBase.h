/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2023 PCSX2 Dev Team
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

/* The render device in C: what every backend has (gs_device), the table
 * each answers through (gs_device_ops), and what is the same for all of
 * them (GSDevice.c) - the texture pool, the present chain the merge and
 * the deinterlacers draw, the VR frame. GSDevice (GSDevice.h) is the C++
 * view a backend derives from; its member functions call these. */
#ifndef GS_DEVICE_BASE_H
#define GS_DEVICE_BASE_H

#include <libretro.h>

#include "common/Pcsx2Defs.h"
#include "common/WindowInfo.h"
#include "../../GSRegs.h"
#include "../../gs_rect.h"
#include "GSTextureBase.h"
#include "GSTexturePool.h"
#include "GSRetireRing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shaders C asks for by number (C++: ShaderConvert, ShaderInterlace) */
#define GS_SHADER_CONVERT_COPY 0
#define GS_SHADER_CONVERT_FLOAT32_TO_RGBA8 13
#define GS_SHADER_INTERLACE_WEAVE 0
#define GS_SHADER_INTERLACE_BOB 1
#define GS_SHADER_INTERLACE_BLEND 2
#define GS_SHADER_INTERLACE_MAD_BUFFER 3
#define GS_SHADER_INTERLACE_MAD_RECONSTRUCT 4

/* GS.h's RenderAPI, and GSDevice::PresentResult */
#define GS_RENDER_API_NONE 0
#define GS_RENDER_API_D3D11 1
#define GS_RENDER_API_D3D12 2
#define GS_RENDER_API_VULKAN 3
#define GS_RENDER_API_OPENGL 4
#define GS_PRESENT_OK 0
#define GS_PRESENT_FRAME_SKIPPED 1
#define GS_PRESENT_DEVICE_LOST 2

/* What a device can do (C++: GSDevice::FeatureSupport) */
typedef struct gs_device_features
{
	unsigned broken_point_sampler : 1;   /* An issue with AMD cards, see the tfx shader */
	unsigned vs_expand : 1;              /* Expands points, lines and sprites in the vertex shader */
	unsigned primitive_id : 1;           /* Primitive ID, for destination alpha by primitive */
	unsigned texture_barrier : 1;        /* Samples the target, with a texture barrier */
	unsigned provoking_vertex_last : 1;  /* Flat shading from a primitive's last vertex */
	unsigned point_expand : 1;           /* Point expansion in hardware */
	unsigned line_expand : 1;            /* Line expansion in hardware */
	unsigned prefer_new_textures : 1;    /* Fills the pool before reusing, to avoid render pass restarts */
	unsigned dxt_textures : 1;           /* DXTn (S3TC, BC1-3) */
	unsigned bptc_textures : 1;          /* BC6 and BC7 */
	unsigned framebuffer_fetch : 1;      /* Reads the framebuffer without texture barriers */
	unsigned clip_control : 1;           /* A 0..1 depth range instead of -1..1 */
	unsigned stencil_buffer : 1;         /* A stencil buffer, which DATE can use */
	unsigned test_and_sample_depth : 1;  /* Samples and tests the depth buffer at once */
	unsigned dual_source_blend : 1;      /* A second output (SRC1) as a blend factor */
	unsigned colclip_hw : 1;             /* A format with 16-bit integer channels for the colour clip */
	unsigned indexed4_conversion : 1;    /* ConvertToIndexedTexture builds a PSMT4 view of a PSMCT32 target too */
	unsigned texture_feedback : 1;       /* A draw samples its own target anywhere, behind a barrier */
} gs_device_features;

/* Every device has dual source blending and the colour clip format but
 * OpenGL ES, which clears them */
void gs_device_features_init(gs_device_features* f);

struct gs_device_ops;
struct gs_vr_ops;

/* How many presents a retired texture is held for when the frontend says
 * nothing of when it is done with it: a guess, which a frontend caching
 * frames for longer would outlive. Where it does say (SyncIndexWaited),
 * this does not apply. */
#define GS_RETIRED_PRESENT_MIN_AGE 8

typedef struct gs_device
{
	/* The table this device answers through, and VR's (NULL in a device
	 * that makes no VR frames) */
	const struct gs_device_ops* m_ops;
	const struct gs_vr_ops* m_vr_ops;

	/* Textures and targets kept for reuse (GSTexturePool.h) */
	gs_tex_pool m_tex_pool;
	WindowInfo m_window_info;

	/* The present chain: the merge, the deinterlacers' outputs, and for the
	 * weave and bob the merge that last carried each field, which they read
	 * the other field's lines from (never handed to the frontend). What is
	 * current is the one to present. */
	gs_texture* m_merge;
	gs_texture* m_weavebob;
	gs_texture* m_blend;
	gs_texture* m_mad;
	gs_texture* m_weave_field[2];
	gs_texture* m_target_tmp;
	gs_texture* m_current;
	gs_texture* m_colclip_rt; /* The colour clip pass's target, while one runs */
	int m_mad_buffer;         /* The adaptive deinterlacer's buffer of the last field */

	/* Textures handed to the frontend by reference cannot be destroyed the
	 * moment a resize replaces them: it samples them in its own
	 * submissions and may replay them for pause or screenshots, none of
	 * which our fences see. They are retired (GSRetireRing.h) and freed
	 * once it is done with them: one full trip of its sync slots after,
	 * where it says (m_sync_*), GS_RETIRED_PRESENT_MIN_AGE presents else. */
	struct gs_retire_ring m_retired_present;
	u32 m_present_age;
	u32 m_sync_waits;
	u32 m_sync_slots;

	/* VR (gs_device_vr_compose). The output is handed over like the
	 * merge and retired like it; the rest is the device's own. The
	 * snapshot holds a depth buffer's Z as bytes. */
	gs_texture* m_vr_out;
	gs_texture* m_vr_atlas;
	gs_texture* m_vr_reduce[2];
	gs_texture* m_vr_range[2];
	gs_texture* m_vr_snapshot;
	u32 m_vr_range_cur;
	u32 m_vr_depth_missing; /* Frames in a row VR had no depth for */

	struct
	{
		u32 start, count;
	} m_vertex, m_index;
	bool m_rbswapped;
	gs_device_features m_features;
} gs_device;

/* (C++ types the table passes on, opaque here) */
struct gs_multi_stretch_rect;
struct GSHWDrawConfig;
struct MoveTexelsConstants;

/* What the five devices - D3D11, D3D12, Vulkan, OpenGL and the software
 * one - answer differently, with the device first. A NULL entry is what
 * gs_device does by default. Textures are the backends' GSTexture, which
 * starts with its gs_texture; rects are GSVector4s. */
struct gs_device_ops
{
	void (*free)(gs_device* dev);
	bool (*create)(gs_device* dev);
	void (*destroy)(gs_device* dev);
	void (*reset_api_state)(gs_device* dev);
	void (*restore_api_state)(gs_device* dev);
	int (*get_render_api)(const gs_device* dev);
	int (*begin_present)(gs_device* dev, bool frame_skip);
	void (*end_present)(gs_device* dev);
	gs_texture* (*create_surface)(gs_device* dev, unsigned type, int width, int height, int levels, unsigned format);
	void* (*create_download_texture)(gs_device* dev, u32 width, u32 height, unsigned format);
	void (*do_merge)(gs_device* dev, gs_texture* sTex[3], gs_rectf* sRect, gs_texture* dTex, gs_rectf* dRect,
		const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c, bool linear);
	/* zrh: the interlace shader's constants (InterlaceConstantBuffer) */
	void (*do_interlace)(gs_device* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
		const gs_rectf* dRect, int shader, bool linear, const gs_rectf* zrh);
	void (*copy_rect)(gs_device* dev, gs_texture* sTex, gs_texture* dTex, const gs_recti* r, u32 destX, u32 destY);
	void (*stretch_rect)(gs_device* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
		const gs_rectf* dRect, int shader, bool linear);
	void (*stretch_rect_mask)(gs_device* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
		const gs_rectf* dRect, bool red, bool green, bool blue, bool alpha, int shader);
	void (*present_rect)(gs_device* dev, gs_texture* sTex, const gs_rectf* sRect, gs_texture* dTex,
		const gs_rectf* dRect);
	void (*draw_multi_stretch_rects)(gs_device* dev, const struct gs_multi_stretch_rect* rects, u32 num_rects,
		gs_texture* dTex, int shader);
	void (*update_clut_texture)(gs_device* dev, gs_texture* sTex, float sScale, float sSample, u32 offsetX,
		u32 offsetY, gs_texture* dTex, u32 dOffset, u32 dSize);
	void (*convert_to_indexed_texture)(gs_device* dev, gs_texture* sTex, float sScale, u32 offsetX, u32 offsetY,
		u32 SBW, u32 SPSM, gs_texture* dTex, u32 DBW, u32 DPSM);
	void (*filtered_downsample_texture)(gs_device* dev, gs_texture* sTex, gs_texture* dTex, u32 downsample_factor,
		const gs_point* clamp_min, const gs_rectf* dRect);
	void (*move_texels)(gs_device* dev, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
		const struct MoveTexelsConstants* cb);
	void (*render_hw)(gs_device* dev, struct GSHWDrawConfig* config);
	void (*clear_sampler_cache)(gs_device* dev);
};

/* The two passes of a VR frame, as their shaders' constants (C++:
 * VRRangeConstants, VREyeConstants; GSDevice.h has what they mean) */
typedef struct gs_vr_range_constants
{
	gs_recti src;
	gs_recti params;
	gs_rectf fparams;
} gs_vr_range_constants;
typedef struct gs_vr_eye_constants
{
	gs_rectf eye_pos;
	gs_rectf eye_quat;
	gs_rectf fov_tan;
	gs_rectf screen;
	gs_rectf depth;
} gs_vr_eye_constants;

/* What a device that makes VR frames answers, beside gs_device_ops: each
 * pass drawn over dRect of dTex from sTex */
struct gs_vr_ops
{
	void (*range)(gs_device* dev, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
		const gs_vr_range_constants* cb);
	void (*eye)(gs_device* dev, gs_texture* sTex, gs_texture* dTex, const gs_rectf* dRect,
		const gs_vr_eye_constants* cb);
};

/* One VR frame (C++: GSVRFrame) */
typedef struct gs_vr_frame
{
	gs_vr_eye_constants eyes[2];
	gs_rectf depth_rect;
	gs_rectf picture_rect;
	gs_texture* depth;
	gs_point eye_size;
	float z_scale;
	bool depth_encoded;
} gs_vr_frame;

/* GSDevice.c */
void gs_device_init(gs_device* dev);
void gs_device_deinit(gs_device* dev);
void gs_device_acquire_window(gs_device* dev);
void gs_device_destroy_base(gs_device* dev);

/* The pool (GSTexturePool.h), with the device's scale and wishes */
gs_tex_pool* gs_device_pool(gs_device* dev);
gs_texture* gs_device_fetch_surface(gs_device* dev, unsigned type, int width, int height, int levels,
	unsigned format, bool clear, bool prefer_reuse);
void gs_device_recycle(gs_device* dev, gs_texture* t);
void gs_device_age_pool(gs_device* dev);
void gs_device_purge_pool(gs_device* dev);
void gs_device_prewarm_pool(gs_device* dev);
gs_texture* gs_device_create_render_target(gs_device* dev, int w, int h, unsigned format, bool clear);
gs_texture* gs_device_create_depth_stencil(gs_device* dev, int w, int h, unsigned format, bool clear);
gs_texture* gs_device_create_texture(gs_device* dev, int w, int h, int mipmap_levels, unsigned format,
	bool prefer_reuse);
int gs_device_mipmap_levels_for_size(int width, int height);
/* The index buffer that expands each quad of four vertices to two
 * triangles; buffer holds GS_DEVICE_EXPAND_BUFFER_SIZE bytes */
#define GS_DEVICE_EXPAND_BUFFER_SIZE (2 * 16383 * 6)
void gs_device_generate_expansion_index_buffer(void* buffer);

/* The present chain */
void gs_device_clear_current(gs_device* dev);
void gs_device_retire_present_texture(gs_device* dev, gs_texture* t);
bool gs_device_resize_render_target(gs_device* dev, gs_texture** t, int w, int h, bool preserve_contents,
	bool recycle, bool defer_destroy);
void gs_device_merge(gs_device* dev, gs_texture* sTex[3], gs_rectf* sRect, gs_rectf* dRect, int fs_x, int fs_y,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c);
void gs_device_interlace(gs_device* dev, int ds_x, int ds_y, int field, int mode, float yoffset, float block,
	bool half_fields);
bool gs_device_swap_present_texture(gs_device* dev, gs_texture* handed, gs_texture* released);

/* VR */
gs_texture* gs_device_vr_compose(gs_device* dev, gs_texture* picture, const gs_vr_frame* frame);
gs_texture* gs_device_vr_blank(gs_device* dev, int eye_w, int eye_h);
bool gs_device_vr_snapshot_depth(gs_device* dev, gs_texture* depth);
void gs_device_vr_release(gs_device* dev);

/* The shaders' names, and the DATM shader for a SetDATM */
const char* gs_shader_convert_name(int shader);
int gs_set_datm_shader(int datm);

/* The hardware blend of each GS blend equation, indexed by
 * ((A * 3 + B) * 3 + C) * 3 + D of the ALPHA register */
typedef struct gs_hw_blend
{
	u16 flags;
	u8 op, src, dst;
} gs_hw_blend;
extern const gs_hw_blend gs_hw_blend_map[81];

/* A new device of each kind, made in its backend's own file; NULL where
 * the kind is not built */
gs_device* gs_device_new_sw(void);
gs_device* gs_device_new_opengl(void);
gs_device* gs_device_new_vulkan(void);
gs_device* gs_device_new_d3d11(void);
gs_device* gs_device_new_d3d12(void);

/* The open device, or NULL (C++: g_gs_device) */
extern gs_device* g_gs_device_c;

#ifdef __cplusplus
}
#endif

#endif
