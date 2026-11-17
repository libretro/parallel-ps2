/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_PCRTC_H
#define GS_PCRTC_H

/* The two read circuits of the PCRTC: what each reads from local memory
 * and where it lands on the screen, from the privileged registers and the
 * PCRTC options. GSState keeps one (C++: GSPCRTCRegs, its view) and fills
 * it at each vsync for the merge. */

#include "common/Pcsx2Defs.h"
#include "GSRegs.h"
#include "gs_rect.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GSVideoMode's values (GS.h) */
#define GS_VMODE_UNKNOWN 0
#define GS_VMODE_NTSC 1
#define GS_VMODE_PAL 2
#define GS_VMODE_VESA 3
#define GS_VMODE_SDTV_480P 4
#define GS_VMODE_HDTV_720P 5
#define GS_VMODE_HDTV_1080I 6

typedef struct gs_pcrtc_display
{
	bool enabled;
	int FBP;
	int FBW;
	int PSM;
	GSRegDISPFB prevFramebufferReg;
	gs_point prevDisplayOffset;
	gs_point displayOffset;
	gs_recti displayRect;
	gs_point magnification;
	gs_point prevFramebufferOffsets;
	gs_point framebufferOffsets;
	gs_recti framebufferRect;
} gs_pcrtc_display;

typedef struct gs_pcrtc
{
	/* One less than the video mode, so that the six named modes index the
	 * six-row tables; Unknown is -1 and reads as NTSC's row */
	int videomode;
	int interlaced;
	int FFMD;
	bool PCRTCSameSrc;
	bool toggling_field;
	gs_pcrtc_display PCRTCDisplays[2];
} gs_pcrtc;

/* The video mode the registers say (GS_VMODE_*), whether they interlace,
 * whether the field really toggles, and the refresh rate of the mode (0
 * where it is not known) */
int gs_regs_video_mode(const GSPrivRegSet* regs);
bool gs_regs_interlaced(const GSPrivRegSet* regs);
bool gs_regs_really_interlaced(const GSPrivRegSet* regs);
float gs_regs_tv_refresh_rate(const GSPrivRegSet* regs);

/* The circuits set from the registers, as a vsync starts */
void gs_pcrtc_reset(gs_pcrtc* p, const GSPrivRegSet* regs);

bool gs_pcrtc_is_analogue(const gs_pcrtc* p);
/* Which display is nearest the zero offset, horizontally (x) and
 * vertically (y) */
gs_point gs_pcrtc_nearest_to_zero_offset(const gs_pcrtc* p);
void gs_pcrtc_set_video_mode(gs_pcrtc* p, int mode);
void gs_pcrtc_enable_displays(gs_pcrtc* p, GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle);
void gs_pcrtc_check_same_source(gs_pcrtc* p);
/* The two reads together cross the 2048 edge of local memory */
bool gs_pcrtc_frame_wrap(const gs_pcrtc* p);
gs_point gs_pcrtc_resolution(const gs_pcrtc* p);
/* A display's read, or both together for -1 */
gs_recti gs_pcrtc_framebuffer_rect(const gs_pcrtc* p, int display);
int gs_pcrtc_framebuffer_bit_depth(const gs_pcrtc* p);
gs_point gs_pcrtc_framebuffer_size(const gs_pcrtc* p, int display);
/* A display's read and screen rects from its registers */
void gs_pcrtc_set_rects(gs_pcrtc* p, int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg);
/* The read offsets put in the reads; a one line difference between the
 * two taken out first when the anti-blur option is on */
void gs_pcrtc_calculate_framebuffer_offset(gs_pcrtc* p, bool scanmask);
/* The offset left of a read, or of both for -1: hardware only unwraps,
 * software keeps the part within the block it reads from */
void gs_pcrtc_remove_framebuffer_offset(gs_pcrtc* p, int display);
/* The displays placed on the screen from their offsets */
void gs_pcrtc_calculate_display_offset(gs_pcrtc* p, bool scanmask);

#ifdef __cplusplus
}
#endif

#endif
