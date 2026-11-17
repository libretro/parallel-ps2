/* SPDX-License-Identifier: LGPL-3.0+ */
/* The PCRTC as GSState.cpp had it before GSPCRTC.c (GSPCRTCRegs and the
 * video mode functions), kept as the reference the C is held to. Only the
 * names it reached through GSState, GSLocalMemory and GSUtil changed. */
#include <stdlib.h>

#include <retro_atomic.h>

#include "common/Pcsx2Defs.h"
#include "GS/GS.h"
#include "GS/GSVector.h"
#include "GS/GSRegs.h"
#include "GS/GSPsm.h"
#include "GS/GSUtil.h"
#include "GS/GSBackend.h"

#include "reference.h"

template <typename T>
static inline T gs_reg_read(const T& reg)
{
	T copy;
#if defined(RETRO_ATOMIC_HAS_64)
	copy.U64 = (u64)retro_atomic_load_acquire_64((const retro_atomic_64_t*)&reg.U64);
#else
	/* No 64-bit atomics on this target (old 32-bit __sync); the read is
	 * as it always was. */
	copy.U64 = reg.U64;
#endif
	return copy;
}

static bool ref_isinterlaced(const GSPrivRegSet* m_regs)
{
	return !!gs_reg_read(m_regs->SMODE2).INT;
}

static bool ref_isReallyInterlaced(const GSPrivRegSet* m_regs)
{
	// The FIELD register only flips if the CMOD field in SMODE1 is set to anything but 0 and Front Porch bottom bit in SYNCV is set.
	return (gs_reg_read(m_regs->SYNCV).VFP & 0x1) && gs_reg_read(m_regs->SMODE1).CMOD;
}

static GSVideoMode ref_GetVideoMode(const GSPrivRegSet* m_regs)
{
	// TODO: Get confirmation of videomode from SYSCALL ? not necessary but would be nice.
	// Other videomodes can't be detected on our side without the help of the data from core
	// You can only identify a limited number of video modes based on the info from CRTC registers.

	const GSRegSMODE1 smode1 = gs_reg_read(m_regs->SMODE1);
	const u8 Colorburst = smode1.CMOD; // Subcarrier frequency
	const u8 PLL_Divider = smode1.LC;  // Phased lock loop divider

	switch (Colorburst)
	{
		case 0:
			if (ref_isinterlaced(m_regs) && PLL_Divider == 22)
				return GSVideoMode::HDTV_1080I;
			else if (!ref_isinterlaced(m_regs) && PLL_Divider == 22)
				return GSVideoMode::HDTV_720P;
			else if (!ref_isinterlaced(m_regs) && PLL_Divider == 32)
				return GSVideoMode::SDTV_480P; // TODO: 576P will also be reported as 480P, find some way to differeniate.
			else
				return GSVideoMode::VESA;
		case 2:
			return GSVideoMode::NTSC;
		case 3:
			return GSVideoMode::PAL;
		default:
			break;
	}
	return GSVideoMode::Unknown;
}

static float ref_GetTvRefreshRate(const GSPrivRegSet* m_regs)
{
	const GSVideoMode videomode = ref_GetVideoMode(m_regs);

	//TODO: Check vertical frequencies for VESA video modes, old ones were untested.

	switch (videomode)
	{
		case GSVideoMode::NTSC:
		case GSVideoMode::SDTV_480P:
			return (60 / 1.001f);
		case GSVideoMode::PAL:
			return 50;
		case GSVideoMode::HDTV_720P:
		case GSVideoMode::HDTV_1080I:
			return 60;
		default:
			break;
	}

	return 0;
}


