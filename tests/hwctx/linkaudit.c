/*  GL link audit for the unix Makefile build.
 *
 *  The core never calls into a GL library itself: GLContext.c hands
 *  glad the frontend's get_proc_address through gladLoadGLLoader(), or
 *  gladLoadGLES2Loader() for an ES context, and every gl* call goes
 *  through the pointers that fills in. Linking
 *  libGL or libGLESv2 on top of that adds nothing but a load-time
 *  dependency: on a toolchain that does not default to --as-needed the
 *  core then refuses to load on a system without that library, even when
 *  it would run on Vulkan or the software renderer.
 *
 *  This checks that GLContext.c loads GL through gladLoadGLLoader, and
 *  that the unix platform block of the Makefile puts no -lGL or -lGLESv2
 *  on the link line.
 *
 *  Usage: tests/hwctx/linkaudit <Makefile> <pcsx2/GS/Renderers/OpenGL/GLContext.c>
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

/* Count -lGL / -lGLESv2 in the non-comment part of each line of [s, e). */
static int gl_links(const char* s, const char* e)
{
	const char* p = s;
	int found = 0, line = 0;

	while (p < e)
	{
		const char* le = strchr(p, '\n');
		const char* c;

		if (!le || le > e)
			le = e;
		line++;
		for (c = p; c < le && *c != '#'; c++)
			if (!strncmp(c, "-lGL", 4) && (c[4] == ' ' || c[4] == '\t' || c[4] == '\n' ||
			    c[4] == '\r' || c[4] == '\0' || !strncmp(c + 4, "ESv2", 4)))
			{
				printf("  unix block line %d: %.*s\n", line, (int)(le - p), p);
				found++;
				break;
			}
		p = le + 1;
	}
	return found;
}

int main(int argc, char** argv)
{
	static const char kUnix[] = "ifneq (,$(findstring unix,$(platform)))";
	char* mk;
	char* ctx;
	const char* s;
	const char* e;
	int bad = 0;

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <Makefile> <GLContext.c>\n", argv[0]);
		return 2;
	}
	mk = read_file(argv[1]);
	ctx = read_file(argv[2]);
	if (!mk || !ctx)
	{
		fprintf(stderr, "cannot read %s\n", !mk ? argv[1] : argv[2]);
		free(mk);
		free(ctx);
		return 2;
	}

	if (!strstr(ctx, "gladLoadGLLoader("))
	{
		printf("  GLContext.c does not load GL through gladLoadGLLoader -- has the loader changed?\n");
		bad++;
	}

	/* the unix block runs to the next platform at the start of a line */
	s = strstr(mk, kUnix);
	e = s ? strstr(s, "\nelse ifeq ($(platform)") : NULL;
	if (!s || !e)
	{
		printf("  unix platform block not found -- has the Makefile changed shape?\n");
		bad++;
	}
	else
		bad += gl_links(s, e);

	free(mk);
	free(ctx);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad ? 1 : 0;
}
