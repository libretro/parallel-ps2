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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>

#include "common/Pcsx2Defs.h"
#include "HostFS.h"

#include "../Common.h"

#include "BiosTools.h"

#define MIN_BIOS_SIZE (4 * _1mb)
#define MAX_BIOS_SIZE (8 * _1mb)

/* How far the romdir is looked for, in 16-byte entries: 8 MiB, the
 * largest image there is. */
#define ROMDIR_SCAN_ENTRIES (512 * 1024)
/* Entries read per call while looking for it: a non-BIOS file the
 * folder scan opens costs a few hundred reads, not half a million. */
#define ROMDIR_SCAN_CHUNK 64

/* --------------------------------------------------------------------
 * romdir structure (packing required!)
 * -------------------------------------------------------------------- */
#pragma pack(push, 1)

typedef struct romdir
{
	char fileName[10];
	u16 extInfoSize;
	u32 fileSize;
} romdir;

#pragma pack(pop)

u32 BiosVersion;
u32 BiosChecksum;
u32 BiosRegion;
bool BiosIsArcade;
bool NoOSD;
bool AllowParams1;
bool AllowParams2;
/* Bounded: the ROM version block these come from is fixed-size. */
char BiosDescription[BIOS_DESCRIPTION_MAX];
char BiosSerial[BIOS_SERIAL_MAX];
char BiosPath[PCSX2_PATH_MAX];
BiosDebugInformation CurrentBiosInformation;

/* Leaves fp just past the RESET entry, which goes in *rd. The table sits
 * on a 16-byte boundary, so whole entries are read a chunk at a time. */
static bool FindRomdir(RFILE *fp, romdir *rd)
{
	romdir chunk[ROMDIR_SCAN_CHUNK];
	u32 scanned = 0;

	while (scanned < ROMDIR_SCAN_ENTRIES)
	{
		const int64_t got = filestream_read(fp, chunk, sizeof(chunk));
		u32 n, i;

		if (got < (int64_t)sizeof(romdir))
			return false;
		n = (u32)(got / (int64_t)sizeof(romdir));
		if (n > ROMDIR_SCAN_ENTRIES - scanned)
			n = ROMDIR_SCAN_ENTRIES - scanned;
		for (i = 0; i < n; i++)
		{
			if (strncmp(chunk[i].fileName, "RESET", sizeof(chunk[i].fileName)) == 0)
			{
				*rd = chunk[i];
				return filestream_seek(fp, (int64_t)(scanned + i + 1) * (int64_t)sizeof(romdir),
						RETRO_VFS_SEEK_POSITION_START) == 0;
			}
		}
		scanned += n;
		if (got < (int64_t)sizeof(chunk))
			return false;
	}
	return false;
}

