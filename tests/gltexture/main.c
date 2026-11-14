/* The GL textures and read-back textures (GS/Renderers/OpenGL/
 * GSTextureOGL.c) against a stub driver and a stub device (GLDevice.h):
 * what each makes, uploads, maps and reads back, in the calls GL sees.
 * The row length GL gets for an upload through the upload buffer is in
 * GL's texels: the format's bytes are not always them (the colclip
 * target is 16 bytes a texel as RGBA32F, where the format math counts
 * 8), and in format bytes a float colclip upload read twice its rows. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/Renderers/OpenGL/GLDevice.h"
#include "GS/Renderers/OpenGL/GLState.h"
#include "GS/Renderers/OpenGL/GSTextureOGL.h"

retro_log_printf_t log_cb = NULL;

static int s_failed;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

/* --- the device ------------------------------------------------------ */

static int s_colclip_float, s_fetch, s_pbo_disabled, s_commits, s_unbinds, s_finishes;
static gs_gl_stream_buffer s_upload;
static int s_have_upload;

void gs_gl_device_commit_clear(gs_texture* t, int use_write_fbo) { (void)t; (void)use_write_fbo; s_commits++; }
void gs_gl_device_unbind_texture(gs_texture* t) { (void)t; s_unbinds++; }
gs_gl_stream_buffer* gs_gl_device_upload_buffer(void) { return s_have_upload ? &s_upload : NULL; }
int gs_gl_device_colclip_float(void) { return s_colclip_float; }
int gs_gl_device_framebuffer_fetch(void) { return s_fetch; }
int gs_gl_device_download_pbo_disabled(void) { return s_pbo_disabled; }
void gs_gl_device_finish_before_cpu_read(void) { s_finishes++; }
GLuint gs_gl_device_fbo_read(void) { return 77; }
GLuint gs_gl_device_default_framebuffer(void) { return 5; }

/* --- the driver ------------------------------------------------------ */

int GLAD_GL_VERSION_4_4, GLAD_GL_ARB_buffer_storage, GLAD_GL_EXT_buffer_storage;

static GLuint s_next;
static int s_live_textures, s_live_buffers, s_mapped_buffers, s_live_syncs;
static GLint s_unpack_row, s_pack_row, s_pack_align;
static GLint s_last_unpack_row; /* as the last upload saw it */
static GLuint s_bound_unpack, s_bound_pack, s_read_fbo, s_read_tex;
static const void* s_last_pixels;
static GLuint s_last_unpack_buffer;
static GLint s_last_x, s_last_y, s_last_level;
static GLsizei s_last_w, s_last_h, s_last_size;
static GLenum s_last_format, s_last_type;
static GLint s_swizzle_a;
static GLsizei s_storage_levels, s_storage_w, s_storage_h;
static GLenum s_storage_internal;
static int s_compressed_uploads, s_uploads, s_mipmaps;
static unsigned char s_pack_mem[1 << 20]; /* the persistent read-back map */

static GLenum APIENTRY s_GetError(void) { return GL_NO_ERROR; }
static void APIENTRY s_GenBuffers(GLsizei n, GLuint* ids) { (void)n; ids[0] = ++s_next; s_live_buffers++; }
static void APIENTRY s_DeleteBuffers(GLsizei n, const GLuint* ids) { (void)n; (void)ids; s_live_buffers--; }
static void APIENTRY s_BindBuffer(GLenum target, GLuint id)
{
	if (target == GL_PIXEL_UNPACK_BUFFER)
		s_bound_unpack = id;
	else if (target == GL_PIXEL_PACK_BUFFER)
		s_bound_pack = id;
}
static void APIENTRY s_BufferData(GLenum t, GLsizeiptr size, const void* data, GLenum usage) { (void)t; (void)size; (void)data; (void)usage; }
static void APIENTRY s_BufferSubData(GLenum t, GLintptr o, GLsizeiptr size, const void* data) { (void)t; (void)o; (void)size; (void)data; }
static const GLubyte* APIENTRY s_GetString(GLenum name) { (void)name; return (const GLubyte*)"ARM"; }
static void APIENTRY s_BufferStorage(GLenum t, GLsizeiptr size, const void* data, GLbitfield flags) { (void)t; (void)size; (void)data; (void)flags; }
static void* APIENTRY s_MapBufferRange(GLenum t, GLintptr o, GLsizeiptr len, GLbitfield access) { (void)t; (void)o; (void)len; (void)access; s_mapped_buffers++; return s_pack_mem; }
static GLboolean APIENTRY s_UnmapBuffer(GLenum t) { (void)t; s_mapped_buffers--; return GL_TRUE; }
static GLsync APIENTRY s_FenceSync(GLenum c, GLbitfield f) { (void)c; (void)f; s_live_syncs++; return (GLsync)(uintptr_t)(1000 + s_live_syncs); }
static GLenum APIENTRY s_ClientWaitSync(GLsync s, GLbitfield f, GLuint64 t) { (void)s; (void)f; (void)t; return GL_ALREADY_SIGNALED; }
static void APIENTRY s_DeleteSync(GLsync s) { if (s) s_live_syncs--; }
static void APIENTRY s_FlushMappedBufferRange(GLenum t, GLintptr o, GLsizeiptr l) { (void)t; (void)o; (void)l; }

