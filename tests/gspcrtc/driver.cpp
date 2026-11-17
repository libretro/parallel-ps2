/* SPDX-License-Identifier: LGPL-3.0+ */
/* GSPCRTC.c held to the C++ it replaced (reference.cpp): the same random
 * registers, options and calls on both, every field and answer compared
 * after each call. */
#include <stdio.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GS/GS.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSPsm.h"
#include "GS/GSUtil.h"
#include "GS/GSBackend.h"
#include "GS/GSPCRTC.h"

#include "reference.h"

extern "C" {
EmuGSOptions GSConfig;
}

static u64 s_rng = 0x9e3779b97f4a7c15ull;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 7;
	s_rng ^= s_rng << 17;
	return (u32)(s_rng >> 16);
}
static u32 rn(u32 n) { return rnd() % n; }

static unsigned long s_fail;
static unsigned long s_checks;
static const char* s_step;

#define CHECK(c) do { s_checks++; if (!(c) && s_fail++ < 10) \
	printf("gspcrtc: %s: %s differs (line %d)\n", s_step, #c, __LINE__); } while (0)

static bool v4eq(const GSVector4i& a, const gs_recti& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
static bool v2eq(const GSVector2i& a, const gs_point& b) { return a.x == b.x && a.y == b.y; }

static void compare(const RefPCRTC& r, const gs_pcrtc& c)
{
	int i;
	CHECK(r.videomode == c.videomode);
	CHECK(r.interlaced == c.interlaced);
	CHECK(r.FFMD == c.FFMD);
	CHECK(r.PCRTCSameSrc == c.PCRTCSameSrc);
	CHECK(r.toggling_field == c.toggling_field);
	for (i = 0; i < 2; i++)
	{
		const RefPCRTC::PCRTCDisplay& a = r.PCRTCDisplays[i];
		const gs_pcrtc_display& b       = c.PCRTCDisplays[i];
		CHECK(a.enabled == b.enabled);
		CHECK(a.FBP == b.FBP && a.FBW == b.FBW && a.PSM == b.PSM);
		CHECK(a.prevFramebufferReg.U64 == b.prevFramebufferReg.U64);
		CHECK(v2eq(a.prevDisplayOffset, b.prevDisplayOffset));
		CHECK(v2eq(a.displayOffset, b.displayOffset));
		CHECK(v4eq(a.displayRect, b.displayRect));
		CHECK(v2eq(a.magnification, b.magnification));
		CHECK(v2eq(a.prevFramebufferOffsets, b.prevFramebufferOffsets));
		CHECK(v2eq(a.framebufferOffsets, b.framebufferOffsets));
		CHECK(v4eq(a.framebufferRect, b.framebufferRect));
	}
}

static void queries(RefPCRTC& r, const gs_pcrtc& c)
{
	int d;
	s_step = "queries";
	CHECK(r.IsAnalogue() == gs_pcrtc_is_analogue(&c));
	CHECK(v2eq(r.NearestToZeroOffset(), gs_pcrtc_nearest_to_zero_offset(&c)));
	CHECK(r.FrameWrap() == gs_pcrtc_frame_wrap(&c));
	CHECK(r.FrameRectMatch() == c.PCRTCSameSrc);
	CHECK(v2eq(r.GetResolution(), gs_pcrtc_resolution(&c)));
	CHECK(r.GetFramebufferBitDepth() == gs_pcrtc_framebuffer_bit_depth(&c));
	for (d = -1; d < 2; d++)
	{
		CHECK(v4eq(r.GetFramebufferRect(d), gs_pcrtc_framebuffer_rect(&c, d)));
		CHECK(v2eq(r.GetFramebufferSize(d), gs_pcrtc_framebuffer_size(&c, d)));
	}
}

/* A register value: often one of the values that pick a branch, else
 * any */
static u32 pick(const u32* v, int n, u32 any)
{
	return rn(3) ? v[rn(n)] : rn(any);
}

static void random_regs(GSPrivRegSet* regs)
{
	static const u32 cmod[] = { 0, 2, 3 };
	static const u32 lc[]   = { 22, 32 };
	static const u32 psm[]  = { PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMZ32, PSMT8 };
	int i;

	regs->PMODE.U64  = 0;
	regs->PMODE.EN1  = rn(4) != 0;
	regs->PMODE.EN2  = rn(3) == 0;
	regs->SMODE1.U64 = 0;
	regs->SMODE1.CMOD = pick(cmod, 3, 4);
	regs->SMODE1.LC   = pick(lc, 2, 128);
	regs->SMODE2.U64  = 0;
	regs->SMODE2.INT  = rn(2);
	regs->SMODE2.FFMD = rn(2);
	regs->SYNCV.U64   = 0;
	regs->SYNCV.VFP   = rn(4);
	for (i = 0; i < 2; i++)
	{
		GSRegDISPLAY* dp = &regs->DISP[i].DISPLAY;
		GSRegDISPFB* fb  = &regs->DISP[i].DISPFB;
		dp->U64  = 0;
		fb->U64  = 0;
		if (rn(10) == 0)
			continue; /* all zero: the circuit Q-Ball sets up not at all */
		dp->DX   = rn(4) ? 400 + rn(500) : rn(4096);
		dp->DY   = rn(4) ? 20 + rn(60) : rn(2048);
		dp->MAGH = rn(4) ? 3 : rn(16);
		dp->MAGV = rn(4) ? 0 : rn(4);
		dp->DW   = rn(4) ? 2559 : rn(4096);
		dp->DH   = rn(4) ? 223 + rn(300) : rn(2048);
		fb->FBP  = rn(4) ? 0 : rn(512);
		fb->FBW  = rn(8) ? 10 : rn(64);
		fb->PSM  = pick(psm, 6, 64);
		fb->DBX  = rn(3) ? 0 : (rn(2) ? rn(4) : rn(2048));
		fb->DBY  = rn(3) ? 0 : (rn(2) ? rn(4) : rn(2048));
		/* a width, height and magnification of zero with a buffer set */
		if (rn(12) == 0)
		{
			dp->DW   = 0;
			dp->DH   = 0;
			dp->MAGH = 0;
		}
	}
	/* The second circuit often a copy of the first, a line or two apart */
	if (rn(2))
	{
		regs->DISP[1] = regs->DISP[0];
		regs->DISP[1].DISPLAY.DY += rn(3);
		regs->DISP[1].DISPFB.DBY += rn(3);
		regs->DISP[1].DISPLAY.DX += rn(2) * 4;
	}
}

static void random_config(void)
{
	static const GSInterlaceMode im[] = { GS_INTERLACE_AUTOMATIC, GS_INTERLACE_OFF, GS_INTERLACE_WEAVE_TFF };
	GSConfig.PCRTCAntiBlur = rn(2);
	GSConfig.PCRTCOffsets  = rn(2);
	GSConfig.PCRTCOverscan = rn(2);
	GSConfig.InterlaceMode = im[rn(3)];
	GSConfig.Renderer      = rn(2) ? GS_RENDERER_SW : GS_RENDERER_VK;
}

int main(void)
{
	static GSPrivRegSet regs;
	static RefPCRTC r;
	static gs_pcrtc c;
	unsigned long it;

	gs_util_init();
	memset(&GSConfig, 0, sizeof(GSConfig));
	memset((void*)&r, 0, sizeof(r));
	memset(&c, 0, sizeof(c));

	for (it = 0; it < 200000; it++)
	{
		int n, k;

		/* now and then a new GSState: no display set up yet */
		if (rn(50) == 0)
		{
			memset((void*)&r, 0, sizeof(r));
			memset(&c, 0, sizeof(c));
		}
		if (rn(4))
			random_config();
		random_regs(&regs);

		s_step = "video mode";
		CHECK(static_cast<int>(ref_video_mode(&regs)) == gs_regs_video_mode(&regs));
		CHECK(ref_interlaced(&regs) == gs_regs_interlaced(&regs));
		CHECK(ref_really_interlaced(&regs) == gs_regs_really_interlaced(&regs));
		CHECK(ref_tv_refresh_rate(&regs) == gs_regs_tv_refresh_rate(&regs));

		/* GSState::ResetPCRTC as it was */
		r.SetVideoMode(ref_video_mode(&regs));
		r.EnableDisplays(regs.PMODE, regs.SMODE2, ref_really_interlaced(&regs));
		r.SetRects(0, regs.DISP[0].DISPLAY, regs.DISP[0].DISPFB);
		r.SetRects(1, regs.DISP[1].DISPLAY, regs.DISP[1].DISPFB);
		gs_pcrtc_reset(&c, &regs);
		s_step = "reset";
		compare(r, c);

		/* the merge's order most of the time, any order else */
		n = 2 + rn(6);
		for (k = 0; k < n; k++)
		{
			const bool scanmask = rn(4) == 0;
			const int op        = rn(4) ? k % 5 : rn(5);
			switch (op)
			{
				case 0:
					s_step = "CheckSameSource";
					r.CheckSameSource();
					gs_pcrtc_check_same_source(&c);
					break;
				case 1:
					s_step = "CalculateFramebufferOffset";
					r.CalculateFramebufferOffset(scanmask);
					gs_pcrtc_calculate_framebuffer_offset(&c, scanmask);
					break;
				case 2:
					s_step = "CalculateDisplayOffset";
					r.CalculateDisplayOffset(scanmask);
					gs_pcrtc_calculate_display_offset(&c, scanmask);
					break;
				case 3:
				{
					const int d = (int)rn(3) - 1;
					s_step = "RemoveFramebufferOffset";
					r.RemoveFramebufferOffset(d);
					gs_pcrtc_remove_framebuffer_offset(&c, d);
					break;
				}
				default:
					s_step = "options";
					if (rn(2))
						random_config();
					break;
			}
			compare(r, c);
			queries(r, c);
		}
	}

	printf("gspcrtc: %lu checks, %lu differed\n", s_checks, s_fail);
	return s_fail ? 1 : 0;
}
