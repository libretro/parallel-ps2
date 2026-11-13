/* A GL driver for the stream buffer tests: every entry point the stream
 * buffer calls folds its name and arguments into one trace hash, buffer
 * storage maps to real memory (so writes past a map are caught), and the
 * fences are counted so none leaks or is deleted twice. */

#include <stdlib.h>
#include <string.h>

#include "glad.h"
#include "common/Pcsx2Defs.h"
#include "stubs.h"

#define MAX_MAPS 8
#define MAX_SYNCS   65536

u32 g_trace = 2166136261u;
u32 g_calls = 0;
int GLAD_GL_VERSION_4_4 = 0, GLAD_GL_ARB_buffer_storage = 0, GLAD_GL_EXT_buffer_storage = 0;
const char* g_vendor = "NVIDIA Corporation";
int g_fail_storage = 0, g_fail_data = 0, g_fail_map = 0;
int g_trace_sync_deletes = 1;
int g_live_syncs = 0, g_null_waits = 0, g_bad_deletes = 0, g_live_maps = 0, g_live_buffers = 0;

static GLuint s_next_buffer;
static u32 s_next_sync;
static GLenum s_error;
static struct { GLuint id; void* ptr; } s_maps[MAX_MAPS];
static GLuint s_bound[4];
static unsigned char s_sync_live[MAX_SYNCS];

void tr(u32 v)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		g_trace ^= (v >> (i * 8)) & 0xFF;
		g_trace *= 16777619u;
	}
}

static void tr_bytes(const void* data, size_t n)
{
	const unsigned char* b = (const unsigned char*)data;
	size_t i;
	tr((u32)n);
	for (i = 0; i < n; i++)
	{
		g_trace ^= b[i];
		g_trace *= 16777619u;
	}
}

static void call(u32 fn)
{
	g_calls++;
	tr(0xC0DE0000u | fn);
}

void stubs_reset(void)
{
	int i;
	for (i = 0; i < MAX_MAPS; i++)
	{
		free(s_maps[i].ptr);
		s_maps[i].ptr = NULL;
		s_maps[i].id  = 0;
	}
	memset(s_bound, 0, sizeof(s_bound));
	memset(s_sync_live, 0, sizeof(s_sync_live));
	g_trace = 2166136261u;
	g_calls = 0;
	s_next_buffer = 0;
	s_next_sync = 0;
	s_error = GL_NO_ERROR;
	g_live_syncs = g_null_waits = g_bad_deletes = g_live_maps = g_live_buffers = 0;
}

static int slot(GLenum target)
{
	switch (target)
	{
		case GL_ARRAY_BUFFER: return 0;
		case GL_ELEMENT_ARRAY_BUFFER: return 1;
		case GL_UNIFORM_BUFFER: return 2;
		default: return 3;
	}
}

static GLenum APIENTRY s_GetError(void)
{
	GLenum e = s_error;
	call(1);
	s_error = GL_NO_ERROR;
	return e;
}

static void APIENTRY s_GenBuffers(GLsizei n, GLuint* ids)
{
	call(2);
	tr((u32)n);
	ids[0] = ++s_next_buffer;
	g_live_buffers++;
}

static void APIENTRY s_DeleteBuffers(GLsizei n, const GLuint* ids)
{
	call(3);
	tr((u32)n);
	tr(ids[0]);
	g_live_buffers--;
}

static void APIENTRY s_BindBuffer(GLenum target, GLuint id)
{
	call(4);
	tr(target);
	tr(id);
	s_bound[slot(target)] = id;
}

static void storage(GLenum target, GLsizeiptr size, GLbitfield flags, u32 fn)
{
	call(fn);
	tr(target);
	tr((u32)size);
	tr(flags);
	if (g_fail_storage)
	{
		g_fail_storage = 0;
		s_error = GL_OUT_OF_MEMORY;
	}
}

static void APIENTRY s_BufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
{
	(void)data;
	storage(target, size, flags, 5);
}

static void APIENTRY s_BufferStorageEXT(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags)
{
	(void)data;
	storage(target, size, flags, 6);
}

