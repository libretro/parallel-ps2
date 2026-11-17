/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GSPCRTC_REFERENCE_H
#define GSPCRTC_REFERENCE_H

struct RefPCRTC
{
	struct PCRTCDisplay
	{
		bool enabled;
		int FBP;
		int FBW;
		int PSM;
		GSRegDISPFB prevFramebufferReg;
		GSVector2i prevDisplayOffset;
		GSVector2i displayOffset;
		GSVector4i displayRect;
		GSVector2i magnification;
		GSVector2i prevFramebufferOffsets;
		GSVector2i framebufferOffsets;
		GSVector4i framebufferRect;

		__fi int Block() const { return FBP << 5; }
	};

	// One less than the GSVideoMode enumerator, so that the six named
	// modes index the six-row VideoMode* tables directly. Unknown has
	// no row and lands on -1; read it through VideoModeRow() rather
	// than subscripting with it. IsAnalogue() wants the raw value.
	int videomode = 0;
	int interlaced = 0;
	int FFMD = 0;
	bool PCRTCSameSrc = false;
	bool toggling_field = false;
	PCRTCDisplay PCRTCDisplays[2] = {};

	bool IsAnalogue();

	// The row of the VideoMode* tables to read for the current mode.
	// GetVideoMode() returns Unknown whenever SMODE1.CMOD holds a value
	// identifying no colorburst -- a 2-bit field, so a game can write
	// one -- and Unknown has no row of its own, so it reads as NTSC.
	// Without this the subtraction above gave -1 and every one of the
	// table reads went an element before the start.
	__fi int VideoModeRow() const { return (videomode < 0) ? 0 : videomode; }

	// Calculates which display is closest to matching zero offsets in either direction.
	GSVector2i NearestToZeroOffset();

	void SetVideoMode(GSVideoMode videoModeIn);

	// Enable each of the displays.
	void EnableDisplays(GSRegPMODE pmode, GSRegSMODE2 smode2, bool smodetoggle);

	void CheckSameSource();

	bool FrameWrap();

	// If the start point of both frames match, we can do a single read
	bool FrameRectMatch();

	GSVector2i GetResolution();

	GSVector4i GetFramebufferRect(int display);

	int GetFramebufferBitDepth();

	GSVector2i GetFramebufferSize(int display);

	// Sets up the rectangles for both the framebuffer read and the displays for the merge circuit.
	void SetRects(int display, GSRegDISPLAY displayReg, GSRegDISPFB framebufferReg);

	// Calculate framebuffer read offsets, should be considered if only one circuit is enabled, or difference is more than 1 line.
	// Only considered if "Anti-blur" is enabled.
	void CalculateFramebufferOffset(bool scanmask);

	// Used in software mode to align the buffer when reading. Offset is accounted for (block aligned) by GetOutput.
	void RemoveFramebufferOffset(int display);

	// If the two displays are offset from each other, move them to the correct offsets.
	// If using screen offsets, calculate the positions here.
	void CalculateDisplayOffset(bool scanmask);
};

GSVideoMode ref_video_mode(const GSPrivRegSet* regs);
bool ref_interlaced(const GSPrivRegSet* regs);
bool ref_really_interlaced(const GSPrivRegSet* regs);
float ref_tv_refresh_rate(const GSPrivRegSet* regs);

#endif
