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

#include <math.h>

#include "GSRenderer.h"
#include "../../GSVR.h"

#include "../../../Host.h"
#include "../../../PerformanceMetrics.h"
#include "../../../Config.h"
#ifdef ENABLE_PCSX2_PROFILER
#include "../../../Profiler.h"
#else
#define PROFILE_SCOPE(z) do {} while (0)
#endif

extern retro_video_refresh_t video_cb;
extern retro_environment_t environ_cb;

GSRenderer* g_gs_renderer = NULL;

void GSState::PurgePool()
{
	// FIXME: PurgePool crashes d3d11+glcore
	switch (g_gs_device->GetRenderAPI())
	{
		default:
			break;
		case RenderAPI::D3D11:
		case RenderAPI::OpenGL:
			return;
	}

	g_gs_device->PurgePool();
}

bool GSState::Merge(int field)
{
	GSVector2i fs(0, 0);
	GSTexture* tex[3] = { nullptr, nullptr, nullptr };
	float tex_scale[3] = { 0.0f, 0.0f, 0.0f };
	int y_offset[3] = { 0, 0, 0 };
	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;

	PCRTCDisplays.SetVideoMode(GetVideoMode());
	PCRTCDisplays.EnableDisplays(m_regs->PMODE, m_regs->SMODE2, isReallyInterlaced());
	PCRTCDisplays.CheckSameSource();

	if (!PCRTCDisplays.PCRTCDisplays[0].enabled && !PCRTCDisplays.PCRTCDisplays[1].enabled)
		return false;

	// Need to do this here, if the user has Anti-Blur enabled, these offsets can get wiped out/changed.
	const bool game_deinterlacing = (m_regs->DISP[0].DISPFB.DBY != PCRTCDisplays.PCRTCDisplays[0].prevFramebufferReg.DBY) !=
									(m_regs->DISP[1].DISPFB.DBY != PCRTCDisplays.PCRTCDisplays[1].prevFramebufferReg.DBY);

	PCRTCDisplays.SetRects(0, m_regs->DISP[0].DISPLAY, m_regs->DISP[0].DISPFB);
	PCRTCDisplays.SetRects(1, m_regs->DISP[1].DISPLAY, m_regs->DISP[1].DISPFB);
	PCRTCDisplays.CalculateDisplayOffset(m_scanmask_used);
	PCRTCDisplays.CalculateFramebufferOffset(m_scanmask_used);

	// For Off interlace with FFMD half-frame mode, both circuits should read from the same single
	// field to avoid reading garbage past the valid field buffer when DBY offsets stack fields.
	if (PCRTCDisplays.FFMD && PCRTCDisplays.interlaced && GSConfig.InterlaceMode == GSInterlaceMode::Off &&
		PCRTCDisplays.PCRTCDisplays[0].enabled && PCRTCDisplays.PCRTCDisplays[1].enabled)
	{
		PCRTCDisplays.PCRTCDisplays[1].framebufferRect = PCRTCDisplays.PCRTCDisplays[0].framebufferRect;
		PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets = PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets;
	}

	// Only need to check the right/bottom on software renderer, hardware always gets the full texture then cuts a bit out later.
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() && !feedback_merge)
	{
		tex[0] = GetOutput(-1, tex_scale[0], y_offset[0]);
		tex[1] = tex[0]; // saves one texture fetch
		y_offset[1] = y_offset[0];
		tex_scale[1] = tex_scale[0];
	}
	else
	{
		const bool use_rc1 =
			PCRTCDisplays.PCRTCDisplays[0].enabled &&                    // RC1 enabled.
				(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) || // Blend RC1 with non-zero alpha.
				(m_regs->PMODE.AMOD == 0) ||                             // Use alpha of RC1.
				(feedback_merge && m_regs->EXTBUF.FBIN == 0));           // Use RC1 for feedback merge.
		
		// The following two flags determine if RC1 output completely overwrites RC2 output
		// due to the alpha used for blending and the respective rectangles of the outputs.
		const bool rc1_contains_rc2 =
			PCRTCDisplays.PCRTCDisplays[0].displayRect.rcontains(PCRTCDisplays.PCRTCDisplays[1].displayRect);
		
		const bool rc1_overwrites_rc2 = use_rc1 && rc1_contains_rc2 && m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;

		const bool use_rc2 =
			PCRTCDisplays.PCRTCDisplays[1].enabled &&                // RC2 enabled.
				((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) || // Blending RC2 and not overwritten by RC1.
				(m_regs->PMODE.AMOD == 1) ||                         // Use alpha of RC2.
				(feedback_merge && m_regs->EXTBUF.FBIN == 1));       // Use RC2 for feedback merge.
		if (use_rc1)
			tex[0] = GetOutput(0, tex_scale[0], y_offset[0]);
		if (use_rc2)
			tex[1] = GetOutput(1, tex_scale[1], y_offset[1]);
		if (feedback_merge)
			tex[2] = GetFeedbackOutput(tex_scale[2]);
	}

	if (!tex[0] && !tex[1])
		return false;

	s_n++;

	GSVector4 src_gs_read[2];
	GSVector4 dst[3];

	// Use offset for bob deinterlacing always, extra offset added later for FFMD mode.
	const bool scanmask_frame = m_scanmask_used && abs(PCRTCDisplays.PCRTCDisplays[0].displayRect.y - PCRTCDisplays.PCRTCDisplays[1].displayRect.y) != 1;
	int field2 = 0;
	int mode = 3; // If the game is manually deinterlacing then we need to bob (if we want to get away with no deinterlacing).
	bool is_bob = GSConfig.InterlaceMode == GSInterlaceMode::BobTFF || GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;

	// FFMD (half frames) requires blend deinterlacing, so automatically use that. Same when SCANMSK is used but not blended in the merge circuit (Alpine Racer 3).
	if (GSConfig.InterlaceMode != GSInterlaceMode::Automatic || (!m_regs->SMODE2.FFMD && !scanmask_frame))
	{
		// If the game is offsetting each frame itself and we're using full height buffers, we can offset this with Bob.
		if (game_deinterlacing && !scanmask_frame && GSConfig.InterlaceMode == GSInterlaceMode::Automatic)
		{
			mode = 1; // Bob.
			is_bob = true;
		}
		else
		{
			field2 = ((static_cast<int>(GSConfig.InterlaceMode) - 2) & 1);
			mode = ((static_cast<int>(GSConfig.InterlaceMode) - 2) >> 1);
		}
	}

	for (int i = 0; i < 2; i++)
	{
		 const GSPCRTCRegs::PCRTCDisplay& curCircuit = PCRTCDisplays.PCRTCDisplays[i];

		if (!curCircuit.enabled || !tex[i])
			continue;

		GSVector4 scale = GSVector4(tex_scale[i]);

		// dst is the final destination rect with offset on the screen.
		dst[i] = scale * GSVector4(curCircuit.displayRect);

		// src_gs_read is the size which we're really reading from GS memory.
		src_gs_read[i] = ((GSVector4(curCircuit.framebufferRect) + GSVector4(0, y_offset[i], 0, y_offset[i])) * scale) / GSVector4(tex[i]->GetSize()).xyxy();
		
		/* A half-height field is not offset here for the weave: the
		 * interlace takes each line's rows from the field that drew it. */
		float interlace_offset = 0.0f;
		// Scanmask frame offsets. It's gross, I'm sorry but it sucks.
		if (m_scanmask_used)
		{
			int displayIntOffset = PCRTCDisplays.PCRTCDisplays[i].displayRect.y - PCRTCDisplays.PCRTCDisplays[1 - i].displayRect.y;
			
			if (displayIntOffset > 0)
			{
				displayIntOffset &= 1;
				dst[i].y -= displayIntOffset * scale.y;
				dst[i].w -= displayIntOffset * scale.y;
				interlace_offset += displayIntOffset;
			}
		}

		dst[i] += GSVector4(0.0f, interlace_offset, 0.0f, interlace_offset);
	}

	if (feedback_merge && tex[2])
	{
		GSVector4 scale = GSVector4(tex_scale[2]);
		GSVector4i feedback_rect;

		// FBIN selects which of the two displays feeds the write-back, but
		// it is a 2-bit field and DISP has two entries, so the two spare
		// values index past the end and read the registers that follow as
		// if they were a DISPLAY. GSRendererHW::GetFeedbackOutput masks it;
		// these two did not.
		const GSRegDISPLAY& feedback_disp = m_regs->DISP[m_regs->EXTBUF.FBIN & 1].DISPLAY;

		// SMPH/SMPV and MAGH/MAGV are 4- and 2-bit fields, so they promote
		// to int and the subtraction is signed: a game writing a
		// magnification above the sampling rate makes the ratio -1 and the
		// divisor zero. The ratio is a downsample factor and is never less
		// than one on hardware.
		const int downsample_h = pcsx2_max_i(1, (int)(m_regs->EXTDATA.SMPH - feedback_disp.MAGH) + 1);
		const int downsample_v = pcsx2_max_i(1, (int)(m_regs->EXTDATA.SMPV - feedback_disp.MAGV) + 1);

		feedback_rect.left = m_regs->EXTBUF.WDX;
		feedback_rect.right = feedback_rect.left + ((m_regs->EXTDATA.WW + 1) / downsample_h);
		feedback_rect.top = m_regs->EXTBUF.WDY;
		feedback_rect.bottom = ((m_regs->EXTDATA.WH + 1) * (2 - m_regs->EXTBUF.WFFMD)) / downsample_v;

		dst[2] = GSVector4(scale * GSVector4(feedback_rect.rsize()));
	}

	/* The VR eyes take their depth from one circuit, the second where
	 * it shows, which is the one a game displays its frame through. */
	{
		const int c = (PCRTCDisplays.PCRTCDisplays[1].enabled && tex[1]) ? 1
		            : (PCRTCDisplays.PCRTCDisplays[0].enabled && tex[0]) ? 0 : -1;
		m_vr_merge_tex = NULL;
		if (c >= 0)
		{
			m_vr_merge_tex = tex[c];
			m_vr_merge_src = src_gs_read[c];
			m_vr_merge_dst = dst[c];
		}
	}

	GSVector2i resolution = PCRTCDisplays.GetResolution();
	fs = GSVector2i(static_cast<int>(static_cast<float>(resolution.x) * GetUpscaleMultiplier()),
		static_cast<int>(static_cast<float>(resolution.y) * GetUpscaleMultiplier()));

	m_real_size = GSVector2i(fs.x, fs.y);

	if ((tex[0] == tex[1]) && (src_gs_read[0] == src_gs_read[1]).alltrue() && (dst[0] == dst[1]).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].displayRect == PCRTCDisplays.PCRTCDisplays[1].displayRect).alltrue() &&
		(PCRTCDisplays.PCRTCDisplays[0].framebufferRect == PCRTCDisplays.PCRTCDisplays[1].framebufferRect).alltrue() &&
		!feedback_merge && !m_regs->PMODE.SLBG)
	{
		// the two outputs are identical, skip drawing one of them (the one that is alpha blended)
		tex[0] = nullptr;
	}

	const u32 c = (m_regs->BGCOLOR.U32[0] & 0x00FFFFFFu) | (m_regs->PMODE.ALP << 24);
	g_gs_device->Merge(tex, src_gs_read, dst, fs, m_regs->PMODE, m_regs->EXTBUF, c);

	if (isReallyInterlaced() && GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const float offset = is_bob ? (tex[1] ? tex_scale[1] : tex_scale[0]) : 0.0f;

		g_gs_device->Interlace(fs, field ^ field2, mode, offset, static_cast<float>(fs.y) / static_cast<float>(resolution.y), m_regs->SMODE2.FFMD != 0);
	}

	if (m_scanmask_used)
		m_scanmask_used--;

	return true;
}

