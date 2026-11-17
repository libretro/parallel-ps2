/* SPDX-License-Identifier: LGPL-3.0+ */

#include <stdlib.h>

#include <retro_atomic.h>

#include "common/Pcsx2Defs.h"
#include "GSPCRTC.h"
#include "GSBackend.h"
#include "GSPsm.h"
#include "GSUtil.h"

/* The horizontal offsets (z) for PAL and NTSC are tweaked: 632 and 652
 * leave a thick black line on the left, these a small one on the right,
 * so some horizontal overscan is expected either way. Width, height,
 * horizontal and vertical offset a mode. */
static const gs_recti gs_vmode_offsets[6] = {
	{ 640, 224, 642, 25 },
	{ 640, 256, 676, 36 },
	{ 640, 480, 276, 34 },
	{ 720, 480, 232, 35 },
	{ 1280, 720, 302, 24 },
	{ 1920, 540, 238, 40 }
};

static const gs_recti gs_vmode_offsets_overscan[6] = {
	{ 711, 240, 498, 17 },
	{ 711, 288, 532, 21 },
	{ 640, 480, 276, 34 },
	{ 720, 480, 232, 35 },
	{ 1280, 720, 302, 24 },
	{ 1920, 540, 238, 40 }
};

static const gs_recti gs_vmode_dividers[6] = {
	{ 3, 0, 2559, 239 },
	{ 3, 0, 2559, 287 },
	{ 1, 0, 1279, 479 },
	{ 1, 0, 1439, 479 },
	{ 0, 0, 1279, 719 },
	{ 0, 0, 1919, 1079 }
};

/* The privileged registers are written by the EE with whole 64-bit
 * stores (gsWrite64_generic) while vsync reads fields of them: one atomic
 * read, then the fields off the copy, so that the value is one that was
 * written rather than halves of two. Pairs with that release store. */
static u64 gs_reg_read(const u64* reg)
{
#if defined(RETRO_ATOMIC_HAS_64)
	return (u64)retro_atomic_load_acquire_64((const retro_atomic_64_t*)reg);
#else
	return *reg;
#endif
}

bool gs_regs_interlaced(const GSPrivRegSet* regs)
{
	GSRegSMODE2 smode2;
	smode2.U64 = gs_reg_read(&regs->SMODE2.U64);
	return !!smode2.INT;
}

bool gs_regs_really_interlaced(const GSPrivRegSet* regs)
{
	/* FIELD only flips with SMODE1.CMOD set and the bottom bit of the
	 * front porch (SYNCV.VFP) set */
	GSRegSYNCV syncv;
	GSRegSMODE1 smode1;
	syncv.U64  = gs_reg_read(&regs->SYNCV.U64);
	smode1.U64 = gs_reg_read(&regs->SMODE1.U64);
	return (syncv.VFP & 0x1) && smode1.CMOD;
}

int gs_regs_video_mode(const GSPrivRegSet* regs)
{
	/* Only what the CRTC registers tell apart: colour burst (CMOD) and
	 * the PLL divider (LC) */
	GSRegSMODE1 smode1;
	smode1.U64 = gs_reg_read(&regs->SMODE1.U64);

	switch (smode1.CMOD)
	{
		case 0:
			if (gs_regs_interlaced(regs) && smode1.LC == 22)
				return GS_VMODE_HDTV_1080I;
			if (!gs_regs_interlaced(regs) && smode1.LC == 22)
				return GS_VMODE_HDTV_720P;
			/* (576P reads as 480P) */
			if (!gs_regs_interlaced(regs) && smode1.LC == 32)
				return GS_VMODE_SDTV_480P;
			return GS_VMODE_VESA;
		case 2:
			return GS_VMODE_NTSC;
		case 3:
			return GS_VMODE_PAL;
		default:
			break;
	}
	return GS_VMODE_UNKNOWN;
}

float gs_regs_tv_refresh_rate(const GSPrivRegSet* regs)
{
	switch (gs_regs_video_mode(regs))
	{
		case GS_VMODE_NTSC:
		case GS_VMODE_SDTV_480P:
			return 60 / 1.001f;
		case GS_VMODE_PAL:
			return 50;
		case GS_VMODE_HDTV_720P:
		case GS_VMODE_HDTV_1080I:
			return 60;
		default:
			break;
	}
	return 0;
}

