/* The GSdx OpenGL renderer against a frontend that gives it an OpenGL
 * core context the way RetroArch does: the context is current on the
 * thread that runs the core, the frontend owns the framebuffer the core
 * draws its picture into (get_current_framebuffer) and hands out the
 * entry points (get_proc_address), and the frame arrives as
 * RETRO_HW_FRAME_BUFFER_VALID in the video callback.
 *
 * The context comes from EGL on a surfaceless display, so no window or
 * X server is needed: Mesa's llvmpipe will do, and is what CI has. The
 * EGL and GL libraries are opened at run time, so the harness builds
 * without their headers; without them, or without a context, the run is
 * a skip.
 *
 * LRPS2_BIOS names the BIOS image to boot (tests/swdraw writes one per
 * case). GN_FRAMES retro_runs are run (30); after the last the picture
 * in the framebuffer is read back, its rows top first as the Vulkan
 * frontend reads an image, and its FNV-1a hash reported ("image: WxH,
 * hash ..."), for the two renderers' pictures of a case to be compared;
 * GN_RGB leaves the alpha out of the hash. GN_DUMP names a .ppm the
 * picture is written to. GN_SCALE is the upscale (1), GN_OPTIONS core
 * options as "key=value;key=value", GN_VERBOSE has every core log line
 * printed. GN_EXPECT_IMAGES is the least frames that must arrive as
 * images (1 with a BIOS).
 *
 * Usage: glnegotiate <path-to-core> <scratch-dir> */

#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include <libretro.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
typedef HMODULE gn_handle_t;
static gn_handle_t gn_dlopen(const char* p) { return LoadLibraryA(p); }
static void gn_mkdir(const char* p) { _mkdir(p); }
#else
#include <dlfcn.h>
#include <sys/stat.h>
typedef void* gn_handle_t;
static gn_handle_t gn_dlopen(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void gn_mkdir(const char* p) { mkdir(p, 0777); }
#endif

typedef void (*gn_fn_t)(void);
static gn_fn_t gn_sym(gn_handle_t h, const char* name)
{
#ifdef _WIN32
	return (gn_fn_t)GetProcAddress(h, name);
#else
	void* obj = dlsym(h, name);
	gn_fn_t fn = 0;
	memcpy(&fn, &obj, sizeof(fn));
	return fn;
#endif
}

#define GN_FAIL(...) do { printf("glnegotiate: FAIL: "); printf(__VA_ARGS__); printf("\n"); exit(1); } while (0)

/* EGL and GL, as much as is used here, declared rather than included. */
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef int EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

typedef gn_fn_t (*PFNEGLGETPROCADDRESS)(const char*);
typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXT)(EGLenum, void*, const EGLint*);
typedef EGLDisplay (*PFNEGLGETDISPLAY)(void*);
typedef EGLBoolean (*PFNEGLINITIALIZE)(EGLDisplay, EGLint*, EGLint*);
typedef EGLBoolean (*PFNEGLBINDAPI)(EGLenum);
typedef EGLBoolean (*PFNEGLCHOOSECONFIG)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLContext (*PFNEGLCREATECONTEXT)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
typedef EGLBoolean (*PFNEGLMAKECURRENT)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (*PFNEGLDESTROYCONTEXT)(EGLDisplay, EGLContext);
typedef EGLBoolean (*PFNEGLTERMINATE)(EGLDisplay);
typedef EGLint (*PFNEGLGETERROR)(void);

typedef unsigned GLenum;
typedef unsigned GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_NO_ERROR 0

