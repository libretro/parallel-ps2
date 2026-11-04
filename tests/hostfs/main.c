/* FileSystem_FindFiles (pcsx2/HostFS.c) over a real folder: files and
 * folders as the flags select, hidden entries only when asked, '.' and
 * '..' never, a wildcard, an exact name, full and relative paths, the
 * recursive walk, results kept or emptied, a broken link left out, and
 * every name freed again (LeakSan's lane checks that). C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "HostFS.h"

static int s_fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fails++; } else printf("  ok: %s\n", what); } while (0)

static char s_root[64];

static void put(const char* rel, size_t size)
{
	char path[256];
	FILE* f;
	size_t i;
	sprintf(path, "%s/%s", s_root, rel);
	f = fopen(path, "wb");
	if (!f) { perror(path); exit(2); }
	for (i = 0; i < size; i++)
		fputc('x', f);
	fclose(f);
}

static void dir(const char* rel)
{
	char path[256];
	sprintf(path, "%s/%s", s_root, rel);
	mkdir(path, 0700);
}

/* Whether the results hold this name (full paths are given relative to
 * the root here and matched as its suffix). */
static int has(const FILESYSTEM_FIND_RESULTS* r, const char* name, int full)
{
	size_t i;
	char want[256];
	if (full)
		sprintf(want, "%s/%s", s_root, name);
	else
		strcpy(want, name);
	for (i = 0; i < r->count; i++)
		if (!strcmp(r->items[i].FileName, want))
			return 1;
	return 0;
}

static const FILESYSTEM_FIND_DATA* get(const FILESYSTEM_FIND_RESULTS* r, const char* name)
{
	size_t i;
	for (i = 0; i < r->count; i++)
		if (!strcmp(r->items[i].FileName, name))
			return &r->items[i];
	return NULL;
}

int main(void)
{
	FILESYSTEM_FIND_RESULTS r;
	const FILESYSTEM_FIND_DATA* d;
	char link_path[256];
	char cmd[128];

	strcpy(s_root, "/tmp/hostfs.XXXXXX");
	if (!mkdtemp(s_root)) { perror("mkdtemp"); return 2; }
	put("a.bin", 10);
	put("b.txt", 3);
	put(".hidden.bin", 1);
	dir("sub");
	put("sub/c.bin", 7);
	dir("sub/deeper");
	put("sub/deeper/d.bin", 5);
	dir(".hiddendir");
	put(".hiddendir/e.bin", 2);
	sprintf(link_path, "%s/broken.bin", s_root);
	if (symlink("/nonexistent/hostfs-target", link_path) != 0)
		perror("symlink");

	memset(&r, 0, sizeof(r));

	printf("== flat ==\n");
	CHECK(FileSystem_FindFiles(s_root, "*", FILESYSTEM_FIND_FILES, &r), "files are found");
	CHECK(r.count == 2 && has(&r, "a.bin", 1) && has(&r, "b.txt", 1),
		"files only, full paths, no hidden file, no broken link");
	d = get(&r, r.count ? r.items[0].FileName : "");
	CHECK(d && d->Attributes == 0, "a file has no directory attribute");
	{
		char p[256];
		sprintf(p, "%s/a.bin", s_root);
		d = get(&r, p);
		CHECK(d && d->Size == 10, "a file's size");
	}

	FileSystem_FindFiles(s_root, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES, &r);
	CHECK(r.count == 3 && has(&r, ".hidden.bin", 1), "a hidden file when asked; the results emptied first");

	FileSystem_FindFiles(s_root, "*", FILESYSTEM_FIND_FOLDERS, &r);
	CHECK(r.count == 1 && has(&r, "sub", 1) && r.items[0].Attributes == FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY,
		"folders only, without '.', '..' or a hidden one");

	FileSystem_FindFiles(s_root, "*.bin", FILESYSTEM_FIND_FILES, &r);
	CHECK(r.count == 1 && has(&r, "a.bin", 1), "a wildcard pattern");

	FileSystem_FindFiles(s_root, "b.txt", FILESYSTEM_FIND_FILES, &r);
	CHECK(r.count == 1 && has(&r, "b.txt", 1), "an exact name");
	CHECK(!FileSystem_FindFiles(s_root, "a.BIN", FILESYSTEM_FIND_FILES, &r) && r.count == 0,
		"an exact name is matched exactly");

	FileSystem_FindFiles(s_root, "a.bin", FILESYSTEM_FIND_FILES, &r);
	FileSystem_FindFiles(s_root, "b.txt", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_KEEP_ARRAY, &r);
	CHECK(r.count == 2, "KEEP_ARRAY adds to what is there");

	printf("== recursive ==\n");
	FileSystem_FindFiles(s_root, "*.bin", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS, &r);
	CHECK(r.count == 3 && has(&r, "a.bin", 0) && has(&r, "sub/c.bin", 0) && has(&r, "sub/deeper/d.bin", 0),
		"every level, relative paths, not into a hidden folder");

	FileSystem_FindFiles(s_root, "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_FOLDERS
		| FILESYSTEM_FIND_RECURSIVE | FILESYSTEM_FIND_RELATIVE_PATHS | FILESYSTEM_FIND_HIDDEN_FILES, &r);
	CHECK(r.count == 9 && has(&r, ".hiddendir/e.bin", 0) && has(&r, "sub/deeper", 0) && has(&r, ".hidden.bin", 0),
		"everything, with hidden entries and folders: nine");

	printf("== edges ==\n");
	CHECK(!FileSystem_FindFiles("", "*", FILESYSTEM_FIND_FILES, &r), "an empty path finds nothing");
	CHECK(!FileSystem_FindFiles("/nonexistent/hostfs", "*", FILESYSTEM_FIND_FILES, &r) && r.count == 0,
		"a missing folder finds nothing");

	FileSystem_FreeFindResults(&r);
	CHECK(r.items == NULL && r.count == 0 && r.capacity == 0, "freeing empties the results");
	CHECK(FileSystem_FindFiles(s_root, "*", FILESYSTEM_FIND_FILES, &r) && r.count == 2, "and they can be used again");
	FileSystem_FreeFindResults(&r);

	sprintf(cmd, "rm -rf '%s'", s_root);
	if (system(cmd) != 0)
		printf("  (could not remove %s)\n", s_root);

	printf("hostfs: %s\n", s_fails ? "FAIL" : "ok");
	return s_fails ? 1 : 0;
}
