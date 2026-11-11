/* Profiler.c and Profiler.h, built with ENABLE_PCSX2_PROFILER as the
 * PROFILER=1 core builds them, which no other build compiles.
 *
 * Zones are exclusive: a nested zone's time is not its parent's, so the
 * zones' ticks sum to exactly the span from the first begin to the last
 * end. The innermost zone is per thread. pcsx2_profile_frame_end resets
 * the counts every 60 frames and reports from the second time on. */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "Profiler.h"

retro_log_printf_t log_cb;
static int report_lines;
static void count_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	if (!strcmp(fmt, "%s"))
		report_lines++;
}

void profiler_test_cxx_scopes(void);
void profiler_test_spin(void)
{
	volatile int i;
	for (i = 0; i < 20000; i++)
	{
	}
}

static int failures;
static void check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static u64 ticks_sum(void)
{
	u64 sum = 0;
	int i;
	for (i = 0; i < PCSX2_ZONE_COUNT; i++)
		sum += pcsx2_profile_zone_ticks[i];
	return sum;
}

static int other_thread_current;
#ifdef _WIN32
static DWORD WINAPI other_thread(LPVOID arg)
#else
static void* other_thread(void* arg)
#endif
{
	(void)arg;
	other_thread_current = pcsx2_profile_current;
	return 0;
}

int main(void)
{
	u64 start, span;
	int outer, inner, i;

	log_cb = count_log;
	memset(pcsx2_profile_zone_ticks, 0, sizeof(pcsx2_profile_zone_ticks));
	memset(pcsx2_profile_zone_calls, 0, sizeof(pcsx2_profile_zone_calls));
	check(pcsx2_profile_current == -1, "no zone open to begin with");

	outer = PROFILE_ZONE_BEGIN(ZONE_VU1);
	start = pcsx2_profile_last;
	profiler_test_spin();
	inner = PROFILE_ZONE_BEGIN(ZONE_GS_TRANSFER);
	profiler_test_spin();

	/* Another thread, while this one is two zones deep. */
	{
#ifdef _WIN32
		HANDLE t = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
		WaitForSingleObject(t, INFINITE);
		CloseHandle(t);
#else
		pthread_t t;
		pthread_create(&t, NULL, other_thread, NULL);
		pthread_join(t, NULL);
#endif
	}
	check(other_thread_current == -1, "another thread has no zone open");

	PROFILE_ZONE_END(ZONE_GS_TRANSFER, inner);
	profiler_test_spin();
	PROFILE_ZONE_END(ZONE_VU1, outer);
	span = pcsx2_profile_last - start;

	check(pcsx2_profile_current == -1, "closing both leaves none open");
	check(pcsx2_profile_zone_calls[PCSX2_ZONE_VU1] == 1 && pcsx2_profile_zone_calls[PCSX2_ZONE_GS_TRANSFER] == 1,
		"one call each");
	check(pcsx2_profile_zone_ticks[PCSX2_ZONE_VU1] > 0 && pcsx2_profile_zone_ticks[PCSX2_ZONE_GS_TRANSFER] > 0,
		"both zones charged");
	check(ticks_sum() == span, "exclusive: the zones sum to the span exactly");

	profiler_test_cxx_scopes();
	check(pcsx2_profile_current == -1 && pcsx2_profile_zone_calls[PCSX2_ZONE_GS_VSYNC] == 1
		&& pcsx2_profile_zone_calls[PCSX2_ZONE_GS_SYNC] == 1,
		"two PROFILE_SCOPEs in one C++ block: two zones, both closed");

	for (i = 0; i < 59; i++)
		PROFILE_FRAME_END();
	check(pcsx2_profile_zone_calls[PCSX2_ZONE_VU1] == 1, "59 frames: counts kept");
	PROFILE_FRAME_END();
	check(ticks_sum() == 0 && pcsx2_profile_zone_calls[PCSX2_ZONE_VU1] == 0 && report_lines == 0,
		"60th frame: counts reset, nothing to compare yet");
	for (i = 0; i < 60; i++)
		PROFILE_FRAME_END();
	check(report_lines == 1 + PCSX2_ZONE_COUNT + 2, "120th frame: a report, a line a zone");

	printf(failures ? "profiler: %d failure(s)\n" : "profiler: ok\n", failures);
	return failures != 0;
}
