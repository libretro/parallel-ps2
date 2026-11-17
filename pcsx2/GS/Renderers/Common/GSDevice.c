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

/* What every render device does the same (GSDeviceBase.h): the pool, the
 * present chain the merge and deinterlacers draw into, the VR frame, and
 * the device the frontend's context is given. */

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GSDeviceBase.h"
#include "../../GSBackend.h"
#include "../../GSEntry.h"

gs_device* g_gs_device_c = NULL;

static void dev_free_texture(gs_texture* t)
{
	if (t)
		t->m_ops->free(t);
}

static gs_rectf rectf(float x, float y, float z, float w)
{
	gs_rectf r;
	r.x = x;
	r.y = y;
	r.z = z;
	r.w = w;
	return r;
}

static gs_recti recti(int x, int y, int z, int w)
{
	gs_recti r;
	r.x = x;
	r.y = y;
	r.z = z;
	r.w = w;
	return r;
}

static void dev_clear_target(gs_texture* t, u32 c)
{
	t->m_state             = GS_TEXSTATE_CLEARED;
	t->m_clear_value.color = c;
}

static void dev_stretch(gs_device* dev, gs_texture* s, gs_rectf sr, gs_texture* d, gs_rectf dr, int shader,
	bool linear)
{
	dev->m_ops->stretch_rect(dev, s, &sr, d, &dr, shader, linear);
}

static void dev_copy(gs_device* dev, gs_texture* s, gs_texture* d, gs_recti r, u32 x, u32 y)
{
	dev->m_ops->copy_rect(dev, s, d, &r, x, y);
}

/* ---- the device, and the frontend's context's ---- */

void gs_device_features_init(gs_device_features* f)
{
	memset(f, 0, sizeof(*f));
	f->dual_source_blend = 1;
	f->colclip_hw        = 1;
}

static gs_texture* dev_pool_create(void* d, unsigned type, int w, int h, int levels, unsigned format)
{
	gs_device* dev = (gs_device*)d;
	return dev->m_ops->create_surface(dev, type, w, h, levels, format);
}

void gs_device_init(gs_device* dev)
{
	memset(dev, 0, sizeof(*dev));
	gs_tex_pool_init(&dev->m_tex_pool, dev_pool_create, dev);
	gs_device_features_init(&dev->m_features);
}

void gs_device_deinit(gs_device* dev)
{
	gs_tex_pool_deinit(&dev->m_tex_pool);
}

void gs_device_acquire_window(gs_device* dev)
{
	dev->m_window_info = host_acquire_render_window();
}

static void retired_present_free(void* tex)
{
	dev_free_texture((gs_texture*)tex);
}

void gs_device_destroy_base(gs_device* dev)
{
	gs_device_clear_current(dev);
	/* Nothing presents after this, so the retired textures go too */
	gs_retire_clear(&dev->m_retired_present, retired_present_free);
	gs_device_purge_pool(dev);
}

/* A device that fails to come up destroys what it managed to make */
static bool dev_open(gs_device* dev)
{
	if (!dev)
		return false;
	g_gs_device_c = dev;
	if (!(dev->m_ops->create ? dev->m_ops->create(dev) : true))
	{
		gs_device_close();
		return false;
	}
	return true;
}

bool gs_device_open(enum retro_hw_context_type context, GSRendererType renderer)
{
	switch (context)
	{
		case RETRO_HW_CONTEXT_D3D11:
#ifdef _WIN32
			if (!dev_open(gs_device_new_d3d11()))
				return false;
#endif
			break;
		case RETRO_HW_CONTEXT_D3D12:
#ifdef _WIN32
			if (!dev_open(gs_device_new_d3d12()))
				return false;
#endif
			break;
		case RETRO_HW_CONTEXT_VULKAN:
#ifdef ENABLE_VULKAN
			/* (paraLLEl-GS fuses device and renderer together) */
			if (renderer != GS_RENDERER_PARALLEL_GS && !dev_open(gs_device_new_vulkan()))
				return false;
#endif
			break;
		case RETRO_HW_CONTEXT_OPENGL:
		case RETRO_HW_CONTEXT_OPENGL_CORE:
		case RETRO_HW_CONTEXT_OPENGLES3:
		case RETRO_HW_CONTEXT_OPENGLES_VERSION:
#ifdef ENABLE_OPENGL
			if (!dev_open(gs_device_new_opengl()))
				return false;
#endif
			break;
		case RETRO_HW_CONTEXT_NONE:
			/* No hardware context (an SDL2 frontend, say): only the software
			 * renderer runs, and the caller forced it before reaching here */
			if (!dev_open(gs_device_new_sw()))
				return false;
			break;
		default:
			return false;
	}
	(void)renderer;

	/* The device is up and has its command buffers, so the pool can be
	 * filled with the sizes a game is going to ask for. Inside a backend's
	 * create is too early: see gs_device_prewarm_pool. */
	if (g_gs_device_c)
		gs_device_prewarm_pool(g_gs_device_c);
	return true;
}

void gs_device_close(void)
{
	gs_device* dev = g_gs_device_c;
	if (!dev)
		return;
	if (dev->m_ops->destroy)
		dev->m_ops->destroy(dev);
	else
		gs_device_destroy_base(dev);
	dev->m_ops->free(dev);
	g_gs_device_c = NULL;
}

void gs_device_clear_sampler_cache(void)
{
	if (g_gs_device_c && g_gs_device_c->m_ops->clear_sampler_cache)
		g_gs_device_c->m_ops->clear_sampler_cache(g_gs_device_c);
}

gs_texture* gs_device_create_clut_target(int width)
{
	if (!g_gs_device_c || !GSIsHardwareRenderer())
		return NULL;
	return gs_device_create_render_target(g_gs_device_c, width, 1, GS_TEX_COLOR, false);
}