bool GSState::BeginPresentFrame(bool frame_skip)
{
	const GSDevice::PresentResult res = g_gs_device->BeginPresent(frame_skip);
	if (res == GSDevice::PresentResult::FrameSkipped)
		return false;
	else if (res == GSDevice::PresentResult::OK) /* All good */
		return true;

	// Device lost, something went really bad.
	// Let's just toss out everything, and try to hobble on.
	if (!GSreopen(true, false, GSConfig))
		return false;

	// First frame after reopening is definitely going to be trash, so skip it.
	return false;
}

void GSState::VSyncBase(u32 field, bool registers_written, bool idle_frame)
{
	bool is_unique_frame       = false;
	const int fb_sprite_blits  = m_disp_fb_sprite_blits;
	const bool fb_sprite_frame = (fb_sprite_blits > 0);
	bool skip_frame            = false;

	m_disp_fb_sprite_blits     = 0;

	if (GSConfig.SkipDuplicateFrames)
	{
		switch (perf_metrics_internal_fps_method())
		{
		case PERF_FPS_GS_PRIVILEGED_REGISTER:
			is_unique_frame = registers_written;
			break;
		case PERF_FPS_DISPFB_BLIT:
			is_unique_frame = fb_sprite_frame;
			break;
		default:
			is_unique_frame = true;
			break;
		}

		if (!is_unique_frame && m_skipped_duplicate_frames < MAX_SKIPPED_DUPLICATE_FRAMES)
		{
			m_skipped_duplicate_frames++;
			skip_frame = true;
		}
		else
			m_skipped_duplicate_frames = 0;
	}

	const bool blank_frame = !Merge(field);

	m_last_draw_n = s_n;
	m_last_transfer_n = s_transfer_n;

	if (skip_frame)
	{
		if (BeginPresentFrame(true))
			g_gs_device->EndPresent();
	}
	else
	{
		GSTexture* current;

		if (!idle_frame)
			g_gs_device->AgePool();

		/* Before the present begins: the VR eyes are drawn into targets of
		 * their own, and OpenGL presents into whatever is bound then. */
		current = VRFrame(g_gs_device->GetCurrent(), blank_frame);

		if (BeginPresentFrame(false))
		{
			if (current)
			{
				GSVector4 src_uv        = (blank_frame) ? GSVector4::zero() : GSVector4(0, 0, 1, 1);
				GSVector4 draw_rect     = GSVector4(0, 0, current->GetWidth(), current->GetHeight());

				g_gs_device->PresentRect(current, src_uv, nullptr, draw_rect);
			}
			else
			{
				PROFILE_SCOPE(ZONE_GS_HANDOVER);
				video_cb(NULL, 0, 0, 0);
			}

			g_gs_device->EndPresent();
		}
	}

	/* perf_metrics_update is only consulted by the
	 * SkipDuplicateFrames code path above; if that feature is off, its
	 * result is never read. Skip the per-frame call (and the wall-clock
	 * read inside it) when not needed. */
	if (GSConfig.SkipDuplicateFrames)
		perf_metrics_update(registers_written, fb_sprite_frame);
}