static void* APIENTRY s_MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
	const GLuint id = s_bound[slot(target)];
	int i;
	call(7);
	tr(target);
	tr((u32)offset);
	tr((u32)length);
	tr(access);
	if (g_fail_map)
	{
		g_fail_map = 0;
		return NULL;
	}
	for (i = 0; i < MAX_MAPS && s_maps[i].ptr; i++)
	{
	}
	if (i == MAX_MAPS)
		return NULL;
	s_maps[i].id  = id;
	s_maps[i].ptr = malloc((size_t)length);
	g_live_maps++;
	return s_maps[i].ptr;
}

static GLboolean APIENTRY s_UnmapBuffer(GLenum target)
{
	const GLuint id = s_bound[slot(target)];
	int i;
	call(8);
	tr(target);
	for (i = 0; i < MAX_MAPS; i++)
	{
		if (s_maps[i].ptr && s_maps[i].id == id)
		{
			free(s_maps[i].ptr);
			s_maps[i].ptr = NULL;
			g_live_maps--;
		}
	}
	return GL_TRUE;
}

static const GLubyte* APIENTRY s_GetString(GLenum name)
{
	call(9);
	tr(name);
	return (const GLubyte*)g_vendor;
}

static void APIENTRY s_BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
	call(10);
	tr(target);
	tr((u32)size);
	tr(usage);
	if (data)
		tr_bytes(data, (size_t)size);
	else if (g_fail_data)
	{
		g_fail_data = 0;
		s_error = GL_OUT_OF_MEMORY;
	}
}

static void APIENTRY s_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
	call(11);
	tr(target);
	tr((u32)offset);
	tr_bytes(data, (size_t)size);
}

static GLsync APIENTRY s_FenceSync(GLenum condition, GLbitfield flags)
{
	call(12);
	tr(condition);
	tr(flags);
	s_next_sync++;
	s_sync_live[s_next_sync % MAX_SYNCS] = 1;
	g_live_syncs++;
	return (GLsync)(uintptr_t)s_next_sync;
}

static GLenum APIENTRY s_ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
	call(13);
	tr((u32)(uintptr_t)sync);
	tr(flags);
	tr((u32)timeout);
	if (!sync)
		g_null_waits++;
	return GL_ALREADY_SIGNALED;
}

static void APIENTRY s_DeleteSync(GLsync sync)
{
	const u32 id = (u32)(uintptr_t)sync;
	if (g_trace_sync_deletes)
	{
		call(14);
		tr(id);
	}
	if (!sync)
		return; /* (silently ignored, as GL does) */
	if (!s_sync_live[id % MAX_SYNCS])
	{
		g_bad_deletes++;
		return;
	}
	s_sync_live[id % MAX_SYNCS] = 0;
	g_live_syncs--;
}

static void APIENTRY s_FlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length)
{
	call(15);
	tr(target);
	tr((u32)offset);
	tr((u32)length);
}

PFNGLGETERRORPROC glad_glGetError                               = s_GetError;
PFNGLGENBUFFERSPROC glad_glGenBuffers                           = s_GenBuffers;
PFNGLDELETEBUFFERSPROC glad_glDeleteBuffers                     = s_DeleteBuffers;
PFNGLBINDBUFFERPROC glad_glBindBuffer                           = s_BindBuffer;
PFNGLBUFFERSTORAGEPROC glad_glBufferStorage                     = s_BufferStorage;
PFNGLBUFFERSTORAGEEXTPROC glad_glBufferStorageEXT               = s_BufferStorageEXT;
PFNGLMAPBUFFERRANGEPROC glad_glMapBufferRange                   = s_MapBufferRange;
PFNGLUNMAPBUFFERPROC glad_glUnmapBuffer                         = s_UnmapBuffer;
PFNGLGETSTRINGPROC glad_glGetString                             = s_GetString;
PFNGLBUFFERDATAPROC glad_glBufferData                           = s_BufferData;
PFNGLBUFFERSUBDATAPROC glad_glBufferSubData                     = s_BufferSubData;
PFNGLFENCESYNCPROC glad_glFenceSync                             = s_FenceSync;
PFNGLCLIENTWAITSYNCPROC glad_glClientWaitSync                   = s_ClientWaitSync;
PFNGLDELETESYNCPROC glad_glDeleteSync                           = s_DeleteSync;
PFNGLFLUSHMAPPEDBUFFERRANGEPROC glad_glFlushMappedBufferRange   = s_FlushMappedBufferRange;