bool gs_device_update_clut_texture(gs_texture* src, float scale, float sample, u32 offset_x, u32 offset_y,
	gs_texture* dst, u32 dst_offset, u32 dst_size)
{
	if (!g_gs_device_c)
		return false;
	g_gs_device_c->m_ops->update_clut_texture(g_gs_device_c, src, scale, sample, offset_x, offset_y, dst,
		dst_offset, dst_size);
	return true;
}

/* ---- the pool ---- */

gs_tex_pool* gs_device_pool(gs_device* dev)
{
	/* Known only once the device is up, changed by settings, and cheap
	 * to read */
	dev->m_tex_pool.software   = dev->m_ops->get_render_api(dev) == GS_RENDER_API_NONE;
	dev->m_tex_pool.prefer_new = dev->m_features.prefer_new_textures;
	dev->m_tex_pool.upscale    = GSConfig.UpscaleMultiplier;
	return &dev->m_tex_pool;
}

gs_texture* gs_device_fetch_surface(gs_device* dev, unsigned type, int width, int height, int levels,
	unsigned format, bool clear, bool prefer_reuse)
{
	return gs_tex_pool_fetch(gs_device_pool(dev), type, width, height, levels, format, clear, prefer_reuse);
}

void gs_device_recycle(gs_device* dev, gs_texture* t)
{
	/* Back into the pool, nothing freed: freeing inside a draw is the
	 * churn the pool exists to stop. What it may hold is the age's call,
	 * once a frame. */
	gs_tex_pool_put(&dev->m_tex_pool, t);
}

void gs_device_age_pool(gs_device* dev)
{
	/* A retired present texture has to outlive the frontend's hold on it.
	 * Where the frontend says when that ends - a backend handing textures
	 * over per sync index, calling SyncIndexWaited after each
	 * wait_sync_index - one full trip of its slots since the texture was
	 * retired is that, exactly. Otherwise there is nothing to go on but a
	 * count of presents. */
	dev->m_present_age++;
	gs_retire_age(&dev->m_retired_present, dev->m_present_age, dev->m_sync_waits, dev->m_sync_slots,
		GS_RETIRED_PRESENT_MIN_AGE, retired_present_free);
	gs_tex_pool_age(gs_device_pool(dev));
}

void gs_device_purge_pool(gs_device* dev)
{
	gs_tex_pool_purge(&dev->m_tex_pool);
}

/* Made once, after the device is up, rather than discovered during the
 * first frames of a game; after, and not from the backend's create, which
 * runs before it has command buffers, where a surface reaches the
 * out-of-memory path that flushes one. A frame buffer is FBW wide, a
 * multiple of 64 pixels to 1024, at the handful of heights the video
 * modes have, all of it in four megabytes of GS memory: so the sizes a
 * target can be are a short list, and at a fixed upscale the host sizes
 * are that list times the scale. The common ones go in the pool, and the
 * first draw of a game takes its targets out of it like any later one.
 * Sizes outside the list allocate the first time they are asked for, and
 * are then kept. */
void gs_device_prewarm_pool(gs_device* dev)
{
	gs_tex_pool_prewarm(gs_device_pool(dev));
}

gs_texture* gs_device_create_render_target(gs_device* dev, int w, int h, unsigned format, bool clear)
{
	return gs_device_fetch_surface(dev, GS_TEXTYPE_RENDER_TARGET, w, h, 1, format, clear, true);
}

gs_texture* gs_device_create_depth_stencil(gs_device* dev, int w, int h, unsigned format, bool clear)
{
	return gs_device_fetch_surface(dev, GS_TEXTYPE_DEPTH_STENCIL, w, h, 1, format, clear, true);
}

gs_texture* gs_device_create_texture(gs_device* dev, int w, int h, int mipmap_levels, unsigned format,
	bool prefer_reuse)
{
	const int levels = mipmap_levels < 0 ? gs_device_mipmap_levels_for_size(w, h) : mipmap_levels;
	return gs_device_fetch_surface(dev, GS_TEXTYPE_TEXTURE, w, h, levels, format, false, prefer_reuse);
}

int gs_device_mipmap_levels_for_size(int width, int height)
{
	/* (GSExtra.h's MAXIMUM_TEXTURE_MIPMAP_LEVELS) */
	const int levels = (int)log2((double)(width > height ? width : height)) + 1;
	return levels < 7 ? levels : 7;
}

void gs_device_generate_expansion_index_buffer(void* buffer)
{
	const u32 max_index = GS_DEVICE_EXPAND_BUFFER_SIZE / 6 / sizeof(u16);
	u16* idx = (u16*)buffer;
	u32 i;

	for (i = 0; i < max_index; i++)
	{
		const u32 base = i * 4;
		*(idx++) = (u16)(base + 0);
		*(idx++) = (u16)(base + 1);
		*(idx++) = (u16)(base + 2);
		*(idx++) = (u16)(base + 1);
		*(idx++) = (u16)(base + 2);
		*(idx++) = (u16)(base + 3);
	}
}

/* ---- the present chain ---- */

void gs_device_retire_present_texture(gs_device* dev, gs_texture* t)
{
	/* Freed by the pool's age once the frontend is done with it */
	gs_retire_add(&dev->m_retired_present, t, dev->m_present_age, dev->m_sync_waits, retired_present_free);
}