static void APIENTRY s_CreateTextures(GLenum target, GLsizei n, GLuint* ids) { (void)target; (void)n; ids[0] = ++s_next; s_live_textures++; }
static void APIENTRY s_DeleteTextures(GLsizei n, const GLuint* ids) { (void)n; (void)ids; s_live_textures--; }
static void APIENTRY s_TextureParameteri(GLuint t, GLenum p, GLint v) { (void)t; if (p == GL_TEXTURE_SWIZZLE_A) s_swizzle_a = v; }
static void APIENTRY s_TextureStorage2D(GLuint t, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
	(void)t;
	s_storage_levels   = levels;
	s_storage_internal = internal;
	s_storage_w        = w;
	s_storage_h        = h;
}
static void APIENTRY s_PixelStorei(GLenum p, GLint v)
{
	if (p == GL_UNPACK_ROW_LENGTH)
		s_unpack_row = v;
	else if (p == GL_PACK_ROW_LENGTH)
		s_pack_row = v;
	else if (p == GL_PACK_ALIGNMENT)
		s_pack_align = v;
}
static void upload(GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
	s_last_level         = level;
	s_last_x             = x;
	s_last_y             = y;
	s_last_w             = w;
	s_last_h             = h;
	s_last_format        = format;
	s_last_type          = type;
	s_last_pixels        = pixels;
	s_last_unpack_row    = s_unpack_row;
	s_last_unpack_buffer = s_bound_unpack;
}
static void APIENTRY s_TextureSubImage2D(GLuint t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
	(void)t;
	s_uploads++;
	upload(level, x, y, w, h, format, type, pixels);
}
static void APIENTRY s_CompressedTextureSubImage2D(GLuint t, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLsizei size, const void* data)
{
	(void)t;
	s_compressed_uploads++;
	upload(level, x, y, w, h, format, 0, data);
	s_last_size = size;
}
static void APIENTRY s_GenerateTextureMipmap(GLuint t) { (void)t; s_mipmaps++; }
static void APIENTRY s_BindFramebuffer(GLenum target, GLuint fbo) { if (target == GL_READ_FRAMEBUFFER) s_read_fbo = fbo; }
static void APIENTRY s_FramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l) { (void)t; (void)a; (void)tt; (void)l; s_read_tex = tex; }
static void APIENTRY s_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels)
{
	s_last_x       = x;
	s_last_y       = y;
	s_last_w       = w;
	s_last_h       = h;
	s_last_format  = format;
	s_last_type    = type;
	s_last_pixels  = pixels;
	s_last_unpack_buffer = s_bound_pack;
}

PFNGLGETERRORPROC glad_glGetError = s_GetError;
PFNGLGENBUFFERSPROC glad_glGenBuffers = s_GenBuffers;
PFNGLDELETEBUFFERSPROC glad_glDeleteBuffers = s_DeleteBuffers;
PFNGLBINDBUFFERPROC glad_glBindBuffer = s_BindBuffer;
PFNGLBUFFERDATAPROC glad_glBufferData = s_BufferData;
PFNGLBUFFERSUBDATAPROC glad_glBufferSubData = s_BufferSubData;
PFNGLGETSTRINGPROC glad_glGetString = s_GetString;
PFNGLBUFFERSTORAGEPROC glad_glBufferStorage = s_BufferStorage;
PFNGLBUFFERSTORAGEEXTPROC glad_glBufferStorageEXT = s_BufferStorage;
PFNGLMAPBUFFERRANGEPROC glad_glMapBufferRange = s_MapBufferRange;
PFNGLUNMAPBUFFERPROC glad_glUnmapBuffer = s_UnmapBuffer;
PFNGLFENCESYNCPROC glad_glFenceSync = s_FenceSync;
PFNGLCLIENTWAITSYNCPROC glad_glClientWaitSync = s_ClientWaitSync;
PFNGLDELETESYNCPROC glad_glDeleteSync = s_DeleteSync;
PFNGLFLUSHMAPPEDBUFFERRANGEPROC glad_glFlushMappedBufferRange = s_FlushMappedBufferRange;
PFNGLCREATETEXTURESPROC glad_glCreateTextures = s_CreateTextures;
PFNGLDELETETEXTURESPROC glad_glDeleteTextures = s_DeleteTextures;
PFNGLTEXTUREPARAMETERIPROC glad_glTextureParameteri = s_TextureParameteri;
PFNGLTEXTURESTORAGE2DPROC glad_glTextureStorage2D = s_TextureStorage2D;
PFNGLPIXELSTOREIPROC glad_glPixelStorei = s_PixelStorei;
PFNGLTEXTURESUBIMAGE2DPROC glad_glTextureSubImage2D = s_TextureSubImage2D;
PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC glad_glCompressedTextureSubImage2D = s_CompressedTextureSubImage2D;
PFNGLGENERATETEXTUREMIPMAPPROC glad_glGenerateTextureMipmap = s_GenerateTextureMipmap;
PFNGLBINDFRAMEBUFFERPROC glad_glBindFramebuffer = s_BindFramebuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC glad_glFramebufferTexture2D = s_FramebufferTexture2D;
PFNGLREADPIXELSPROC glad_glReadPixels = s_ReadPixels;

