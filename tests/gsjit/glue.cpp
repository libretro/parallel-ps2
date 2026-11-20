/* The software renderer's code-generator map over the real GSCodeReserve,
 * with a stand-in generator, for main.c. A key's low 32 bits name the
 * function and its high 32 bits the bytes it takes; the generator writes
 * the name's low byte over them, and like the real generator's emitter
 * stops writing at the end of the room it is given. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSFunctionMap.h"

static void gsjit_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
extern "C" retro_log_printf_t log_cb = gsjit_log;

typedef const u8* FakeFn;

struct FakeCG
{
	u8* m_code;
	size_t m_size;

	FakeCG(u64 key, void* code, size_t room)
		: m_code(static_cast<u8*>(code))
	{
		const size_t want = (size_t)(key >> 32);
		m_size = want < room ? want : room;
		memset(m_code, (int)(key & 0xff), m_size);
	}
	size_t getSize() const { return m_size; }
	const u8* getCode() const { return m_code; }
};

/* The map's allocations: GSFlatMap.c's calloc is this (build.sh). */
static unsigned s_callocs;
extern "C" void* gsjit_calloc(size_t n, size_t size)
{
	s_callocs++;
	return calloc(n, size);
}
extern "C" unsigned gsjit_allocations(void) { return s_callocs; }

static GSCodeGeneratorFunctionMap<FakeCG, u64, FakeFn, 256> s_map;

extern "C" void gsjit_assign(unsigned char* base, size_t size)
{
	g_gs_code_reserve.reserve.baseptr   = base;
	g_gs_code_reserve.reserve.size      = size;
	g_gs_code_reserve.reserve.allocator = NULL;
	gs_code_reserve_reset();
}

extern "C" const unsigned char* gsjit_get(unsigned long long key)
{
	return s_map[key];
}

/* What GSDrawScanline::ResetCodeCache does. */
extern "C" void gsjit_reset(void)
{
	s_map.Clear();
	gs_code_reserve_reset();
}

extern "C" void gsjit_release(void)
{
	s_map.Clear();
	code_reserve_init(&g_gs_code_reserve.reserve);
}