void gs_device_clear_current(gs_device* dev)
{
	dev->m_current = NULL;

	/* The colour clip target is checked out of the pool for the length of
	 * a pass and handed back by whichever draw resolves it. Torn down, or
	 * the state dropped for a savestate load or a reopen, with one still
	 * out, nothing would free it: the pool's purge sees only the pool. */
	dev_free_texture(dev->m_colclip_rt);
	dev->m_colclip_rt = NULL;

	/* The frontend may still be showing one of these: it replays the last
	 * frame while the core is paused, and a savestate load or a reset
	 * lands in the middle of that. They are retired like any other
	 * present texture. */
	gs_device_retire_present_texture(dev, dev->m_merge);
	gs_device_retire_present_texture(dev, dev->m_weavebob);
	gs_device_retire_present_texture(dev, dev->m_blend);
	gs_device_retire_present_texture(dev, dev->m_mad);
	dev_free_texture(dev->m_weave_field[0]);
	dev_free_texture(dev->m_weave_field[1]);
	dev_free_texture(dev->m_target_tmp);

	dev->m_merge          = NULL;
	dev->m_weavebob       = NULL;
	dev->m_blend          = NULL;
	dev->m_mad            = NULL;
	dev->m_weave_field[0] = NULL;
	dev->m_weave_field[1] = NULL;
	dev->m_target_tmp     = NULL;

	gs_device_vr_release(dev);
}

bool gs_device_resize_render_target(gs_device* dev, gs_texture** t, int w, int h, bool preserve_contents,
	bool recycle, bool defer_destroy)
{
	gs_texture* orig = *t;
	gs_texture* tex;
	bool keep;

	if (orig && orig->m_size.x == w && orig->m_size.y == h)
	{
		if (!preserve_contents)
			orig->m_state = GS_TEXSTATE_INVALIDATED;
		return true;
	}

	keep = preserve_contents && orig;
	tex  = gs_device_fetch_surface(dev, GS_TEXTYPE_RENDER_TARGET, w, h, 1, orig ? orig->m_format : GS_TEX_COLOR,
		!keep, true);
	if (!tex)
	{
		log_cb(RETRO_LOG_INFO, "%dx%d texture allocation failed in ResizeTexture()\n", w, h);
		return false;
	}

	if (keep)
		dev_stretch(dev, orig, rectf(0.0f, 0.0f, 1.0f, 1.0f), tex,
			rectf(0.0f, 0.0f, (float)orig->m_size.x, (float)orig->m_size.y), GS_SHADER_CONVERT_COPY, true);

	if (orig)
	{
		if (defer_destroy)
			gs_device_retire_present_texture(dev, orig);
		else if (recycle)
			gs_device_recycle(dev, orig);
		else
			dev_free_texture(orig);
	}

	*t = tex;
	return true;
}

bool gs_device_swap_present_texture(gs_device* dev, gs_texture* handed, gs_texture* released)
{
	gs_texture** owner;
	if (handed == dev->m_merge)
		owner = &dev->m_merge;
	else if (handed == dev->m_weavebob)
		owner = &dev->m_weavebob;
	else if (handed == dev->m_blend)
		owner = &dev->m_blend;
	else if (handed == dev->m_vr_out)
		owner = &dev->m_vr_out;
	else
		return false;
	*owner = released;
	if (dev->m_current == handed)
		dev->m_current = released;
	return true;
}

void gs_device_merge(gs_device* dev, gs_texture* sTex[3], gs_rectf* sRect, gs_rectf* dRect, int fs_x, int fs_y,
	const GSRegPMODE* PMODE, const GSRegEXTBUF* EXTBUF, u32 c)
{
	if (gs_device_resize_render_target(dev, &dev->m_merge, fs_x, fs_y, false, false, true))
		dev->m_ops->do_merge(dev, sTex, sRect, dev->m_merge, dRect, PMODE, EXTBUF, c, GSConfig.PCRTCOffsets);
	dev->m_current = dev->m_merge;
}

/* One interlace pass of sTex into dTex, moved down by yoffset; the
 * adaptive buffer's into the half of dTex its buffer index names */
static void dev_interlace_pass(gs_device* dev, gs_texture* sTex, gs_texture* dTex, int shader, bool linear,
	float yoffset, int buf, float block)
{
	const float w = (float)dTex->m_size.x;
	const float h = (float)dTex->m_size.y;
	const gs_rectf sRect = rectf(0.0f, 0.0f, 1.0f, 1.0f);
	gs_rectf dRect = rectf(0.0f, yoffset, w, h + yoffset);
	const gs_rectf zrh = rectf((float)buf, 1.0f / h, h, block);

	if (shader == GS_SHADER_INTERLACE_MAD_BUFFER)
	{
		const float half = h * 0.5f;
		if ((buf >> 1) == 1)
			dRect.y += half;
		else
			dRect.w -= half;
	}
	dev->m_ops->do_interlace(dev, sTex, &sRect, dTex, &dRect, shader, linear, &zrh);
}

/* block: the rows of one frame line in the output, the upscale. The fields
 * weave by lines, each line the block of rows its field drew for it; a
 * half-height field (FFMD) was stretched twice by the merge, and the shader
 * takes the line's rows from its block there (block passed positive).
 *
 * What is presented - the weave, bob and blend outputs, and the adaptive
 * reconstruction - is drawn whole every field, into whichever texture the
 * backend gave back for it (gs_device_swap_present_texture): the frontend
 * still holds the ones it was handed before. The weave and the adaptive
 * buffer read the source with the linear sampler: the rows the shader
 * places between a field's rows are the mean of the two, read at the
 * edge between them. */
