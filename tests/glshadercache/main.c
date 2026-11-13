/* GLShaderCache.c against the program tests' stub GL driver and real
 * files. A long run of program requests, reopenings and damaged cache
 * files makes the same GL calls, gives the same answers and leaves the
 * same bytes on disk as the C++ class it replaced (the trace below was
 * taken from it): caches written before read the same. Then what is new:
 * a session's additions go into index slots and a binary buffer made as
 * the cache opens, and an entry whose offset and size wrap is no entry. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "GS/Renderers/OpenGL/GLShaderCache.h"
#include "ShaderCacheVersion.h"
#include "../glprogram/stubs.h"

extern GLint g_binary_formats;

static gs_gl_shader_cache s_cache;
static gs_gl_program s_progs[4];

#define S_INIT()                 gs_gl_shader_cache_init(&s_cache)
#define S_OPEN(gles, dir)        gs_gl_shader_cache_open(&s_cache, gles, dir)
#define S_CLOSE()                gs_gl_shader_cache_close(&s_cache)
#define S_FREE()                 gs_gl_shader_cache_free(&s_cache)
#define S_DESTROY(i)             gs_gl_program_destroy(&s_progs[i])
#define S_GET(i, v, vl, f, fl)   gs_gl_shader_cache_get_program(&s_cache, &s_progs[i], v, vl, f, fl)
#define S_PROGRAM_ID(i)          s_progs[i].program_id

#include "scenario.h"

/* The C++ class's trace of this scenario */
#define EXPECTED_TRACE 0xb5f06262u

static int s_failed = 0;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

static void session_room(const char* dir)
{
	gs_gl_shader_cache c;
	gs_gl_program p;
	const unsigned char* slots;
	const void* blob;
	u32 i;

	remove(s_index_path);
	remove(s_blob_path);
	gs_gl_shader_cache_init(&c);
	gs_gl_program_init(&p);
	check(gs_gl_shader_cache_open(&c, 0, dir), "opened");
	slots = c.index.slots;
	blob  = c.blob.items;
	check(slots != NULL && blob != NULL && c.blob.capacity >= 65536, "slots and a binary buffer made as it opens");
	for (i = 0; i < 200; i++)
	{
		gs_gl_program_destroy(&p);
		gs_gl_shader_cache_get_program(&c, &p, s_text, 10 + i % 60, s_text, 10 + i / 60);
	}
	check(c.index.slots == slots && c.blob.items == blob, "a session's programs grow neither");
	check(c.index.count > 0, "and some were added");
	gs_gl_program_destroy(&p);
	gs_gl_shader_cache_free(&c);
}

/* An entry whose offset and size wrap past 4 GiB, which a 32-bit sum let
 * through as inside the blob: before another entry it is damage, and the
 * cache is made new */
static void wrapped_entry(const char* dir)
{
	gs_gl_shader_cache c;
	FILE* f;
	unsigned char entry[52];
	const u32 version = SHADER_CACHE_VERSION, off = 0xFFFFFFF0u, size = 0x20u, fmt = 1;

	remove(s_index_path);
	remove(s_blob_path);
	memset(entry, 0, sizeof(entry));
	memcpy(entry + 40, &off, 4);
	memcpy(entry + 44, &size, 4);
	memcpy(entry + 48, &fmt, 4);
	f = fopen(s_index_path, "wb");
	fwrite(&version, 4, 1, f);
	fwrite(entry, sizeof(entry), 1, f);
	memset(entry + 40, 0, 12); /* (a good one after it: the blob's first byte) */
	entry[44] = 1;
	fwrite(entry, sizeof(entry), 1, f);
	fclose(f);
	f = fopen(s_blob_path, "wb");
	fwrite(entry, sizeof(entry), 1, f);
	fclose(f);

	gs_gl_shader_cache_init(&c);
	check(gs_gl_shader_cache_open(&c, 0, dir), "opened over a wrapping entry");
	check(c.index.count == 0, "the wrapping entry is not taken");
	gs_gl_shader_cache_free(&c);
	f = fopen(s_index_path, "rb");
	check(f != NULL, "the cache was made new");
	if (f)
	{
		fseek(f, 0, SEEK_END);
		check(ftell(f) == 4, "with only its version");
		fclose(f);
	}
}

static void no_binaries(const char* dir)
{
	gs_gl_shader_cache c;
	gs_gl_program p;
	remove(s_index_path);
	remove(s_blob_path);
	g_binary_formats = 0;
	gs_gl_shader_cache_init(&c);
	gs_gl_program_init(&p);
	check(gs_gl_shader_cache_open(&c, 0, dir), "opened without binary formats");
	check(c.blob_file == NULL && c.names == NULL, "no files then");
	gs_gl_shader_cache_get_program(&c, &p, s_text, 20, s_text, 40);
	check(c.index.count == 0, "nothing kept");
	gs_gl_program_destroy(&p);
	gs_gl_shader_cache_free(&c);
	g_binary_formats = 1;
}

int main(int argc, char** argv)
{
	const char* dir = ".";
	u32 trace;
	int i;

	/* (in the files' folder, so the paths logged are the same anywhere) */
	if (argc > 1 && chdir(argv[1]) != 0)
	{
		printf("FAIL: no folder %s\n", argv[1]);
		return 1;
	}
	for (i = 0; i < 4; i++)
		gs_gl_program_init(&s_progs[i]);
	trace = run_scenario(dir, 4000);
	printf("trace %08x after %u GL calls\n", (unsigned)trace, (unsigned)g_calls);
	if (argc > 2 && strcmp(argv[2], "--print") == 0)
		return 0;
	check(trace == EXPECTED_TRACE, "the GL calls, answers and files match the C++ class");

	session_room(dir);
	wrapped_entry(dir);
	no_binaries(dir);

	if (s_failed)
		return 1;
	printf("glshadercache: ok\n");
	return 0;
}
