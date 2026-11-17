/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSMERGE_REFERENCE_H
#define GSMERGE_REFERENCE_H

#include <stdlib.h>

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/GSPCRTC.h"
#include "GS/GSVR.h"
#include "GS/GSBackend.h"
#include "PerformanceMetrics.h"

/* What the renderer answers, the same for the reference and the C
 * (driver.cpp) */
gs_texture* mock_get_output(int i, float* scale, int* y_offset);
gs_texture* mock_get_feedback_output(float* scale);
bool mock_get_vr_depth(gs_texture* color, gs_texture** depth, bool* encoded, float* z_scale);
float mock_upscale(void);

/* GSState's members the vsync used */
struct RefPresent
{
	/* GSState::GSPCRTCRegs, as it is now (GSPCRTC.c, held to the C++
	 * apart by tests/gspcrtc) */
	struct GSPCRTCRegs : gs_pcrtc
	{
		typedef gs_pcrtc_display PCRTCDisplay;
		void SetVideoMode(GSVideoMode mode) { gs_pcrtc_set_video_mode(this, static_cast<int>(mode)); }
		void EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool t) { gs_pcrtc_enable_displays(this, pmode, smode2, t); }
		void CheckSameSource() { gs_pcrtc_check_same_source(this); }
		bool FrameWrap() const { return gs_pcrtc_frame_wrap(this); }
		bool FrameRectMatch() const { return PCRTCSameSrc; }
		GSVector2i GetResolution() const { const gs_point r = gs_pcrtc_resolution(this); return GSVector2i(r.x, r.y); }
		void SetRects(int d, GSRegDISPLAY a, GSRegDISPFB b) { gs_pcrtc_set_rects(this, d, a, b); }
		void CalculateFramebufferOffset(bool s) { gs_pcrtc_calculate_framebuffer_offset(this, s); }
		void CalculateDisplayOffset(bool s) { gs_pcrtc_calculate_display_offset(this, s); }
		GSVector4i DisplayRect(int i) const { return GS_V4I(&PCRTCDisplays[i].displayRect); }
		GSVector4i FramebufferRect(int i) const { return GS_V4I(&PCRTCDisplays[i].framebufferRect); }
	};
	struct Ops
	{
		bool (*get_vr_depth)(RefPresent* gs, GSTexture* color, GSTexture** depth, bool* encoded, float* z_scale);
	};
	static constexpr u32 MAX_SKIPPED_DUPLICATE_FRAMES = 3;

	GSPCRTCRegs PCRTCDisplays;
	GSPrivRegSet* m_regs;
	u8 m_scanmask_used;
	int s_n;
	int s_transfer_n;
	const Ops* m_ops;
	GSVector2i m_real_size;
	bool m_vr_active;
	GSVector4 m_vr_merge_src;
	GSVector4 m_vr_merge_dst;
	GSTexture* m_vr_merge_tex;
	GSVector2i m_vr_eye_size;
	bool m_vr_map_set;
	u32 m_skipped_duplicate_frames;
	int m_last_draw_n;
	int m_last_transfer_n;

	GSVideoMode GetVideoMode() { return static_cast<GSVideoMode>(gs_regs_video_mode(m_regs)); }
	bool isReallyInterlaced() { return gs_regs_really_interlaced(m_regs); }
	GSTexture* GetOutput(int i, float& scale, int& y_offset) { return static_cast<GSTexture*>(mock_get_output(i, &scale, &y_offset)); }
	GSTexture* GetFeedbackOutput(float& scale) { return static_cast<GSTexture*>(mock_get_feedback_output(&scale)); }
	float GetUpscaleMultiplier() { return mock_upscale(); }

	bool Merge(int field);
	bool BeginPresentFrame(bool frame_skip);
	void VSyncBase(u32 field, bool registers_written, bool idle_frame);
	void VRClearViews();
	GSTexture* VRFrame(GSTexture* current, bool blank_frame);
	bool IsIdleFrame() const;
};

#endif
