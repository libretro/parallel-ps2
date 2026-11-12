/* The disc playlist (disk_list.h). */
#include <stdio.h>
#include <string.h>

#include <libretro.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <compat/strl.h>

#include "common/Pcsx2Defs.h"
#include "disk_list.h"

extern retro_log_printf_t log_cb;

static char     disk_paths[DISK_LIST_SLOTS][PCSX2_PATH_MAX];
static unsigned disk_count = 0;
static int      disk_index = 0;

static int  initial_index = 0;
static char initial_path[PCSX2_PATH_MAX];

/* The M3U reader's line, directory and joined path. */
static char m3u_line[PCSX2_PATH_MAX];
static char m3u_dir[PCSX2_PATH_MAX];
static char m3u_abs[PCSX2_PATH_MAX];

void disk_list_clear(void)
{
	disk_count = 0;
	disk_index = 0;
}

unsigned disk_list_count(void)
{
	return disk_count;
}

const char* disk_list_path(unsigned index)
{
	return index < disk_count ? disk_paths[index] : NULL;
}

int disk_list_index(void)
{
	return disk_index;
}

bool disk_list_set_index(unsigned index)
{
	if (index >= disk_count)
		return false;
	disk_index = (int)index;
	return true;
}

bool disk_list_append(const char* path)
{
	if (disk_count >= DISK_LIST_SLOTS)
		return false;
	strlcpy(disk_paths[disk_count++], path ? path : "", sizeof(disk_paths[0]));
	return true;
}

bool disk_list_replace(unsigned index, const char* path)
{
	if (index >= disk_count)
		return false;

	if (path)
	{
		strlcpy(disk_paths[index], path, sizeof(disk_paths[index]));
		return true;
	}

	memmove(disk_paths[index], disk_paths[index + 1],
		(disk_count - index - 1) * sizeof(disk_paths[0]));
	disk_count--;
	if (!disk_count)
		disk_index = -1;
	else if (disk_index > (int)index)
		disk_index--;
	/* Removing the entry the tray is pointing at shifts whatever followed
	 * it down into that slot; removing the last entry leaves the index
	 * past the end. */
	if (disk_index >= (int)disk_count)
		disk_index = (int)disk_count - 1;
	return true;
}

void disk_list_set_initial(unsigned index, const char* path)
{
	initial_index = (int)index;
	strlcpy(initial_path, path ? path : "", sizeof(initial_path));
}

/* libretro's contract is to fall back to index 0 when the index is out
 * of range or the path at that index is not the one the frontend named,
 * which is how a playlist edited between sessions stops the wrong disc
 * being inserted. */
void disk_list_apply_initial(void)
{
	if (     initial_index > 0
		 && initial_index < (int)disk_count
		 && (!initial_path[0]
			 || !strcmp(disk_paths[initial_index], initial_path)))
		disk_list_set_index((unsigned)initial_index);
	else
		disk_list_set_index(0);
}

void disk_list_cue_first_track(char* path, size_t size)
{
	/* PCSX2 doesn't handle cue files, so find the first
	 * 'FILE "<gametrack>.bin" BINARY' line and take the track from it. */
	char buffer[1024];
	const char* line_start = "FILE \"";
	const char* line_end   = "\" BINARY";
	RFILE* cue_file;

	/* Through the VFS, like every other file this core opens: fopen()
	 * takes the path in the local 8-bit encoding, which on Windows
	 * cannot name every path. */
	cue_file = filestream_open(path,
			RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!cue_file)
	{
		log_cb(RETRO_LOG_ERROR, "Failed to open cue file.\n");
		return;
	}

	while (filestream_gets(cue_file, buffer, sizeof(buffer)))
	{
		char* start = strstr(buffer, line_start);
		char* end;
		if (!start)
			continue;
		start += strlen(line_start);
		end = strstr(start, line_end);
		if (!end)
			continue;
		*end = '\0';
		filestream_close(cue_file);
		/* An absolute FILE entry is already the path; a relative one is
		 * under the cue's own directory. */
		if (path_is_absolute(start))
			strlcpy(path, start, size);
		else
		{
			/* path_basedir writes "./" for a bare name. */
			char basedir[PCSX2_PATH_MAX + 2];
			strlcpy(basedir, path, sizeof(basedir));
			path_basedir(basedir);
			snprintf(path, size, "%s%s", basedir, start);
		}
		return;
	}

	log_cb(RETRO_LOG_ERROR, "Failed to find a valid track from cue file.\n");
	filestream_close(cue_file);
}

bool disk_list_load_m3u(const char* m3u_path, char* boot, size_t size)
{
	RFILE* fd = filestream_open(m3u_path,
			RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	size_t len;
	const char* first;

	if (!fd)
		return false;

	m3u_abs[0] = '\0';
	strlcpy(m3u_dir, m3u_path, sizeof(m3u_dir));
	path_basedir(m3u_dir);

	while (disk_count < DISK_LIST_M3U_MAX
		&& filestream_gets(fd, m3u_line, sizeof(m3u_line)) != NULL)
	{
		if (m3u_line[0] == '#' || m3u_line[0] == ';')
			continue;

		len = strlen(m3u_line);
		if (len && m3u_line[len - 1] == '\n')
			m3u_line[--len] = '\0';
		if (len && m3u_line[len - 1] == '\r')
			m3u_line[--len] = '\0';

		/* A blank line, CRLF or not, joined to the playlist's directory
		 * yields the directory itself, which path_is_valid() is happy
		 * with: a trailing blank line, which most editors leave, added
		 * the folder as a disc. */
		if (!len)
			continue;

		/* An absolute entry is already the path. Joining it to the
		 * playlist's directory produced things like '/tmp//tmp/game.iso',
		 * so every M3U written with absolute paths - legal, and what a
		 * generator that does not know where the playlist will live has
		 * to emit - listed no discs at all. */
		if (path_is_absolute(m3u_line))
			strlcpy(m3u_abs, m3u_line, sizeof(m3u_abs));
		else
			fill_pathname_join(m3u_abs, m3u_dir, m3u_line, sizeof(m3u_abs));

		if (path_is_valid(m3u_abs))
		{
			disk_list_append(m3u_abs);
			log_cb(RETRO_LOG_INFO, "Disk #%u added from M3U: \"%s\".\n", disk_count, m3u_abs);
		}
	}

	/* The handle is released on every path: a multi-disc load used to
	 * leak an RFILE and the 64 KiB buffer the VFS attaches to it. */
	filestream_close(fd);

	disk_list_apply_initial();
	/* The disc to boot; with nothing listed or an empty slot chosen, the
	 * last path the reader formed, as before. */
	first = disk_index >= 0 ? disk_list_path((unsigned)disk_index) : NULL;
	strlcpy(boot, first && first[0] ? first : m3u_abs, size);
	return true;
}
