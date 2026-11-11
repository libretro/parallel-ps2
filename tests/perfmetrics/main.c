/* PerformanceMetrics.c, the real unit: which signal SkipDuplicateFrames
 * takes for a finished internal frame. Decided once a 32-vsync window;
 * privileged register writes win unless the gamefix turns them off,
 * then DISPFB blits, else none. reset restarts the window, clear also
 * forgets the method. */
#include <stdio.h>

#include "PerformanceMetrics.h"

static bool blit_hack;
bool emu_config_blit_internal_fps_hack(void) { return blit_hack; }

static int failures;
static void check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

/* n vsyncs, the first writes registers / blits as asked, the rest idle. */
static void frames(int n, bool reg, bool blit)
{
	int i;
	for (i = 0; i < n; i++)
		perf_metrics_update(i == 0 && reg, i == 0 && blit);
}

int main(void)
{
	perf_metrics_clear();
	check(perf_metrics_internal_fps_method() == PERF_FPS_NONE, "nothing seen: none");

	frames(31, true, true);
	check(perf_metrics_internal_fps_method() == PERF_FPS_NONE, "31 vsyncs: not decided yet");
	frames(1, false, false);
	check(perf_metrics_internal_fps_method() == PERF_FPS_GS_PRIVILEGED_REGISTER,
		"32nd vsync: register writes win over blits");

	frames(32, false, true);
	check(perf_metrics_internal_fps_method() == PERF_FPS_DISPFB_BLIT, "blits alone: DISPFB");

	blit_hack = true;
	frames(32, true, true);
	check(perf_metrics_internal_fps_method() == PERF_FPS_DISPFB_BLIT,
		"the gamefix on: register writes do not count");
	frames(32, true, false);
	check(perf_metrics_internal_fps_method() == PERF_FPS_NONE,
		"the gamefix on, no blits: none");
	blit_hack = false;

	frames(32, false, false);
	check(perf_metrics_internal_fps_method() == PERF_FPS_NONE, "an idle window: none");

	/* A reset mid-window drops what the window had seen. */
	frames(32, true, false);
	frames(16, false, true);
	perf_metrics_reset();
	frames(32, false, false);
	check(perf_metrics_internal_fps_method() == PERF_FPS_NONE, "reset: the blit before it is forgotten");

	frames(32, true, false);
	perf_metrics_clear();
	check(perf_metrics_internal_fps_method() == PERF_FPS_NONE, "clear: the method is forgotten");

	printf(failures ? "perfmetrics: %d failure(s)\n" : "perfmetrics: ok\n", failures);
	return failures != 0;
}
