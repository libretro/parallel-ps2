/* GLStreamBuffer.c against a stub GL driver. A long run of creates on
 * every kind of driver, maps, writes, unmaps and binds makes the same GL
 * calls in the same order with the same arguments as the C++ classes it
 * replaced (the trace below was taken from them). Then what they got
 * wrong: a storage buffer that had wrapped leaked the fences it had not
 * waited for, and read a fence past the array when its blocks were all
 * fenced; and a mapping the driver refused was used as a NULL pointer. */

#include <stdio.h>
#include <string.h>

#include "GS/Renderers/OpenGL/GLStreamBuffer.h"
#include "stubs.h"

static gs_gl_stream_buffer s_bufs[3];

static void s_map(int i, u32 align, u32 min_size, void* out)
{
	const gs_gl_stream_map r = gs_gl_stream_buffer_map(&s_bufs[i], align, min_size);
	memcpy(out, &r, sizeof(r));
}

#define S_CREATE(i, t, s)   gs_gl_stream_buffer_create(&s_bufs[i], t, s)
#define S_DESTROY(i)        gs_gl_stream_buffer_destroy(&s_bufs[i])
#define S_VALID(i)          (s_bufs[i].kind != GS_GL_STREAM_NONE)
#define S_CHUNK(i)          gs_gl_stream_buffer_chunk_size(&s_bufs[i])
#define S_MAP(i, a, n, m)   s_map(i, a, n, m)
#define S_UNMAP(i, n)       gs_gl_stream_buffer_unmap(&s_bufs[i], n)
#define S_BIND(i)           gs_gl_stream_buffer_bind(&s_bufs[i])
#define S_UNBIND(i)         gs_gl_stream_buffer_unbind(&s_bufs[i])

#include "scenario.h"

/* The C++ classes' trace of this scenario */
#define EXPECTED_TRACE 0x1a60dc3cu

static int s_failed = 0;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

static void released(const char* when)
{
	char what[128];
	sprintf(what, "%s: no fence left", when);
	check(g_live_syncs == 0, what);
	sprintf(what, "%s: no fence deleted twice", when);
	check(g_bad_deletes == 0, what);
	sprintf(what, "%s: no mapping or buffer left", when);
	check(g_live_maps == 0 && g_live_buffers == 0, what);
}

/* Every block fenced, none waited for yet: the case that read past the
 * fences. Then the destroy must delete all sixteen. */
static void all_fenced(void)
{
	gs_gl_stream_buffer sb;
	gs_gl_stream_map m;
	u32 i;
	stubs_reset();
	GLAD_GL_VERSION_4_4 = 1;
	check(gs_gl_stream_buffer_create(&sb, GL_ARRAY_BUFFER, 4096), "storage buffer made");
	check(sb.kind == GS_GL_STREAM_STORAGE, "of the storage kind");
	for (i = 0; i < 16; i++)
	{
		m = gs_gl_stream_buffer_map(&sb, 1, 256);
		check(m.buffer_offset == i * 256, "maps follow on");
		gs_gl_stream_buffer_unmap(&sb, 256);
	}
	/* the next map fences all sixteen and wraps */
	m = gs_gl_stream_buffer_map(&sb, 1, 256);
	check(m.buffer_offset == 0, "the map past the end wraps");
	check(g_live_syncs == 14, "fourteen fences outstanding after the wrap (the first two waited for)");
	gs_gl_stream_buffer_destroy(&sb);
	released("all fenced");
}

static void refused_map(void)
{
	gs_gl_stream_buffer sb;
	gs_gl_stream_map m;
	stubs_reset();
	GLAD_GL_VERSION_4_4 = 1;
	g_vendor   = "ARM";
	g_fail_map = 1;
	check(gs_gl_stream_buffer_create(&sb, GL_UNIFORM_BUFFER, 1024), "a buffer is still made");
	check(sb.kind == GS_GL_STREAM_DATA, "of the copied kind when the mapping is refused");
	m = gs_gl_stream_buffer_map(&sb, 16, 1024);
	check(m.pointer != NULL, "and its map has room");
	memset(m.pointer, 0x5A, 1024);
	gs_gl_stream_buffer_unmap(&sb, 1024);
	gs_gl_stream_buffer_destroy(&sb);
	check(sb.kind == GS_GL_STREAM_NONE && sb.ptr == NULL, "destroy leaves none");
	gs_gl_stream_buffer_destroy(&sb);
	released("refused map");
}

int main(int argc, char** argv)
{
	u32 trace = run_scenario(200000);
	printf("trace %08x after %u GL calls\n", (unsigned)trace, (unsigned)g_calls);
	if (argc > 1 && strcmp(argv[1], "--print") == 0)
		return 0;
	check(trace == EXPECTED_TRACE, "the GL calls match the C++ classes");
	check(g_null_waits == 0, "no wait on a missing fence");
	released("scenario");

	all_fenced();
	refused_map();

	if (s_failed)
		return 1;
	printf("glstream: ok\n");
	return 0;
}
