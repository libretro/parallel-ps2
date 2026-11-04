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

/* Host filesystem policy that has no libretro-common equivalent: the
 * recursive directory walker.  Everything else the old FileSystem layer
 * provided is direct libretro-common calls at the call sites now. C89. */

#ifndef PCSX2_HOSTFS_H
#define PCSX2_HOSTFS_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <streams/file_stream.h>

#ifdef _WIN32
#define FS_OSPATH_SEPARATOR_CHARACTER '\\'
#define FS_OSPATH_SEPARATOR_STR "\\"
#else
#define FS_OSPATH_SEPARATOR_CHARACTER '/'
#define FS_OSPATH_SEPARATOR_STR "/"
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum FILESYSTEM_FILE_ATTRIBUTES
{
	FILESYSTEM_FILE_ATTRIBUTE_DIRECTORY = 1,
	FILESYSTEM_FILE_ATTRIBUTE_READ_ONLY = 2,
	FILESYSTEM_FILE_ATTRIBUTE_COMPRESSED = 4
};

enum FILESYSTEM_FIND_FLAGS
{
	FILESYSTEM_FIND_RECURSIVE = (1 << 0),
	FILESYSTEM_FIND_RELATIVE_PATHS = (1 << 1),
	FILESYSTEM_FIND_HIDDEN_FILES = (1 << 2),
	FILESYSTEM_FIND_FOLDERS = (1 << 3),
	FILESYSTEM_FIND_FILES = (1 << 4),
	FILESYSTEM_FIND_KEEP_ARRAY = (1 << 5)
};

typedef struct FILESYSTEM_FIND_DATA
{
	char* FileName; /* malloc'd; the results own it */
	int64_t Size;
	uint32_t Attributes;
} FILESYSTEM_FIND_DATA;

/* What a search found, in the order the directories gave it. Start it
 * zeroed; FileSystem_FreeFindResults empties it again. */
typedef struct FILESYSTEM_FIND_RESULTS
{
	FILESYSTEM_FIND_DATA* items;
	size_t count;
	size_t capacity;
} FILESYSTEM_FIND_RESULTS;

/* The entries of path matching pattern ('*' and '?' wildcards, or a
 * name), as flags select. Without FILESYSTEM_FIND_KEEP_ARRAY the results
 * are emptied first. True if anything was found. */
bool FileSystem_FindFiles(const char* path, const char* pattern, uint32_t flags,
	FILESYSTEM_FIND_RESULTS* results);
void FileSystem_FreeFindResults(FILESYSTEM_FIND_RESULTS* results);

#ifdef __cplusplus
}
#endif

#endif