// The horizontal offset values (under z) for PAL and NTSC have been tweaked
// they should be apparently 632 and 652 respectively, but that causes a thick black line on the left
// these values leave a small black line on the right in a bunch of games, but it's not so bad.
// The only conclusion I can come to is there is horizontal overscan expected so there would normally
// be black borders either side anyway, or both sides slightly covered.
static inline constexpr GSVector4i VideoModeOffsets[6] = {
	GSVector4i::cxpr(640, 224, 642, 25),
	GSVector4i::cxpr(640, 256, 676, 36),
	GSVector4i::cxpr(640, 480, 276, 34),
	GSVector4i::cxpr(720, 480, 232, 35),
	GSVector4i::cxpr(1280, 720, 302, 24),
	GSVector4i::cxpr(1920, 540, 238, 40)
};

static inline constexpr GSVector4i VideoModeOffsetsOverscan[6] = {
	GSVector4i::cxpr(711, 240, 498, 17),
	GSVector4i::cxpr(711, 288, 532, 21),
	GSVector4i::cxpr(640, 480, 276, 34),
	GSVector4i::cxpr(720, 480, 232, 35),
	GSVector4i::cxpr(1280, 720, 302, 24),
	GSVector4i::cxpr(1920, 540, 238, 40)
};

static inline constexpr GSVector4i VideoModeDividers[6] = {
	GSVector4i::cxpr(3, 0, 2559, 239),
	GSVector4i::cxpr(3, 0, 2559, 287),
	GSVector4i::cxpr(1, 0, 1279, 479),
	GSVector4i::cxpr(1, 0, 1439, 479),
	GSVector4i::cxpr(0, 0, 1279, 719),
	GSVector4i::cxpr(0, 0, 1919, 1079)
};

bool RefPCRTC::IsAnalogue()
{
	const GSVideoMode video = static_cast<GSVideoMode>(videomode + 1);
	return video == GSVideoMode::NTSC || video == GSVideoMode::PAL || video == GSVideoMode::HDTV_1080I;
}

// Calculates which display is closest to matching zero offsets in either direction.
GSVector2i RefPCRTC::NearestToZeroOffset()
{
	GSVector2i returnValue = { 1, 1 };

	if (!PCRTCDisplays[0].enabled && !PCRTCDisplays[1].enabled)
		return returnValue;

	for (int i = 0; i < 2; i++)
	{
		if (!PCRTCDisplays[i].enabled)
		{
			returnValue.x = 1 - i;
			returnValue.y = 1 - i;
			return returnValue;
		}
	}

	if (abs(PCRTCDisplays[0].displayOffset.x - VideoModeOffsets[VideoModeRow()].z) <
		abs(PCRTCDisplays[1].displayOffset.x - VideoModeOffsets[VideoModeRow()].z))
		returnValue.x = 0;

	// When interlaced, the vertical base offset is doubled
	const int verticalOffset = VideoModeOffsets[VideoModeRow()].w * (1 << interlaced);

	if (abs(PCRTCDisplays[0].displayOffset.y - verticalOffset) <
		abs(PCRTCDisplays[1].displayOffset.y - verticalOffset))
		returnValue.y = 0;

	return returnValue;
}

void RefPCRTC::SetVideoMode(GSVideoMode videoModeIn)
{
	videomode = static_cast<int>(videoModeIn) - 1;
}

// Enable each of the displays.
void RefPCRTC::EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle)
{
	PCRTCDisplays[0].enabled = pmode.EN1;
	PCRTCDisplays[1].enabled = pmode.EN2;

	interlaced = smode2.INT && IsAnalogue();
	FFMD = smode2.FFMD;
	toggling_field = smodetoggle && IsAnalogue();
}

void RefPCRTC::CheckSameSource()
{
	if (PCRTCDisplays[0].enabled != PCRTCDisplays[1].enabled || (PCRTCDisplays[0].enabled | PCRTCDisplays[1].enabled) == false)
	{
		PCRTCSameSrc = false;
		return;
	}

	PCRTCSameSrc = PCRTCDisplays[0].FBP == PCRTCDisplays[1].FBP &&
	PCRTCDisplays[0].FBW == PCRTCDisplays[1].FBW &&
	gs_util_has_compatible_bits(PCRTCDisplays[0].PSM, PCRTCDisplays[1].PSM);
}

