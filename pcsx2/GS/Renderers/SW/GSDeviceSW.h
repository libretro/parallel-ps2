/* SPDX-License-Identifier: LGPL-3.0+ */
#ifndef GS_DEVICE_SW_H
#define GS_DEVICE_SW_H

/* The device where there is no GPU context (RetroArch's SDL2 driver,
 * say): every composite and present done on the CPU, on the textures in
 * CPU memory GSTextureSW.c makes, and the frame handed to the frontend
 * as a raw XRGB8888 buffer, its own where the frontend offers one. Only
 * the SW renderer draws through it: the HW renderers are not compatible
 * with it.
 *
 * The textures hold the PS2's ABGR (R in byte 0) as the SW renderer
 * wrote it; the one conversion, to XRGB8888 (B in byte 0, alpha opaque),
 * is on the way to the frontend. */

#include <boolean.h>

#include "common/Pcsx2Defs.h"
#include "../Common/GSDeviceBase.h"
#include "../Common/GSScratch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_device_sw
{
	gs_device base;

	/* The frame when the frontend has no buffer of its own to offer:
	 * XRGB8888, rows of present_pitch bytes */
	u8* present_buffer;
	int present_buffer_capacity; /* bytes */
	int present_pitch;
	int present_width;
	int present_height;

	/* Whether the frontend offers its buffer (asked again when the
	 * frame's size changes), and this frame's if so: it is only good
	 * for the frame it was asked for */
	bool direct_rendering_checked;
	bool direct_rendering_supported;
	u8* direct_render_buffer;
	size_t direct_render_pitch;

	/* A frame was presented since BeginPresent: EndPresent hands one
	 * over only then */
	bool present_pending;

	/* The half-height fields' motion-adaptive rows, kept between frames */
	struct gs_scratch scratch_mad;
} gs_device_sw;

/* The device's table (gs_device_ops) */
extern const struct gs_device_ops gs_device_sw_ops;

/* (gs_device_open's, GSDeviceBase.h: gs_device_new_sw) */

#ifdef __cplusplus
}
#endif

#endif
