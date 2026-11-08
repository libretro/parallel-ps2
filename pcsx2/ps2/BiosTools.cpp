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

#include <compat/strl.h>
#include "../FormatString.h"
#include "common/Pcsx2Defs.h"
#include <cstring>
#include <utility>

#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>

#include "HostFS.h"

#include "../Common.h"
#include "../Config.h"

#include "BiosTools.h"

static constexpr u32 MIN_BIOS_SIZE = 4 * _1mb;
static constexpr u32 MAX_BIOS_SIZE = 8 * _1mb;

// --------------------------------------------------------------------------------------
// romdir structure (packing required!)
// --------------------------------------------------------------------------------------
//
#pragma pack(push, 1)

struct romdir
{
	char fileName[10];
	u16 extInfoSize;
	u32 fileSize;
};

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

static bool LoadBiosVersion(RFILE *fp, u32 *version, char *description, size_t description_size, u32 *region, char *zone, size_t zone_size, char *serial, size_t serial_size)
{
	romdir rd;
	/* The serial comes from EXTINFO, which not every BIOS has (FPS2BIOS
	 * has none): without it the serial is empty, not what the buffer
	 * held - IsBIOS's is uninitialised, and BiosSerial would keep the
	 * last BIOS's. */
	if (serial_size)
		serial[0] = '\0';
	for (u32 i = 0; i < 512 * 1024; i++)
	{
		if (filestream_read(fp, &rd, sizeof(rd)) != (int64_t)(sizeof(rd)))
			return false;

		if (strncmp(rd.fileName, "RESET", sizeof(rd.fileName)) == 0)
			break; /* found romdir */
	}

	s64 fileOffset = 0;
	s64 fileSize = filestream_get_size(fp);
	bool foundRomVer = false;
	char romver[14 + 1] = {}; // ascii version loaded from disk.
	char extinfo[15 + 1] = {}; // ascii version loaded from disk.

	// ensure it's a null-terminated and not zero-length string
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

	if (foundRomVer)
	{
		switch (romver[4])
		{
			// clang-format off
			case 'J': strlcpy(zone, "Japan", zone_size);  (*region) = 0;  break;
			case 'A': strlcpy(zone, "USA", zone_size);    (*region) = 1;  break;
			case 'E': strlcpy(zone, "Europe", zone_size); (*region) = 2;  break;
			// case 'E': strlcpy(zone, "Oceania", zone_size);(*region) = 3;  break; // Not implemented
			case 'H': strlcpy(zone, "Asia", zone_size);   (*region) = 4;  break;
			// case 'E': strlcpy(zone, "Russia", zone_size); (*region) = 3;  break; // Not implemented
			case 'C': strlcpy(zone, "China", zone_size);  (*region) = 6;  break;
			// case 'A': strlcpy(zone, "Mexico", zone_size); (*region) = 7;  break; // Not implemented
			case 'T': strlcpy(zone, (romver[5]=='Z') ? "COH-H" : "T10K", zone_size);   (*region) = 8;  break;
			case 'X': strlcpy(zone, "Test", zone_size);   (*region) = 9;  break;
			case 'P': strlcpy(zone, "Free", zone_size);   (*region) = 10; break;
			// clang-format on
			default:
				zone[0] = romver[4];
				zone[1] = '\0';
				(*region) = 0;
				break;
		}
		// TODO: some regions can be detected only from rom1
		/* switch (rom1:DVDID[4])
		{
			// clang-format off
			case 'O': strlcpy(zone, "Oceania", zone_size);(*region) = 3;  break;
			case 'R': strlcpy(zone, "Russia", zone_size); (*region) = 5;  break;
			case 'M': strlcpy(zone, "Mexico", zone_size); (*region) = 7;  break;
			// clang-format on
		} */

		char vermaj[3] = {romver[0], romver[1], 0};
		char vermin[3] = {romver[2], romver[3], 0};
		snprintf(description, description_size, "%-7s v%s.%s(%c%c/%c%c/%c%c%c%c)  %s %s",
			zone,
			vermaj, vermin,
			romver[12], romver[13], // day
			romver[10], romver[11], // month
			romver[6], romver[7], romver[8], romver[9], // year!
			(romver[5] == 'C') ? "Console" : (romver[5] == 'D') ? "Devel" :
																  "",
			serial);

		(*version) = strtol(vermaj, (char**)NULL, 0) << 8;
		(*version) |= strtol(vermin, (char**)NULL, 0);

		log_cb(RETRO_LOG_INFO, "Bios Found: %s\n", description);
	}
	else
		return false;

	if (fileSize < (int)fileOffset)
	{
		{
			const size_t used = strlen(description);
			snprintf(description + used, description_size - used, " %d%%",
					(int)((fileSize * 100) / (int)fileOffset));
		}
		// we force users to have correct bioses,
		// not that lame scph10000 of 513KB ;-)
	}

	return true;
}