/* The row of the tables for the mode: Unknown (-1) reads as NTSC */
static int gs_pcrtc_row(const gs_pcrtc* p)
{
	return (p->videomode < 0) ? 0 : p->videomode;
}

static const gs_recti* gs_pcrtc_offsets(const gs_pcrtc* p)
{
	return GSConfig.PCRTCOverscan ? &gs_vmode_offsets_overscan[gs_pcrtc_row(p)] : &gs_vmode_offsets[gs_pcrtc_row(p)];
}

static gs_recti gs_rect_union(const gs_recti* a, const gs_recti* b)
{
	gs_recti r;
	r.x = (a->x < b->x) ? a->x : b->x;
	r.y = (a->y < b->y) ? a->y : b->y;
	r.z = (a->z > b->z) ? a->z : b->z;
	r.w = (a->w > b->w) ? a->w : b->w;
	return r;
}

/* r with its sides moved out to multiples of the block size */
static gs_recti gs_rect_align_outside(const gs_recti* r, gs_int2 bs)
{
	gs_recti a;
	a.x = r->x & ~(bs.x - 1);
	a.y = r->y & ~(bs.y - 1);
	a.z = (r->z + bs.x - 1) & ~(bs.x - 1);
	a.w = (r->w + bs.y - 1) & ~(bs.y - 1);
	return a;
}

void gs_pcrtc_reset(gs_pcrtc* p, const GSPrivRegSet* regs)
{
	gs_pcrtc_set_video_mode(p, gs_regs_video_mode(regs));
	gs_pcrtc_enable_displays(p, regs->PMODE, regs->SMODE2, gs_regs_really_interlaced(regs));
	gs_pcrtc_set_rects(p, 0, regs->DISP[0].DISPLAY, regs->DISP[0].DISPFB);
	gs_pcrtc_set_rects(p, 1, regs->DISP[1].DISPLAY, regs->DISP[1].DISPFB);
}

bool gs_pcrtc_is_analogue(const gs_pcrtc* p)
{
	const int mode = p->videomode + 1;
	return mode == GS_VMODE_NTSC || mode == GS_VMODE_PAL || mode == GS_VMODE_HDTV_1080I;
}

gs_point gs_pcrtc_nearest_to_zero_offset(const gs_pcrtc* p)
{
	const gs_pcrtc_display* d = p->PCRTCDisplays;
	const gs_recti* off = &gs_vmode_offsets[gs_pcrtc_row(p)];
	gs_point r;
	int vertical;
	int i;

	r.x = 1;
	r.y = 1;
	if (!d[0].enabled && !d[1].enabled)
		return r;

	for (i = 0; i < 2; i++)
	{
		if (!d[i].enabled)
		{
			r.x = 1 - i;
			r.y = 1 - i;
			return r;
		}
	}

	if (abs(d[0].displayOffset.x - off->z) < abs(d[1].displayOffset.x - off->z))
		r.x = 0;

	/* interlaced, the vertical base offset doubles */
	vertical = off->w * (1 << p->interlaced);
	if (abs(d[0].displayOffset.y - vertical) < abs(d[1].displayOffset.y - vertical))
		r.y = 0;

	return r;
}

void gs_pcrtc_set_video_mode(gs_pcrtc* p, int mode)
{
	p->videomode = mode - 1;
}

void gs_pcrtc_enable_displays(gs_pcrtc* p, GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle)
{
	p->PCRTCDisplays[0].enabled = pmode.EN1;
	p->PCRTCDisplays[1].enabled = pmode.EN2;

	p->interlaced     = smode2.INT && gs_pcrtc_is_analogue(p);
	p->FFMD           = smode2.FFMD;
	p->toggling_field = smodetoggle && gs_pcrtc_is_analogue(p);
}

void gs_pcrtc_check_same_source(gs_pcrtc* p)
{
	const gs_pcrtc_display* d = p->PCRTCDisplays;

	if (d[0].enabled != d[1].enabled || !(d[0].enabled | d[1].enabled))
	{
		p->PCRTCSameSrc = false;
		return;
	}

	p->PCRTCSameSrc = d[0].FBP == d[1].FBP && d[0].FBW == d[1].FBW &&
		gs_util_has_compatible_bits(d[0].PSM, d[1].PSM);
}

