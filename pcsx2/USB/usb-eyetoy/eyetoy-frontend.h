/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#pragma once

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* All three run on the main thread. Load asks the frontend for its camera
 * and microphone, and must be called from retro_load_game; Run starts and
 * stops the camera and collects microphone samples, once per retro_run;
 * Unload lets both go. */
void eyetoy_frontend_load(retro_environment_t env_cb);
void eyetoy_frontend_run(void);
void eyetoy_frontend_unload(void);

#ifdef __cplusplus
}
#endif