template <size_t _size>
static void ChecksumIt(u32& result, const u8 (&srcdata)[_size])
{
	for (size_t i = 0; i < _size / 4; ++i)
		result ^= ((u32*)srcdata)[i];
}

// Attempts to load a BIOS rom sub-component, by trying multiple combinations of base
// filename and extension.  The bios specified in the user's configuration is used as
// the base.
//
// Parameters:
//   ext - extension of the sub-component to load. Valid options are ROM1 and ROM2.
//
template <size_t _size>
static void LoadExtraRom(const char* ext, u8 (&dest)[_size])
{
	// Try first a basic extension concatenation (normally results in something like name.bin.rom1)
	std::string Bios1(FormatString::Format("%s.%s", BiosPath, ext));

	s64 filesize;
	if ((filesize = path_get_size(Bios1.c_str())) <= 0)
	{
		// Try the name properly extensioned next (name.rom1)
		if (strrchr(BiosPath, '.'))
		{
			/* The name's own extension, if it has one, gives way to ext. */
			const char* full = BiosPath;
			const char* dot  = strrchr(path_basename(full), '.');
			Bios1.assign(full, dot ? (size_t)(dot - full) : strlen(BiosPath));
			Bios1 += '.';
			Bios1 += ext;
		}
		else
			Bios1 = BiosPath;
		if ((filesize = path_get_size(Bios1.c_str())) <= 0)
		{
			log_cb(RETRO_LOG_INFO, "BIOS %s module not found, skipping...\n", ext);
			return;
		}
	}

	RFILE *fp = filestream_open(Bios1.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	const size_t want = static_cast<size_t>(pcsx2_min_s64(_size, filesize));
	if (!fp || filestream_read(fp, dest, want) != (int64_t)want)
		log_cb(RETRO_LOG_WARN, "BIOS Warning: %s could not be read (permission denied?)\n", ext);
	if (fp)
		filestream_close(fp);
}

static void LoadIrx(const std::string& filename, u8* dest, size_t maxSize)
{
	RFILE *fp = filestream_open(filename.c_str(), RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return;

	const s64 filesize = filestream_get_size(fp);
	const s64 readSize = pcsx2_min_s64(filesize, static_cast<s64>(maxSize));
	if (filestream_read(fp, dest, readSize) == (int64_t)(readSize))
	{
		filestream_close(fp);
		return;
	}

	log_cb(RETRO_LOG_WARN, "IRX Warning: %s could not be read\n", filename.c_str());
	filestream_close(fp);
}

static std::string FindBiosImage(void)
{
	log_cb(RETRO_LOG_INFO, "Searching for a BIOS image in '%s'...\n", EmuFolders::Bios);

	FILESYSTEM_FIND_RESULTS results = {};
	if (!FileSystem_FindFiles(EmuFolders::Bios, "*", FILESYSTEM_FIND_FILES, &results))
	{
		FileSystem_FreeFindResults(&results);
		return std::string();
	}

	u32 version, region;
	char description[BIOS_DESCRIPTION_MAX];
	char zone[BIOS_ZONE_MAX];
	for (size_t i = 0; i < results.count; i++)
	{
		const FILESYSTEM_FIND_DATA& fd = results.items[i];
		if (fd.Size < MIN_BIOS_SIZE || fd.Size > MAX_BIOS_SIZE)
			continue;

		if (IsBIOS(fd.FileName, &version, description, sizeof(description), &region, zone, sizeof(zone)))
		{
			log_cb(RETRO_LOG_INFO, "Using BIOS '%s' (%s %s)\n", fd.FileName, description, zone);
			std::string found(fd.FileName);
			FileSystem_FreeFindResults(&results);
			return found;
		}
	}

	FileSystem_FreeFindResults(&results);
	log_cb(RETRO_LOG_ERROR, "Unable to auto locate a BIOS image\n");
	return std::string();
}

// Loads the configured BIOS ROM file into PS2 memory.  PS2 memory must be allocated prior to
// this method being called.
//
// Remarks:
//   This function does not fail if ROM1 or ROM2 files are missing, since none are
//   explicitly required for most emulation tasks.
//
// Exceptions:
//   BadStream - Thrown if the primary BIOS file (usually .bin) is not found, corrupted, etc.
//
bool LoadBIOS(void)
{
	char path[PCSX2_PATH_MAX];

	EmuConfig.FullpathToBios(path, sizeof(path));
	if (path[0] == '\0' || !path_is_valid(path))
	{
		if (path[0])
			log_cb(RETRO_LOG_WARN, "Configured BIOS '%s' does not exist, trying to find an alternative.\n",
				EmuConfig.BaseFilenames.Bios);

		{
			const std::string found(FindBiosImage());
			if (found.empty())
				return false;
			strlcpy(path, found.c_str(), sizeof(path));
		}
	}

	RFILE *fp = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return false;

	const s64 filesize = filestream_get_size(fp);
	if (filesize <= 0)
	{
		filestream_close(fp);
		return false;
	}

	char zone[BIOS_ZONE_MAX];

	zone[0] = '\0';
	LoadBiosVersion(fp, &BiosVersion, BiosDescription, sizeof(BiosDescription),
			&BiosRegion, zone, sizeof(zone), BiosSerial, sizeof(BiosSerial));
	BiosIsArcade = strcmp(zone, "COH-H") == 0;
	if (BiosIsArcade)
		log_cb(RETRO_LOG_INFO, "BIOS is an arcade (COH-H) BIOS\n");

	if (filestream_seek(fp, 0, RETRO_VFS_SEEK_POSITION_START) ||
		filestream_read(fp, eeMem->ROM, static_cast<size_t>(pcsx2_min_s64(Ps2MemSize::Rom, filesize))) != (int64_t)pcsx2_min_s64(Ps2MemSize::Rom, filesize))
	{
		filestream_close(fp);
		return false;
	}

	// If file is less than 2MB it doesn't have an OSD (Devel consoles)
	// So skip HLEing OSDSys Param stuff
	if (filesize < 2465792)
		NoOSD = true;
	else
		NoOSD = false;

	BiosChecksum = 0;
	ChecksumIt(BiosChecksum, eeMem->ROM);
	strlcpy(BiosPath, path, PCSX2_PATH_MAX);

	LoadExtraRom("rom1", eeMem->ROM1);
	LoadExtraRom("rom2", eeMem->ROM2);

	if (strlen(EmuConfig.CurrentIRX) > 3)
		LoadIrx(EmuConfig.CurrentIRX, &eeMem->ROM[0x3C0000], sizeof(eeMem->ROM) - 0x3C0000);

	CurrentBiosInformation.eeThreadListAddr = 0;
	filestream_close(fp);
	return true;
}

bool IsBIOS(const char *filename, u32 *version, char *description, size_t description_size, u32 *region, char *zone, size_t zone_size)
{
	char serial[BIOS_SERIAL_MAX];
	RFILE *fp = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!fp)
		return false;
	// FPS2BIOS is smaller and of variable size
	bool ret = LoadBiosVersion(fp, version, description, description_size, region, zone, zone_size, serial, sizeof(serial));
	filestream_close(fp);
	return ret;
}

bool IsBIOSAvailable(const char* full_path)
{
	// We can't use EmuConfig here since it may not be loaded yet.
	if (full_path && full_path[0] && path_is_valid(full_path))
		return true;

	// No bios configured or the configured name is missing, check for one in the BIOS directory.
	const std::string auto_path(FindBiosImage());
	return !auto_path.empty() && path_is_valid(auto_path.c_str());
}