bool RefPCRTC::FrameWrap()
{
	const GSVector4i combined_rect = GSVector4i(PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect));
	return combined_rect.w >= 2048 || combined_rect.z >= 2048;
}

// If the start point of both frames match, we can do a single read
bool RefPCRTC::FrameRectMatch()
{
	return PCRTCSameSrc;
}

GSVector2i RefPCRTC::GetResolution()
{
	GSVector2i resolution;

	const GSVector4i offsets = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()] : VideoModeOffsetsOverscan[VideoModeRow()];
	const bool is_full_height = interlaced || (toggling_field && GSConfig.InterlaceMode != GS_INTERLACE_OFF);

	if (!GSConfig.PCRTCOffsets)
	{
		if (PCRTCDisplays[0].enabled && PCRTCDisplays[1].enabled)
		{
			const GSVector4i combined_size = PCRTCDisplays[0].displayRect.runion(PCRTCDisplays[1].displayRect);
			resolution = { combined_size.width(), combined_size.height() };
		}
		else if (PCRTCDisplays[0].enabled)
		{
			resolution = { PCRTCDisplays[0].displayRect.width(), PCRTCDisplays[0].displayRect.height() };
		}
		else
		{
			resolution = { PCRTCDisplays[1].displayRect.width(), PCRTCDisplays[1].displayRect.height() };
		}
	}
	else
	{
		const int shift = is_full_height ? 1 : 0;
		resolution = { offsets.x, offsets.y << shift };
	}

	resolution.x = pcsx2_min_f(resolution.x, offsets.x);
	resolution.y = pcsx2_min_f(resolution.y, is_full_height ? offsets.y << 1 : offsets.y);

	return resolution;
}


GSVector4i RefPCRTC::GetFramebufferRect(int display)
{
	if (display == -1)
		return GSVector4i(PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect));
	return PCRTCDisplays[display].framebufferRect;
}

int RefPCRTC::GetFramebufferBitDepth()
{
	if (PCRTCDisplays[0].enabled)
		return gs_psm[PCRTCDisplays[0].PSM].bpp;
	else if (PCRTCDisplays[1].enabled)
		return gs_psm[PCRTCDisplays[1].PSM].bpp;

	return 32;
}

