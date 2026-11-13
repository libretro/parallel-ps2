/* The GL device's vertex arrays (GS/Renderers/OpenGL/GLVertexInput.c)
 * against a stub driver that keeps vertex array and buffer names apart, as
 * GL does: the layout and expansion indices it sets up, and that teardown
 * deletes exactly what was made. The device's teardown deleted the
 * expansion index buffer's name as a vertex array, which leaked the
 * expansion array and could delete another array of the same name. */

#include <stdio.h>
#include <string.h>

#include "glad.h"
#include "GS/Renderers/OpenGL/GLState.h"
#include "GS/Renderers/OpenGL/GLVertexInput.h"

#define MAX_NAMES 64

retro_log_printf_t log_cb = NULL;

static int s_vao_live[MAX_NAMES], s_buf_live[MAX_NAMES];
static GLuint s_next_vao, s_next_buf, s_bound_vao, s_bound_array, s_bound_element;
static int s_bad_deletes, s_enabled;
static GLuint s_ibo_of_vao[MAX_NAMES];
static GLsizeiptr s_ibo_size;
static u16 s_ibo_first[12], s_ibo_last[6];
static GLuint s_ssbo;
static GLsizeiptr s_ssbo_size;
typedef struct attrib
{
	GLint size;
	GLenum type;
	int integer, normalized;
	GLsizei stride;
	size_t offset;
	GLuint buffer;
} attrib;
static attrib s_attribs[8];
static int s_failed;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

static void APIENTRY s_GenVertexArrays(GLsizei n, GLuint* ids)
{
	(void)n;
	ids[0] = ++s_next_vao;
	s_vao_live[ids[0]] = 1;
}

static void APIENTRY s_DeleteVertexArrays(GLsizei n, const GLuint* ids)
{
	(void)n;
	if (ids[0] >= MAX_NAMES || !s_vao_live[ids[0]])
		s_bad_deletes++;
	else
		s_vao_live[ids[0]] = 0;
}

static void APIENTRY s_BindVertexArray(GLuint id)
{
	s_bound_vao = id;
}

static void APIENTRY s_GenBuffers(GLsizei n, GLuint* ids)
{
	(void)n;
	ids[0] = ++s_next_buf;
	s_buf_live[ids[0]] = 1;
}

static void APIENTRY s_DeleteBuffers(GLsizei n, const GLuint* ids)
{
	(void)n;
	if (ids[0] >= MAX_NAMES || !s_buf_live[ids[0]])
		s_bad_deletes++;
	else
		s_buf_live[ids[0]] = 0;
}

static void APIENTRY s_BindBuffer(GLenum target, GLuint id)
{
	if (target == GL_ARRAY_BUFFER)
		s_bound_array = id;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
	{
		/* (element buffers are vertex array state) */
		s_bound_element = id;
		s_ibo_of_vao[s_bound_vao] = id;
	}
}

static void APIENTRY s_BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
	const u16* idx = (const u16*)data;
	(void)usage;
	check(target == GL_ELEMENT_ARRAY_BUFFER && data != NULL, "only the expansion indices are uploaded");
	s_ibo_size = size;
	memcpy(s_ibo_first, idx, sizeof(s_ibo_first));
	memcpy(s_ibo_last, idx + size / 2 - 6, sizeof(s_ibo_last));
}

static void APIENTRY s_EnableVertexAttribArray(GLuint index)
{
	s_enabled |= 1 << index;
}

static void set_attrib(GLuint index, GLint size, GLenum type, int integer, int normalized, GLsizei stride, const void* ptr)
{
	attrib* a     = &s_attribs[index];
	a->size       = size;
	a->type       = type;
	a->integer    = integer;
	a->normalized = normalized;
	a->stride     = stride;
	a->offset     = (size_t)ptr;
	a->buffer     = s_bound_array;
}

static void APIENTRY s_VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* ptr)
{
	set_attrib(index, size, type, 0, normalized, stride, ptr);
}

static void APIENTRY s_VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* ptr)
{
	set_attrib(index, size, type, 1, 0, stride, ptr);
}