bool gs_pcrtc_frame_wrap(const gs_pcrtc* p)
{
	const gs_recti r = gs_rect_union(&p->PCRTCDisplays[0].framebufferRect, &p->PCRTCDisplays[1].framebufferRect);
	return r.w >= 2048 || r.z >= 2048;
}

gs_point gs_pcrtc_resolution(const gs_pcrtc* p)
{
	const gs_pcrtc_display* d = p->PCRTCDisplays;
	const gs_recti* off = gs_pcrtc_offsets(p);
	const bool full_height = p->interlaced || (p->toggling_field && GSConfig.InterlaceMode != GS_INTERLACE_OFF);
	const int max_y = full_height ? off->y << 1 : off->y;
	gs_point r;

	if (!GSConfig.PCRTCOffsets)
	{
		gs_recti s;
		if (d[0].enabled && d[1].enabled)
			s = gs_rect_union(&d[0].displayRect, &d[1].displayRect);
		else if (d[0].enabled)
			s = d[0].displayRect;
		else
			s = d[1].displayRect;
		r.x = s.z - s.x;
		r.y = s.w - s.y;
	}
	else
	{
		r.x = off->x;
		r.y = off->y << (full_height ? 1 : 0);
	}

	if (r.x > off->x)
		r.x = off->x;
	if (r.y > max_y)
		r.y = max_y;
	return r;
}

gs_recti gs_pcrtc_framebuffer_rect(const gs_pcrtc* p, int display)
{
	if (display == -1)
		return gs_rect_union(&p->PCRTCDisplays[0].framebufferRect, &p->PCRTCDisplays[1].framebufferRect);
	return p->PCRTCDisplays[display].framebufferRect;
}

int gs_pcrtc_framebuffer_bit_depth(const gs_pcrtc* p)
{
	if (p->PCRTCDisplays[0].enabled)
		return gs_psm[p->PCRTCDisplays[0].PSM].bpp;
	if (p->PCRTCDisplays[1].enabled)
		return gs_psm[p->PCRTCDisplays[1].PSM].bpp;
	return 32;
}

gs_point gs_pcrtc_framebuffer_size(const gs_pcrtc* p, int display)
{
	const gs_pcrtc_display* d = p->PCRTCDisplays;
	const bool hw = emu_gs_hardware_renderer(&GSConfig);
	int max_height = gs_pcrtc_offsets(p)->y;
	gs_point size;
	gs_recti r;
	int min_mag;
	int offset;

	if (!(p->FFMD && p->interlaced))
		max_height *= 2;

	if (display == -1)
	{
		gs_recti disp;

		r = gs_rect_union(&d[0].framebufferRect, &d[1].framebufferRect);

		if (r.z >= 2048)
		{
			const int high_x = (d[0].framebufferRect.x > d[1].framebufferRect.x) ? d[0].framebufferRect.x : d[1].framebufferRect.x;
			r.z -= hw ? 2048 : high_x;
			r.x = 0;
		}

		if (r.w >= 2048)
		{
			const int high_y = (d[0].framebufferRect.y > d[1].framebufferRect.y) ? d[0].framebufferRect.y : d[1].framebufferRect.y;
			r.w -= hw ? 2048 : high_y;
			r.y = 0;
		}

		/* The read capped to the tallest display, or the hardware
		 * renderer gets messy */
		min_mag = (d[0].magnification.y < d[1].magnification.y) ? d[0].magnification.y : d[1].magnification.y;
		if (min_mag < 1)
			min_mag = 1;
		disp   = gs_rect_union(&d[0].displayRect, &d[1].displayRect);
		offset = disp.y;

		if (p->FFMD && p->interlaced)
			offset = (offset - 1) / 2;

		/* Hardware cannot offset the read, so it reads wider */
		if (hw)
		{
			r.z += (d[0].framebufferOffsets.x > d[1].framebufferOffsets.x) ? d[0].framebufferOffsets.x : d[1].framebufferOffsets.x;
			r.w += (d[0].framebufferOffsets.y > d[1].framebufferOffsets.y) ? d[0].framebufferOffsets.y : d[1].framebufferOffsets.y;
		}

		max_height += r.y;

		offset = (max_height / min_mag) - offset;
		if (r.w > offset)
			r.w = offset;

		/* Off, not interlaced: the read covers the whole display height */
		if (GSConfig.InterlaceMode == GS_INTERLACE_OFF && !p->interlaced)
		{
			const int h = (d[0].displayRect.w > d[1].displayRect.w) ? d[0].displayRect.w : d[1].displayRect.w;
			if (r.w < h)
				r.w = h;
		}
	}
	else
	{
		r = d[display].framebufferRect;

		if (r.z >= 2048)
			r.z -= r.x;

		if (r.w >= 2048)
			r.w -= r.y;

		min_mag = d[display].magnification.y;
		if (min_mag < 1)
			min_mag = 1;
		offset = d[display].displayRect.y;

		if (p->FFMD && p->interlaced)
			offset = (offset - 1) / 2;

		max_height += r.y;

		offset = (max_height / min_mag) - offset;
		if (r.w > offset)
			r.w = offset;

		if (GSConfig.InterlaceMode == GS_INTERLACE_OFF && r.w < d[display].displayRect.w)
			r.w = d[display].displayRect.w;
	}

	size.x = r.z;
	size.y = r.w;
	return size;
}

