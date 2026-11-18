/* The stub driver's log and answers, and what the device needs of the
 * rest of the core, kept to what both the C++ device it replaced and the
 * C one call: the device is all that runs. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "glad.h"
#include "stubs.h"

u64 g_hash  = 1469598103934665603ULL;
u64 g_words = 0;
static FILE* s_dump;
static unsigned s_next_id;
static u64 s_next_sync;

void log_word(u64 v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		g_hash ^= (v >> (i * 8)) & 0xFF;
		g_hash *= 1099511628211ULL;
	}
	g_words++;
	if (s_dump || getenv("GLDEV_DUMP"))
	{
		if (!s_dump)
			s_dump = fopen(getenv("GLDEV_DUMP"), "w");
		if (s_dump)
			fprintf(s_dump, "%llx\n", (unsigned long long)v);
	}
}

void log_float(float f)
{
	u32 v;
	memcpy(&v, &f, sizeof(v));
	log_word(v);
}

void log_double(double d)
{
	u64 v;
	memcpy(&v, &d, sizeof(v));
	log_word(v);
}

void log_bytes(const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	size_t i;
	log_word(n);
	for (i = 0; i < n; i += 8)
	{
		u64 v = 0;
		size_t k;
		for (k = 0; k < 8 && i + k < n; k++)
			v |= (u64)b[i + k] << (k * 8);
		log_word(v);
	}
}

void log_string(const char* s)
{
	log_bytes(s, strlen(s));
}

void log_name(const char* s)
{
	if (s_dump)
		fprintf(s_dump, "%s\n", s);
	log_string(s);
}

void stub_gen(int n, unsigned* ids)
{
	int i;
	for (i = 0; i < n; i++)
	{
		ids[i] = ++s_next_id;
		log_word(ids[i]);
	}
}

unsigned stub_new_id(void)
{
	return ++s_next_id;
}

void* stub_new_sync(void)
{
	return (void*)(uintptr_t)++s_next_sync;
}

u64 stub_sync_id(const void* sync)
{
	return (u64)(uintptr_t)sync;
}

void stub_shader_source(int count, const char* const* strings, const int* lengths)
{
	int i;
	for (i = 0; i < count; i++)
		log_bytes(strings[i], (lengths && lengths[i] >= 0) ? (size_t)lengths[i] : strlen(strings[i]));
}

void stub_get_integerv(unsigned pname, int* data)
{
	switch (pname)
	{
		case GL_MAJOR_VERSION: data[0] = g_gles ? 3 : 4; break;
		case GL_MINOR_VERSION: data[0] = g_gles ? 1 : 6; break;
		case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: data[0] = 256; break;
		case GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS: data[0] = 16; break;
		case GL_ALIASED_POINT_SIZE_RANGE: data[0] = 1; data[1] = 2047; break;
		default: data[0] = 0; break;
	}
}

int stub_get_object_iv(unsigned pname)
{
	static unsigned polls;
	switch (pname)
	{
		case GL_INFO_LOG_LENGTH:
		case GL_PROGRAM_BINARY_LENGTH:
			return 0;
		case GL_COMPLETION_STATUS_KHR:
			/* (one poll in three finds the driver's threads not done) */
			return (polls++ % 3) != 0;
		default:
			return 1; /* compiled, linked, done compiling */
	}
}

const unsigned char* stub_get_string(unsigned name)
{
	switch (name)
	{
		case GL_VENDOR: return (const unsigned char*)"Stub Vendor";
		case GL_RENDERER: return (const unsigned char*)"stub renderer";
		case GL_VERSION: return (const unsigned char*)(g_gles ? "OpenGL ES 3.1" : "4.6");
		default: return NULL;
	}
}

static void* s_maps[16];
static int s_nmaps;

void* stub_map(size_t size)
{
	void* p = malloc(size);
	if (p && s_nmaps < 16)
		s_maps[s_nmaps++] = p;
	return p;
}

void stub_unmap_all(void)
{
	while (s_nmaps > 0)
		free(s_maps[--s_nmaps]);
}

/* ==== the frontend ==== */

static void stub_log(enum retro_log_level level, const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	log_word(0x10000u | (u32)level);
	log_string(buf);
}
retro_log_printf_t log_cb = stub_log;

static void stub_video(const void* data, unsigned width, unsigned height, size_t pitch)
{
	log_name("video_cb");
	log_word((u64)(uintptr_t)data);
	log_word(width);
	log_word(height);
	log_word(pitch);
}
retro_video_refresh_t video_cb = stub_video;

static uintptr_t stub_current_framebuffer(void)
{
	return 7;
}

static retro_proc_address_t stub_proc_address(const char* sym)
{
	(void)sym;
	return NULL;
}

struct retro_hw_render_callback hw_render;

void stub_frontend_init(void)
{
	hw_render.get_current_framebuffer = stub_current_framebuffer;
	hw_render.get_proc_address        = stub_proc_address;
}
