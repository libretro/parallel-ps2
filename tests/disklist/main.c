/* The disc playlist behind libretro's disk control (libretro/disk_list.c):
 * removal and the tray index, empty slots, the resume disc, M3U and cue
 * sheets. Files are made under argv[1]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>
#include "disk_list.h"

static void quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = quiet_log;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put(const char* path, const char* text)
{
	FILE* f = fopen(path, "wb");
	if (!f)
	{
		printf("cannot write %s\n", path);
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

static void reset(void)
{
	disk_list_clear();
	disk_list_set_initial(0, NULL);
}

int main(int argc, char** argv)
{
	char dir[1024], a[1100], b[1100], m3u[1100], cue[1100], text[4096], boot[2048], path[2048];
	unsigned i;

	if (argc != 2)
	{
		printf("usage: %s <scratch dir>\n", argv[0]);
		return 2;
	}
	snprintf(dir, sizeof(dir), "%s", argv[1]);

	/* Removing an entry before the tray's shifts the index down with it. */
	reset();
	CHECK(disk_list_append("/d/0.iso") && disk_list_append("/d/1.iso") && disk_list_append("/d/2.iso"));
	CHECK(disk_list_set_index(2));
	CHECK(disk_list_replace(0, NULL));
	CHECK(disk_list_count() == 2 && disk_list_index() == 1);
	CHECK(!strcmp(disk_list_path(0), "/d/1.iso") && !strcmp(disk_list_path(1), "/d/2.iso"));
	CHECK(disk_list_path(2) == NULL);

	/* ...also when the tray is not at the end, where only the shift
	 * keeps it on the same disc. */
	CHECK(disk_list_append("/d/3.iso") && disk_list_append("/d/4.iso"));
	CHECK(disk_list_set_index(2));
	CHECK(disk_list_replace(0, NULL));
	CHECK(disk_list_index() == 1 && !strcmp(disk_list_path(1), "/d/3.iso"));
	CHECK(disk_list_replace(2, NULL) && disk_list_replace(1, NULL));
	CHECK(disk_list_count() == 1 && disk_list_index() == 0);
	CHECK(disk_list_append("/d/x.iso"));
	CHECK(disk_list_set_index(1));

	/* Removing the last entry while the tray points at it clamps. */
	CHECK(disk_list_replace(1, NULL));
	CHECK(disk_list_count() == 1 && disk_list_index() == 0);
	/* Removing the only one leaves no disc to point at. */
	CHECK(disk_list_replace(0, NULL));
	CHECK(disk_list_count() == 0 && disk_list_index() == -1);
	CHECK(!disk_list_replace(0, NULL) && !disk_list_set_index(0));

	/* A replace names the slot; an appended slot is empty until then. */
	reset();
	CHECK(disk_list_append(NULL));
	CHECK(disk_list_count() == 1 && !strcmp(disk_list_path(0), ""));
	CHECK(disk_list_replace(0, "/d/x.iso") && !strcmp(disk_list_path(0), "/d/x.iso"));

	/* The list is fixed storage: a full one refuses, it does not grow. */
	reset();
	for (i = 0; i < DISK_LIST_SLOTS; i++)
		CHECK(disk_list_append(NULL));
	CHECK(!disk_list_append("/d/one-too-many.iso"));
	CHECK(disk_list_count() == DISK_LIST_SLOTS);

	/* The resume disc: taken when the playlist agrees, else the first. */
	reset();
	disk_list_append("/d/0.iso");
	disk_list_append("/d/1.iso");
	disk_list_append("/d/2.iso");
	disk_list_set_initial(2, "/d/2.iso");
	disk_list_apply_initial();
	CHECK(disk_list_index() == 2);
	disk_list_set_initial(2, "/d/moved.iso");
	disk_list_apply_initial();
	CHECK(disk_list_index() == 0);
	disk_list_set_initial(7, "");
	disk_list_apply_initial();
	CHECK(disk_list_index() == 0);
	disk_list_set_initial(1, "");
	disk_list_apply_initial();
	CHECK(disk_list_index() == 1);

	/* M3U: comments, CRLF and blank lines, relative and absolute
	 * entries, and a missing disc, which is skipped. */
	snprintf(a, sizeof(a), "%s/a.iso", dir);
	snprintf(b, sizeof(b), "%s/b.iso", dir);
	put(a, "a");
	put(b, "b");
	snprintf(m3u, sizeof(m3u), "%s/game.m3u", dir);
	snprintf(text, sizeof(text), "# disc list\r\na.iso\r\n\r\n;note\nmissing.iso\n%s\n\n", b);
	put(m3u, text);

	reset();
	CHECK(disk_list_load_m3u(m3u, boot, sizeof(boot)));
	CHECK(disk_list_count() == 2);
	CHECK(disk_list_count() == 2 && !strcmp(disk_list_path(0), a) && !strcmp(disk_list_path(1), b));
	CHECK(disk_list_index() == 0 && !strcmp(boot, a));

	/* Resuming on the second disc boots it. */
	reset();
	disk_list_set_initial(1, b);
	CHECK(disk_list_load_m3u(m3u, boot, sizeof(boot)));
	CHECK(disk_list_index() == 1 && !strcmp(boot, b));

	/* A playlist that cannot be read lists nothing. */
	reset();
	snprintf(path, sizeof(path), "%s/none.m3u", dir);
	CHECK(!disk_list_load_m3u(path, boot, sizeof(boot)));
	CHECK(disk_list_count() == 0);

	/* An M3U lists at most DISK_LIST_M3U_MAX discs. */
	reset();
	text[0] = '\0';
	for (i = 0; i < DISK_LIST_M3U_MAX + 3; i++)
		strcat(text, "a.iso\n");
	put(m3u, text);
	CHECK(disk_list_load_m3u(m3u, boot, sizeof(boot)));
	CHECK(disk_list_count() == DISK_LIST_M3U_MAX);

	/* Cue: a relative track is under the sheet's directory, an absolute
	 * one is taken as it is, and a sheet with no track leaves the path. */
	snprintf(cue, sizeof(cue), "%s/game.cue", dir);
	put(cue, "REM x\r\nFILE \"game (Track 1).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n");
	snprintf(path, sizeof(path), "%s", cue);
	disk_list_cue_first_track(path, sizeof(path));
	snprintf(text, sizeof(text), "%s/game (Track 1).bin", dir);
	CHECK(!strcmp(path, text));

	snprintf(text, sizeof(text), "FILE \"%s\" BINARY\n", a);
	put(cue, text);
	snprintf(path, sizeof(path), "%s", cue);
	disk_list_cue_first_track(path, sizeof(path));
	CHECK(!strcmp(path, a));

	put(cue, "FILE \"game.bin\" WAVE\n");
	snprintf(path, sizeof(path), "%s", cue);
	disk_list_cue_first_track(path, sizeof(path));
	CHECK(!strcmp(path, cue));

	if (fails)
	{
		printf("disklist: %d failed\n", fails);
		return 1;
	}
	printf("disklist: ok\n");
	return 0;
}