void gs_pcrtc_set_rects(gs_pcrtc* p, int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg)
{
	gs_pcrtc_display* d = &p->PCRTCDisplays[display];
	const gs_recti* div = &gs_vmode_dividers[gs_pcrtc_row(p)];
	u32 DW, DH;
	int renderWidth, renderHeight;
	u32 finalDisplayWidth, finalDisplayHeight;
	bool interlaced_resolution;

	d->FBP                = framebufferReg.FBP;
	d->FBW                = framebufferReg.FBW;
	d->PSM                = framebufferReg.PSM;
	d->prevFramebufferReg = framebufferReg;

	/* Probably not really enabled, and it would make a mess: Q-Ball
	 * Billiards enables both circuits but sets up only one */
	if (d->FBW == 0 && displayReg.DW == 0 && displayReg.DH == 0 && displayReg.MAGH == 0)
	{
		d->enabled = false;
		return;
	}

	d->magnification.x = displayReg.MAGH + 1;
	d->magnification.y = displayReg.MAGV + 1;
	DW = displayReg.DW + 1;
	DH = displayReg.DH + 1;

	renderWidth  = DW / d->magnification.x;
	renderHeight = DH / d->magnification.y;

	/* With screen offsets the picture is fitted to the screen size */
	if (GSConfig.PCRTCOffsets)
	{
		finalDisplayWidth  = DW / (div->x + 1);
		finalDisplayHeight = DH / (div->y + 1);
	}
	else
	{
		finalDisplayWidth  = renderWidth;
		finalDisplayHeight = renderHeight;
		if (finalDisplayWidth > DW / (div->x + 1))
			finalDisplayWidth = DW / (div->x + 1);
		if (finalDisplayHeight > DH / (div->y + 1))
			finalDisplayHeight = DH / (div->y + 1);
	}

	d->prevFramebufferOffsets = d->framebufferOffsets;
	d->framebufferRect.x      = 0;
	d->framebufferRect.y      = 0;
	d->framebufferRect.z      = renderWidth;

	/* Half height read with FFMD and INT, rounded up as if odd */
	if (p->FFMD && p->interlaced)
		d->framebufferRect.w = (renderHeight + 1) >> (p->FFMD * p->interlaced);
	else
		d->framebufferRect.w = renderHeight;
	d->framebufferOffsets.x = framebufferReg.DBX;
	d->framebufferOffsets.y = framebufferReg.DBY;

	/* Interlacing without the flag: the height is likely reported wrong
	 * (Q-Ball Billiards) */
	interlaced_resolution = p->interlaced || (p->toggling_field && GSConfig.InterlaceMode != GS_INTERLACE_OFF);
	if (interlaced_resolution && !p->interlaced)
		finalDisplayHeight *= 2;

	d->displayRect.x       = 0;
	d->displayRect.y       = 0;
	d->displayRect.z       = (int)finalDisplayWidth;
	d->displayRect.w       = (int)finalDisplayHeight;
	d->prevDisplayOffset   = d->displayOffset;
	d->displayOffset.x     = displayReg.DX;
	d->displayOffset.y     = displayReg.DY;
}