void gs_device_interlace(gs_device* dev, int ds_x, int ds_y, int field, int mode, float yoffset, float block,
	bool half_fields)
{
	float offset;

	if (!half_fields)
		block = -block;

	/* No merge (its target could not be made): nothing to deinterlace, and
	 * the frame is skipped. A target of the deinterlacer's that could not
	 * be made below shows the merge as it is instead. */
	if (!dev->m_merge)
	{
		dev->m_current = NULL;
		return;
	}
	offset = GSConfig.DisableInterlaceOffset ? 0.0f : yoffset * (float)field;

	switch (mode)
	{
		case 0: /* Weave */
		{
			/* This field's lines from this frame's merge, the other's from
			 * the merge that last carried that field, which is kept for it;
			 * the one it replaces is the next merge target. */
			gs_texture* const other = dev->m_weave_field[field ^ 1];
			gs_texture* const next  = dev->m_weave_field[field];
			if (!gs_device_resize_render_target(dev, &dev->m_weavebob, ds_x, ds_y, false, false, true))
			{
				dev->m_current = dev->m_merge;
				break;
			}
			if (!other)
				dev_clear_target(dev->m_weavebob, 0);
			dev_interlace_pass(dev, dev->m_merge, dev->m_weavebob, GS_SHADER_INTERLACE_WEAVE, true, offset, field,
				block);
			if (other)
				dev_interlace_pass(dev, other, dev->m_weavebob, GS_SHADER_INTERLACE_WEAVE, true,
					GSConfig.DisableInterlaceOffset ? 0.0f : yoffset * (float)(field ^ 1), field ^ 1, block);
			dev->m_weave_field[field] = dev->m_merge;
			dev->m_merge              = next;
			dev->m_current            = dev->m_weavebob;
			break;
		}
		case 1: /* Bob */
		{
			/* The field is reversed here, countering the bounce. The rows
			 * above the ones the field is moved down to show the top of the
			 * other field, from the merge that last carried it, kept as the
			 * weave keeps it. */
			const float bob_offset = yoffset * (float)(1 - field);
			gs_texture* const other = dev->m_weave_field[field ^ 1];
			gs_texture* const next  = dev->m_weave_field[field];
			if (!gs_device_resize_render_target(dev, &dev->m_weavebob, ds_x, ds_y, false, false, true))
			{
				dev->m_current = dev->m_merge;
				break;
			}
			if (bob_offset > 0.0f && other)
				dev_stretch(dev, other, rectf(0.0f, 0.0f, 1.0f, bob_offset / (float)ds_y), dev->m_weavebob,
					rectf(0.0f, 0.0f, (float)ds_x, bob_offset), GS_SHADER_CONVERT_COPY, true);
			else if (bob_offset > 0.0f)
				dev_clear_target(dev->m_weavebob, 0);
			dev_interlace_pass(dev, dev->m_merge, dev->m_weavebob, GS_SHADER_INTERLACE_BOB, true, bob_offset, 0,
				block);
			dev->m_weave_field[field] = dev->m_merge;
			dev->m_merge              = next;
			dev->m_current            = dev->m_weavebob;
			break;
		}
		case 2: /* Blend */
			if (!gs_device_resize_render_target(dev, &dev->m_weavebob, ds_x, ds_y, true, false, true)
				|| !gs_device_resize_render_target(dev, &dev->m_blend, ds_x, ds_y, false, false, true))
			{
				dev->m_current = dev->m_merge;
				break;
			}
			dev_interlace_pass(dev, dev->m_merge, dev->m_weavebob, GS_SHADER_INTERLACE_WEAVE, true, offset, field,
				block);
			dev_interlace_pass(dev, dev->m_weavebob, dev->m_blend, GS_SHADER_INTERLACE_BLEND, false, 0.0f, 0,
				block);
			dev->m_current = dev->m_blend;
			break;
		case 3: /* FastMAD motion adaptive deinterlacing */
		{
			int buf = dev->m_mad_buffer;
			buf++;
			buf &= ~1;
			buf |= field;
			buf &= 3;
			dev->m_mad_buffer = buf;
			if (!gs_device_resize_render_target(dev, &dev->m_mad, ds_x, ds_y * 2, true, false, true)
				|| !gs_device_resize_render_target(dev, &dev->m_weavebob, ds_x, ds_y, false, false, true))
			{
				dev->m_current = dev->m_merge;
				break;
			}
			if (half_fields)
			{
				/* The last four fields, each whole at its own height (the
				 * merge drew it twice as tall): the reconstruction places
				 * each at its own lines, and a line where the picture moves
				 * takes the current field's own rows there. */
				const float slot_h = (float)dev->m_mad->m_size.y * 0.25f;
				const gs_rectf sRect = rectf(0.0f, 0.0f, 1.0f, 1.0f);
				const gs_rectf dRect = rectf(0.0f, slot_h * (float)buf, (float)dev->m_mad->m_size.x,
					slot_h * (float)(buf + 1));
				const gs_rectf zrh = rectf((float)buf, 0.0f, 0.0f, block);
				dev->m_ops->do_interlace(dev, dev->m_merge, &sRect, dev->m_mad, &dRect, GS_SHADER_INTERLACE_BOB,
					true, &zrh);
				dev_interlace_pass(dev, dev->m_mad, dev->m_weavebob, GS_SHADER_INTERLACE_MAD_RECONSTRUCT, true,
					0.0f, buf, block);
			}
			else
			{
				dev_interlace_pass(dev, dev->m_merge, dev->m_mad, GS_SHADER_INTERLACE_MAD_BUFFER, true, offset,
					buf, block);
				dev_interlace_pass(dev, dev->m_mad, dev->m_weavebob, GS_SHADER_INTERLACE_MAD_RECONSTRUCT, false,
					0.0f, buf, block);
			}
			dev->m_current = dev->m_weavebob;
			break;
		}
		default:
			dev->m_current = dev->m_merge;
			break;
	}
}

/* ---- VR ----
 * See GSVR.h for what the eyes are. The work is in two shaders, which a
 * backend that has them answers through m_vr_ops; this is the part the
 * same for every backend.
 *
 * Both read one texture, an atlas of everything an eye needs, since the
 * utility pipelines of every backend bind one: the picture, the bytes of
 * the GS's Z for each of its pixels under it, and in the texel under
 * that the range of Z the frame uses. The range is what makes depth mean
 * the same from game to game: a game puts its Z where it likes in the 24
 * or 32 bits of the buffer, and the eyes see the frame's nearest Z at the
 * nearest depth and its farthest at the farthest. It is reduced on the
 * GPU, the atlas's depth to one texel in three passes, and eased from
 * frame to frame so that a muzzle flash or an object passing the camera
 * does not make the scene breathe. */