GSVector2i RefPCRTC::GetFramebufferSize(int display)
{
	int max_height = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()].y : VideoModeOffsetsOverscan[VideoModeRow()].y;

	if (!(FFMD && interlaced))
		max_height *= 2;

	if (display == -1)
	{
		GSVector4i combined_rect = PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect);

		if (combined_rect.z >= 2048)
		{
			const int high_x = (PCRTCDisplays[0].framebufferRect.x > PCRTCDisplays[1].framebufferRect.x) ? PCRTCDisplays[0].framebufferRect.x : PCRTCDisplays[1].framebufferRect.x;
			combined_rect.z -= emu_gs_hardware_renderer(&GSConfig) ? 2048 : high_x;
			combined_rect.x = 0;
		}

		if (combined_rect.w >= 2048)
		{
			const int high_y = (PCRTCDisplays[0].framebufferRect.y > PCRTCDisplays[1].framebufferRect.y) ? PCRTCDisplays[0].framebufferRect.y : PCRTCDisplays[1].framebufferRect.y;
			combined_rect.w -= emu_gs_hardware_renderer(&GSConfig) ? 2048 : high_y;
			combined_rect.y = 0;
		}

		// Cap the framebuffer read to the maximum display height, otherwise the hardware renderer gets messy.
		const int min_mag = pcsx2_max_f(1, pcsx2_min_f(PCRTCDisplays[0].magnification.y, PCRTCDisplays[1].magnification.y));
		int offset = PCRTCDisplays[0].displayRect.runion(PCRTCDisplays[1].displayRect).y;

		if (FFMD && interlaced)
		{
			offset = (offset - 1) / 2;
		}

		// Hardware mode needs a wider framebuffer as it can't offset the read.
		if (emu_gs_hardware_renderer(&GSConfig))
		{
			combined_rect.z += pcsx2_max_f(PCRTCDisplays[0].framebufferOffsets.x, PCRTCDisplays[1].framebufferOffsets.x);
			combined_rect.w += pcsx2_max_f(PCRTCDisplays[0].framebufferOffsets.y, PCRTCDisplays[1].framebufferOffsets.y);
		}

		max_height += combined_rect.y;

		offset = (max_height / min_mag) - offset;
		combined_rect.w = pcsx2_min_f(combined_rect.w, offset);

		// For Off mode with non-interlaced content, ensure the framebuffer covers the full display height
		// (avoids truncation when the video mode offset cap is smaller than the actual frame height).
		if (GSConfig.InterlaceMode == GS_INTERLACE_OFF && !interlaced)
			combined_rect.w = pcsx2_max_f(combined_rect.w, pcsx2_max_f(PCRTCDisplays[0].displayRect.w, PCRTCDisplays[1].displayRect.w));

		return GSVector2i(combined_rect.z, combined_rect.w);
	}
	else
	{
		GSVector4i out_rect = PCRTCDisplays[display].framebufferRect;

		if (out_rect.z >= 2048)
			out_rect.z -= out_rect.x;

		if (out_rect.w >= 2048)
			out_rect.w -= out_rect.y;

		// Cap the framebuffer read to the maximum display height, otherwise the hardware renderer gets messy.
		const int min_mag = pcsx2_max_f(1, PCRTCDisplays[display].magnification.y);
		int offset = PCRTCDisplays[display].displayRect.y;

		if (FFMD && interlaced)
			offset = (offset - 1) / 2;

		max_height += out_rect.y;

		offset = (max_height / min_mag) - offset;
		out_rect.w = pcsx2_min_f(out_rect.w, offset);

		// For Off mode, ensure the framebuffer covers the full display height.
		if (GSConfig.InterlaceMode == GS_INTERLACE_OFF)
			out_rect.w = pcsx2_max_f(out_rect.w, PCRTCDisplays[display].displayRect.w);

		return GSVector2i(out_rect.z, out_rect.w);
	}
}