typedef const GLubyte* (*PFNGLGETSTRING)(GLenum);
typedef void (*PFNGLGENTEXTURES)(GLsizei, GLuint*);
typedef void (*PFNGLBINDTEXTURE)(GLenum, GLuint);
typedef void (*PFNGLTEXIMAGE2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void (*PFNGLTEXPARAMETERI)(GLenum, GLenum, GLint);
typedef void (*PFNGLGENFRAMEBUFFERS)(GLsizei, GLuint*);
typedef void (*PFNGLBINDFRAMEBUFFER)(GLenum, GLuint);
typedef void (*PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (*PFNGLGENRENDERBUFFERS)(GLsizei, GLuint*);
typedef void (*PFNGLBINDRENDERBUFFER)(GLenum, GLuint);
typedef void (*PFNGLRENDERBUFFERSTORAGE)(GLenum, GLenum, GLsizei, GLsizei);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFER)(GLenum, GLenum, GLenum, GLuint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void (*PFNGLREADPIXELS)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
typedef void (*PFNGLPIXELSTOREI)(GLenum, GLint);
typedef void (*PFNGLFINISH)(void);
typedef GLenum (*PFNGLGETERROR)(void);
typedef void (*PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
typedef void (*PFNGLDELETETEXTURES)(GLsizei, const GLuint*);
typedef void (*PFNGLDELETERENDERBUFFERS)(GLsizei, const GLuint*);

static PFNEGLGETPROCADDRESS p_eglGetProcAddress;
static PFNEGLINITIALIZE p_eglInitialize;
static PFNEGLBINDAPI p_eglBindAPI;
static PFNEGLCHOOSECONFIG p_eglChooseConfig;
static PFNEGLCREATECONTEXT p_eglCreateContext;
static PFNEGLMAKECURRENT p_eglMakeCurrent;
static PFNEGLDESTROYCONTEXT p_eglDestroyContext;
static PFNEGLTERMINATE p_eglTerminate;
static PFNEGLGETERROR p_eglGetError;
static EGLDisplay s_display;
static EGLContext s_context;

static PFNGLGETSTRING p_glGetString;
static PFNGLGENTEXTURES p_glGenTextures;
static PFNGLBINDTEXTURE p_glBindTexture;
static PFNGLTEXIMAGE2D p_glTexImage2D;
static PFNGLTEXPARAMETERI p_glTexParameteri;
static PFNGLGENFRAMEBUFFERS p_glGenFramebuffers;
static PFNGLBINDFRAMEBUFFER p_glBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2D p_glFramebufferTexture2D;
static PFNGLGENRENDERBUFFERS p_glGenRenderbuffers;
static PFNGLBINDRENDERBUFFER p_glBindRenderbuffer;
static PFNGLRENDERBUFFERSTORAGE p_glRenderbufferStorage;
static PFNGLFRAMEBUFFERRENDERBUFFER p_glFramebufferRenderbuffer;
static PFNGLCHECKFRAMEBUFFERSTATUS p_glCheckFramebufferStatus;
static PFNGLREADPIXELS p_glReadPixels;
static PFNGLPIXELSTOREI p_glPixelStorei;
static PFNGLFINISH p_glFinish;
static PFNGLGETERROR p_glGetError;
static PFNGLDELETEFRAMEBUFFERS p_glDeleteFramebuffers;
static PFNGLDELETETEXTURES p_glDeleteTextures;
static PFNGLDELETERENDERBUFFERS p_glDeleteRenderbuffers;


/* (the typedef names above are upper case: a second macro for them) */
#define GN_GLFN(var, type, name) do { var = (type)p_eglGetProcAddress(name); if (!var) return 0; } while (0)

/* An OpenGL 3.3 core context on a surfaceless display, current on this
 * thread. 0 when there is none to be had. */
static int gn_context(void)
{
	static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
	gn_handle_t egl;
	PFNEGLGETPLATFORMDISPLAYEXT get_platform_display;
	PFNEGLGETDISPLAY get_display;
	EGLConfig cfg;
	EGLint n = 0, major = 0, minor = 0;
#ifdef _WIN32
	egl = gn_dlopen("libEGL.dll");
#else
	egl = gn_dlopen("libEGL.so.1");
	if (!egl)
		egl = gn_dlopen("libEGL.so");
#endif
	if (!egl)
		return 0;
	p_eglGetProcAddress = (PFNEGLGETPROCADDRESS)gn_sym(egl, "eglGetProcAddress");
	if (!p_eglGetProcAddress)
		return 0;
	GN_GLFN(p_eglInitialize, PFNEGLINITIALIZE, "eglInitialize");
	GN_GLFN(p_eglBindAPI, PFNEGLBINDAPI, "eglBindAPI");
	GN_GLFN(p_eglChooseConfig, PFNEGLCHOOSECONFIG, "eglChooseConfig");
	GN_GLFN(p_eglCreateContext, PFNEGLCREATECONTEXT, "eglCreateContext");
	GN_GLFN(p_eglMakeCurrent, PFNEGLMAKECURRENT, "eglMakeCurrent");
	GN_GLFN(p_eglDestroyContext, PFNEGLDESTROYCONTEXT, "eglDestroyContext");
	GN_GLFN(p_eglTerminate, PFNEGLTERMINATE, "eglTerminate");
	GN_GLFN(p_eglGetError, PFNEGLGETERROR, "eglGetError");
	get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXT)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
	get_display          = (PFNEGLGETDISPLAY)p_eglGetProcAddress("eglGetDisplay");
	s_display = get_platform_display ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	if (s_display == EGL_NO_DISPLAY && get_display)
		s_display = get_display(EGL_DEFAULT_DISPLAY);
	if (s_display == EGL_NO_DISPLAY || !p_eglInitialize(s_display, &major, &minor))
		return 0;
	if (!p_eglBindAPI(EGL_OPENGL_API) || !p_eglChooseConfig(s_display, cfg_attr, &cfg, 1, &n) || n < 1)
		return 0;
	s_context = p_eglCreateContext(s_display, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (s_context == EGL_NO_CONTEXT || !p_eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_context))
		return 0;
	GN_GLFN(p_glGetString, PFNGLGETSTRING, "glGetString");
	GN_GLFN(p_glGenTextures, PFNGLGENTEXTURES, "glGenTextures");
	GN_GLFN(p_glBindTexture, PFNGLBINDTEXTURE, "glBindTexture");
	GN_GLFN(p_glTexImage2D, PFNGLTEXIMAGE2D, "glTexImage2D");
	GN_GLFN(p_glTexParameteri, PFNGLTEXPARAMETERI, "glTexParameteri");
	GN_GLFN(p_glGenFramebuffers, PFNGLGENFRAMEBUFFERS, "glGenFramebuffers");
	GN_GLFN(p_glBindFramebuffer, PFNGLBINDFRAMEBUFFER, "glBindFramebuffer");
	GN_GLFN(p_glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2D, "glFramebufferTexture2D");
	GN_GLFN(p_glGenRenderbuffers, PFNGLGENRENDERBUFFERS, "glGenRenderbuffers");
	GN_GLFN(p_glBindRenderbuffer, PFNGLBINDRENDERBUFFER, "glBindRenderbuffer");
	GN_GLFN(p_glRenderbufferStorage, PFNGLRENDERBUFFERSTORAGE, "glRenderbufferStorage");
	GN_GLFN(p_glFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFER, "glFramebufferRenderbuffer");
	GN_GLFN(p_glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUS, "glCheckFramebufferStatus");
	GN_GLFN(p_glReadPixels, PFNGLREADPIXELS, "glReadPixels");
	GN_GLFN(p_glPixelStorei, PFNGLPIXELSTOREI, "glPixelStorei");
	GN_GLFN(p_glFinish, PFNGLFINISH, "glFinish");
	GN_GLFN(p_glGetError, PFNGLGETERROR, "glGetError");
	GN_GLFN(p_glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERS, "glDeleteFramebuffers");
	GN_GLFN(p_glDeleteTextures, PFNGLDELETETEXTURES, "glDeleteTextures");
	GN_GLFN(p_glDeleteRenderbuffers, PFNGLDELETERENDERBUFFERS, "glDeleteRenderbuffers");
	printf("  context: OpenGL %s (%s), EGL %d.%d\n", (const char*)p_glGetString(GL_VERSION),
		(const char*)p_glGetString(GL_RENDERER), major, minor);
	return 1;
}

/* The frontend's framebuffer the core draws into, at the core's largest
 * size. */
static GLuint s_fbo, s_fbo_tex, s_fbo_depth;
static unsigned s_fbo_w, s_fbo_h;

static int gn_framebuffer(unsigned w, unsigned h)
{
	p_glGenTextures(1, &s_fbo_tex);
	p_glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
	p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	p_glGenRenderbuffers(1, &s_fbo_depth);
	p_glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_depth);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
	p_glGenFramebuffers(1, &s_fbo);
	p_glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_fbo_depth);
	if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return 0;
	p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
	s_fbo_w = w;
	s_fbo_h = h;
	return 1;
}