/* --- VR (GSVR.h) ---------------------------------------------------------
 * The frontend says each frame whether it shows a left/right pair; the
 * eyes are made only when it does, and the frame as it is otherwise,
 * with any map sent before taken back. */

/* The window the stereoscopic eyes look through, in units of its width:
 * at a distance of one width, the eyes a twenty-fifth of it apart. On a
 * window a metre and a half wide that is six centimetres, about a
 * person's. */
#define VR_STEREO_DISTANCE   1.0f
#define VR_STEREO_SEPARATION 0.04f
/* At the default depth the nearest of the frame is a little in front of
 * the window and the farthest at three and a third times its distance;
 * at twice the depth the farthest is as good as infinity. A comfortable
 * depth for a picture on a display. */
#define VR_NEAR_PER_DEPTH    0.12f
#define VR_FAR_PER_DEPTH     0.7f
#define VR_FAR_LEAST         0.02f
/* The most march steps a pixel takes. */
#define VR_MAX_STEPS         64.0f
/* The widest and tallest an eye is drawn. */
#define VR_MAX_EYE           4096

void GSState::VRClearViews()
{
	struct retro_video_views map;

	if (!m_vr_map_set)
		return;

	map.views                   = NULL;
	map.num_views               = 0;
	map.flags                   = 0;
	map.reference_space         = RETRO_VR_REFERENCE_SPACE_LOCAL;
	map.recommended_view_width  = 0;
	map.recommended_view_height = 0;
	environ_cb(RETRO_ENVIRONMENT_SET_VIDEO_VIEWS, &map);
	m_vr_map_set = false;
}

