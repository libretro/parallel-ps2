/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#include "videodev.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The host's own camera and microphone, for frontends that offer neither.
 * Linux only (V4L2 and ALSA); NULL elsewhere. */
eyetoy_video_device* eyetoy_host_video_create(void);
eyetoy_mic* eyetoy_host_mic_create(void);

#ifdef __cplusplus
}
#endif
