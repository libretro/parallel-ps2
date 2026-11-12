/* The recompiler switches are seen where they are tested.
 *
 * Config.h defines one switch per recompiled instruction group
 * (FPU_RECOMPILE, MMI_RECOMPILE, ...), and a unit tests its switch with
 * #ifdef or #ifndef to choose between the recompiled group and calls into
 * the interpreter. A unit that tests a switch before Config.h is included
 * reads it as undefined: it still compiles, and the whole group silently
 * becomes interpreter calls while the rest of the recompiler keeps that
 * group's registers cached on the host. So every unit that tests a switch
 * includes Config.h itself, above the first test.
 *
 * Usage: recswitch <Config.h> <source>...
 * Exits non-zero naming each test that comes before Config.h.
 *
 * Build and run, from tests/recswitch:
 *   cc -O2 -std=c89 -pedantic -Wall recswitch.c -o recswitch
 *   ./recswitch ../../pcsx2/Config.h ../../pcsx2/x86/iFPU.c
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_SWITCHES 64
#define NAME_LEN 64
#define LINE_LEN 1024

static char switches[MAX_SWITCHES][NAME_LEN];
static int num_switches;

static const char *skip_space(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* The directive a line holds ("define", "ifdef", ...), or NULL. */
static const char *directive(const char *line, char *word, size_t size)
{
	const char *p = skip_space(line);
	size_t n = 0;
	if (*p != '#')
		return NULL;
	p = skip_space(p + 1);
	while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < size)
		word[n++] = *p++;
	word[n] = '\0';
	return p;
}

static int is_ident(int c)
{
	return isalnum(c) || c == '_';
}

/* Whether `name` appears in `text` as a whole identifier. */
static int names(const char *text, const char *name)
{
	const size_t len = strlen(name);
	const char *p = text;
	while ((p = strstr(p, name)) != NULL)
	{
		if ((p == text || !is_ident((unsigned char)p[-1])) && !is_ident((unsigned char)p[len]))
			return 1;
		p += len;
	}
	return 0;
}

static int load_switches(const char *path)
{
	char line[LINE_LEN], word[16];
	FILE *f = fopen(path, "r");
	if (!f)
	{
		printf("  cannot read %s\n", path);
		return 0;
	}
	while (fgets(line, sizeof(line), f))
	{
		const char *p = directive(line, word, sizeof(word));
		size_t n = 0;
		char name[NAME_LEN];
		if (!p || strcmp(word, "define") != 0)
			continue;
		p = skip_space(p);
		while (is_ident((unsigned char)*p) && n + 1 < sizeof(name))
			name[n++] = *p++;
		name[n] = '\0';
		if (n > 10 && strcmp(name + n - 10, "_RECOMPILE") == 0 && num_switches < MAX_SWITCHES)
			strcpy(switches[num_switches++], name);
	}
	fclose(f);
	return num_switches;
}

static int check_source(const char *path)
{
	char line[LINE_LEN], word[16];
	int line_no = 0, config_seen = 0, fail = 0, i;
	FILE *f = fopen(path, "r");
	if (!f)
	{
		printf("  cannot read %s\n", path);
		return 1;
	}
	while (fgets(line, sizeof(line), f))
	{
		const char *p = directive(line, word, sizeof(word));
		line_no++;
		if (!p)
			continue;
		if (strcmp(word, "include") == 0)
		{
			if (strstr(p, "Config.h"))
				config_seen = 1;
			continue;
		}
		if (config_seen || (strcmp(word, "if") != 0 && strcmp(word, "ifdef") != 0 &&
		                       strcmp(word, "ifndef") != 0 && strcmp(word, "elif") != 0))
			continue;
		for (i = 0; i < num_switches; i++)
			if (names(p, switches[i]))
			{
				printf("  %s:%d tests %s before including Config.h\n", path, line_no, switches[i]);
				fail++;
			}
	}
	fclose(f);
	return fail;
}

int main(int argc, char **argv)
{
	int fail = 0, i;
	if (argc < 3)
	{
		printf("usage: recswitch <Config.h> <source>...\n");
		return 2;
	}
	if (!load_switches(argv[1]))
	{
		printf("  no *_RECOMPILE switches in %s\n", argv[1]);
		return 1;
	}
	for (i = 2; i < argc; i++)
		fail += check_source(argv[i]);
	printf("recompiler switches: %s\n", fail ? "FAIL" : "ok");
	return fail ? 1 : 0;
}
