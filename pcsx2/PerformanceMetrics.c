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

#include "common/Pcsx2Types.h"
#include "PerformanceMetrics.h"

/* Which GS signal the running game marks a finished internal frame with,
 * for SkipDuplicateFrames: privileged register writes, else DISPFB blits.
 * That is a property of what the game does, not of wall time, so it is
 * decided afresh over a fixed window of vsyncs: 32, about half a second
 * at 60 fps. */
#define UPDATE_INTERVAL_FRAMES 32

static enum perf_fps_method s_internal_fps_method = PERF_FPS_NONE;
static u32 s_gs_framebuffer_blits_since_last_update;
static u32 s_gs_privileged_register_writes_since_last_update;
static u32 s_frames_since_last_update;

void perf_metrics_clear(void)
{
	perf_metrics_reset();
	s_internal_fps_method = PERF_FPS_NONE;
}

void perf_metrics_reset(void)
{
	s_gs_framebuffer_blits_since_last_update = 0;
	s_gs_privileged_register_writes_since_last_update = 0;
	s_frames_since_last_update = 0;
}

void perf_metrics_update(bool gs_register_write, bool fb_blit)
{
	s_gs_privileged_register_writes_since_last_update += gs_register_write ? 1 : 0;
	s_gs_framebuffer_blits_since_last_update += fb_blit ? 1 : 0;

	if (++s_frames_since_last_update < UPDATE_INTERVAL_FRAMES)
		return;

	/* Register writes first: they are less likely to be a false positive.
	 * The gamefix is read here, once a window, not every vsync. */
	if (s_gs_privileged_register_writes_since_last_update > 0 && !emu_config_blit_internal_fps_hack())
		s_internal_fps_method = PERF_FPS_GS_PRIVILEGED_REGISTER;
	else if (s_gs_framebuffer_blits_since_last_update > 0)
		s_internal_fps_method = PERF_FPS_DISPFB_BLIT;
	else
		s_internal_fps_method = PERF_FPS_NONE;

	perf_metrics_reset();
}

enum perf_fps_method perf_metrics_internal_fps_method(void)
{
	return s_internal_fps_method;
}
