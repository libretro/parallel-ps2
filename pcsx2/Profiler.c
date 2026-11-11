/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2025 PCSX2 Dev Team
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

#include "Profiler.h"

#ifdef ENABLE_PCSX2_PROFILER

#include <stdio.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include <libretro.h>

u64 pcsx2_profile_zone_ticks[PCSX2_ZONE_COUNT];
u64 pcsx2_profile_zone_calls[PCSX2_ZONE_COUNT];
PCSX2_PROFILE_TLS int pcsx2_profile_current = -1;
PCSX2_PROFILE_TLS u64 pcsx2_profile_last;

static const char* const s_zone_names[PCSX2_ZONE_COUNT] =
{
	"IOP exec",
	"VU1",
	"VIF unpack",
	"SPU2 mix",
	"GS transfer",
	"GS vsync",
	"GS gpu wait",
	"GS drawkick",
	"GS regwrite",
	"GS kick tex",
	"GS vs flush",
	"GS vs circ",
	"GS vs merge",
	"GS vs image",
	"GS handover",
};

/* The instrument is not free, and for a zone entered tens of thousands of
 * times a frame it is a material share of that zone's own number: VIF
 * unpack is entered ~36500 times per frame, so at ~58 ns of clock reads
 * per scope it accrues milliseconds of its own overhead. Calibrate once
 * and report the corrected time alongside the raw, so a hot zone is not
 * mistaken for expensive work when it is really expensive measurement. */
static u64 s_scope_overhead_ticks;
static double s_ns_per_tick;

/* The report goes to the frontend's log, where RetroArch keeps it: on
 * Windows a RetroArch without a console drops stderr, and a profile that
 * cannot be read was not taken. stderr only when there is no log. */
static void profile_printf(const char* fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (log_cb)
		log_cb(RETRO_LOG_INFO, "%s", line);
	else
		fputs(line, stderr);
}

u64 pcsx2_profile_now_ns(void)
{
#ifdef _WIN32
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	return (u64)((double)count.QuadPart * 1e9 / (double)freq.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

static void Calibrate(void)
{
	const int N = 200000;
	u64 wall0, wall1, tick0, tick1, sink = 0;
	int i;

	/* The counter is not a nanosecond clock on either architecture, so
	 * measure it against the real one once. */
	wall0 = pcsx2_profile_now_ns();
	tick0 = pcsx2_profile_now_ticks();
	for (i = 0; i < N; i++)
	{
		const u64 a = pcsx2_profile_now_ticks();
		const u64 b = pcsx2_profile_now_ticks();
		sink += b - a;
	}
	tick1 = pcsx2_profile_now_ticks();
	wall1 = pcsx2_profile_now_ns();

	s_ns_per_tick = (tick1 > tick0) ?
		(double)(wall1 - wall0) / (double)(tick1 - tick0) : 1.0;
	s_scope_overhead_ticks = (tick1 - tick0) / (u64)N;

	if (sink == ~0ull)
		profile_printf("unreachable\n");
}

/* Report every 60 frames. The zones nest -- VU1 and GS transfer happen
 * inside EE exec -- so the columns do not sum to the frame time and are
 * not meant to: each is "how long is spent here". */
void pcsx2_profile_frame_end(void)
{
	static u64 last_report;
	static u32 frames;
	u64 now;
	int i;

	frames++;
	if ((frames % 60) != 0)
		return;

	if (s_ns_per_tick == 0.0)
		Calibrate();

	now = pcsx2_profile_now_ns();
	if (last_report != 0)
	{
		const double wall_ms = (double)(now - last_report) / 1e6;
		double measured_ms = 0.0;
		double measured_overhead_ms = 0.0;

		profile_printf("[profile] 60 frames in %.1f ms wall (%.2f ms/frame)\n",
		        wall_ms, wall_ms / 60.0);
		for (i = 0; i < PCSX2_ZONE_COUNT; i++)
		{
			const double ms = (double)pcsx2_profile_zone_ticks[i] * s_ns_per_tick / 1e6;
			const double overhead_ms =
				(double)(pcsx2_profile_zone_calls[i] * s_scope_overhead_ticks) * s_ns_per_tick / 1e6;
			const double net_ms = ms > overhead_ms ? ms - overhead_ms : 0.0;
			measured_ms += ms;
			measured_overhead_ms += overhead_ms;
			profile_printf("[profile]   %-12s %8.2f ms raw  %8.2f ms net  %6.2f%%  %10llu calls  %7.0f ns/call\n",
			        s_zone_names[i], ms, net_ms,
			        wall_ms > 0.0 ? 100.0 * net_ms / wall_ms : 0.0,
			        (unsigned long long)pcsx2_profile_zone_calls[i],
			        pcsx2_profile_zone_calls[i] ? (double)pcsx2_profile_zone_ticks[i] * s_ns_per_tick / (double)pcsx2_profile_zone_calls[i] : 0.0);
		}

		/* Not a remainder: zones run on two threads, so their busy times
		 * can sum to more than wall without anything being wrong. Above
		 * 100% the two threads overlapped, below both had idle time. */
		profile_printf("[profile]   %-12s %8.2f ms      %6.2f%% of wall (two threads; >100%% means overlap)\n",
		        "zones total", measured_ms,
		        wall_ms > 0.0 ? 100.0 * measured_ms / wall_ms : 0.0);
		profile_printf("[profile]   instrument   %8.2f ms      %6.2f%%  at %llu ns/scope -- subtracted from the net column\n",
		        measured_overhead_ms,
		        wall_ms > 0.0 ? 100.0 * measured_overhead_ms / wall_ms : 0.0,
		        (unsigned long long)((double)s_scope_overhead_ticks * s_ns_per_tick));
	}

	for (i = 0; i < PCSX2_ZONE_COUNT; i++)
	{
		pcsx2_profile_zone_ticks[i] = 0;
		pcsx2_profile_zone_calls[i] = 0;
	}
	last_report = now;
}

#endif
