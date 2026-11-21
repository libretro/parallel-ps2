/* Granite's logs (pgs_log, GS/Renderers/parallel-gs/PGSLog.c): each
 * reaches the frontend's log once, at its level, from the thread that
 * logged it; a line past the buffer is cut, not lost; with no log, a
 * call does nothing. */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>
#include <rthreads/rthreads.h>
#include <retro_atomic.h>

void pgs_log(int level, const char *fmt, ...);

retro_log_printf_t log_cb;

static int fails;

#define CHECK(c) \
	do \
	{ \
		if (!(c)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			fails++; \
		} \
	} while (0)

static char got[4096];
static int got_level, got_count;
static retro_atomic_int_t per_thread[4];
static retro_atomic_int_t bad_lines;

static void capture(enum retro_log_level level, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vsnprintf(got, sizeof(got), fmt, va);
	va_end(va);
	got_level = (int)level;
	got_count++;
}

/* (counted with atomics: the logging takes no lock, nor does this) */
static void count_line(enum retro_log_level level, const char* fmt, ...)
{
	char line[256];
	int t, n;
	va_list va;
	va_start(va, fmt);
	vsnprintf(line, sizeof(line), fmt, va);
	va_end(va);
	if (sscanf(line, "thread %d line %d", &t, &n) == 2 && t >= 0 && t < 4 && level == RETRO_LOG_WARN)
		retro_atomic_fetch_add_int(&per_thread[t], 1);
	else
		retro_atomic_fetch_add_int(&bad_lines, 1);
}

static void logger(void* data)
{
	int t = (int)(size_t)data, i;
	for (i = 0; i < 5000; i++)
		pgs_log(RETRO_LOG_WARN, "thread %d line %d\n", t, i);
}

int main(void)
{
	static char longline[3000];
	sthread_t* th[4];
	int i;

	log_cb = NULL;
	pgs_log(RETRO_LOG_ERROR, "nowhere %d\n", 1); /* (no log: nothing) */

	log_cb = capture;
	pgs_log(RETRO_LOG_ERROR, "e %d %s\n", 7, "x");
	CHECK(got_count == 1 && got_level == RETRO_LOG_ERROR && !strcmp(got, "e 7 x\n"));
	pgs_log(RETRO_LOG_WARN, "w\n");
	CHECK(got_count == 2 && got_level == RETRO_LOG_WARN);
	pgs_log(RETRO_LOG_INFO, "%s", "i");
	CHECK(got_count == 3 && got_level == RETRO_LOG_INFO && !strcmp(got, "i"));
	/* (a % in what is logged stays a %) */
	pgs_log(RETRO_LOG_INFO, "%s", "100%d");
	CHECK(!strcmp(got, "100%d"));

	memset(longline, 'a', sizeof(longline) - 1);
	pgs_log(RETRO_LOG_INFO, "%s", longline);
	CHECK(got_count == 5 && strlen(got) == 1023 && got[0] == 'a' && got[1022] == 'a');

	/* from four threads at once, every line whole and counted once */
	log_cb = count_line;
	for (i = 0; i < 4; i++)
		retro_atomic_int_init(&per_thread[i], 0);
	retro_atomic_int_init(&bad_lines, 0);
	for (i = 0; i < 4; i++)
		th[i] = sthread_create(logger, (void*)(size_t)i);
	for (i = 0; i < 4; i++)
		sthread_join(th[i]);
	for (i = 0; i < 4; i++)
		CHECK(retro_atomic_load_acquire_int(&per_thread[i]) == 5000);
	CHECK(retro_atomic_load_acquire_int(&bad_lines) == 0);

	if (fails)
	{
		printf("pgslog: %d failed\n", fails);
		return 1;
	}
	printf("pgslog: ok\n");
	return 0;
}
