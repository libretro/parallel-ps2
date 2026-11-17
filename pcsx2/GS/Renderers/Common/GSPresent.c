/* SPDX-License-Identifier: LGPL-3.0+ */

#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "common/Pcsx2Defs.h"
#include "GSPresent.h"
#include "GSDeviceBase.h"
#include "../../GSBackend.h"
#include "../../GSVR.h"
#include "../../../PerformanceMetrics.h"
#include "../../../Profiler.h"

static void gs_rectf_set(gs_rectf* r, float x, float y, float z, float w)
{
	r->x = x;
	r->y = y;
	r->z = z;
	r->w = w;
}

static bool gs_rectf_eq(const gs_rectf* a, const gs_rectf* b)
{
	return a->x == b->x && a->y == b->y && a->z == b->z && a->w == b->w;
}

static bool gs_recti_eq(const gs_recti* a, const gs_recti* b)
{
	return a->x == b->x && a->y == b->y && a->z == b->z && a->w == b->w;
}

static bool gs_recti_contains(const gs_recti* a, const gs_recti* b)
{
	/* (GSVector4i::rcontains: a clamped into b is b) */
	gs_recti c = *a;
	gs_recti_clamp_into(&c, b);
	return gs_recti_eq(&c, b);
}

void gs_present_init(gs_present* p, void* gs, const gs_present_ops* ops, gs_pcrtc* pcrtc, u8* scanmask_used,
	int* draw_n, const int* transfer_n)
{
	memset(p, 0, sizeof(*p));
	p->gs            = gs;
	p->ops           = ops;
	p->pcrtc         = pcrtc;
	p->scanmask_used = scanmask_used;
	p->draw_n        = draw_n;
	p->transfer_n    = transfer_n;
}

bool gs_present_idle_frame(const gs_present* p)
{
	return p->last_draw_n == *p->draw_n && p->last_transfer_n == *p->transfer_n;
}

void gs_present_purge_pool(void)
{
	/* (it crashes D3D11 and OpenGL) */
	const int api = g_gs_device_c->m_ops->get_render_api(g_gs_device_c);
	if (api == GS_RENDER_API_D3D11 || api == GS_RENDER_API_OPENGL)
		return;
	gs_device_purge_pool(g_gs_device_c);
}

