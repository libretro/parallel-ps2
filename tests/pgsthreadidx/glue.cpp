/* C entry points to Granite's thread indices, for main.c. */
#include "thread_id.hpp"

extern "C" void tid_set_count(unsigned count) { Util::set_thread_index_count(count); }
extern "C" void tid_register(unsigned index) { Util::register_thread_index(index); }
extern "C" unsigned tid_get(void) { return Util::get_current_thread_index(); }

/* (Granite's logs go to the core's log, which this test has not) */
extern "C" void pgs_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}