/* An upload buffer of the copied kind (no buffer storage), its copy in
 * s_mem so writes past it are the stub's to see */
static void make_upload(u32 size)
{
	check(gs_gl_stream_buffer_create(&s_upload, GL_PIXEL_UNPACK_BUFFER, size), "upload buffer made");
	s_have_upload = 1;
}

/* --- the cases ------------------------------------------------------- */

static unsigned char s_src[256 * 64 * 16];

static gs_texture_rect rect(int l, int t, int r, int b)
{
	gs_texture_rect rc;
	rc.left   = l;
	rc.top    = t;
	rc.right  = r;
	rc.bottom = b;
	return rc;
}

static void creation(void)
{
	gs_texture* t;
	t = gs_texture_gl_create(GS_TEXTYPE_TEXTURE, 0, 5, 4, GS_TEX_COLOR);
	check(t->m_size.x == 1 && t->m_size.y == 5, "no size is 1");
	check(s_storage_levels == 4 && s_storage_internal == GL_RGBA8 && s_storage_w == 1 && s_storage_h == 5,
		"a texture's levels, its storage");
	t->m_ops->free(t);
	t = gs_texture_gl_create(GS_TEXTYPE_RENDER_TARGET, 64, 32, 4, GS_TEX_COLOR);
	check(t->m_mipmap_levels == 1 && s_storage_levels == 1, "a target has one level whatever is asked");
	t->m_ops->free(t);
	s_swizzle_a = 0;
	t = gs_texture_gl_create(GS_TEXTYPE_TEXTURE, 16, 16, 1, GS_TEX_UNORM8);
	check(s_swizzle_a == GL_RED && s_storage_internal == GL_R8, "a palette reads red as alpha");
	t->m_ops->free(t);
	s_fetch = 1;
	t = gs_texture_gl_create(GS_TEXTYPE_DEPTH_STENCIL, 16, 16, 1, GS_TEX_DEPTH_STENCIL);
	check(s_storage_internal == GL_DEPTH_COMPONENT32F && ((gs_texture_gl*)t)->int_shift == 2,
		"depth only where the shader fetches the framebuffer");
	t->m_ops->free(t);
	s_fetch = 0;
	check(s_live_textures == 0, "every texture deleted");
}

static void free_unbinds(void)
{
	gs_texture* t   = gs_texture_gl_create(GS_TEXTYPE_TEXTURE, 8, 8, 1, GS_TEX_COLOR);
	const GLuint id = ((gs_texture_gl*)t)->id;
	const int unbinds = s_unbinds;
	g_gl_state.tex_unit[0] = id;
	g_gl_state.tex_unit[2] = id;
	g_gl_state.tex_unit[1] = id + 1000;
	t->m_ops->free(t);
	check(s_unbinds == unbinds + 1, "a texture is detached from the framebuffer as it goes");
	check(g_gl_state.tex_unit[0] == 0 && g_gl_state.tex_unit[2] == 0 && g_gl_state.tex_unit[1] == id + 1000,
		"and forgotten by the units it was bound to, only those");
}