#define VR_REDUCE_BLOCK 16
#define VR_RANGE_EASE 0.2f
/* How many frames in a row without depth keep the last frame's */
#define VR_DEPTH_HOLD 8
/* (GSDevice.h: VR_RANGE_FROM_Z, VR_RANGE_SMOOTH) */
#define VR_FROM_Z 1
#define VR_SMOOTH 2

void gs_device_vr_release(gs_device* dev)
{
	/* The frontend may still be showing it */
	gs_device_retire_present_texture(dev, dev->m_vr_out);
	dev->m_vr_out = NULL;

	dev_free_texture(dev->m_vr_atlas);
	dev_free_texture(dev->m_vr_reduce[0]);
	dev_free_texture(dev->m_vr_reduce[1]);
	dev_free_texture(dev->m_vr_range[0]);
	dev_free_texture(dev->m_vr_range[1]);
	dev_free_texture(dev->m_vr_snapshot);
	dev->m_vr_atlas         = NULL;
	dev->m_vr_reduce[0]     = NULL;
	dev->m_vr_reduce[1]     = NULL;
	dev->m_vr_range[0]      = NULL;
	dev->m_vr_range[1]      = NULL;
	dev->m_vr_range_cur     = 0;
	dev->m_vr_snapshot      = NULL;
	dev->m_vr_depth_missing = VR_DEPTH_HOLD;
}

bool gs_device_vr_snapshot_depth(gs_device* dev, gs_texture* depth)
{
	/* A buffer still waiting on its clear holds one value, nothing to see
	 * depth in */
	if (!dev->m_vr_ops || !depth || depth->m_state != GS_TEXSTATE_DIRTY)
		return false;
	if (!gs_device_resize_render_target(dev, &dev->m_vr_snapshot, depth->m_size.x, depth->m_size.y, false, true,
			false))
		return false;
	dev_stretch(dev, depth, rectf(0.0f, 0.0f, 1.0f, 1.0f), dev->m_vr_snapshot,
		rectf(0.0f, 0.0f, (float)depth->m_size.x, (float)depth->m_size.y), GS_SHADER_CONVERT_FLOAT32_TO_RGBA8, false);
	return true;
}

gs_texture* gs_device_vr_blank(gs_device* dev, int eye_w, int eye_h)
{
	if (eye_w <= 0 || eye_h <= 0)
		return NULL;
	if (!gs_device_resize_render_target(dev, &dev->m_vr_out, 2 * eye_w, eye_h, false, false, true))
		return NULL;
	dev_clear_target(dev->m_vr_out, 0xFF000000u);
	return dev->m_vr_out;
}

/* The depth of the picture reduced to its range and eased towards the
 * last frame's, into texel (0, 2h) of the atlas */
