/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PCSX2_PERFORMANCEMETRICS_H
#define PCSX2_PERFORMANCEMETRICS_H

#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The signal a game marks its finished internal frames with. */
enum perf_fps_method
{
	PERF_FPS_NONE = 0,
	PERF_FPS_GS_PRIVILEGED_REGISTER,
	PERF_FPS_DISPFB_BLIT
};

/* Forget the method too, for a new game; reset only restarts the window. */
void perf_metrics_clear(void);
void perf_metrics_reset(void);
/* Once a vsync: whether the frame wrote a privileged GS register, and
 * whether it blitted to the display framebuffer. */
void perf_metrics_update(bool gs_register_write, bool fb_blit);
enum perf_fps_method perf_metrics_internal_fps_method(void);

/* Gamefixes.BlitInternalFPSHack (Pcsx2Config.cpp). */
bool emu_config_blit_internal_fps_hack(void);

#ifdef __cplusplus
}
#endif

#endif
