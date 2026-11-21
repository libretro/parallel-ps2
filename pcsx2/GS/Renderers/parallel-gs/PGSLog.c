/* Granite's and paraLLEl-GS's logs (LOGE/LOGW/LOGI, Granite/util/
 * logging.hpp) go to the frontend's log from any thread, at their
 * RETRO_LOG_* level. A line past the buffer is cut: nothing is
 * allocated. */

#include <stdarg.h>
#include <stdio.h>

#include <libretro.h>

extern retro_log_printf_t log_cb;

void pgs_log(int level, const char *fmt, ...)
{
	char buf[1024];
	va_list va;
	if (!log_cb)
		return;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	log_cb((enum retro_log_level)level, "%s", buf);
}
