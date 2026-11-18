/* Every hardware backend checks the colour-clip render target it makes
 * before it draws into it.
 *
 * A colour-clip draw renders into a wider-format copy of the target,
 * made on the draw that first needs it. That allocation can fail - out of
 * video memory, or a size the driver refuses - and a backend that goes on
 * binds a null target, copies the target into it and records the null as
 * the current colour-clip texture. So in each backend's draw, the
 * statement after
 *
 *     <name> = ... CreateRenderTarget(..., GSTexture::Format::ColorClip ...);
 *
 * (or in C, gs_device_create_render_target(..., GS_TEX_COLOR_CLIP ...))
 * must be the check of <name> for null. The draw needs a live GPU device
 * on every backend, so this reads the sources.
 *
 * C89. Usage: gscolclip_audit <device source>... */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 1024

/* The name assigned on line, or 0. */
static int assigned_name(const char *line, char *name, size_t cap)
{
	const char *eq = strstr(line, " = ");
	const char *end, *start;
	size_t n;
	if (!eq)
		return 0;
	end = eq;
	start = end;
	while (start > line && (isalnum((unsigned char)start[-1]) || start[-1] == '_'))
		start--;
	n = (size_t)(end - start);
	if (!n || n >= cap)
		return 0;
	memcpy(name, start, n);
	name[n] = '\0';
	return 1;
}

/* Whether line checks name for null: if (!name) */
static int checks_null(const char *line, const char *name)
{
	char want[160];
	sprintf(want, "if (!%s)", name);
	return strstr(line, want) != NULL;
}

static int audit(const char *path, int *sites)
{
	char line[LINE_MAX_LEN], name[128];
	int lineno = 0, pending = 0, pending_line = 0, bad = 0;
	FILE *f = fopen(path, "r");
	if (!f)
	{
		printf("  FAIL: cannot open %s\n", path);
		return 1;
	}
	while (fgets(line, sizeof(line), f))
	{
		const char *p = line;
		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (pending)
		{
			if (*p == '\n' || *p == '\r' || *p == '\0')
				continue;
			if (!checks_null(p, name))
			{
				printf("  FAIL: %s:%d makes the colour-clip target '%s' and does not check it\n",
						path, pending_line, name);
				bad++;
			}
			pending = 0;
		}
		if (((strstr(p, "CreateRenderTarget(") && strstr(p, "Format::ColorClip"))
			|| (strstr(p, "gs_device_create_render_target(") && strstr(p, "GS_TEX_COLOR_CLIP")))
		 && assigned_name(p, name, sizeof(name)))
		{
			pending = 1;
			pending_line = lineno;
			(*sites)++;
		}
	}
	fclose(f);
	return bad;
}

int main(int argc, char **argv)
{
	int i, bad = 0, sites = 0;
	for (i = 1; i < argc; i++)
	{
		int before = sites;
		bad += audit(argv[i], &sites);
		if (sites == before)
		{
			printf("  FAIL: %s makes no colour-clip target; the audit no longer sees it\n", argv[i]);
			bad++;
		}
	}
	printf(bad ? "colour-clip targets: FAILED (%d)\n" : "colour-clip targets: ok, %d checked\n",
			bad ? bad : sites);
	return bad != 0;
}