// Sets up the rectangles for both the framebuffer read and the displays for the merge circuit.
void RefPCRTC::SetRects(int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg)
{
	// Save framebuffer information first, while we're here.
	PCRTCDisplays[display].FBP = framebufferReg.FBP;
	PCRTCDisplays[display].FBW = framebufferReg.FBW;
	PCRTCDisplays[display].PSM = framebufferReg.PSM;
	PCRTCDisplays[display].prevFramebufferReg = framebufferReg;
	// Probably not really enabled but will cause a mess.
	// Q-Ball Billiards enables both circuits but doesn't set one of them up.
	if (PCRTCDisplays[display].FBW == 0 && displayReg.DW == 0 && displayReg.DH == 0 && displayReg.MAGH == 0)
	{
		PCRTCDisplays[display].enabled = false;
		return;
	}
	PCRTCDisplays[display].magnification = GSVector2i(displayReg.MAGH + 1, displayReg.MAGV + 1);
	const u32 DW = displayReg.DW + 1;
	const u32 DH = displayReg.DH + 1;

	const int renderWidth = DW / PCRTCDisplays[display].magnification.x;
	const int renderHeight = DH / PCRTCDisplays[display].magnification.y;

	u32 finalDisplayWidth = renderWidth;
	u32 finalDisplayHeight = renderHeight;
	// When using screen offsets the screen gets squashed/resized in to the actual screen size.
	if (GSConfig.PCRTCOffsets)
	{
		finalDisplayWidth = DW / (VideoModeDividers[VideoModeRow()].x + 1);
		finalDisplayHeight = DH / (VideoModeDividers[VideoModeRow()].y + 1);
	}
	else
	{
		finalDisplayWidth = pcsx2_min_f(finalDisplayWidth, DW / (VideoModeDividers[VideoModeRow()].x + 1));
		finalDisplayHeight = pcsx2_min_f(finalDisplayHeight, DH / (VideoModeDividers[VideoModeRow()].y + 1));
	}

	// Framebuffer size and offsets.
	PCRTCDisplays[display].prevFramebufferOffsets = PCRTCDisplays[display].framebufferOffsets;
	PCRTCDisplays[display].framebufferRect.x = 0;
	PCRTCDisplays[display].framebufferRect.y = 0;
	PCRTCDisplays[display].framebufferRect.z = renderWidth;

	if(FFMD && interlaced) // Round up the height as if it's an odd value, this will cause havok with the merge circuit.
		PCRTCDisplays[display].framebufferRect.w = (renderHeight + 1) >> (FFMD * interlaced); // Half height read if FFMD + INT enabled.
	else
		PCRTCDisplays[display].framebufferRect.w = renderHeight;
	PCRTCDisplays[display].framebufferOffsets.x = framebufferReg.DBX;
	PCRTCDisplays[display].framebufferOffsets.y = framebufferReg.DBY;

	const bool is_interlaced_resolution = interlaced || (toggling_field && GSConfig.InterlaceMode != GS_INTERLACE_OFF);

	// If the interlace flag isn't set, but it's still interlacing, the height is likely reported wrong.
	// Q-Ball Billiards.
	if (is_interlaced_resolution && !interlaced)
		finalDisplayHeight *= 2;

	// Display size and offsets.
	PCRTCDisplays[display].displayRect.x = 0;
	PCRTCDisplays[display].displayRect.y = 0;
	PCRTCDisplays[display].displayRect.z = finalDisplayWidth;
	PCRTCDisplays[display].displayRect.w = finalDisplayHeight;
	PCRTCDisplays[display].prevDisplayOffset = PCRTCDisplays[display].displayOffset;
	PCRTCDisplays[display].displayOffset.x = displayReg.DX;
	PCRTCDisplays[display].displayOffset.y = displayReg.DY;
}