bool gs_present_merge(gs_present* p, int field)
{
	gs_device* dev            = g_gs_device_c;
	gs_pcrtc* pc              = p->pcrtc;
	gs_pcrtc_display* disp    = pc->PCRTCDisplays;
	const GSPrivRegSet* regs  = p->regs;
	const bool feedback_merge = regs->EXTWRITE.WRITE == 1;
	const int scanmask_used   = *p->scanmask_used;
	gs_texture* tex[3];
	float tex_scale[3];
	int y_offset[3];
	gs_rectf src_gs_read[2];
	gs_rectf dst[3];
	bool game_deinterlacing, scanmask_frame, is_bob;
	int field2, mode, i;
	gs_point resolution, fs;
	float mult;
	u32 c;

	tex[0] = tex[1] = tex[2] = NULL;
	tex_scale[0] = tex_scale[1] = tex_scale[2] = 0.0f;
	y_offset[0] = y_offset[1] = y_offset[2] = 0;
	/* (rects of the circuits not drawn: zero, not whatever was there) */
	memset(src_gs_read, 0, sizeof(src_gs_read));
	memset(dst, 0, sizeof(dst));

	gs_pcrtc_set_video_mode(pc, gs_regs_video_mode(regs));
	gs_pcrtc_enable_displays(pc, regs->PMODE, regs->SMODE2, gs_regs_really_interlaced(regs));
	gs_pcrtc_check_same_source(pc);

	if (!disp[0].enabled && !disp[1].enabled)
		return false;

	/* Before anti-blur can change the offsets: one circuit's read moved
	 * and not the other's is the game deinterlacing itself */
	game_deinterlacing = (regs->DISP[0].DISPFB.DBY != disp[0].prevFramebufferReg.DBY) !=
		(regs->DISP[1].DISPFB.DBY != disp[1].prevFramebufferReg.DBY);

	gs_pcrtc_set_rects(pc, 0, regs->DISP[0].DISPLAY, regs->DISP[0].DISPFB);
	gs_pcrtc_set_rects(pc, 1, regs->DISP[1].DISPLAY, regs->DISP[1].DISPFB);
	gs_pcrtc_calculate_display_offset(pc, scanmask_used != 0);
	gs_pcrtc_calculate_framebuffer_offset(pc, scanmask_used != 0);

	/* Off with FFMD half frames: both circuits read the one field, not
	 * past it where the DBY offsets stack the fields */
	if (pc->FFMD && pc->interlaced && GSConfig.InterlaceMode == GS_INTERLACE_OFF && disp[0].enabled && disp[1].enabled)
	{
		disp[1].framebufferRect    = disp[0].framebufferRect;
		disp[1].framebufferOffsets = disp[0].framebufferOffsets;
	}

	/* Software checks the right and bottom only: hardware takes the
	 * whole texture and cuts out of it later */
	if (pc->PCRTCSameSrc && !gs_pcrtc_frame_wrap(pc) && !feedback_merge)
	{
		if (p->ops->get_output)
			tex[0] = p->ops->get_output(p->gs, -1, &tex_scale[0], &y_offset[0]);
		tex[1]       = tex[0];
		y_offset[1]  = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		/* RC1 is read when it blends with non-zero alpha, gives the alpha,
		 * or feeds the feedback write */
		const bool use_rc1 = disp[0].enabled &&
			(!(regs->PMODE.MMOD == 1 && regs->PMODE.ALP == 0) || regs->PMODE.AMOD == 0 ||
				(feedback_merge && regs->EXTBUF.FBIN == 0));
		/* RC2 is not when RC1 covers it at full alpha */
		const bool rc1_overwrites_rc2 = use_rc1 && gs_recti_contains(&disp[0].displayRect, &disp[1].displayRect) &&
			regs->PMODE.MMOD == 1 && regs->PMODE.ALP == 255;
		const bool use_rc2 = disp[1].enabled &&
			((regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || regs->PMODE.AMOD == 1 ||
				(feedback_merge && regs->EXTBUF.FBIN == 1));

		if (use_rc1 && p->ops->get_output)
			tex[0] = p->ops->get_output(p->gs, 0, &tex_scale[0], &y_offset[0]);
		if (use_rc2 && p->ops->get_output)
			tex[1] = p->ops->get_output(p->gs, 1, &tex_scale[1], &y_offset[1]);
		if (feedback_merge && p->ops->get_feedback_output)
			tex[2] = p->ops->get_feedback_output(p->gs, &tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
		return false;

	(*p->draw_n)++;

	/* Bob always offsets; FFMD adds its own later */
	scanmask_frame = scanmask_used && abs(disp[0].displayRect.y - disp[1].displayRect.y) != 1;
	field2         = 0;
	mode           = 3; /* a game deinterlacing itself is bobbed */
	is_bob         = GSConfig.InterlaceMode == GS_INTERLACE_BOB_TFF || GSConfig.InterlaceMode == GS_INTERLACE_BOB_BFF;

	/* FFMD half frames need the blend, and so does SCANMSK not blended
	 * in the merge (Alpine Racer 3) */
	if (GSConfig.InterlaceMode != GS_INTERLACE_AUTOMATIC || (!regs->SMODE2.FFMD && !scanmask_frame))
	{
		/* A game offsetting each frame itself, on full height buffers,
		 * is bobbed */
		if (game_deinterlacing && !scanmask_frame && GSConfig.InterlaceMode == GS_INTERLACE_AUTOMATIC)
		{
			mode   = 1;
			is_bob = true;
		}
		else
		{
			field2 = (((int)GSConfig.InterlaceMode - 2) & 1);
			mode   = (((int)GSConfig.InterlaceMode - 2) >> 1);
		}
	}

	for (i = 0; i < 2; i++)
	{
		const gs_pcrtc_display* cc = &disp[i];
		const float s              = tex_scale[i];
		float tw, th;
		float interlace_offset = 0.0f;

		if (!cc->enabled || !tex[i])
			continue;

		/* where it goes on the screen */
		gs_rectf_set(&dst[i], s * (float)cc->displayRect.x, s * (float)cc->displayRect.y,
			s * (float)cc->displayRect.z, s * (float)cc->displayRect.w);

		/* what it reads of local memory, in the texture */
		tw = (float)tex[i]->m_size.x;
		th = (float)tex[i]->m_size.y;
		gs_rectf_set(&src_gs_read[i],
			(((float)cc->framebufferRect.x + 0.0f) * s) / tw,
			(((float)cc->framebufferRect.y + (float)y_offset[i]) * s) / th,
			(((float)cc->framebufferRect.z + 0.0f) * s) / tw,
			(((float)cc->framebufferRect.w + (float)y_offset[i]) * s) / th);

		/* (a half-height field is not offset here for the weave: the
		 * interlace takes each line's rows from the field that drew it) */
		if (scanmask_used)
		{
			int off = disp[i].displayRect.y - disp[1 - i].displayRect.y;

			if (off > 0)
			{
				off &= 1;
				dst[i].y -= (float)off * s;
				dst[i].w -= (float)off * s;
				interlace_offset += (float)off;
			}
		}

		dst[i].x += 0.0f;
		dst[i].y += interlace_offset;
		dst[i].z += 0.0f;
		dst[i].w += interlace_offset;
	}

	if (feedback_merge && tex[2])
	{
		/* FBIN is two bits for two displays: the spare values would read
		 * past DISP */
		const GSRegDISPLAY* fd = &regs->DISP[regs->EXTBUF.FBIN & 1].DISPLAY;
		/* A downsample of at least one, whatever the magnification */
		int down_h = (int)(regs->EXTDATA.SMPH - fd->MAGH) + 1;
		int down_v = (int)(regs->EXTDATA.SMPV - fd->MAGV) + 1;
		int left, right, top, bottom;
		const float s = tex_scale[2];

		if (down_h < 1)
			down_h = 1;
		if (down_v < 1)
			down_v = 1;
		left   = regs->EXTBUF.WDX;
		right  = left + ((regs->EXTDATA.WW + 1) / down_h);
		top    = regs->EXTBUF.WDY;
		bottom = ((regs->EXTDATA.WH + 1) * (2 - regs->EXTBUF.WFFMD)) / down_v;

		gs_rectf_set(&dst[2], s * 0.0f, s * 0.0f, s * (float)(right - left), s * (float)(bottom - top));
	}

	/* The VR eyes take their depth from one circuit: the second where it
	 * shows, the one a game displays its frame through */
	{
		const int vc = (disp[1].enabled && tex[1]) ? 1 : (disp[0].enabled && tex[0]) ? 0 : -1;
		p->vr_merge_tex = NULL;
		if (vc >= 0)
		{
			p->vr_merge_tex = tex[vc];
			p->vr_merge_src = src_gs_read[vc];
			p->vr_merge_dst = dst[vc];
		}
	}

	resolution = gs_pcrtc_resolution(pc);
	mult       = p->ops->get_upscale_multiplier ? p->ops->get_upscale_multiplier(p->gs) : 1.0f;
	fs.x       = (int)((float)resolution.x * mult);
	fs.y       = (int)((float)resolution.y * mult);

	p->real_size = fs;

	/* The two outputs the same: the alpha blended one is not drawn */
	if (tex[0] == tex[1] && gs_rectf_eq(&src_gs_read[0], &src_gs_read[1]) && gs_rectf_eq(&dst[0], &dst[1]) &&
		gs_recti_eq(&disp[0].displayRect, &disp[1].displayRect) &&
		gs_recti_eq(&disp[0].framebufferRect, &disp[1].framebufferRect) && !feedback_merge && !regs->PMODE.SLBG)
		tex[0] = NULL;

	c = (regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | ((u32)regs->PMODE.ALP << 24);
	gs_device_merge(dev, tex, src_gs_read, dst, fs.x, fs.y, &regs->PMODE, &regs->EXTBUF, c);

	if (gs_regs_really_interlaced(regs) && GSConfig.InterlaceMode != GS_INTERLACE_OFF)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		gs_device_interlace(dev, fs.x, fs.y, field ^ field2, mode, offset, (float)fs.y / (float)resolution.y,
			regs->SMODE2.FFMD != 0);
	}

	if (scanmask_used)
		(*p->scanmask_used)--;

	return true;
}

static bool gs_present_begin(bool frame_skip)
{
	const int res = g_gs_device_c->m_ops->begin_present(g_gs_device_c, frame_skip);
	if (res == GS_PRESENT_FRAME_SKIPPED)
		return false;
	if (res == GS_PRESENT_OK)
		return true;

	/* Device lost: everything tossed out, to hobble on; the first frame
	 * after a reopen is trash, so it is skipped */
	GSreopen(true, false, &GSConfig);
	return false;
}

/* --- VR (GSVR.h) ---------------------------------------------------------
 * The frontend says each frame whether it shows a left/right pair; the
 * eyes are made only when it does, and the frame as it is otherwise,
 * with any map sent before taken back. */

/* The window the eyes look through, in units of its width: at a
 * distance of one width, the eyes a twenty-fifth of it apart. On a window
 * a metre and a half wide that is six centimetres, about a person's. */
#define VR_STEREO_DISTANCE   1.0f
#define VR_STEREO_SEPARATION 0.04f
/* At the default depth the nearest of the frame is a little in front of
 * the window and the farthest at three and a third times its distance;
 * at twice the depth the farthest is as good as infinity. */
#define VR_NEAR_PER_DEPTH    0.12f
#define VR_FAR_PER_DEPTH     0.7f
#define VR_FAR_LEAST         0.02f
/* The most march steps a pixel takes */
#define VR_MAX_STEPS         64.0f
/* The widest and tallest an eye is drawn */
#define VR_MAX_EYE           4096

static void gs_present_vr_clear_views(gs_present* p)
{
	struct retro_video_views map;

	if (!p->vr_map_set)
		return;

	map.views                   = NULL;
	map.num_views               = 0;
	map.flags                   = 0;
	map.reference_space         = RETRO_VR_REFERENCE_SPACE_LOCAL;
	map.recommended_view_width  = 0;
	map.recommended_view_height = 0;
	environ_cb(RETRO_ENVIRONMENT_SET_VIDEO_VIEWS, &map);
	p->vr_map_set = false;
}

static gs_texture* gs_present_vr_frame(gs_present* p, gs_texture* current, bool blank_frame)
{
	gs_device* dev = g_gs_device_c;
	struct retro_video_view views[2];
	struct retro_video_views map;
	gs_vr_frame frame;
	gs_texture* out;
	unsigned status        = 0;
	const unsigned stereo  = RETRO_VIDEO_VIEWS_STATUS_PRESENTS | RETRO_VIDEO_VIEWS_STATUS_STEREO;
	int w, h, eye, max_w, max_h;
	float aspect, w_near, w_far, sh;

	if (gs_vr_mode == GSVR_OFF || !dev->m_vr_ops)
	{
		p->vr_active = false;
		gs_present_vr_clear_views(p);
		return current;
	}

	if (!current)
	{
		gs_present_vr_clear_views(p);
		return current;
	}

	if (!environ_cb(RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS, &status))
		status = 0;

	p->vr_active = (status & stereo) == stereo;
	if (!p->vr_active)
	{
		gs_present_vr_clear_views(p);
		return current;
	}

	/* A blank frame is black in both eyes, at the size the map describes,
	 * so the frontend keeps the layout; the renderer goes on keeping
	 * track of depth for the frames either side */
	if (blank_frame)
	{
		if (!p->vr_map_set)
			return current;
		out = gs_device_vr_blank(dev, p->vr_eye_size.x, p->vr_eye_size.y);
		if (!out)
		{
			gs_present_vr_clear_views(p);
			return current;
		}
		return out;
	}

	w      = current->m_size.x;
	h      = current->m_size.y;
	aspect = (gs_vr_aspect > 0.0f) ? gs_vr_aspect : (float)w / (float)h;

	/* The depth under the picture: the part of the depth buffer under
	 * the part of the circuit's texture the merge read, where it put it.
	 * The buffer's texels are the frame buffer's, both at the target
	 * scale. */
	memset(&frame, 0, sizeof(frame));
	if (p->vr_merge_tex && p->ops->get_vr_depth && p->real_size.x > 0 && p->real_size.y > 0 &&
		p->ops->get_vr_depth(p->gs, p->vr_merge_tex, &frame.depth, &frame.depth_encoded, &frame.z_scale))
	{
		const float tw = (float)p->vr_merge_tex->m_size.x;
		const float th = (float)p->vr_merge_tex->m_size.y;
		const float dw = (float)frame.depth->m_size.x;
		const float dh = (float)frame.depth->m_size.y;
		const float px = (float)w / p->real_size.x;
		const float py = (float)h / p->real_size.y;
		gs_rectf_set(&frame.depth_rect, (p->vr_merge_src.x * tw) / dw, (p->vr_merge_src.y * th) / dh,
			(p->vr_merge_src.z * tw) / dw, (p->vr_merge_src.w * th) / dh);
		gs_rectf_set(&frame.picture_rect, p->vr_merge_dst.x * px, p->vr_merge_dst.y * py,
			p->vr_merge_dst.z * px, p->vr_merge_dst.w * py);
	}
	else
		frame.depth = NULL;

	w_near = 1.0f + VR_NEAR_PER_DEPTH * gs_vr_depth;
	w_far  = 1.0f - VR_FAR_PER_DEPTH * gs_vr_depth;
	if (w_far < VR_FAR_LEAST)
		w_far = VR_FAR_LEAST;

	/* Each eye sees exactly the window, one unit wide, off centre, so the
	 * window itself is where the eyes agree */
	sh = 1.0f / aspect;
	for (eye = 0; eye < 2; eye++)
	{
		const float x = (eye == 0 ? -0.5f : 0.5f) * VR_STEREO_SEPARATION;
		gs_rectf_set(&frame.eyes[eye].eye_pos, x, 0.0f, 0.0f, 0.0f);
		gs_rectf_set(&frame.eyes[eye].eye_quat, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_rectf_set(&frame.eyes[eye].fov_tan, (0.5f + x) / VR_STEREO_DISTANCE, (0.5f - x) / VR_STEREO_DISTANCE,
			0.5f * sh / VR_STEREO_DISTANCE, 0.5f * sh / VR_STEREO_DISTANCE);
		gs_rectf_set(&frame.eyes[eye].screen, 1.0f, sh, VR_STEREO_DISTANCE, VR_MAX_STEPS);
		gs_rectf_set(&frame.eyes[eye].depth, w_near, w_far, frame.z_scale, 0.0f);
	}

	/* Both eyes side by side in one frame. OpenGL draws it into the
	 * frontend's framebuffer, the size of the largest frame the geometry
	 * allows. */
	frame.eye_size.x = w;
	frame.eye_size.y = h;
	max_w            = 2 * VR_MAX_EYE;
	max_h            = VR_MAX_EYE;
	if (dev->m_ops->get_render_api(dev) == GS_RENDER_API_OPENGL)
	{
		max_w = (int)dev->m_window_info.surface_width;
		max_h = (int)dev->m_window_info.surface_height;
	}
	if (2 * frame.eye_size.x > max_w || frame.eye_size.y > max_h)
	{
		const float fx = (float)max_w / (2.0f * frame.eye_size.x);
		const float fy = (float)max_h / (float)frame.eye_size.y;
		const float fit = (fx < fy) ? fx : fy;
		frame.eye_size.x = (int)(frame.eye_size.x * fit);
		frame.eye_size.y = (int)(frame.eye_size.y * fit);
		if (frame.eye_size.x < 1)
			frame.eye_size.x = 1;
		if (frame.eye_size.y < 1)
			frame.eye_size.y = 1;
	}

	out = gs_device_vr_compose(dev, current, &frame);
	if (!out)
	{
		gs_present_vr_clear_views(p);
		return current;
	}

	/* Each eye is the picture, so it has the picture's aspect */
	for (eye = 0; eye < 2; eye++)
	{
		views[eye].x            = (unsigned)(eye * frame.eye_size.x);
		views[eye].y            = 0;
		views[eye].width        = (unsigned)frame.eye_size.x;
		views[eye].height       = (unsigned)frame.eye_size.y;
		views[eye].screen       = 0;
		views[eye].eye          = (eye == 0) ? RETRO_VIDEO_VIEW_EYE_LEFT : RETRO_VIDEO_VIEW_EYE_RIGHT;
		views[eye].aspect_ratio = aspect;
	}
	map.views                   = views;
	map.num_views               = 2;
	map.flags                   = 0;
	map.reference_space         = RETRO_VR_REFERENCE_SPACE_LOCAL;
	map.recommended_view_width  = 0;
	map.recommended_view_height = 0;

	if (!environ_cb(RETRO_ENVIRONMENT_SET_VIDEO_VIEWS, &map))
	{
		/* Refused, and the previous map stands: taken back too */
		gs_present_vr_clear_views(p);
		return current;
	}

	if (!p->vr_map_set)
		log_cb(RETRO_LOG_INFO, "VR: stereoscopic, %dx%d per eye.\n", frame.eye_size.x, frame.eye_size.y);
	p->vr_map_set  = true;
	p->vr_eye_size = frame.eye_size;

	return out;
}

void gs_present_vsync(gs_present* p, u32 field, bool registers_written, bool idle_frame)
{
	gs_device* dev             = g_gs_device_c;
	const int fb_sprite_blits  = m_disp_fb_sprite_blits;
	const bool fb_sprite_frame = fb_sprite_blits > 0;
	bool skip_frame            = false;
	bool blank_frame;

	m_disp_fb_sprite_blits = 0;

	if (GSConfig.SkipDuplicateFrames)
	{
		bool unique_frame;

		switch (perf_metrics_internal_fps_method())
		{
			case PERF_FPS_GS_PRIVILEGED_REGISTER:
				unique_frame = registers_written;
				break;
			case PERF_FPS_DISPFB_BLIT:
				unique_frame = fb_sprite_frame;
				break;
			default:
				unique_frame = true;
				break;
		}

		if (!unique_frame && p->skipped_duplicate_frames < GS_MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			p->skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
			p->skipped_duplicate_frames = 0;
	}

	blank_frame = !gs_present_merge(p, (int)field);

	p->last_draw_n     = *p->draw_n;
	p->last_transfer_n = *p->transfer_n;

	if (skip_frame)
	{
		if (gs_present_begin(true))
			dev->m_ops->end_present(dev);
	}
	else
	{
		gs_texture* current;

		if (!idle_frame)
			gs_device_age_pool(dev);

		/* Before the present begins: the VR eyes are drawn into targets
		 * of their own, and OpenGL presents into whatever is bound then */
		current = gs_present_vr_frame(p, dev->m_current, blank_frame);

		if (gs_present_begin(false))
		{
			if (current)
			{
				gs_rectf src_uv, draw_rect;
				if (blank_frame)
					gs_rectf_set(&src_uv, 0.0f, 0.0f, 0.0f, 0.0f);
				else
					gs_rectf_set(&src_uv, 0.0f, 0.0f, 1.0f, 1.0f);
				gs_rectf_set(&draw_rect, 0.0f, 0.0f, (float)current->m_size.x, (float)current->m_size.y);
				dev->m_ops->present_rect(dev, current, &src_uv, NULL, &draw_rect);
			}
			else
			{
				const int prof = PROFILE_ZONE_BEGIN(ZONE_GS_HANDOVER);
				video_cb(NULL, 0, 0, 0);
				PROFILE_ZONE_END(ZONE_GS_HANDOVER, prof);
			}

			dev->m_ops->end_present(dev);
		}
	}

	/* Only the duplicate skip reads it: not called, nor the clock read
	 * in it, with that off */
	if (GSConfig.SkipDuplicateFrames)
		perf_metrics_update(registers_written, fb_sprite_frame);
}