static uintptr_t gn_get_current_framebuffer(void) { return s_fbo; }
static retro_proc_address_t gn_get_proc_address(const char* sym) { return (retro_proc_address_t)p_eglGetProcAddress(sym); }

/* The frame handed over: counted; its picture read back on the frame
 * asked for, rows top first, hashed and dumped as asked. */
static struct retro_hw_render_callback s_hw;
static unsigned s_images, s_frames, s_last_w, s_last_h;
static unsigned long long s_hash;
static int s_hash_wanted;

static void gn_read_image(unsigned w, unsigned h)
{
	const int rgb_only = getenv("GN_RGB") != NULL;
	unsigned char* rows = (unsigned char*)malloc((size_t)w * h * 4);
	unsigned long long hash = 14695981039346656037ULL;
	unsigned x, y;
	const char* dump = getenv("GN_DUMP");
	if (!rows)
		GN_FAIL("out of memory");
	p_glFinish();
	p_glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
	p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
	p_glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, rows);
	p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	if (p_glGetError() != GL_NO_ERROR)
		GN_FAIL("the picture could not be read back");
	/* bottom_left_origin: the rows come bottom first */
	for (y = 0; y < h; y++)
	{
		const unsigned char* row = rows + (size_t)(h - 1 - y) * w * 4;
		for (x = 0; x < w * 4; x++)
			if (!rgb_only || (x & 3) != 3)
				hash = (hash ^ row[x]) * 1099511628211ULL;
	}
	if (dump)
	{
		FILE* f = fopen(dump, "wb");
		if (!f)
			GN_FAIL("cannot write %s", dump);
		fprintf(f, "P6\n%u %u\n255\n", w, h);
		for (y = 0; y < h; y++)
		{
			const unsigned char* row = rows + (size_t)(h - 1 - y) * w * 4;
			for (x = 0; x < w; x++)
				fwrite(row + x * 4, 1, 3, f);
		}
		fclose(f);
	}
	free(rows);
	s_hash   = hash;
	s_last_w = w;
	s_last_h = h;
}