static void APIENTRY s_BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
	check(target == GL_SHADER_STORAGE_BUFFER && index == 2 && offset == 0, "the vertices are storage buffer 2");
	s_ssbo      = buffer;
	s_ssbo_size = size;
}

PFNGLGENVERTEXARRAYSPROC glad_glGenVertexArrays                 = s_GenVertexArrays;
PFNGLDELETEVERTEXARRAYSPROC glad_glDeleteVertexArrays           = s_DeleteVertexArrays;
PFNGLBINDVERTEXARRAYPROC glad_glBindVertexArray                 = s_BindVertexArray;
PFNGLGENBUFFERSPROC glad_glGenBuffers                           = s_GenBuffers;
PFNGLDELETEBUFFERSPROC glad_glDeleteBuffers                     = s_DeleteBuffers;
PFNGLBINDBUFFERPROC glad_glBindBuffer                           = s_BindBuffer;
PFNGLBUFFERDATAPROC glad_glBufferData                           = s_BufferData;
PFNGLENABLEVERTEXATTRIBARRAYPROC glad_glEnableVertexAttribArray = s_EnableVertexAttribArray;
PFNGLVERTEXATTRIBPOINTERPROC glad_glVertexAttribPointer         = s_VertexAttribPointer;
PFNGLVERTEXATTRIBIPOINTERPROC glad_glVertexAttribIPointer       = s_VertexAttribIPointer;
PFNGLBINDBUFFERRANGEPROC glad_glBindBufferRange                 = s_BindBufferRange;

static int live(const int* names)
{
	int i, n = 0;
	for (i = 0; i < MAX_NAMES; i++)
		n += names[i];
	return n;
}

/* The stream buffers the device makes first: only their names and sizes
 * matter here */
static void stream(gs_gl_stream_buffer* sb, GLenum target, u32 size)
{
	memset(sb, 0, sizeof(*sb));
	sb->target = target;
	sb->size   = size;
	s_GenBuffers(1, &sb->buffer_id);
}

/* The device's teardown before this, to show the checks catch it */
static void old_teardown(gs_gl_vertex_input* vi)
{
	glBindVertexArray(0);
	if (vi->expand_ibo != 0)
		glDeleteVertexArrays(1, &vi->expand_ibo);
	if (vi->vao != 0)
		glDeleteVertexArrays(1, &vi->vao);
	if (vi->expand_ibo)
		glDeleteBuffers(1, &vi->expand_ibo);
}

static void reset_driver(void)
{
	memset(s_vao_live, 0, sizeof(s_vao_live));
	memset(s_buf_live, 0, sizeof(s_buf_live));
	memset(s_ibo_of_vao, 0, sizeof(s_ibo_of_vao));
	memset(s_attribs, 0, sizeof(s_attribs));
	s_bad_deletes = s_enabled = 0;
	s_ibo_size = s_ssbo_size = 0;
	s_ssbo = 0;
	/* names in both spaces start alike, as drivers hand them out */
	s_next_vao = s_next_buf = 0;
	gs_gl_state_clear();
}

static void old_is_caught(void)
{
	gs_gl_vertex_input vi;
	gs_gl_stream_buffer vb, ib;
	reset_driver();
	gs_gl_vertex_input_begin(&vi);
	stream(&vb, GL_ARRAY_BUFFER, 1u << 20);
	stream(&ib, GL_ELEMENT_ARRAY_BUFFER, 1u << 19);
	gs_gl_vertex_input_finish(&vi, &vb, &ib, 1, 1);
	old_teardown(&vi);
	check(s_bad_deletes != 0 || live(s_vao_live) != 0, "the old teardown fails these checks");
}

