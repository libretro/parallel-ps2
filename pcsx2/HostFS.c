/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "HostFS.h"

#include <stdlib.h>
#include <string.h>

#include <compat/fnmatch.h>
#include <file/file_path.h>
#include <retro_dirent.h>

/* a and b joined, with sep between them when sep is not 0; malloc'd */
static char* hostfs_join(const char* a, char sep, const char* b)
{
	const size_t la = strlen(a);
	const size_t lb = strlen(b);
	char* s         = (char*)malloc(la + lb + 2);
	char* p;
	if (!s)
		return NULL;
	memcpy(s, a, la);
	p = s + la;
	if (sep)
		*p++ = sep;
	memcpy(p, b, lb + 1);
	return s;
}

static bool hostfs_push(FILESYSTEM_FIND_RESULTS* results, char* name, int64_t size, uint32_t attributes)
{
	FILESYSTEM_FIND_DATA* d;
	if (results->count == results->capacity)
	{
		const size_t grown = results->capacity ? results->capacity * 2 : 16;
		FILESYSTEM_FIND_DATA* items = (FILESYSTEM_FIND_DATA*)realloc(results->items, grown * sizeof(*items));
		if (!items)
			return false;
		results->items    = items;
		results->capacity = grown;
	}
	d             = &results->items[results->count++];
	d->FileName   = name;
	d->Size       = size;
	d->Attributes = attributes;
	return true;
}

static uint32_t hostfs_find(const char* dir_path, const char* rel_prefix,
	const char* pattern, uint32_t flags, FILESYSTEM_FIND_RESULTS* results)
{
	/* Small speed optimization for the '*' case. */
	const bool has_wildcards = (strpbrk(pattern, "*?") != NULL);
	const bool match_all     = has_wildcards && !strcmp(pattern, "*");
	uint32_t found           = 0;
	struct RDIR* dir         = retro_opendir_include_hidden(dir_path, true);

	if (!dir)
		return 0;
	if (retro_dirent_error(dir))
	{
		retro_closedir(dir);
		return 0;
	}

	while (retro_readdir(dir))
	{
		const char* name = retro_dirent_get_name(dir);
		bool is_dir;
		char* full_path;
		char* out_name;
		int64_t size;

		if (name[0] == '.')
		{
			if (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))
				continue;
			if (!(flags & FILESYSTEM_FIND_HIDDEN_FILES))
				continue;
		}

		is_dir    = retro_dirent_is_dir(dir, NULL);
		full_path = hostfs_join(dir_path, FS_OSPATH_SEPARATOR_CHARACTER, name);
		if (!full_path)
			continue;

		if (is_dir)
		{
			if (flags & FILESYSTEM_FIND_RECURSIVE)
			{
				char* child_prefix = hostfs_join(rel_prefix, 0, name);
				char* with_sep     = child_prefix ? hostfs_join(child_prefix, 0, FS_OSPATH_SEPARATOR_STR) : NULL;
				if (with_sep)
					found += hostfs_find(full_path, with_sep, pattern, flags, results);
				free(child_prefix);
				free(with_sep);
			}

			if (!(flags & FILESYSTEM_FIND_FOLDERS))
			{
				free(full_path);
				continue;
			}
		}
		else if (!(flags & FILESYSTEM_FIND_FILES))
		{
			free(full_path);
			continue;
		}

		if (has_wildcards)
		{
			if (!match_all && rl_fnmatch(pattern, name, 0) != 0)
			{
				free(full_path);
				continue;
			}
		}
		else if (strcmp(name, pattern) != 0)
		{
			free(full_path);
			continue;
		}

		/* Only an entry that survived the filters is sized, and an
		 * entry that cannot be sized is not an entry - a broken
		 * symlink has nothing behind it to report. */
		size = (int64_t)path_get_size(full_path);
		if (size < 0)
		{
			free(full_path);
			continue;
		}

		if (flags & FILESYSTEM_FIND_RELATIVE_PATHS)
		{
			out_name = hostfs_join(rel_prefix, 0, name);
			free(full_path);
		}
		else
			out_name = full_path;

		if (!out_name)
			continue;
		if (!hostfs_push(results, out_name, size, is_dir ? FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY : 0))
		{
			free(out_name);
			continue;
		}
		found++;
	}

	retro_closedir(dir);
	return found;
}

void FileSystem_FreeFindResults(FILESYSTEM_FIND_RESULTS* results)
{
	size_t i;
	for (i = 0; i < results->count; i++)
		free(results->items[i].FileName);
	free(results->items);
	results->items    = NULL;
	results->count    = 0;
	results->capacity = 0;
}

bool FileSystem_FindFiles(const char* path, const char* pattern, uint32_t flags,
	FILESYSTEM_FIND_RESULTS* results)
{
	if (path[0] == '\0')
		return false;
	if (!(flags & FILESYSTEM_FIND_KEEP_ARRAY))
	{
		size_t i;
		for (i = 0; i < results->count; i++)
			free(results->items[i].FileName);
		results->count = 0;
	}
	return hostfs_find(path, "", pattern, flags, results) > 0;
}