static bool LoadBiosVersion(RFILE *fp, u32 *version, char *description, size_t description_size, u32 *region, char *zone, size_t zone_size, char *serial, size_t serial_size)
{
	romdir rd;
	s64 fileOffset = 0;
	s64 fileSize;
	bool foundRomVer = false;
	char romver[14 + 1];  /* ascii version loaded from disk */
	char extinfo[15 + 1]; /* ascii version loaded from disk */
	char vermaj[3];
	char vermin[3];

	memset(romver, 0, sizeof(romver));
	memset(extinfo, 0, sizeof(extinfo));
	/* The serial comes from EXTINFO, which not every BIOS has (FPS2BIOS
	 * has none): without it the serial is empty, not what the buffer
	 * held - IsBIOS's is uninitialised, and BiosSerial would keep the
	 * last BIOS's. */
	if (serial_size)
		serial[0] = '\0';
	if (!FindRomdir(fp, &rd))
		return false;

	fileSize = filestream_get_size(fp);

	/* ensure it's a null-terminated and not zero-length string */
	while (rd.fileName[0] != '\0' && strnlen(rd.fileName, sizeof(rd.fileName)) != sizeof(rd.fileName))
	{
		if (strncmp(rd.fileName, "EXTINFO", sizeof(rd.fileName)) == 0)
		{
			s64 pos = filestream_tell(fp);
			if (filestream_seek(fp, fileOffset + 0x10, RETRO_VFS_SEEK_POSITION_START) != 0 ||
				filestream_read(fp, extinfo, 15) != (int64_t)(15) || filestream_seek(fp, pos, RETRO_VFS_SEEK_POSITION_START) != 0)
				break;
			strlcpy(serial, extinfo, serial_size);
		}

		if (strncmp(rd.fileName, "ROMVER", sizeof(rd.fileName)) == 0)
		{
			s64 pos = filestream_tell(fp);
			if (filestream_seek(fp, fileOffset, RETRO_VFS_SEEK_POSITION_START) != 0 ||
				filestream_read(fp, romver, 14) != (int64_t)(14) || filestream_seek(fp, pos, RETRO_VFS_SEEK_POSITION_START) != 0)
				break;

			foundRomVer = true;
		}

		if ((rd.fileSize % 0x10) == 0)
			fileOffset += rd.fileSize;
		else
			fileOffset += (rd.fileSize + 0x10) & 0xfffffff0;

		if (filestream_read(fp, &rd, sizeof(rd)) != (int64_t)(sizeof(rd)))
			break;
	}

	fileOffset -= ((rd.fileSize + 0x10) & 0xfffffff0) - rd.fileSize;

	if (!foundRomVer)
		return false;

	switch (romver[4])
	{
		/* clang-format off */
		case 'J': strlcpy(zone, "Japan", zone_size);  (*region) = 0;  break;
		case 'A': strlcpy(zone, "USA", zone_size);    (*region) = 1;  break;
		case 'E': strlcpy(zone, "Europe", zone_size); (*region) = 2;  break;
		/* case 'E': Oceania, region 3 - not implemented */
		case 'H': strlcpy(zone, "Asia", zone_size);   (*region) = 4;  break;
		/* case 'E': Russia, region 3 - not implemented */
		case 'C': strlcpy(zone, "China", zone_size);  (*region) = 6;  break;
		/* case 'A': Mexico, region 7 - not implemented */
		case 'T': strlcpy(zone, (romver[5]=='Z') ? "COH-H" : "T10K", zone_size);   (*region) = 8;  break;
		case 'X': strlcpy(zone, "Test", zone_size);   (*region) = 9;  break;
		case 'P': strlcpy(zone, "Free", zone_size);   (*region) = 10; break;
		/* clang-format on */
		default:
			zone[0] = romver[4];
			zone[1] = '\0';
			(*region) = 0;
			break;
	}
	/* TODO: some regions can be detected only from rom1's DVDID[4]:
	 * 'O' Oceania (3), 'R' Russia (5), 'M' Mexico (7). */

	vermaj[0] = romver[0];
	vermaj[1] = romver[1];
	vermaj[2] = 0;
	vermin[0] = romver[2];
	vermin[1] = romver[3];
	vermin[2] = 0;
	snprintf(description, description_size, "%-7s v%s.%s(%c%c/%c%c/%c%c%c%c)  %s %s",
		zone,
		vermaj, vermin,
		romver[12], romver[13], /* day */
		romver[10], romver[11], /* month */
		romver[6], romver[7], romver[8], romver[9], /* year! */
		(romver[5] == 'C') ? "Console" : (romver[5] == 'D') ? "Devel" : "",
		serial);

	(*version) = strtol(vermaj, (char**)NULL, 0) << 8;
	(*version) |= strtol(vermin, (char**)NULL, 0);

	log_cb(RETRO_LOG_INFO, "Bios Found: %s\n", description);

	/* We force users to have correct BIOSes, not that lame scph10000
	 * of 513KB ;-) */
	if (fileSize < (int)fileOffset)
	{
		const size_t used = strlen(description);
		snprintf(description + used, description_size - used, " %d%%",
				(int)((fileSize * 100) / (int)fileOffset));
	}

	return true;
}

static u32 ChecksumIt(const u8 *srcdata, size_t size)
{
	const u32 *words = (const u32*)srcdata;
	u32 result = 0;
	size_t i;
	for (i = 0; i < size / 4; ++i)
		result ^= words[i];
	return result;
}

/* Loads a BIOS ROM sub-component (ext is "rom1" or "rom2") from beside the
 * BIOS: name.bin.rom1 first, then name.rom1. Missing is not an error;
 * most games never touch either. The name is built in the caller's
 * buffer, which keeps LoadBIOS's frame to one path. */
