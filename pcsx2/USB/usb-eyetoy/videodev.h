/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#include "common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum eyetoy_frame_format
{
	EYETOY_FORMAT_MPEG,
	EYETOY_FORMAT_JPEG,
	EYETOY_FORMAT_YUV400
};

enum eyetoy_device_type
{
	EYETOY_TYPE_EYETOY,
	EYETOY_TYPE_OV511P
};

/* A camera: a table of operations behind a pointer, one table per kind.
 * Everything a device needs is allocated when it is created, none when the
 * game opens it. */
typedef struct eyetoy_video_device eyetoy_video_device;
typedef struct eyetoy_video_ops
{
	int (*open)(eyetoy_video_device* dev, int width, int height, enum eyetoy_frame_format format, int mirror);
	int (*close)(eyetoy_video_device* dev);
	/* The newest frame, up to len bytes; 0 when none is new. */
	int (*get_image)(eyetoy_video_device* dev, u8* buf, size_t len);
	void (*set_mirroring)(eyetoy_video_device* dev, bool state);
	/* Closes it and frees everything. */
	void (*destroy)(eyetoy_video_device* dev);
} eyetoy_video_ops;

struct eyetoy_video_device
{
	const eyetoy_video_ops* ops;
};

/* The frontend's camera when it has one, else the host's own where there
 * is a backend, else a test pattern; NULL only when memory runs out. */
eyetoy_video_device* eyetoy_video_create(void);

/* 16 kHz mono, as the EyeToy's microphone delivers it. */
typedef struct eyetoy_mic eyetoy_mic;
typedef struct eyetoy_mic_ops
{
	/* How many samples were available, up to max; never waits. */
	u32 (*read)(eyetoy_mic* mic, s16* samples, u32 max);
	void (*destroy)(eyetoy_mic* mic);
} eyetoy_mic_ops;

struct eyetoy_mic
{
	const eyetoy_mic_ops* ops;
};

/* The frontend's microphone when it has one, else the host's; NULL when
 * neither exists. */
eyetoy_mic* eyetoy_mic_create(void);

#ifdef __cplusplus
}
#endif
