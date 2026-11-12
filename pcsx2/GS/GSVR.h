/*  PCSX2 - PS2 Emulator for PCs
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 */

#pragma once

/* --- VR -----------------------------------------------------------------
 * The 'VR' core option, as the GS sees it. The libretro side
 * (libretro/main.c) owns the option; the GS reads it at each vsync.
 *
 * A PS2 game hands the GS screen-space triangles, so there is no scene to
 * draw a second time from another eye. What the hardware renderer does
 * have is the depth buffer the displayed frame was drawn with. Each eye
 * is made from the frame and that depth: the frame is a window onto a
 * relief, every pixel pushed back by its depth, and each eye looks
 * through the window from where it is.
 *
 *   off           the frame as it is
 *   stereoscopic  two eyes either side of the centre of the window, as a
 *                 left/right pair the frontend shows in 3D, on a 3D
 *                 display or as a screen in a headset
 *
 * Only the Vulkan and OpenGL/GLES devices make the eyes (they set
 * GSDevice::m_vr_ops); everything else shows the frame as it is. */

enum GSVRMode
{
	GSVR_OFF          = 0,
	GSVR_STEREOSCOPIC = 1,
};

/* Defined in libretro/main.c. */
extern int   gs_vr_mode;   /* GSVRMode */
extern float gs_vr_depth;  /* depth strength, 1 the default */
extern float gs_vr_aspect; /* the picture's display aspect ratio */