/* An offset past the 2048 edge, as the distance back to it */
static int gs_pcrtc_unwrap(int v, int size)
{
	return (v + size > 2048) ? abs(v - 2048) : v;
}

void gs_pcrtc_calculate_framebuffer_offset(gs_pcrtc* p, bool scanmask)
{
	gs_pcrtc_display* d = p->PCRTCDisplays;
	int i;

	if (GSConfig.PCRTCAntiBlur && p->PCRTCSameSrc && !scanmask)
	{
		gs_point fb0, fb1;

		fb0.x = gs_pcrtc_unwrap(d[0].framebufferOffsets.x, d[0].displayRect.z);
		fb0.y = gs_pcrtc_unwrap(d[0].framebufferOffsets.y, d[0].displayRect.w);
		fb1.x = gs_pcrtc_unwrap(d[1].framebufferOffsets.x, d[1].displayRect.z);
		fb1.y = gs_pcrtc_unwrap(d[1].framebufferOffsets.y, d[1].displayRect.w);

		if (abs(fb1.y - fb0.y) == 1 && d[0].displayRect.y == d[1].displayRect.y)
		{
			if (fb1.y < fb0.y)
				d[0].framebufferOffsets.y = fb1.y;
			else
				d[1].framebufferOffsets.y = fb0.y;
		}
		if (abs(fb1.x - fb0.x) == 1 && d[0].displayRect.x == d[1].displayRect.x)
		{
			if (fb1.x < fb0.x)
				d[0].framebufferOffsets.x = fb1.x;
			else
				d[1].framebufferOffsets.x = fb0.x;
		}
	}

	for (i = 0; i < 2; i++)
	{
		d[i].framebufferRect.x += d[i].framebufferOffsets.x;
		d[i].framebufferRect.z += d[i].framebufferOffsets.x;
		d[i].framebufferRect.y += d[i].framebufferOffsets.y;
		d[i].framebufferRect.w += d[i].framebufferOffsets.y;
	}
}

static void gs_rect_sub_xy(gs_recti* r, int x, int y)
{
	r->x -= x;
	r->y -= y;
	r->z -= x;
	r->w -= y;
}

void gs_pcrtc_remove_framebuffer_offset(gs_pcrtc* p, int display)
{
	gs_pcrtc_display* d = p->PCRTCDisplays;
	const bool hw = emu_gs_hardware_renderer(&GSConfig);

	if (display >= 0)
	{
		gs_pcrtc_display* e = &d[display];

		/* Hardware only handles a wrapped read */
		if (hw)
		{
			if (e->framebufferRect.z >= 2048)
			{
				e->displayRect.x += 2048 - e->framebufferRect.x;
				e->displayRect.z += 2048 - e->framebufferRect.x;
				e->framebufferRect.x = 0;
				e->framebufferRect.z -= 2048;
			}
			if (e->framebufferRect.w >= 2048)
			{
				e->displayRect.y += 2048 - e->framebufferRect.y;
				e->displayRect.w += 2048 - e->framebufferRect.y;
				e->framebufferRect.y = 0;
				e->framebufferRect.w -= 2048;
			}
		}
		else
		{
			const gs_recti r = gs_rect_align_outside(&e->framebufferRect, gs_psm[e->PSM].bs);
			gs_rect_sub_xy(&e->framebufferRect, r.x, r.y);
		}
	}
	else if (!hw)
	{
		/* Software reads block aligned and leaves the rest of the offset
		 * within the block; hardware reads the whole framebuffer and
		 * keeps the offset */
		const gs_recti u = gs_rect_union(&d[0].framebufferRect, &d[1].framebufferRect);
		const gs_recti r = gs_rect_align_outside(&u, gs_psm[d[1].PSM].bs);
		gs_rect_sub_xy(&d[0].framebufferRect, r.x, r.y);
		gs_rect_sub_xy(&d[1].framebufferRect, r.x, r.y);
	}
}

/* With both displays on and anti-blur, a vertical offset under four lines
 * between them is taken off the lower one */