// Calculate framebuffer read offsets, should be considered if only one circuit is enabled, or difference is more than 1 line.
// Only considered if "Anti-blur" is enabled.
void RefPCRTC::CalculateFramebufferOffset(bool scanmask)
{
	if (GSConfig.PCRTCAntiBlur && PCRTCSameSrc && !scanmask)
	{
		GSVector2i fb0 = GSVector2i(PCRTCDisplays[0].framebufferOffsets.x, PCRTCDisplays[0].framebufferOffsets.y);
		GSVector2i fb1 = GSVector2i(PCRTCDisplays[1].framebufferOffsets.x, PCRTCDisplays[1].framebufferOffsets.y);

		if (fb0.x + PCRTCDisplays[0].displayRect.z > 2048)
		{
			fb0.x -= 2048;
			fb0.x = abs(fb0.x);
		}
		if (fb0.y + PCRTCDisplays[0].displayRect.w > 2048)
		{
			fb0.y -= 2048;
			fb0.y = abs(fb0.y);
		}
		if (fb1.x + PCRTCDisplays[1].displayRect.z > 2048)
		{
			fb1.x -= 2048;
			fb1.x = abs(fb1.x);
		}
		if (fb1.y + PCRTCDisplays[1].displayRect.w > 2048)
		{
			fb1.y -= 2048;
			fb1.y = abs(fb1.y);
		}

		if (abs(fb1.y - fb0.y) == 1
			&& PCRTCDisplays[0].displayRect.y == PCRTCDisplays[1].displayRect.y)
		{
			if (fb1.y < fb0.y)
				PCRTCDisplays[0].framebufferOffsets.y = fb1.y;
			else
				PCRTCDisplays[1].framebufferOffsets.y = fb0.y;
		}
		if (abs(fb1.x - fb0.x) == 1
			&& PCRTCDisplays[0].displayRect.x == PCRTCDisplays[1].displayRect.x)
		{
			if (fb1.x < fb0.x)
				PCRTCDisplays[0].framebufferOffsets.x = fb1.x;
			else
				PCRTCDisplays[1].framebufferOffsets.x = fb0.x;
		}
	}
	PCRTCDisplays[0].framebufferRect.x += PCRTCDisplays[0].framebufferOffsets.x;
	PCRTCDisplays[0].framebufferRect.z += PCRTCDisplays[0].framebufferOffsets.x;
	PCRTCDisplays[0].framebufferRect.y += PCRTCDisplays[0].framebufferOffsets.y;
	PCRTCDisplays[0].framebufferRect.w += PCRTCDisplays[0].framebufferOffsets.y;

	PCRTCDisplays[1].framebufferRect.x += PCRTCDisplays[1].framebufferOffsets.x;
	PCRTCDisplays[1].framebufferRect.z += PCRTCDisplays[1].framebufferOffsets.x;
	PCRTCDisplays[1].framebufferRect.y += PCRTCDisplays[1].framebufferOffsets.y;
	PCRTCDisplays[1].framebufferRect.w += PCRTCDisplays[1].framebufferOffsets.y;
}

// Used in software mode to align the buffer when reading. Offset is accounted for (block aligned) by GetOutput.
void RefPCRTC::RemoveFramebufferOffset(int display)
{
	if (display >= 0)
	{
		// Hardware needs nothing but handling for wrapped framebuffers.
		if (emu_gs_hardware_renderer(&GSConfig))
		{
			if (PCRTCDisplays[display].framebufferRect.z >= 2048)
			{
				PCRTCDisplays[display].displayRect.x += 2048 - PCRTCDisplays[display].framebufferRect.x;
				PCRTCDisplays[display].displayRect.z += 2048 - PCRTCDisplays[display].framebufferRect.x;
				PCRTCDisplays[display].framebufferRect.x = 0;
				PCRTCDisplays[display].framebufferRect.z -= 2048;
			}
			if (PCRTCDisplays[display].framebufferRect.w >= 2048)
			{
				PCRTCDisplays[display].displayRect.y += 2048 - PCRTCDisplays[display].framebufferRect.y;
				PCRTCDisplays[display].displayRect.w += 2048 - PCRTCDisplays[display].framebufferRect.y;
				PCRTCDisplays[display].framebufferRect.y = 0;
				PCRTCDisplays[display].framebufferRect.w -= 2048;
			}
		}
		else
		{
			const gs_psm_attr& psm = gs_psm[PCRTCDisplays[display].PSM];

			// Software mode - See note below.
			GSVector4i r = PCRTCDisplays[display].framebufferRect;
			r = r.ralign<Align_Outside>(GSVector2i(psm.bs.x, psm.bs.y));

			PCRTCDisplays[display].framebufferRect.z -= r.x;
			PCRTCDisplays[display].framebufferRect.w -= r.y;
			PCRTCDisplays[display].framebufferRect.x -= r.x;
			PCRTCDisplays[display].framebufferRect.y -= r.y;
		}
	}
	else
	{
		// Software Mode Note:
		// This code is to read the framebuffer nicely block aligned in software, then leave the remaining offset in to the block.
		// In hardware mode this doesn't happen, it reads the whole framebuffer, so we need to keep the offset.
		if (!emu_gs_hardware_renderer(&GSConfig))
		{
			const gs_psm_attr& psm = gs_psm[PCRTCDisplays[1].PSM];

			GSVector4i r = PCRTCDisplays[0].framebufferRect.runion(PCRTCDisplays[1].framebufferRect);
			r = r.ralign<Align_Outside>(GSVector2i(psm.bs.x, psm.bs.y));

			PCRTCDisplays[0].framebufferRect.x -= r.x;
			PCRTCDisplays[0].framebufferRect.y -= r.y;
			PCRTCDisplays[0].framebufferRect.z -= r.x;
			PCRTCDisplays[0].framebufferRect.w -= r.y;
			PCRTCDisplays[1].framebufferRect.x -= r.x;
			PCRTCDisplays[1].framebufferRect.y -= r.y;
			PCRTCDisplays[1].framebufferRect.z -= r.x;
			PCRTCDisplays[1].framebufferRect.w -= r.y;
		}
	}
}