static void uploads(void)
{
	gs_texture* t = gs_texture_gl_create(GS_TEXTYPE_TEXTURE, 128, 64, 2, GS_TEX_COLOR);
	gs_texture_rect rc = rect(3, 4, 3 + 20, 4 + 10);
	const int uploads = s_uploads;
	size_t i;
	for (i = 0; i < sizeof(s_src); i++)
		s_src[i] = (unsigned char)(i * 13 + 1);

	/* past the levels: done, nothing sent */
	check(t->m_ops->update(t, &rc, s_src, 512, 5) == 1 && s_uploads == uploads, "a level the texture lacks is no upload");

	/* no upload buffer: straight to GL, rows in texels of the source */
	s_have_upload = 0;
	t->m_needs_mipmaps_generated = 0;
	check(t->m_ops->update(t, &rc, s_src, 512, 1), "uploaded");
	check(s_last_unpack_row == 128 && s_last_pixels == s_src && s_last_level == 1 && s_last_x == 3 && s_last_y == 4
			&& s_last_w == 20 && s_last_h == 10 && s_last_format == GL_RGBA && s_last_type == GL_UNSIGNED_BYTE,
		"straight: the source's rows, rectangle and level");
	check(s_unpack_row == 0 && t->m_needs_mipmaps_generated, "the row length restored, mipmaps due");

	/* through the upload buffer: copied packed to 64-byte rows */
	make_upload(1 << 20);
	check(t->m_ops->update(t, &rc, s_src, 512, 0), "uploaded through the buffer");
	check(s_last_unpack_buffer == s_upload.buffer_id && s_last_unpack_row == 32,
		"the buffer's rows: 80 bytes rounded to 128, 32 texels");
	check(s_last_pixels == (const void*)(uintptr_t)0, "at the buffer's start");
	check(!memcmp(s_upload.ptr, s_src, 80) && !memcmp(s_upload.ptr + 128, s_src + 512, 80)
			&& !memcmp(s_upload.ptr + 128 * 9, s_src + 512 * 9, 80),
		"each row copied to its place");
	check(s_bound_unpack == 0, "the buffer unbound after");
	t->m_ops->free(t);
	gs_gl_stream_buffer_destroy(&s_upload);
	s_have_upload = 0;
}

static void colclip_float(void)
{
	gs_texture* t;
	gs_texture_rect rc = rect(0, 0, 10, 4);
	gs_texture_map m;
	s_colclip_float = 1;
	make_upload(1 << 20);
	t = gs_texture_gl_create(GS_TEXTYPE_RENDER_TARGET, 64, 64, 1, GS_TEX_COLOR_CLIP);
	check(s_storage_internal == GL_RGBA32F && ((gs_texture_gl*)t)->int_shift == 4, "float colclip");

	/* 10 texels of 16 bytes: 160 bytes, a 192-byte row, 12 texels */
	check(t->m_ops->update(t, &rc, s_src, 1024, 0), "uploaded");
	check(s_last_unpack_row == 12, "the upload's row length in GL's texels (12, not the format's 24)");
	check(!memcmp(s_upload.ptr + 192, s_src + 1024, 160), "the second row where 12 texels put it");

	check(t->m_ops->map(t, &m, &rc, 0) && m.pitch == 192, "mapped with the same rows");
	t->m_ops->unmap(t);
	check(s_last_unpack_row == 12 && s_last_w == 10 && s_last_h == 4, "and unmapped with them");
	t->m_ops->free(t);
	gs_gl_stream_buffer_destroy(&s_upload);
	s_have_upload   = 0;
	s_colclip_float = 0;
}

static void compressed(void)
{
	gs_texture* t      = gs_texture_gl_create(GS_TEXTYPE_TEXTURE, 64, 64, 1, GS_TEX_BC1);
	gs_texture_rect rc = rect(0, 0, 16, 8);
	gs_texture_map m;
	make_upload(1 << 20);
	check(t->m_ops->update(t, &rc, s_src, 32, 0), "uploaded");
	check(s_compressed_uploads == 1 && s_last_unpack_row == 16 && s_last_size == 64 && s_last_pixels == s_src,
		"compressed: blocks of the source, 32 bytes a block row is 16 texels, two block rows");
	check(!t->m_ops->map(t, &m, &rc, 0), "and never mapped");
	t->m_ops->free(t);
	gs_gl_stream_buffer_destroy(&s_upload);
	s_have_upload = 0;
}