GSTexture* GSState::VRFrame(GSTexture* current, bool blank_frame)
{
	struct retro_video_view views[2];
	struct retro_video_views map;
	GSVRFrame frame;
	GSTexture* out;
	unsigned status = 0;
	const unsigned stereo = RETRO_VIDEO_VIEWS_STATUS_PRESENTS | RETRO_VIDEO_VIEWS_STATUS_STEREO;
	int w, h, eye, max_w, max_h;
	float aspect, w_near, w_far, sh;

	if (gs_vr_mode == GSVR_OFF || !g_gs_device->m_vr_ops)
	{
		m_vr_active = false;
		VRClearViews();
		return current;
	}

	if (!current)
	{
		VRClearViews();
		return current;
	}

	if (!environ_cb(RETRO_ENVIRONMENT_GET_VIDEO_VIEWS_STATUS, &status))
		status = 0;

	m_vr_active = (status & stereo) == stereo;
	if (!m_vr_active)
	{
		VRClearViews();
		return current;
	}

	/* A blank frame is black in both eyes, at the size the map describes,
	 * so the frontend keeps the layout; the renderer goes on keeping track
	 * of depth for the frames either side. */
	if (blank_frame)
	{
		if (!m_vr_map_set)
			return current;
		out = g_gs_device->VRBlank(m_vr_eye_size);
		if (!out)
		{
			VRClearViews();
			return current;
		}
		return out;
	}

	w      = current->GetWidth();
	h      = current->GetHeight();
	aspect = (gs_vr_aspect > 0.0f) ? gs_vr_aspect : (float)w / (float)h;

	/* The depth under the picture: the part of the depth buffer under the
	 * part of the circuit's texture Merge read, where Merge put it. The
	 * buffer's texels are the frame buffer's, both at the target scale. */
	frame.depth         = NULL;
	frame.depth_encoded = false;
	frame.z_scale       = 0.0f;
	frame.depth_rect    = GSVector4::zero();
	frame.picture_rect  = GSVector4::zero();
	if (m_vr_merge_tex && m_ops && m_ops->get_vr_depth && m_real_size.x > 0 && m_real_size.y > 0 &&
		m_ops->get_vr_depth(this, m_vr_merge_tex, &frame.depth, &frame.depth_encoded, &frame.z_scale))
	{
		const GSVector4 tex_size(GSVector4(m_vr_merge_tex->GetSize()).xyxy());
		const GSVector4 depth_size(GSVector4(frame.depth->GetSize()).xyxy());
		const GSVector4 to_picture(GSVector4((float)w / m_real_size.x, (float)h / m_real_size.y).xyxy());
		frame.depth_rect   = (m_vr_merge_src * tex_size) / depth_size;
		frame.picture_rect = m_vr_merge_dst * to_picture;
	}
	else
		frame.depth = NULL;

	w_near = 1.0f + VR_NEAR_PER_DEPTH * gs_vr_depth;
	w_far  = pcsx2_max_f(VR_FAR_LEAST, 1.0f - VR_FAR_PER_DEPTH * gs_vr_depth);

	/* Each eye sees exactly the window, one unit wide, off centre, so the
	 * window itself is where the eyes agree. */
	sh = 1.0f / aspect;
	for (eye = 0; eye < 2; eye++)
	{
		const float x = (eye == 0 ? -0.5f : 0.5f) * VR_STEREO_SEPARATION;
		frame.eyes[eye].eye_pos  = GSVector4(x, 0.0f, 0.0f, 0.0f);
		frame.eyes[eye].eye_quat = GSVector4(0.0f, 0.0f, 0.0f, 1.0f);
		frame.eyes[eye].fov_tan  = GSVector4((0.5f + x) / VR_STEREO_DISTANCE, (0.5f - x) / VR_STEREO_DISTANCE,
			0.5f * sh / VR_STEREO_DISTANCE, 0.5f * sh / VR_STEREO_DISTANCE);
		frame.eyes[eye].screen   = GSVector4(1.0f, sh, VR_STEREO_DISTANCE, VR_MAX_STEPS);
		frame.eyes[eye].depth    = GSVector4(w_near, w_far, frame.z_scale, 0.0f);
	}

	/* Both eyes side by side in one frame. OpenGL draws it into the
	 * frontend's framebuffer, which is the size of the largest frame the
	 * geometry allows. */
	frame.eye_size = GSVector2i(w, h);
	max_w          = 2 * VR_MAX_EYE;
	max_h          = VR_MAX_EYE;
	if (g_gs_device->GetRenderAPI() == RenderAPI::OpenGL)
	{
		max_w = g_gs_device->GetWindowWidth();
		max_h = g_gs_device->GetWindowHeight();
	}
	if (2 * frame.eye_size.x > max_w || frame.eye_size.y > max_h)
	{
		const float fit = pcsx2_min_f((float)max_w / (2.0f * frame.eye_size.x),
			(float)max_h / (float)frame.eye_size.y);
		frame.eye_size.x = pcsx2_max_i(1, (int)(frame.eye_size.x * fit));
		frame.eye_size.y = pcsx2_max_i(1, (int)(frame.eye_size.y * fit));
	}

	out = g_gs_device->VRCompose(current, frame);
	if (!out)
	{
		VRClearViews();
		return current;
	}

	/* Each eye is the picture, so it has the picture's aspect. */
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
		/* Refused, and the previous map stands: take it back too. */
		VRClearViews();
		return current;
	}

	if (!m_vr_map_set)
		log_cb(RETRO_LOG_INFO, "VR: stereoscopic, %dx%d per eye.\n", frame.eye_size.x, frame.eye_size.y);
	m_vr_map_set  = true;
	m_vr_eye_size = frame.eye_size;

	return out;
}

bool GSState::IsIdleFrame() const
{
	return (m_last_draw_n == s_n && m_last_transfer_n == s_transfer_n);
}
