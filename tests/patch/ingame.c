/*  The core's built-in game patches (libretro/patches.c), driven with
 *  every serial and CRC they name, under every renderer and hint
 *  setting. C89, as is everything it links; the stubs are in ingame_glue.c.
 *
 *  - Each patch block is reachable: every message it logs is logged by
 *    some serial. Two branches of one chain testing the same serial leave
 *    the second one dead.
 *  - One call patches one game: the messages a call logs all name the
 *    same game, so no block hangs off another game's serial.
 *  - Every patch line goes through the real parser without an error.
 *  - Every message is a format string with nothing to format but the
 *    CRC: a bare percent sign is a conversion, reading an argument that
 *    was never passed.
 *
 *  Usage: tests/patch/patch_ingame libretro/patches.c
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libretro.h>

void ingame_harness_call(const char *serial, unsigned crc,
	const char *renderer, int widescreen, int framerate);

#define MAX_ITEMS 4096
#define MAX_TEXT  256

static int failures;

static void fail(const char *fmt, ...)
{
	va_list ap;
	failures++;
	printf("  ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* What the source says: the serials and CRCs the branches test, and
 * the text of every message. */
static char     s_serial[MAX_ITEMS][11];
static int      s_nserial;
static unsigned s_crc[MAX_ITEMS];
static int      s_ncrc;
static char     s_msg[MAX_ITEMS][MAX_TEXT];
static int      s_seen[MAX_ITEMS];
static int      s_badfmt[MAX_ITEMS];
static int      s_nmsg;

/* What the current call did. */
static char s_game[MAX_TEXT];
static int  s_games;
static int  s_twogames;
static char s_call[96];

/* "[PATCH] [Name (Region)]: ..." -> name folded to letters and digits,
 * so "Echo Night: Beyond" and "Echo Night - Beyond" are one game. */
static void game_of(const char *fmt, char *out)
{
	const char *p   = fmt + strlen("[PATCH] [");
	const char *end = strstr(p, " (");
	size_t n        = 0;

	if (!end)
		end = strchr(p, ']');
	if (!end)
		end = p + strlen(p);
	for (; p < end && n + 1 < MAX_TEXT; p++)
		if (isalnum((unsigned char)*p))
			out[n++] = (char)tolower((unsigned char)*p);
	out[n] = '\0';
}

static void check_format(const char *fmt, int msg)
{
	const char *p;

	if (msg < 0 || s_badfmt[msg])
		return;
	for (p = fmt; (p = strchr(p, '%')) != NULL; )
	{
		if (p[1] == '%')
			p += 2;
		else if (!strncmp(p, "%08X", 4))
			p += 4;
		else
		{
			fail("a conversion with nothing to convert: %s", fmt);
			s_badfmt[msg] = 1;
			return;
		}
	}
}

static void test_log(enum retro_log_level level, const char *fmt, ...)
{
	char game[MAX_TEXT];
	int i;
	int msg = -1;

	if (level == RETRO_LOG_ERROR)
	{
		fail("%s: a patch line was refused by the parser\n", s_call);
		return;
	}
	if (strncmp(fmt, "[PATCH] [", 9))
		return;
	for (i = 0; i < s_nmsg; i++)
		if (!strcmp(s_msg[i], fmt))
		{
			s_seen[i] = 1;
			msg       = i;
		}
	check_format(fmt, msg);
	game_of(fmt, game);
	if (s_games == 0)
		strcpy(s_game, game);
	else if (strcmp(s_game, game) && !s_twogames)
	{
		fail("%s: patches two games, %s and %s\n", s_call, s_game, game);
		s_twogames = 1;
	}
	s_games++;
}
retro_log_printf_t log_cb = test_log;

static void add_serial(const char *s)
{
	int i;
	for (i = 0; i < s_nserial; i++)
		if (!strcmp(s_serial[i], s))
			return;
	if (s_nserial < MAX_ITEMS)
		strcpy(s_serial[s_nserial++], s);
}

static void add_crc(unsigned c)
{
	int i;
	for (i = 0; i < s_ncrc; i++)
		if (s_crc[i] == c)
			return;
	if (s_ncrc < MAX_ITEMS)
		s_crc[s_ncrc++] = c;
}

static int read_source(const char *path)
{
	static const char log_open[] = "log_cb(RETRO_LOG_INFO, \"[PATCH] [";
	FILE *f = fopen(path, "r");
	char line[1024];

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
	{
		const char *p;

		/* Serials: "SXXX-12345" */
		for (p = line; (p = strchr(p, '"')) != NULL; p++)
			if (strlen(p) > 11 && isupper((unsigned char)p[1]) && p[5] == '-'
			 && p[11] == '"' && isdigit((unsigned char)p[6])
			 && isdigit((unsigned char)p[10]))
			{
				char s[11];
				memcpy(s, p + 1, 10);
				s[10] = '\0';
				add_serial(s);
			}
		/* CRCs: game_crc == 0x12345678 */
		for (p = line; (p = strstr(p, "game_crc == 0x")) != NULL; p++)
			add_crc((unsigned)strtoul(p + strlen("game_crc == "), NULL, 16));
		/* Messages, unescaped as the compiler would. */
		if ((p = strstr(line, log_open)) != NULL && s_nmsg < MAX_ITEMS)
		{
			char *out = s_msg[s_nmsg];
			size_t n  = 0;

			for (p += strlen("log_cb(RETRO_LOG_INFO, \"");
			     *p && *p != '"' && n + 1 < MAX_TEXT; p++)
			{
				if (*p == '\\' && p[1] == 'n')
				{
					out[n++] = '\n';
					p++;
				}
				else if (*p == '\\' && p[1])
				{
					out[n++] = p[1];
					p++;
				}
				else
					out[n++] = *p;
			}
			out[n] = '\0';
			s_nmsg++;
		}
	}
	fclose(f);
	return 1;
}

int main(int argc, char **argv)
{
	/* The renderers the patches tell apart, and one they do not. */
	static const char *const renderers[] = {
		"Auto", "paraLLEl-GS", "Software", "Software (HW)", "Software (SW)"
	};
	const char *path = argc > 1 ? argv[1] : "libretro/patches.c";
	int s, c, r, ws, fps, i;
	int calls = 0;
	int dead  = 0;

	if (!read_source(path))
	{
		fprintf(stderr, "cannot open %s\n", path);
		return 2;
	}
	add_crc(0);

	for (s = 0; s < s_nserial; s++)
		for (c = 0; c < s_ncrc; c++)
			for (r = 0; r < (int)(sizeof(renderers) / sizeof(renderers[0])); r++)
				for (ws = 1; ws <= 4; ws++)
					for (fps = 1; fps <= 2; fps++)
					{
						sprintf(s_call, "%s %08X %s ws%d fps%d", s_serial[s],
							s_crc[c], renderers[r], ws, fps);
						s_games = 0;
						if (c == 0 && r == 0 && ws == 1 && fps == 1)
							s_twogames = 0;
						ingame_harness_call(s_serial[s], s_crc[c], renderers[r], ws, fps);
						calls++;
					}

	for (i = 0; i < s_nmsg; i++)
		if (!s_seen[i])
		{
			fail("never reached: %s", s_msg[i]);
			dead++;
		}

	printf("ingame: %d serials, %d CRCs, %d calls, %d of %d patch blocks reached\n",
	       s_nserial, s_ncrc, calls, s_nmsg - dead, s_nmsg);
	if (s_nserial == 0 || s_nmsg == 0)
		failures++;
	printf(failures ? "ingame: FAILED (%d)\n" : "ingame: ok\n", failures);
	return failures != 0;
}
