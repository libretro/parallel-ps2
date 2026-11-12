/*  Hardware-context request audit.
 *
 *  The GL renderer runs on desktop GL 3.3 and on OpenGL ES 3.1. The ES
 *  version a core asks for reaches eglCreateContext as asked, and a driver
 *  returns the newest version compatible with it, so asking for 3.2 gets
 *  a 3.1 driver no context at all while asking for 3.1 gets a 3.2 driver
 *  3.2. ES 2 cannot run the renderer, and neither can ES 3.0, which is
 *  what bare RETRO_HW_CONTEXT_OPENGLES3 means: a frontend accepts that,
 *  the device refuses what it was given, and there is no way back to the
 *  software renderer. So the core asks for ES 3.1 through
 *  RETRO_HW_CONTEXT_OPENGLES_VERSION only, never for ES 2, bare ES 3 or
 *  3.2; it asks for ES only after desktop GL was refused; it asks for a
 *  shared GL context only once a GL context was accepted; and when no
 *  context it can draw into is accepted it asks for none and runs the
 *  software renderer.
 *
 *  This checks that shape in the sources: libretro_set_hw_render and
 *  libretro_select_hw_render (libretro/main.c) and the GS context
 *  switches (pcsx2/GS/GS.cpp).
 *
 *  Usage: tests/hwctx/ctxaudit <libretro/main.c> <pcsx2/GS/GS.cpp>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* read_file(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	char* buf;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char*)malloc((size_t)n + 1);
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[n] = '\0';
	fclose(f);
	return buf;
}

/* Body of the function whose definition starts with sig, as [*s, *e). */
static int body(const char* src, const char* sig, const char** s, const char** e)
{
	*s = strstr(src, sig);
	*e = *s ? strstr(*s, "\n}\n") : NULL;
	return *s && *e;
}

/* First and last occurrence of needle in [s, e), or NULL. */
static const char* first_in(const char* s, const char* e, const char* needle)
{
	const char* p = strstr(s, needle);
	return (p && p < e) ? p : NULL;
}

static const char* last_in(const char* s, const char* e, const char* needle)
{
	const char* last = NULL;
	const char* p = s;
	while ((p = strstr(p, needle)) && p < e)
		last = p++;
	return last;
}

static int count_in(const char* s, const char* e, const char* needle)
{
	int n = 0;
	const char* p = s;
	while ((p = strstr(p, needle)) && p < e)
	{
		n++;
		p++;
	}
	return n;
}

static int verdict(const char* what, int ok)
{
	printf("  %-56s %s\n", what, ok ? "ok" : "WRONG");
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	static const char kNone[] = "return libretro_set_hw_render(RETRO_HW_CONTEXT_NONE);\n}\n";
	static const char kAsk31[] =
		"hw_render.context_type  = RETRO_HW_CONTEXT_OPENGLES_VERSION;\n"
		"\t\t\thw_render.version_major = 3;\n"
		"\t\t\thw_render.version_minor = 1;\n";
	char* m;
	char* g;
	const char *s, *e;
	int bad = 0;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <libretro/main.c> <pcsx2/GS/GS.cpp>\n", argv[0]);
		return 2;
	}
	m = read_file(argv[1]);
	g = read_file(argv[2]);
	if (!m || !g)
	{
		fprintf(stderr, "cannot read the inputs\n");
		free(m);
		free(g);
		return 2;
	}

	if (!body(m, "static bool libretro_set_hw_render(", &s, &e))
		bad += verdict("libretro_set_hw_render found", 0);
	else
	{
		bad += verdict("libretro_set_hw_render accepts no ES 2 type", !first_in(s, e, "RETRO_HW_CONTEXT_OPENGLES2"));
		bad += verdict("libretro_set_hw_render asks for ES 3.1", first_in(s, e, kAsk31) != NULL);
		bad += verdict("libretro_set_hw_render never asks for ES 3.2", !first_in(s, e, "version_minor = 2"));
		bad += verdict("libretro_set_hw_render never falls back to bare ES 3",
			!first_in(s, e, "context_type  = RETRO_HW_CONTEXT_OPENGLES3;"));
		{
			const char* accepted = first_in(s, e, "RETRO_ENVIRONMENT_SET_HW_RENDER");
			const char* shared   = first_in(s, e, "RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT");
			bad += verdict("libretro_set_hw_render asks for a shared context", shared != NULL);
			bad += verdict("libretro_set_hw_render shares only an accepted context", accepted && shared && accepted < shared);
		}
	}

	if (!body(m, "static bool libretro_select_hw_render(", &s, &e))
		bad += verdict("libretro_select_hw_render found", 0);
	else
	{
		const char* last_gl = last_in(s, e, "libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGL)");
		const char* last_es = last_in(s, e, "libretro_set_hw_render(RETRO_HW_CONTEXT_OPENGLES");

		bad += verdict("libretro_select_hw_render asks for no ES 2 type", !first_in(s, e, "RETRO_HW_CONTEXT_OPENGLES2"));
		bad += verdict("libretro_select_hw_render falls back to ES", last_es != NULL);
		bad += verdict("libretro_select_hw_render asks for ES after desktop GL", last_gl && last_es && last_gl < last_es);
		/* e points at the "\n}\n" that closes it */
		bad += verdict("libretro_select_hw_render ends on CONTEXT_NONE",
			(size_t)(e + 3 - s) >= strlen(kNone) - 1 && !strncmp(e + 3 - (strlen(kNone) - 1), kNone + 1, strlen(kNone) - 1));
	}

	{
		const char* ge = g + strlen(g);
		bad += verdict("GS.cpp maps no ES 2 type to a device", !first_in(g, ge, "RETRO_HW_CONTEXT_OPENGLES2"));
		/* GSsetRenderer and OpenGSDevice */
		bad += verdict("GS.cpp maps OPENGLES3 in both switches", count_in(g, ge, "case RETRO_HW_CONTEXT_OPENGLES3:") == 2);
		bad += verdict("GS.cpp maps OPENGLES_VERSION in both switches",
			count_in(g, ge, "case RETRO_HW_CONTEXT_OPENGLES_VERSION:") == 2);
	}

	free(m);
	free(g);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
