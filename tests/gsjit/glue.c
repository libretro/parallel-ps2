/* The software renderer's code map (GSCodeMap.c) over the real
 * GSCodeReserve, with a stand-in generator, for main.c. A key's low 32
 * bits name the function and its high 32 bits the bytes it takes; the
 * generator writes the name's low byte over them, and like the real
 * generator's emitter stops writing at the end of the room it is given. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include "GS/Renderers/Common/GSCodeMap.h"
#include "GS/Renderers/Common/GSCodeReserve.h"

static void gsjit_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
retro_log_printf_t log_cb = gsjit_log;

static size_t fake_gen(u64 key, void* code, size_t room)
{
	const size_t want = (size_t)(key >> 32);
	const size_t size = want < room ? want : room;
	memset(code, (int)(key & 0xff), size);
	return size;
}

/* The map's allocations: GSFlatMap.c's calloc is this (build.sh). */
static unsigned s_callocs;
void* gsjit_calloc(size_t n, size_t size)
{
	s_callocs++;
	return calloc(n, size);
}
unsigned gsjit_allocations(void) { return s_callocs; }

static gs_code_map s_map;

void gsjit_assign(unsigned char* base, size_t size)
{
	gs_code_map_init(&s_map, fake_gen, 256);
	g_gs_code_reserve.reserve.baseptr   = base;
	g_gs_code_reserve.reserve.size      = size;
	g_gs_code_reserve.reserve.allocator = NULL;
	gs_code_reserve_reset();
}

const unsigned char* gsjit_get(unsigned long long key)
{
	return (const unsigned char*)gs_code_map_get(&s_map, key);
}

/* What the scanline code's reset does (gs_draw_scanline_reset_code_cache) */
void gsjit_reset(void)
{
	gs_code_map_clear(&s_map);
	gs_code_reserve_reset();
}

void gsjit_release(void)
{
	gs_code_map_free(&s_map);
	code_reserve_init(&g_gs_code_reserve.reserve);
}