static void gn_video(const void* data, unsigned w, unsigned h, size_t pitch)
{
	(void)pitch;
	s_frames++;
	if (data != RETRO_HW_FRAME_BUFFER_VALID)
		return;
	s_images++;
	if (s_hash_wanted && w && h && w <= s_fbo_w && h <= s_fbo_h)
		gn_read_image(w, h);
}

static void gn_audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t gn_audio_batch(const int16_t* d, size_t f) { (void)d; return f; }
static void gn_poll(void) { }
static int16_t gn_input(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

static void gn_log(enum retro_log_level level, const char* fmt, ...)
{
	va_list ap;
	if (level < RETRO_LOG_WARN && !getenv("GN_VERBOSE"))
		return;
	va_start(ap, fmt);
	printf("  [core] ");
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static char s_system_dir[1024];
static const char* s_bios_name = "lrps2_glnegotiate.bin";

/* GN_OPTIONS "key=value;key=value": the value under key, or NULL. */
static const char* gn_option(const char* key)
{
	static char value[256];
	const char* opts = getenv("GN_OPTIONS");
	const size_t klen = strlen(key);
	while (opts && *opts)
	{
		const char* end = strchr(opts, ';');
		const size_t len = end ? (size_t)(end - opts) : strlen(opts);
		if (len > klen && !strncmp(opts, key, klen) && opts[klen] == '=' && len - klen - 1 < sizeof(value))
		{
			memcpy(value, opts + klen + 1, len - klen - 1);
			value[len - klen - 1] = 0;
			return value;
		}
		opts = end ? end + 1 : NULL;
	}
	return NULL;
}

static bool gn_environment(unsigned cmd, void* data)
{
	struct retro_variable* var;
	switch (cmd)
	{
		case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
			((struct retro_log_callback*)data)->log = gn_log;
			return true;
		case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
			*(const char**)data = s_system_dir;
			return true;
		case RETRO_ENVIRONMENT_GET_LANGUAGE:
			*(unsigned*)data = RETRO_LANGUAGE_ENGLISH;
			return true;
		case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
			*(unsigned*)data = 2;
			return true;
		case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
			return true;
		case RETRO_ENVIRONMENT_SET_HW_RENDER:
		{
			struct retro_hw_render_callback* hw = (struct retro_hw_render_callback*)data;
			if (hw->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && hw->context_type != RETRO_HW_CONTEXT_OPENGL)
				return false;
			hw->get_current_framebuffer = gn_get_current_framebuffer;
			hw->get_proc_address        = gn_get_proc_address;
			memcpy(&s_hw, hw, sizeof(s_hw));
			return true;
		}
		case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
			*(enum retro_hw_context_type*)data = RETRO_HW_CONTEXT_OPENGL_CORE;
			return true;
		case RETRO_ENVIRONMENT_GET_VARIABLE:
			var = (struct retro_variable*)data;
			var->value = NULL;
			if      (!strcmp(var->key, "pcsx2_bios"))     var->value = s_bios_name;
			else if (!strcmp(var->key, "pcsx2_renderer")) var->value = "OpenGL";
			else if (!strcmp(var->key, "pcsx2_fastboot")) var->value = gn_option(var->key) ? gn_option(var->key) : "disabled";
			else if (!strcmp(var->key, "pcsx2_upscale_multiplier")) var->value = getenv("GN_SCALE") ? getenv("GN_SCALE") : "1x";
			if (!var->value)
				var->value = gn_option(var->key);
			return var->value != NULL;
		case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
			*(bool*)data = false;
			return true;
		default:
			return false;
	}
}

typedef void retro_set_environment_t(retro_environment_t);
typedef void retro_set_video_refresh_t(retro_video_refresh_t);
typedef void retro_set_audio_sample_t(retro_audio_sample_t);
typedef void retro_set_audio_sample_batch_t(retro_audio_sample_batch_t);
typedef void retro_set_input_poll_t(retro_input_poll_t);
typedef void retro_set_input_state_t(retro_input_state_t);
typedef void retro_simple_t(void);
typedef bool retro_load_game_t(const struct retro_game_info*);
typedef void retro_get_system_av_info_t(struct retro_system_av_info*);

static int gn_copy(const char* src, const char* dst)
{
	static unsigned char buf[1 << 16];
	size_t n;
	FILE *in = fopen(src, "rb"), *out;
	if (!in)
		return 0;
	out = fopen(dst, "wb");
	if (!out)
	{
		fclose(in);
		return 0;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		fwrite(buf, 1, n, out);
	fclose(in);
	return fclose(out) == 0;
}

int main(int argc, char** argv)
{
	gn_handle_t h;
	char path[1200];
	retro_set_environment_t* set_environment;
	retro_set_video_refresh_t* set_video;
	retro_set_audio_sample_t* set_audio;
	retro_set_audio_sample_batch_t* set_audio_batch;
	retro_set_input_poll_t* set_poll;
	retro_set_input_state_t* set_input;
	retro_simple_t *init_fn, *deinit_fn, *unload_game, *run;
	retro_load_game_t* load_game;
	retro_get_system_av_info_t* av_info;
	struct retro_system_av_info av;
	const char* bios = getenv("LRPS2_BIOS");
	const int frames = getenv("GN_FRAMES") ? atoi(getenv("GN_FRAMES")) : 30;
	const unsigned expect_images = getenv("GN_EXPECT_IMAGES") ? (unsigned)atoi(getenv("GN_EXPECT_IMAGES")) : (bios ? 1u : 0u);
	int frame;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 3)
		GN_FAIL("usage: glnegotiate <path-to-core> <scratch-dir>");
	if (strlen(argv[2]) + 48 > sizeof(s_system_dir))
		GN_FAIL("scratch path too long");
	if (!bios)
		GN_FAIL("LRPS2_BIOS names the BIOS image to boot (tests/swdraw writes one)");
	if (!gn_context())
	{
		printf("glnegotiate: skip: no OpenGL context (EGL surfaceless)\n");
		return 0;
	}

	strcpy(s_system_dir, argv[2]);
	sprintf(path, "%s/pcsx2", s_system_dir);
	gn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios", s_system_dir);
	gn_mkdir(path);
	sprintf(path, "%s/pcsx2/bios/%s", s_system_dir, s_bios_name);
	if (!gn_copy(bios, path))
		GN_FAIL("cannot write %s", path);

	h = gn_dlopen(argv[1]);
	if (!h)
		GN_FAIL("cannot load %s", argv[1]);
	set_environment = (retro_set_environment_t*)gn_sym(h, "retro_set_environment");
	set_video       = (retro_set_video_refresh_t*)gn_sym(h, "retro_set_video_refresh");
	set_audio       = (retro_set_audio_sample_t*)gn_sym(h, "retro_set_audio_sample");
	set_audio_batch = (retro_set_audio_sample_batch_t*)gn_sym(h, "retro_set_audio_sample_batch");
	set_poll        = (retro_set_input_poll_t*)gn_sym(h, "retro_set_input_poll");
	set_input       = (retro_set_input_state_t*)gn_sym(h, "retro_set_input_state");
	init_fn         = (retro_simple_t*)gn_sym(h, "retro_init");
	deinit_fn       = (retro_simple_t*)gn_sym(h, "retro_deinit");
	unload_game     = (retro_simple_t*)gn_sym(h, "retro_unload_game");
	run             = (retro_simple_t*)gn_sym(h, "retro_run");
	load_game       = (retro_load_game_t*)gn_sym(h, "retro_load_game");
	av_info         = (retro_get_system_av_info_t*)gn_sym(h, "retro_get_system_av_info");
	if (!set_environment || !set_video || !set_audio || !set_audio_batch || !set_poll || !set_input
		|| !init_fn || !deinit_fn || !unload_game || !run || !load_game || !av_info)
		GN_FAIL("core is missing a libretro export");

	set_environment(gn_environment);
	set_video(gn_video);
	set_audio(gn_audio);
	set_audio_batch(gn_audio_batch);
	set_poll(gn_poll);
	set_input(gn_input);
	init_fn();
	if (!load_game(NULL))
		GN_FAIL("retro_load_game failed");
	if (s_hw.context_type != RETRO_HW_CONTEXT_OPENGL_CORE && s_hw.context_type != RETRO_HW_CONTEXT_OPENGL)
		GN_FAIL("the core did not ask for an OpenGL context");
	av_info(&av);
	printf("  core asks for OpenGL %u.%u, pictures up to %ux%u\n", s_hw.version_major, s_hw.version_minor,
		av.geometry.max_width, av.geometry.max_height);
	if (!gn_framebuffer(av.geometry.max_width ? av.geometry.max_width : 640, av.geometry.max_height ? av.geometry.max_height : 480))
		GN_FAIL("the framebuffer is not complete");
	if (s_hw.context_reset)
		s_hw.context_reset();

	for (frame = 0; frame < frames; frame++)
	{
		s_hash_wanted = (frame == frames - 1);
		run();
	}
	printf("  %d frames run, %u presented, %u as images\n", frames, s_frames, s_images);
	if (s_images < expect_images)
		GN_FAIL("%u frames arrived as images, %u expected", s_images, expect_images);
	if (s_hash_wanted && s_last_w)
		printf("  image: %ux%u, hash %016llx\n", s_last_w, s_last_h, s_hash);
	else if (s_hash_wanted && expect_images)
		GN_FAIL("the last frame did not arrive as an image");

	if (s_hw.context_destroy)
		s_hw.context_destroy();
	unload_game();
	deinit_fn();
	p_glDeleteFramebuffers(1, &s_fbo);
	p_glDeleteTextures(1, &s_fbo_tex);
	p_glDeleteRenderbuffers(1, &s_fbo_depth);
	p_eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	p_eglDestroyContext(s_display, s_context);
	p_eglTerminate(s_display);
	printf("glnegotiate: ok\n");
	return 0;
}