static void run(int expand, int ssbo)
{
	static const struct
	{
		GLint size;
		GLenum type;
		int integer, normalized;
		GLsizei stride;
		size_t offset;
	} layout[8] = {
		{2, GL_FLOAT, 0, 0, 32, 0}, {2, GL_FLOAT, 0, 0, 32, 16}, {4, GL_UNSIGNED_BYTE, 0, 0, 32, 8},
		{1, GL_FLOAT, 0, 0, 32, 12}, {2, GL_UNSIGNED_SHORT, 1, 0, 32, 16}, {1, GL_UNSIGNED_INT, 1, 0, 32, 20},
		{2, GL_UNSIGNED_SHORT, 1, 0, 32, 24}, {4, GL_UNSIGNED_BYTE, 0, 1, 32, 28}};
	gs_gl_vertex_input vi;
	gs_gl_stream_buffer vb, ib;
	int i;

	reset_driver();
	gs_gl_vertex_input_begin(&vi);
	check(vi.vao != 0 && s_bound_vao == vi.vao && g_gl_state.vao == vi.vao, "the drawing array is made and bound");
	stream(&vb, GL_ARRAY_BUFFER, 1u << 20);
	stream(&ib, GL_ELEMENT_ARRAY_BUFFER, 1u << 19);

	check(gs_gl_vertex_input_finish(&vi, &vb, &ib, expand, ssbo), "finished");
	check(s_enabled == 0xFF, "eight attributes enabled");
	for (i = 0; i < 8; i++)
	{
		const attrib* a = &s_attribs[i];
		check(a->size == layout[i].size && a->type == layout[i].type && a->integer == layout[i].integer
				&& a->normalized == layout[i].normalized && a->stride == layout[i].stride && a->offset == layout[i].offset,
			"the attribute layout is the device's");
		check(a->buffer == vb.buffer_id, "attributes read the vertex stream");
	}
	check(s_ibo_of_vao[vi.vao] == ib.buffer_id, "the drawing array takes the index stream");

	if (expand)
	{
		check(vi.expand_vao != 0 && vi.expand_vao != vi.vao && s_vao_live[vi.expand_vao], "the expansion array is made");
		check(vi.expand_ibo != 0 && s_buf_live[vi.expand_ibo], "the expansion indices are a buffer");
		check(s_ibo_of_vao[vi.expand_vao] == vi.expand_ibo, "the expansion array takes them");
		check(s_bound_array == vb.buffer_id, "uploads still go to the vertex stream");
		check(s_ibo_size == GS_GL_EXPAND_QUADS * 6 * 2, "the expansion indices' size");
		check(s_ibo_first[0] == 0 && s_ibo_first[1] == 1 && s_ibo_first[2] == 2 && s_ibo_first[3] == 1
				&& s_ibo_first[4] == 2 && s_ibo_first[5] == 3 && s_ibo_first[6] == 4 && s_ibo_first[11] == 7,
			"two triangles a quad");
		check(s_ibo_last[0] == (GS_GL_EXPAND_QUADS - 1) * 4 && s_ibo_last[5] == (GS_GL_EXPAND_QUADS - 1) * 4 + 3,
			"up to the last quad");
		check(ssbo ? (s_ssbo == vb.buffer_id && s_ssbo_size == (GLsizeiptr)vb.size) : s_ssbo == 0,
			"the vertex stream is a storage buffer when the shader reads it so");
	}
	else
		check(vi.expand_vao == 0 && vi.expand_ibo == 0 && s_ibo_size == 0, "no expansion objects without expand");

	gs_gl_vertex_input_destroy(&vi);
	check(s_bad_deletes == 0, "nothing deleted that was not made, or in the wrong name space");
	check(live(s_vao_live) == 0, "every vertex array deleted");
	check(live(s_buf_live) == 2 && s_buf_live[vb.buffer_id] && s_buf_live[ib.buffer_id], "the expansion indices deleted, the streams left");
	check(s_bound_vao == 0 && g_gl_state.vao == 0, "no array bound, and the state cache knows");
	check(vi.vao == 0 && vi.expand_vao == 0 && vi.expand_ibo == 0, "none after");
	gs_gl_vertex_input_destroy(&vi);
	check(s_bad_deletes == 0, "a second destroy deletes nothing");
}

int main(void)
{
	run(1, 1);
	run(1, 0);
	run(0, 0);
	old_is_caught();
	if (s_failed)
		return 1;
	printf("glvertexinput: ok\n");
	return 0;
}