// If the two displays are offset from each other, move them to the correct offsets.
// If using screen offsets, calculate the positions here.
void RefPCRTC::CalculateDisplayOffset(bool scanmask)
{
	const bool both_enabled = PCRTCDisplays[0].enabled && PCRTCDisplays[1].enabled;
	// Offsets are generally ignored, the "hacky" way of doing the displays, but direct to framebuffers.
	if (!GSConfig.PCRTCOffsets)
	{
		const GSVector4i offsets = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()] : VideoModeOffsetsOverscan[VideoModeRow()];
		int int_off[2]           = { 0, 0 };
		GSVector2i zeroDisplay   = NearestToZeroOffset();
		GSVector2i baseOffset    = PCRTCDisplays[zeroDisplay.y].displayOffset;

		if (both_enabled)
		{
			int blurOffset = abs(PCRTCDisplays[1].displayOffset.y - PCRTCDisplays[0].displayOffset.y);
			if (GSConfig.PCRTCAntiBlur && !scanmask && blurOffset < 4)
			{
				if (PCRTCDisplays[1].displayOffset.y > PCRTCDisplays[0].displayOffset.y)
					PCRTCDisplays[1].displayOffset.y -= blurOffset;
				else
					PCRTCDisplays[0].displayOffset.y -= blurOffset;
			}
		}

		// If there's a single pixel offset, account for it else it can throw interlacing out.
		for (int i = 0; i < 2; i++)
		{
			if (!PCRTCDisplays[i].enabled)
				continue;

			// Should this be MAGV/H in the DISPLAY register rather than the "default" magnification?
			const int offset = (PCRTCDisplays[i].displayOffset.y - (offsets.w * (interlaced + 1))) / (VideoModeDividers[VideoModeRow()].y + 1);

			if (offset > 4)
				continue;

			int_off[i] = offset & 1;
			if (offset < 0)
				int_off[i] = -int_off[i];

			PCRTCDisplays[i].displayRect.y += int_off[i];
			PCRTCDisplays[i].displayRect.w += int_off[i];
		}

		// Handle difference in offset between the two displays, used in games like DmC and Time Crisis 2 (for split screen).
		// Offset is not screen based, but relative to each other.
		if (both_enabled)
		{
			GSVector2i offset = {
				(PCRTCDisplays[1 - zeroDisplay.x].displayOffset.x - PCRTCDisplays[zeroDisplay.x].displayOffset.x) / (VideoModeDividers[VideoModeRow()].x + 1),
				(PCRTCDisplays[1 - zeroDisplay.y].displayOffset.y - PCRTCDisplays[zeroDisplay.y].displayOffset.y) / (VideoModeDividers[VideoModeRow()].y + 1)
			};

			if (offset.x >= 4 || !GSConfig.PCRTCAntiBlur || scanmask)
			{
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.x += offset.x;
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.z += offset.x;
			}
			if (offset.y >= 4 || !GSConfig.PCRTCAntiBlur || scanmask)
			{
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.y += offset.y - int_off[1 - zeroDisplay.y];
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.w += offset.y - int_off[1 - zeroDisplay.y];
			}

			baseOffset = PCRTCDisplays[zeroDisplay.y].displayOffset;
		}

		// Handle any large vertical offset from the zero position on the screen.
		// Example: Hokuto no Ken, does a rougly -14 offset to bring the screen up.
		// Ignore the lowest bit, we've already accounted for this
		int vOffset = ((static_cast<int>(baseOffset.y) - (offsets.w * (interlaced + 1))) / (VideoModeDividers[VideoModeRow()].y + 1));

		if(vOffset <= 4 && vOffset != 0)
		{
			PCRTCDisplays[0].displayRect.y += vOffset - int_off[0];
			PCRTCDisplays[0].displayRect.w += vOffset - int_off[0];
			PCRTCDisplays[1].displayRect.y += vOffset - int_off[1];
			PCRTCDisplays[1].displayRect.w += vOffset - int_off[1];
		}
	}
	else // We're using screen offsets, so just calculate the entire offset.
	{
		const GSVector4i offsets = !GSConfig.PCRTCOverscan ? VideoModeOffsets[VideoModeRow()] : VideoModeOffsetsOverscan[VideoModeRow()];
		GSVector2i zeroDisplay = NearestToZeroOffset();

		if (both_enabled)
		{
			int blurOffset = abs(PCRTCDisplays[1].displayOffset.y - PCRTCDisplays[0].displayOffset.y);
			if (GSConfig.PCRTCAntiBlur && !scanmask && blurOffset < 4)
			{
				if (PCRTCDisplays[1].displayOffset.y > PCRTCDisplays[0].displayOffset.y)
					PCRTCDisplays[1].displayOffset.y -= blurOffset;
				else
					PCRTCDisplays[0].displayOffset.y -= blurOffset;
			}
		}

		for (int i = 0; i < 2; i++)
		{
			// Should this be MAGV/H in the DISPLAY register rather than the "default" magnification?
			const GSVector2i offset = {
				(static_cast<int>(PCRTCDisplays[i].displayOffset.x) - offsets.z) / (VideoModeDividers[VideoModeRow()].x + 1),
				(static_cast<int>(PCRTCDisplays[i].displayOffset.y) - (offsets.w * (interlaced + 1))) / (VideoModeDividers[VideoModeRow()].y + 1)
			};

			PCRTCDisplays[i].displayRect.x += offset.x;
			PCRTCDisplays[i].displayRect.z += offset.x;
			PCRTCDisplays[i].displayRect.y += offset.y;
			PCRTCDisplays[i].displayRect.w += offset.y;
		}

		if (both_enabled)
		{
			const GSVector2i offset = {
				(PCRTCDisplays[1 - zeroDisplay.x].displayRect.x - PCRTCDisplays[zeroDisplay.x].displayRect.x),
				(PCRTCDisplays[1 - zeroDisplay.y].displayRect.y - PCRTCDisplays[zeroDisplay.y].displayRect.y)
			};

			if (offset.x > 0 && offset.x < 4 && GSConfig.PCRTCAntiBlur)
			{
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.x -= offset.x;
				PCRTCDisplays[1 - zeroDisplay.x].displayRect.z -= offset.x;
			}
			if (offset.y > 0 && offset.y < 4 && GSConfig.PCRTCAntiBlur)
			{
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.y -= offset.y;
				PCRTCDisplays[1 - zeroDisplay.y].displayRect.w -= offset.y;
			}
		}
	}
}

GSVideoMode ref_video_mode(const GSPrivRegSet* regs) { return ref_GetVideoMode(regs); }
bool ref_interlaced(const GSPrivRegSet* regs) { return ref_isinterlaced(regs); }
bool ref_really_interlaced(const GSPrivRegSet* regs) { return ref_isReallyInterlaced(regs); }
float ref_tv_refresh_rate(const GSPrivRegSet* regs) { return ref_GetTvRefreshRate(regs); }