static void vr_depth(gs_device* dev, const gs_vr_frame* frame, int w, int h)
{
	const gs_rectf dst = rectf(frame->picture_rect.x, frame->picture_rect.y + (float)h, frame->picture_rect.z,
		frame->picture_rect.w + (float)h);
	const int w1 = (w + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
	const int h1 = (h + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
	const int w2 = (w1 + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
	const int h2 = (h1 + VR_REDUCE_BLOCK - 1) / VR_REDUCE_BLOCK;
	bool ok;
	int i;

	dev_stretch(dev, frame->depth, frame->depth_rect, dev->m_vr_atlas, dst,
		frame->depth_encoded ? GS_SHADER_CONVERT_COPY : GS_SHADER_CONVERT_FLOAT32_TO_RGBA8, false);

	/* The last level has a column more, for last frame's range: the pass
	 * that eases towards it reads both through its one texture */
	ok = gs_device_resize_render_target(dev, &dev->m_vr_reduce[0], w1, h1, false, true, false)
		&& gs_device_resize_render_target(dev, &dev->m_vr_reduce[1], w2 + 1, h2, false, true, false);
	for (i = 0; ok && i < 2; i++)
	{
		/* Made once and kept: each holds a frame's range for the next.
		 * Cleared to zero, which is no range. */
		if (!dev->m_vr_range[i])
			dev->m_vr_range[i] = gs_device_create_render_target(dev, 1, 1, GS_TEX_COLOR, true);
		ok = dev->m_vr_range[i] != NULL;
	}

	if (ok)
	{
		const u32 prev = dev->m_vr_range_cur;
		const u32 next = prev ^ 1;
		const gs_rectf r1 = rectf(0.0f, 0.0f, (float)w1, (float)h1);
		const gs_rectf r2 = rectf(0.0f, 0.0f, (float)w2, (float)h2);
		const gs_rectf r3 = rectf(0.0f, 0.0f, 1.0f, 1.0f);
		gs_vr_range_constants cb;

		cb.src     = recti(0, h, w, h);
		cb.params  = recti(VR_REDUCE_BLOCK, VR_FROM_Z, 0, 0);
		cb.fparams = rectf(frame->z_scale, 0.0f, 0.0f, 0.0f);
		dev->m_vr_ops->range(dev, dev->m_vr_atlas, dev->m_vr_reduce[0], &r1, &cb);

		dev_copy(dev, dev->m_vr_range[prev], dev->m_vr_reduce[1], recti(0, 0, 1, 1), (u32)w2, 0);
		cb.src    = recti(0, 0, w1, h1);
		cb.params = recti(VR_REDUCE_BLOCK, 0, 0, 0);
		dev->m_vr_ops->range(dev, dev->m_vr_reduce[0], dev->m_vr_reduce[1], &r2, &cb);

		cb.src     = recti(0, 0, w2, h2);
		cb.params  = recti(w2 > h2 ? w2 : h2, VR_SMOOTH, w2, 0);
		cb.fparams = rectf(frame->z_scale, VR_RANGE_EASE, 0.0f, 0.0f);
		dev->m_vr_ops->range(dev, dev->m_vr_reduce[1], dev->m_vr_range[next], &r3, &cb);

		dev->m_vr_range_cur = next;
		dev_copy(dev, dev->m_vr_range[next], dev->m_vr_atlas, recti(0, 0, 1, 1), 0, (u32)(2 * h));
	}
}

gs_texture* gs_device_vr_compose(gs_device* dev, gs_texture* picture, const gs_vr_frame* frame)
{
	int w, h, eye;
	bool hold;

	if (!dev->m_vr_ops || !picture || frame->eye_size.x <= 0 || frame->eye_size.y <= 0)
		return NULL;

	w = picture->m_size.x;
	h = picture->m_size.y;

	/* A frame now and then whose depth cannot be found keeps the last
	 * one's, rather than the picture going flat for a frame: depth that
	 * comes and goes is worse than depth a frame old */
	hold = !frame->depth && dev->m_vr_atlas && dev->m_vr_atlas->m_size.x == w
		&& dev->m_vr_atlas->m_size.y == 2 * h + 1 && dev->m_vr_depth_missing < VR_DEPTH_HOLD;
	dev->m_vr_depth_missing = frame->depth ? 0 : dev->m_vr_depth_missing + 1;

	if (!gs_device_resize_render_target(dev, &dev->m_vr_atlas, w, 2 * h + 1, hold, true, false))
		return NULL;

	/* Zero under the picture is the far plane wherever the game's frame
	 * buffer does not reach, and zero in the range texel an empty range,
	 * which the eyes see as a flat window */
	if (!hold)
		dev_clear_target(dev->m_vr_atlas, 0);
	dev_copy(dev, picture, dev->m_vr_atlas, recti(0, 0, w, h), 0, 0);

	if (frame->depth)
		vr_depth(dev, frame, w, h);

	if (!gs_device_resize_render_target(dev, &dev->m_vr_out, 2 * frame->eye_size.x, frame->eye_size.y, false, false,
			true))
		return NULL;

	for (eye = 0; eye < 2; eye++)
	{
		const gs_rectf r = rectf((float)(eye * frame->eye_size.x), 0.0f, (float)((eye + 1) * frame->eye_size.x),
			(float)frame->eye_size.y);
		dev->m_vr_ops->eye(dev, dev->m_vr_atlas, dev->m_vr_out, &r, &frame->eyes[eye]);
	}
	return dev->m_vr_out;
}

/* ---- tables ---- */

int gs_set_datm_shader(int datm)
{
	/* SetDATM: DATM0, DATM1, DATM0_RTA_CORRECTION, DATM1_RTA_CORRECTION;
	 * ShaderConvert: DATM_1 2, DATM_0 3, DATM_1_RTA_CORRECTION 4,
	 * DATM_0_RTA_CORRECTION 5 */
	switch (datm)
	{
		case 3: return 4;
		case 2: return 5;
		case 1: return 2;
		default: return 3;
	}
}

const char* gs_shader_convert_name(int shader)
{
	/* ShaderConvert's order */
	static const char* const names[] = {
		"ps_copy",
		"ps_convert_rgba8_16bits",
		"ps_datm1",
		"ps_datm0",
		"ps_datm1_rta_correction",
		"ps_datm0_rta_correction",
		"ps_colclip_init",
		"ps_colclip_resolve",
		"ps_rta_correction",
		"ps_rta_decorrection",
		"ps_filter_transparency",
		"ps_convert_float32_32bits",
		"ps_convert_float32_32bits",
		"ps_convert_float32_rgba8",
		"ps_convert_float32_rgba8",
		"ps_convert_float16_rgb5a1",
		"ps_convert_rgba8_float32",
		"ps_convert_rgba8_float24",
		"ps_convert_rgba8_float16",
		"ps_convert_rgb5a1_float16",
		"ps_convert_rgba8_float32_biln",
		"ps_convert_rgba8_float24_biln",
		"ps_convert_rgba8_float16_biln",
		"ps_convert_rgb5a1_float16_biln",
		"ps_convert_float32_float24",
		"ps_depth_copy",
		"ps_downsample_copy",
		"ps_convert_rgba_8i",
		"ps_move_texels",
		"ps_convert_clut_4",
		"ps_convert_clut_8",
		"ps_yuv",
	};
	if (shader < 0 || shader >= (int)(sizeof(names) / sizeof(names[0])))
		return "ShaderConvertUnknownShader";
	return names[shader];
}

/* GSDevice::BlendFactor and BlendOp, and HWBlendFlags */
enum
{
	SRC_COLOR, INV_SRC_COLOR, DST_COLOR, INV_DST_COLOR,
	SRC1_COLOR, INV_SRC1_COLOR, SRC_ALPHA, INV_SRC_ALPHA,
	DST_ALPHA, INV_DST_ALPHA, SRC1_ALPHA, INV_SRC1_ALPHA,
	CONST_COLOR, INV_CONST_COLOR, CONST_ONE, CONST_ZERO
};
enum
{
	OP_ADD, OP_SUBTRACT, OP_REV_SUBTRACT
};
#define BLEND_CD 0x1
#define BLEND_HW1 0x2
#define BLEND_HW2 0x4
#define BLEND_HW3 0x8
#define BLEND_HW4 0x10
#define BLEND_HW5 0x20
#define BLEND_HW6 0x40
#define BLEND_HW7 0x80
#define BLEND_HW8 0x100
#define BLEND_HW9 0x200
#define BLEND_MIX1 0x400
#define BLEND_MIX2 0x800
#define BLEND_MIX3 0x1000
#define BLEND_ACCU 0x2000
#define BLEND_NO_REC 0x4000
#define BLEND_A_MAX 0x8000

const gs_hw_blend gs_hw_blend_map[81] = {
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 0000: (Cs - Cs)*As + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 0001: (Cs - Cs)*As + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 0002: (Cs - Cs)*As +  0 ==> 0 */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 0010: (Cs - Cs)*Ad + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 0011: (Cs - Cs)*Ad + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 0012: (Cs - Cs)*Ad +  0 ==> 0 */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 0020: (Cs - Cs)*F  + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 0021: (Cs - Cs)*F  + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 0022: (Cs - Cs)*F  +  0 ==> 0 */
	{ BLEND_A_MAX | BLEND_MIX2 , OP_SUBTRACT     , CONST_ONE       , SRC1_COLOR}      , /* 0100: (Cs - Cd)*As + Cs ==> Cs*(As + 1) - Cd*As */
	{ BLEND_MIX1               , OP_ADD          , SRC1_COLOR      , INV_SRC1_COLOR}  , /* 0101: (Cs - Cd)*As + Cd ==> Cs*As + Cd*(1 - As) */
	{ BLEND_MIX1               , OP_SUBTRACT     , SRC1_COLOR      , SRC1_COLOR}      , /* 0102: (Cs - Cd)*As +  0 ==> Cs*As - Cd*As */
	{ BLEND_A_MAX              , OP_SUBTRACT     , CONST_ONE       , DST_ALPHA}       , /* 0110: (Cs - Cd)*Ad + Cs ==> Cs*(Ad + 1) - Cd*Ad */
	{ 0                        , OP_ADD          , DST_ALPHA       , INV_DST_ALPHA}   , /* 0111: (Cs - Cd)*Ad + Cd ==> Cs*Ad + Cd*(1 - Ad) */
	{ BLEND_HW5                , OP_SUBTRACT     , DST_ALPHA       , DST_ALPHA}       , /* 0112: (Cs - Cd)*Ad +  0 ==> Cs*Ad - Cd*Ad */
	{ BLEND_A_MAX | BLEND_MIX2 , OP_SUBTRACT     , CONST_ONE       , CONST_COLOR}     , /* 0120: (Cs - Cd)*F  + Cs ==> Cs*(F + 1) - Cd*F */
	{ BLEND_MIX1               , OP_ADD          , CONST_COLOR     , INV_CONST_COLOR} , /* 0121: (Cs - Cd)*F  + Cd ==> Cs*F + Cd*(1 - F) */
	{ BLEND_MIX1               , OP_SUBTRACT     , CONST_COLOR     , CONST_COLOR}     , /* 0122: (Cs - Cd)*F  +  0 ==> Cs*F - Cd*F */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 0200: (Cs -  0)*As + Cs ==> Cs*(As + 1) */
	{ BLEND_ACCU               , OP_ADD          , SRC1_COLOR      , CONST_ONE}       , /* 0201: (Cs -  0)*As + Cd ==> Cs*As + Cd */
	{ BLEND_NO_REC             , OP_ADD          , SRC1_COLOR      , CONST_ZERO}      , /* 0202: (Cs -  0)*As +  0 ==> Cs*As */
	{ BLEND_A_MAX | BLEND_HW8  , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 0210: (Cs -  0)*Ad + Cs ==> Cs*(Ad + 1) */
	{ BLEND_HW3                , OP_ADD          , DST_ALPHA       , CONST_ONE}       , /* 0211: (Cs -  0)*Ad + Cd ==> Cs*Ad + Cd */
	{ BLEND_HW3                , OP_ADD          , DST_ALPHA       , CONST_ZERO}      , /* 0212: (Cs -  0)*Ad +  0 ==> Cs*Ad */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 0220: (Cs -  0)*F  + Cs ==> Cs*(F + 1) */
	{ BLEND_ACCU               , OP_ADD          , CONST_COLOR     , CONST_ONE}       , /* 0221: (Cs -  0)*F  + Cd ==> Cs*F + Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_COLOR     , CONST_ZERO}      , /* 0222: (Cs -  0)*F  +  0 ==> Cs*F */
	{ BLEND_MIX3               , OP_ADD          , INV_SRC1_COLOR  , SRC1_COLOR}      , /* 1000: (Cd - Cs)*As + Cs ==> Cd*As + Cs*(1 - As) */
	{ BLEND_A_MAX | BLEND_MIX1 , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ONE}       , /* 1001: (Cd - Cs)*As + Cd ==> Cd*(As + 1) - Cs*As */
	{ BLEND_MIX1               , OP_REV_SUBTRACT , SRC1_COLOR      , SRC1_COLOR}      , /* 1002: (Cd - Cs)*As +  0 ==> Cd*As - Cs*As */
	{ 0                        , OP_ADD          , INV_DST_ALPHA   , DST_ALPHA}       , /* 1010: (Cd - Cs)*Ad + Cs ==> Cd*Ad + Cs*(1 - Ad) */
	{ BLEND_A_MAX              , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ONE}       , /* 1011: (Cd - Cs)*Ad + Cd ==> Cd*(Ad + 1) - Cs*Ad */
	{ BLEND_HW5                , OP_REV_SUBTRACT , DST_ALPHA       , DST_ALPHA}       , /* 1012: (Cd - Cs)*Ad +  0 ==> Cd*Ad - Cs*Ad */
	{ BLEND_MIX3               , OP_ADD          , INV_CONST_COLOR , CONST_COLOR}     , /* 1020: (Cd - Cs)*F  + Cs ==> Cd*F + Cs*(1 - F) */
	{ BLEND_A_MAX | BLEND_MIX1 , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ONE}       , /* 1021: (Cd - Cs)*F  + Cd ==> Cd*(F + 1) - Cs*F */
	{ BLEND_MIX1               , OP_REV_SUBTRACT , CONST_COLOR     , CONST_COLOR}     , /* 1022: (Cd - Cs)*F  +  0 ==> Cd*F - Cs*F */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 1100: (Cd - Cd)*As + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 1101: (Cd - Cd)*As + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 1102: (Cd - Cd)*As +  0 ==> 0 */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 1110: (Cd - Cd)*Ad + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 1111: (Cd - Cd)*Ad + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 1112: (Cd - Cd)*Ad +  0 ==> 0 */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 1120: (Cd - Cd)*F  + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 1121: (Cd - Cd)*F  + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 1122: (Cd - Cd)*F  +  0 ==> 0 */
	{ BLEND_HW4                , OP_ADD          , CONST_ONE       , SRC1_COLOR}      , /* 1200: (Cd -  0)*As + Cs ==> Cs + Cd*As */
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , SRC1_COLOR}      , /* 1201: (Cd -  0)*As + Cd ==> Cd*(1 + As) */
	{ BLEND_HW2                , OP_ADD          , DST_COLOR       , SRC1_COLOR}      , /* 1202: (Cd -  0)*As +  0 ==> Cd*As */
	{ BLEND_HW6                , OP_ADD          , CONST_ONE       , DST_ALPHA}       , /* 1210: (Cd -  0)*Ad + Cs ==> Cs + Cd*Ad */
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , DST_ALPHA}       , /* 1211: (Cd -  0)*Ad + Cd ==> Cd*(1 + Ad) */
	{ BLEND_HW5                , OP_ADD          , CONST_ZERO      , DST_ALPHA}       , /* 1212: (Cd -  0)*Ad +  0 ==> Cd*Ad */
	{ BLEND_HW4                , OP_ADD          , CONST_ONE       , CONST_COLOR}     , /* 1220: (Cd -  0)*F  + Cs ==> Cs + Cd*F */
	{ BLEND_HW1                , OP_ADD          , DST_COLOR       , CONST_COLOR}     , /* 1221: (Cd -  0)*F  + Cd ==> Cd*(1 + F) */
	{ BLEND_HW2                , OP_ADD          , DST_COLOR       , CONST_COLOR}     , /* 1222: (Cd -  0)*F  +  0 ==> Cd*F */
	{ BLEND_NO_REC             , OP_ADD          , INV_SRC1_COLOR  , CONST_ZERO}      , /* 2000: (0  - Cs)*As + Cs ==> Cs*(1 - As) */
	{ BLEND_ACCU               , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ONE}       , /* 2001: (0  - Cs)*As + Cd ==> Cd - Cs*As */
	{ BLEND_NO_REC             , OP_REV_SUBTRACT , SRC1_COLOR      , CONST_ZERO}      , /* 2002: (0  - Cs)*As +  0 ==> 0 - Cs*As */
	{ BLEND_HW9                , OP_ADD          , INV_DST_ALPHA   , CONST_ZERO}      , /* 2010: (0  - Cs)*Ad + Cs ==> Cs*(1 - Ad) */
	{ BLEND_HW3                , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ONE}       , /* 2011: (0  - Cs)*Ad + Cd ==> Cd - Cs*Ad */
	{ 0                        , OP_REV_SUBTRACT , DST_ALPHA       , CONST_ZERO}      , /* 2012: (0  - Cs)*Ad +  0 ==> 0 - Cs*Ad */
	{ BLEND_NO_REC             , OP_ADD          , INV_CONST_COLOR , CONST_ZERO}      , /* 2020: (0  - Cs)*F  + Cs ==> Cs*(1 - F) */
	{ BLEND_ACCU               , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ONE}       , /* 2021: (0  - Cs)*F  + Cd ==> Cd - Cs*F */
	{ BLEND_NO_REC             , OP_REV_SUBTRACT , CONST_COLOR     , CONST_ZERO}      , /* 2022: (0  - Cs)*F  +  0 ==> 0 - Cs*F */
	{ BLEND_HW4                , OP_SUBTRACT     , CONST_ONE       , SRC1_COLOR}      , /* 2100: (0  - Cd)*As + Cs ==> Cs - Cd*As */
	{ 0                        , OP_ADD          , CONST_ZERO      , INV_SRC1_COLOR}  , /* 2101: (0  - Cd)*As + Cd ==> Cd*(1 - As) */
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , SRC1_COLOR}      , /* 2102: (0  - Cd)*As +  0 ==> 0 - Cd*As */
	{ BLEND_HW6                , OP_SUBTRACT     , CONST_ONE       , DST_ALPHA}       , /* 2110: (0  - Cd)*Ad + Cs ==> Cs - Cd*Ad */
	{ BLEND_HW7                , OP_ADD          , CONST_ZERO      , INV_DST_ALPHA}   , /* 2111: (0  - Cd)*Ad + Cd ==> Cd*(1 - Ad) */
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , DST_ALPHA}       , /* 2112: (0  - Cd)*Ad +  0 ==> 0 - Cd*Ad */
	{ BLEND_HW4                , OP_SUBTRACT     , CONST_ONE       , CONST_COLOR}     , /* 2120: (0  - Cd)*F  + Cs ==> Cs - Cd*F */
	{ 0                        , OP_ADD          , CONST_ZERO      , INV_CONST_COLOR} , /* 2121: (0  - Cd)*F  + Cd ==> Cd*(1 - F) */
	{ 0                        , OP_SUBTRACT     , CONST_ZERO      , CONST_COLOR}     , /* 2122: (0  - Cd)*F  +  0 ==> 0 - Cd*F */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 2200: (0  -  0)*As + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 2201: (0  -  0)*As + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 2202: (0  -  0)*As +  0 ==> 0 */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 2210: (0  -  0)*Ad + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 2211: (0  -  0)*Ad + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 2212: (0  -  0)*Ad +  0 ==> 0 */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ONE       , CONST_ZERO}      , /* 2220: (0  -  0)*F  + Cs ==> Cs */
	{ BLEND_CD                 , OP_ADD          , CONST_ZERO      , CONST_ONE}       , /* 2221: (0  -  0)*F  + Cd ==> Cd */
	{ BLEND_NO_REC             , OP_ADD          , CONST_ZERO      , CONST_ZERO}      , /* 2222: (0  -  0)*F  +  0 ==> 0 */
};