static void LoadExtraRom(const char* ext, u8 *dest, size_t size, char* name, size_t name_size)
{
	RFILE *fp;
	s64 filesize;
	size_t want;

	/* A name that does not fit names no file. */
	filesize = (size_t)snprintf(name, name_size, "%s.%s", BiosPath, ext) < name_size
		? path_get_size(name) : 0;
	if (filesize <= 0)
	{
		if (strrchr(BiosPath, '.'))
		{
			/* The name's own extension, if it has one, gives way to ext. */
			const char* dot = strrchr(path_basename(BiosPath), '.');
			const size_t len = dot ? (size_t)(dot - BiosPath) : strlen(BiosPath);
			snprintf(name, name_size, "%.*s.%s", (int)len, BiosPath, ext);
		}
		else
			strlcpy(name, BiosPath, name_size);
		if ((filesize = path_get_size(name)) <= 0)
		{
			log_cb(RETRO_LOG_INFO, "BIOS %s module not found, skipping...\n", ext);
			return;
		}
	}

	fp   = filestream_open(name, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	want = (size_t)pcsx2_min_s64((s64)size, filesize);
	if (!fp || filestream_read(fp, dest, want) != (int64_t)want)
		log_cb(RETRO_LOG_WARN, "BIOS Warning: %s could not be read (permission denied?)\n", ext);
	if (fp)
		filestream_close(fp);
}

static void LoadIrx(const char* filename, u8* dest, size_t maxSize)
{
	s64 filesize, readSize;
	RFILE *fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return;

	filesize = filestream_get_size(fp);
	readSize = pcsx2_min_s64(filesize, (s64)maxSize);
	if (filestream_read(fp, dest, readSize) != (int64_t)(readSize))
		log_cb(RETRO_LOG_WARN, "IRX Warning: %s could not be read\n", filename);
	filestream_close(fp);
}

/* The first BIOS image in the BIOS folder, into out; false if none. */
static bool FindBiosImage(char* out, size_t out_size)
{
	const char* dir = emu_folders_bios();
	FILESYSTEM_FIND_RESULTS results;
	u32 version, region;
	char description[BIOS_DESCRIPTION_MAX];
	char zone[BIOS_ZONE_MAX];
	size_t i;

	memset(&results, 0, sizeof(results));
	out[0] = '\0';
	log_cb(RETRO_LOG_INFO, "Searching for a BIOS image in '%s'...\n", dir);

	if (FileSystem_FindFiles(dir, "*", FILESYSTEM_FIND_FILES, &results))
	{
		for (i = 0; i < results.count; i++)
		{
			const FILESYSTEM_FIND_DATA* fd = &results.items[i];
			if (fd->Size < MIN_BIOS_SIZE || fd->Size > MAX_BIOS_SIZE)
				continue;

			if (IsBIOS(fd->FileName, &version, description, sizeof(description), &region, zone, sizeof(zone)))
			{
				log_cb(RETRO_LOG_INFO, "Using BIOS '%s' (%s %s)\n", fd->FileName, description, zone);
				strlcpy(out, fd->FileName, out_size);
				break;
			}
		}
	}

	FileSystem_FreeFindResults(&results);
	if (!out[0])
		log_cb(RETRO_LOG_ERROR, "Unable to auto locate a BIOS image\n");
	return out[0] != '\0';
}

/* Whether the BIOS at path is a COH-H one. */
static bool IsArcadeBios(const char* path)
{
	u32 version, region;
	char description[BIOS_DESCRIPTION_MAX];
	char zone[BIOS_ZONE_MAX];
	return IsBIOS(path, &version, description, sizeof(description), &region, zone, sizeof(zone))
		&& strcmp(zone, "COH-H") == 0;
}

bool FindArcadeBios(const char* dir, const char* selected, char* out, size_t out_size)
{
	FILESYSTEM_FIND_RESULTS results;
	const char* best = NULL;
	size_t i;

	if (selected && selected[0])
	{
		char path[PCSX2_PATH_MAX];
		pcsx2_path_join(path, sizeof(path), dir, selected);
		if (IsArcadeBios(path))
		{
			strlcpy(out, selected, out_size);
			return true;
		}
	}

	/* By name, so the choice does not follow the directory's order. */
	memset(&results, 0, sizeof(results));
	if (FileSystem_FindFiles(dir, "*", FILESYSTEM_FIND_FILES, &results))
		for (i = 0; i < results.count; i++)
		{
			const FILESYSTEM_FIND_DATA* fd = &results.items[i];
			const char* name = path_basename(fd->FileName);
			if (fd->Size < MIN_BIOS_SIZE || fd->Size > MAX_BIOS_SIZE)
				continue;
			if ((!best || strcmp(name, best) < 0) && IsArcadeBios(fd->FileName))
				best = name;
		}
	if (best)
		strlcpy(out, best, out_size);
	FileSystem_FreeFindResults(&results);
	return best != NULL;
}

/* Loads the configured BIOS ROM file into PS2 memory, which must already
 * be allocated. Missing ROM1 or ROM2 files are not a failure, since none
 * are required for most emulation tasks; a primary image that is missing
 * or unreadable is. */
bool LoadBIOS(void)
{
	char path[PCSX2_PATH_MAX];
	char zone[BIOS_ZONE_MAX];
	RFILE *fp;
	s64 filesize, romsize;

	emu_config_bios_path(path, sizeof(path));
	if (path[0] == '\0' || !path_is_valid(path))
	{
		if (path[0])
			log_cb(RETRO_LOG_WARN, "Configured BIOS '%s' does not exist, trying to find an alternative.\n",
				emu_config_bios_name());
		if (!FindBiosImage(path, sizeof(path)))
			return false;
	}

	fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return false;

	filesize = filestream_get_size(fp);
	if (filesize <= 0)
	{
		filestream_close(fp);
		return false;
	}

	zone[0] = '\0';
	LoadBiosVersion(fp, &BiosVersion, BiosDescription, sizeof(BiosDescription),
			&BiosRegion, zone, sizeof(zone), BiosSerial, sizeof(BiosSerial));
	BiosIsArcade = strcmp(zone, "COH-H") == 0;
	if (BiosIsArcade)
		log_cb(RETRO_LOG_INFO, "BIOS is an arcade (COH-H) BIOS\n");

	/* Straight into the ROM, no staging buffer. */
	romsize = pcsx2_min_s64(PS2MEM_ROM, filesize);
	if (filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_START) ||
		filestream_read(fp, eeMem->ROM, romsize) != (int64_t)romsize)
	{
		filestream_close(fp);
		return false;
	}
	filestream_close(fp);

	/* If file is less than 2MB it doesn't have an OSD (Devel consoles),
	 * so skip HLEing OSDSys Param stuff. */
	NoOSD = filesize < 2465792;

	BiosChecksum = ChecksumIt(eeMem->ROM, sizeof(eeMem->ROM));
	strlcpy(BiosPath, path, PCSX2_PATH_MAX);

	/* path is in BiosPath now; its buffer names the extra ROMs. */
	LoadExtraRom("rom1", eeMem->ROM1, sizeof(eeMem->ROM1), path, sizeof(path));
	LoadExtraRom("rom2", eeMem->ROM2, sizeof(eeMem->ROM2), path, sizeof(path));

	if (emu_config_irx_injected())
		LoadIrx(emu_config_current_irx(), &eeMem->ROM[0x3C0000], sizeof(eeMem->ROM) - 0x3C0000);

	CurrentBiosInformation.eeThreadListAddr = 0;
	return true;
}

bool IsBIOS(const char *filename, u32 *version, char *description, size_t description_size, u32 *region, char *zone, size_t zone_size)
{
	char serial[BIOS_SERIAL_MAX];
	bool ret;
	RFILE *fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return false;
	/* FPS2BIOS is smaller and of variable size */
	ret = LoadBiosVersion(fp, version, description, description_size, region, zone, zone_size, serial, sizeof(serial));
	filestream_close(fp);
	return ret;
}

bool IsBIOSAvailable(const char* full_path)
{
	char found[PCSX2_PATH_MAX];

	/* EmuConfig may not be loaded yet, so the caller passes the path. */
	if (full_path && full_path[0] && path_is_valid(full_path))
		return true;

	/* No BIOS configured, or the configured one is missing: look for
	 * one in the BIOS folder. */
	return FindBiosImage(found, sizeof(found)) && path_is_valid(found);
}