static void maps(void)
{
	gs_texture* t;
	gs_texture_map m;
	gs_texture_rect rc = rect(2, 2, 6, 5);
	t = gs_texture_gl_create(GS_TEXTYPE_TEXTURE, 32, 32, 1, GS_TEX_COLOR);
	check(!t->m_ops->map(t, &m, &rc, 0), "no upload buffer, no map");
	make_upload(1 << 20);
	check(t->m_ops->map(t, &m, NULL, 0) && m.pitch == 128 && m.bits == s_upload.ptr, "the whole texture mapped");
	memset(m.bits, 0xAB, 128 * 32);
	t->m_needs_mipmaps_generated = 0;
	t->m_ops->unmap(t);
	check(s_last_x == 0 && s_last_y == 0 && s_last_w == 32 && s_last_h == 32 && s_last_unpack_row == 32
			&& s_last_unpack_buffer == s_upload.buffer_id && t->m_needs_mipmaps_generated,
		"unmapped: the area mapped, from the buffer");
	check(t->m_ops->map(t, &m, &rc, 0) && m.pitch == 64, "a rectangle's rows");
	t->m_ops->unmap(t);
	check(s_last_x == 2 && s_last_y == 2 && s_last_w == 4 && s_last_h == 3, "its area");
	t->m_ops->generate_mipmap(t);
	check(s_mipmaps == 1, "mipmaps made");
	t->m_ops->free(t);
	t = gs_texture_gl_create(GS_TEXTYPE_DEPTH_STENCIL, 32, 32, 1, GS_TEX_DEPTH_STENCIL);
	check(!t->m_ops->map(t, &m, &rc, 0), "a depth target is not mapped");
	t->m_ops->free(t);
	gs_gl_stream_buffer_destroy(&s_upload);
	s_have_upload = 0;
}

static void read_back(void)
{
	gs_texture* tex = gs_texture_gl_create(GS_TEXTYPE_RENDER_TARGET, 64, 64, 1, GS_TEX_COLOR);
	gs_download_texture* d;
	gs_texture_rect drc = rect(4, 2, 4 + 10, 2 + 3);
	gs_texture_rect src = rect(8, 8, 18, 11);
	const int finishes  = s_finishes;

	/* persistently mapped */
	GLAD_GL_VERSION_4_4 = 1;
	d = gs_download_texture_gl_create(64, 64, GS_TEX_COLOR);
	check(d && d->m_map_pointer == s_pack_mem && s_mapped_buffers == 1, "a mapped pack buffer");
	d->m_ops->copy_from_texture(d, &drc, tex, &src, 0, 1);
	check(s_finishes == finishes + 1, "the GL finished before the CPU reads, where it must");
	check(d->m_current_pitch == 64 && s_pack_row == 0 && s_pack_align == 4, "packed to the copy: 40 bytes, a 64-byte row");
	check(s_read_tex == ((gs_texture_gl*)tex)->id && s_last_unpack_buffer == ((gs_download_texture_gl*)d)->buffer_id
			&& s_last_pixels == (void*)(uintptr_t)(2 * 64 + 4 * 4) && s_last_w == 10 && s_last_h == 3 && s_last_x == 8,
		"read from the texture into the buffer at the rectangle's offset");
	check(s_read_fbo == 5 && s_bound_pack == 0, "the frontend's framebuffer back, the buffer unbound");
	check(d->m_needs_flush && s_live_syncs == 1, "a fence to flush on");
	d->m_ops->copy_from_texture(d, &drc, tex, &src, 0, 1);
	check(s_live_syncs == 1, "a second copy replaces the fence");
	d->m_ops->flush(d);
	check(!d->m_needs_flush && s_live_syncs == 0, "flushed");
	d->m_ops->free(d);
	check(s_mapped_buffers == 0, "unmapped as it goes");

	/* CPU memory where the PBO is off */
	s_pbo_disabled = 1;
	d = gs_download_texture_gl_create(64, 64, GS_TEX_COLOR);
	check(d && d->m_map_pointer == ((gs_download_texture_gl*)d)->cpu_buffer && ((gs_download_texture_gl*)d)->buffer_id == 0,
		"CPU memory");
	d->m_ops->copy_from_texture(d, &drc, tex, &src, 0, 0);
	check(d->m_current_pitch == 256 && s_pack_row == 0 && !d->m_needs_flush, "the texture's own width packed, read at once");
	check(s_last_pixels == (void*)(((gs_download_texture_gl*)d)->cpu_buffer + 2 * 256 + 4 * 4), "into the memory");
	check(d->m_ops->map(d, &drc), "always mapped");
	d->m_ops->free(d);
	s_pbo_disabled      = 0;
	GLAD_GL_VERSION_4_4 = 0;
	tex->m_ops->free(tex);
}

int main(void)
{
	creation();
	free_unbinds();
	uploads();
	colclip_float();
	compressed();
	maps();
	read_back();
	check(s_live_textures == 0 && s_live_buffers == 0 && s_live_syncs == 0, "everything made was released");
	if (s_failed)
		return 1;
	printf("gltexture: ok\n");
	return 0;
}