static void gs_pcrtc_deblur_offsets(gs_pcrtc* p, bool scanmask)
{
	gs_pcrtc_display* d = p->PCRTCDisplays;
	const int blur      = abs(d[1].displayOffset.y - d[0].displayOffset.y);

	if (GSConfig.PCRTCAntiBlur && !scanmask && blur < 4)
	{
		if (d[1].displayOffset.y > d[0].displayOffset.y)
			d[1].displayOffset.y -= blur;
		else
			d[0].displayOffset.y -= blur;
	}
}

static void gs_rect_add_x(gs_recti* r, int x)
{
	r->x += x;
	r->z += x;
}

static void gs_rect_add_y(gs_recti* r, int y)
{
	r->y += y;
	r->w += y;
}

void gs_pcrtc_calculate_display_offset(gs_pcrtc* p, bool scanmask)
{
	gs_pcrtc_display* d     = p->PCRTCDisplays;
	const bool both_enabled = d[0].enabled && d[1].enabled;
	const gs_recti* off     = gs_pcrtc_offsets(p);
	const gs_recti* div     = &gs_vmode_dividers[gs_pcrtc_row(p)];
	const gs_point zero     = gs_pcrtc_nearest_to_zero_offset(p);
	int i;

	if (both_enabled)
		gs_pcrtc_deblur_offsets(p, scanmask);

	/* Offsets mostly ignored: the displays straight from the reads */
	if (!GSConfig.PCRTCOffsets)
	{
		int int_off[2];
		gs_point base;
		int voff;

		int_off[0] = 0;
		int_off[1] = 0;

		/* A single line offset kept, or it throws interlacing out */
		for (i = 0; i < 2; i++)
		{
			int o;

			if (!d[i].enabled)
				continue;

			o = (d[i].displayOffset.y - (off->w * (p->interlaced + 1))) / (div->y + 1);
			if (o > 4)
				continue;

			int_off[i] = o & 1;
			if (o < 0)
				int_off[i] = -int_off[i];

			gs_rect_add_y(&d[i].displayRect, int_off[i]);
		}

		/* The two displays' offset from each other (DmC, Time Crisis 2
		 * split screen) */
		if (both_enabled)
		{
			const int ox = 1 - zero.x;
			const int oy = 1 - zero.y;
			const int dx = (d[ox].displayOffset.x - d[zero.x].displayOffset.x) / (div->x + 1);
			const int dy = (d[oy].displayOffset.y - d[zero.y].displayOffset.y) / (div->y + 1);

			if (dx >= 4 || !GSConfig.PCRTCAntiBlur || scanmask)
				gs_rect_add_x(&d[ox].displayRect, dx);
			if (dy >= 4 || !GSConfig.PCRTCAntiBlur || scanmask)
				gs_rect_add_y(&d[oy].displayRect, dy - int_off[oy]);
		}

		/* A large vertical offset from the screen's zero (Hokuto no Ken
		 * brings the screen up by about 14), its lowest bit already
		 * taken */
		base = d[zero.y].displayOffset;
		voff = (base.y - (off->w * (p->interlaced + 1))) / (div->y + 1);

		if (voff <= 4 && voff != 0)
		{
			gs_rect_add_y(&d[0].displayRect, voff - int_off[0]);
			gs_rect_add_y(&d[1].displayRect, voff - int_off[1]);
		}
	}
	else /* screen offsets: the whole offset */
	{
		for (i = 0; i < 2; i++)
		{
			gs_rect_add_x(&d[i].displayRect, (d[i].displayOffset.x - off->z) / (div->x + 1));
			gs_rect_add_y(&d[i].displayRect, (d[i].displayOffset.y - (off->w * (p->interlaced + 1))) / (div->y + 1));
		}

		if (both_enabled)
		{
			const int ox = 1 - zero.x;
			const int oy = 1 - zero.y;
			const int dx = d[ox].displayRect.x - d[zero.x].displayRect.x;
			const int dy = d[oy].displayRect.y - d[zero.y].displayRect.y;

			if (dx > 0 && dx < 4 && GSConfig.PCRTCAntiBlur)
				gs_rect_add_x(&d[ox].displayRect, -dx);
			if (dy > 0 && dy < 4 && GSConfig.PCRTCAntiBlur)
				gs_rect_add_y(&d[oy].displayRect, -dy);
		}
	}
}
