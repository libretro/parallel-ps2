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

#include <array>
#include <string/stdstring.h>
#include "common/Pcsx2Defs.h"
#include <cstdlib>
#include <cstring> /* memset */
#include <utility>
#include <vector>

#include <file/file_path.h>
#include <streams/file_stream.h>
#include <compat/strl.h>

#include "HostFS.h"

#include "MemoryCardFile.h"

#include "Config.h"
#include "Host.h"
#include "Memory.h"
#include "memcard_ecc.h"

#define MCD_SIZE 131072 /* Legacy PSX card default size = 1024 * 8 * 16 = 131072 */

#define MC2_MBSIZE 1081344 // Size of a single megabyte of card data = 1024 * 528 * 2 = 1081344

#define MC2_ERASE_SIZE 8448 /* 528 * 16 */

static bool FileMcd_Open = false;

// --------------------------------------------------------------------------------------
//  FileMemoryCard
// --------------------------------------------------------------------------------------
// Provides thread-safe direct file IO mapping.
//
class FileMemoryCard
{
protected:
	RFILE* m_file[8] = {};
	s64 m_fileSize[8] = {};
	std::string m_filenames[8] = {};
	u8* m_cardData[8] = {};   /* full card image held in RAM (new[] at Open), one per slot */
	size_t m_cardSize[8] = {};   /* allocated/used length of m_cardData[slot] */
	u64 m_chksum[8] = {};
	bool m_ispsx[8] = {};
	u32 m_chkaddr = 0;

public:
	FileMemoryCard();
	~FileMemoryCard();

	void Lock();
	void Unlock();

	void Open();
	void Close();

	s32 IsPresent(uint slot);
	void GetSizeInfo(uint slot, McdSizeInfo& outways);
	bool IsPSX(uint slot);
	s32 Read(uint slot, u8* dest, u32 adr, int size);
	s32 Save(uint slot, const u8* src, u32 adr, int size);
	s32 EraseBlock(uint slot, u32 adr);
	u64 GetCRC(uint slot);

protected:
	bool Seek(RFILE* f, u32 adr);
	bool Create(const char* mcdFile, uint sizeInMB);
};

uint FileMcd_GetMtapPort(uint slot)
{
	switch (slot)
	{
		case 1:
		case 5:
		case 6:
		case 7:
			return 1;
		case 0:
		case 2:
		case 3:
		case 4:
		default:
			break;
	}

	return 0;
}

// Returns the multitap slot number, range 1 to 3 (slot 0 refers to the standard
// 1st and 2nd player slots).
uint FileMcd_GetMtapSlot(uint slot)
{
	switch (slot)
	{
		case 2:
		case 3:
		case 4:
			return slot - 1;
		case 5:
		case 6:
		case 7:
			return slot - 4;
		case 0:
		case 1:
		default:
			break;
	}

	return 0; // technically unreachable.
}

bool FileMcd_IsMultitapSlot(uint slot)
{
	return (slot > 1);
}

void FileMcd_GetDefaultName(char* out, size_t out_size, uint slot)
{
	if (FileMcd_IsMultitapSlot(slot))
	{
		snprintf(out, out_size, "Mcd-Multitap%u-Slot%02u.ps2",
				FileMcd_GetMtapPort(slot) + 1, FileMcd_GetMtapSlot(slot) + 1);
		return;
	}
	snprintf(out, out_size, "Mcd%03u.ps2", slot + 1);
}

FileMemoryCard::FileMemoryCard()
{
	for (u8 slot = 0; slot < 8; slot++)
		m_fileSize[slot] = -1;
}

FileMemoryCard::~FileMemoryCard()
{
	/* Free any card images still held in RAM. Close() also frees and NULLs
	 * these; freeing here too (with delete[] nullptr being a no-op) makes a
	 * teardown that bypassed Close, or a never-opened slot, leak-free. */
	for (int slot = 0; slot < 8; ++slot)
	{
		delete[] m_cardData[slot];
		m_cardData[slot] = nullptr;
		m_cardSize[slot] = 0;
	}
}

/* A real card's MagicGate ID (an arcade dongle's), when its 8 bytes sit
 * beside the image as <image>.id: dongle.ps2 and dongle.id. */
static u8 s_mcd_mg_id[8][8];
static bool s_mcd_has_mg_id[8];

static void FileMcd_LoadCardId(int slot, const char* image)
{
	/* Off the stack, like Open's paths. */
	char* path = (char*)malloc(PCSX2_PATH_MAX);
	void* buf = NULL;
	int64_t len = 0;

	if (!path)
		return;
	strlcpy(path, image, PCSX2_PATH_MAX);
	path_remove_extension(path);
	strlcat(path, ".id", PCSX2_PATH_MAX);
	if (path_is_valid(path) && filestream_read_file(path, &buf, &len))
	{
		if (len == 8)
		{
			memcpy(s_mcd_mg_id[slot], buf, 8);
			s_mcd_has_mg_id[slot] = true;
			log_cb(RETRO_LOG_INFO, "Memcard: slot %d answers MagicGate with the ID in %s\n", slot, path);
		}
		else
			log_cb(RETRO_LOG_WARN, "Memcard: %s is %d bytes, not 8; ignored\n", path, (int)len);
		free(buf);
	}
	free(path);
}

void FileMemoryCard::Open()
{
	/* The card path and its staging name, off the stack. */
	char* paths = (char*)malloc(2 * PCSX2_PATH_MAX);
	if (!paths)
		return;
	char* fname   = paths;
	char* newname = paths + PCSX2_PATH_MAX;

	for (int slot = 0; slot < 8; ++slot)
	{
		m_filenames[slot] = {};
		s_mcd_has_mg_id[slot] = false;

		if (FileMcd_IsMultitapSlot(slot))
		{
			if (!EmuConfig.MultitapPort0_Enabled && (FileMcd_GetMtapPort(slot) == 0))
				continue;
			if (!EmuConfig.MultitapPort1_Enabled && (FileMcd_GetMtapPort(slot) == 1))
				continue;
		}

		bool cont = false;

		EmuConfig.FullpathToMcd(fname, PCSX2_PATH_MAX, slot);

		if (fname[0] == '\0')
			cont = true;

		if (!EmuConfig.Mcd[slot].Enabled)
			cont = true;

		if (EmuConfig.Mcd[slot].Type != MemoryCardType::File)
			cont = true;

		if (cont)
			continue;

		if (path_get_size(fname) <= 0)
		{
			// FIXME : Ideally this should prompt the user for the size of the
			// memory card file they would like to create, instead of trying to
			// create one automatically.

			if (!Create(fname, 8))
				log_cb(RETRO_LOG_ERROR, "Could not create a memory card: \n\n%s\n\n\n",
					fname);
		}

		// [TODO] : Add memcard size detection and report it to the console log.
		//   (8MB, 256Mb, formatted, unformatted, etc ...)

		if (string_ends_with(fname, ".bin"))
		{
			if (snprintf(newname, PCSX2_PATH_MAX, "%sx", fname) >= PCSX2_PATH_MAX)
			{
				/* Truncation folds newname back onto fname and the
				 * converter would read and write the same file. */
				log_cb(RETRO_LOG_ERROR, "Memcard: path too long to stage ECC conversion: %s\n", fname);
				continue;
			}
			if (!memcard_noecc_to_raw(fname, newname))
			{
				filestream_delete(newname);
				continue;
			}

			// store the original filename
			m_file[slot] = filestream_open(newname, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		}
		else
			m_file[slot] = filestream_open(fname, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);

		if (m_file[slot]) // Load the whole card into RAM
		{
			FileMcd_LoadCardId(slot, fname);
			m_fileSize[slot]  = filestream_get_size(m_file[slot]);
			m_filenames[slot] = fname;
			m_ispsx[slot]     = m_fileSize[slot] == 0x20000;
			m_chkaddr = 0x210;

			/* Read the entire card image into a RAM buffer once. All
			 * subsequent Read/Save/EraseBlock operate on this buffer
			 * (writes are still persisted through m_file immediately, so
			 * durability is unchanged). This removes the per-operation
			 * seek+read on every memcard access. */
			if (m_fileSize[slot] > 0)
			{
				m_cardSize[slot] = static_cast<size_t>(m_fileSize[slot]);
				m_cardData[slot] = new u8[m_cardSize[slot]];
				filestream_seek(m_file[slot], 0, RETRO_VFS_SEEK_POSITION_START);
				const int64_t got = filestream_read(m_file[slot], m_cardData[slot], m_cardSize[slot]);
				if (got != (int64_t)(m_cardSize[slot]))
				{
					log_cb(RETRO_LOG_ERROR, "Error reading memcard.\n");
					/* What was not read is blank card, not whatever the
					 * allocation held. */
					const size_t have = got > 0 ? static_cast<size_t>(got) : 0;
					memset(m_cardData[slot] + have, 0xff, m_cardSize[slot] - have);
				}
			}

			/* Checksum word lives at m_chkaddr; take it from the buffer
			 * rather than a separate seek+read. */
			if (!m_ispsx[slot] && m_cardData[slot] &&
				(m_chkaddr + sizeof(m_chksum[slot])) <= m_cardSize[slot])
				memcpy(&m_chksum[slot], m_cardData[slot] + m_chkaddr, sizeof(m_chksum[slot]));
		}
	}
	free(paths);
}

void FileMemoryCard::Close()
{
	for (int slot = 0; slot < 8; ++slot)
	{
		if (!m_file[slot])
			continue;

		// Store checksum
		if (!m_ispsx[slot] && filestream_seek(m_file[slot], m_chkaddr, RETRO_VFS_SEEK_POSITION_START) == 0)
			filestream_write(m_file[slot], &m_chksum[slot], sizeof(m_chksum[slot]));

		filestream_close(m_file[slot]);
		m_file[slot] = nullptr;

		if (string_ends_with(m_filenames[slot].c_str(), ".bin"))
		{
			const std::string name_in(m_filenames[slot] + 'x');
			if (memcard_raw_to_noecc(name_in.c_str(), m_filenames[slot].c_str()))
				filestream_delete(name_in.c_str());
		}

		/* Release the RAM card image (also freed in the destructor; NULL
		 * after free so a later dtor pass is a no-op). */
		delete[] m_cardData[slot];
		m_cardData[slot] = nullptr;
		m_cardSize[slot] = 0;

		m_filenames[slot] = {};
		m_fileSize[slot]  = -1;
	}
}

// Returns FALSE if the seek failed (is outside the bounds of the file).
bool FileMemoryCard::Seek(RFILE* f, u32 adr)
{
	return (filestream_seek(f, adr, RETRO_VFS_SEEK_POSITION_START) == 0);
}

// returns FALSE if an error occurred (either permission denied or disk full)
bool FileMemoryCard::Create(const char* mcdFile, uint sizeInMB)
{
	u8* buf = (u8*)malloc(MC2_ERASE_SIZE);
	if (!buf)
		return false;
	/* Written beside mcdFile and renamed into place whole, so a full disk
	 * cannot leave a short card that later opens as a smaller one. */
	RFILE *fp = filestream_open_atomic(mcdFile);
	bool ok = fp != nullptr;

	memset(buf, 0xff, MC2_ERASE_SIZE);

	for (uint i = 0; ok && i < (MC2_MBSIZE * sizeInMB) / MC2_ERASE_SIZE; i++)
		ok = filestream_write(fp, buf, MC2_ERASE_SIZE) == (int64_t)(MC2_ERASE_SIZE);
	free(buf);
	if (!fp)
		return false;
	switch (filestream_commit_atomic(fp, mcdFile, ok))
	{
		case 0:
			return true;
		case 1:
		{
			char tmp[PCSX2_PATH_MAX];
			if (filestream_atomic_temp_name(tmp, sizeof(tmp), mcdFile))
				filestream_delete(tmp);
			return false;
		}
		default:
			return false;
	}
}

s32 FileMemoryCard::IsPresent(uint slot)
{
	return m_file[slot] != nullptr;
}

void FileMemoryCard::GetSizeInfo(uint slot, McdSizeInfo& outways)
{
	outways.SectorSize = 512;             // 0x0200
	outways.EraseBlockSizeInSectors = 16; // 0x0010
	outways.Xor = 18;                     // 0x12, XOR 02 00 00 10

	if (m_file[slot])
		outways.McdSizeInSectors = static_cast<u32>(m_fileSize[slot]) / (outways.SectorSize + outways.EraseBlockSizeInSectors);
	else
		outways.McdSizeInSectors = 0x4000;

	u8* pdata = (u8*)&outways.McdSizeInSectors;
	outways.Xor ^= pdata[0] ^ pdata[1] ^ pdata[2] ^ pdata[3];
}

bool FileMemoryCard::IsPSX(uint slot)
{
	return m_ispsx[slot];
}

s32 FileMemoryCard::Read(uint slot, u8* dest, u32 adr, int size)
{
	if (!m_cardData[slot])
	{
		memset(dest, 0, size);
		return 1;
	}
	/* Bounds check mirrors the old Seek() out-of-range failure. */
	if (static_cast<size_t>(adr) + size > m_cardSize[slot])
		return 0;
	memcpy(dest, m_cardData[slot] + adr, size);
	return 1;
}

s32 FileMemoryCard::Save(uint slot, const u8* src, u32 adr, int size)
{
	if (!m_cardData[slot])
		return 1;

	if (static_cast<size_t>(adr) + size > m_cardSize[slot])
		return 0;

	u8* dst = m_cardData[slot] + adr;

	if (m_ispsx[slot])
	{
		/* PSX: straight copy (no read-modify-write, matches old path). */
		memcpy(dst, src, size);
	}
	else
	{
		/* PS2: AND-merge new data into the existing card bytes (which are
		 * already in the RAM buffer, so no read-back from disk), then fold
		 * the merged region into the running checksum. */
		for (int i = 0; i < size; i++)
			dst[i] &= src[i];

		// Checksumness
		{
			u64* pdata = (u64*)dst;
			u32 loops = size / 8;

			for (u32 i = 0; i < loops; i++)
				m_chksum[slot] ^= pdata[i];
		}
	}

	/* Persist just the changed region immediately (one write, durability
	 * unchanged from the old per-operation fwrite). */
	if (!Seek(m_file[slot], adr))
		return 0;

	if (filestream_write(m_file[slot], dst, size) == (int64_t)(size))
		return 1;

	return 0;
}

s32 FileMemoryCard::EraseBlock(uint slot, u32 adr)
{
	if (!m_cardData[slot])
		return 1;

	if (static_cast<size_t>(adr) + MC2_ERASE_SIZE > m_cardSize[slot])
		return 0;

	/* Update the RAM image AND disk; without the buffer update a later Read
	 * would return stale (pre-erase) bytes. */
	memset(m_cardData[slot] + adr, 0xff, MC2_ERASE_SIZE);

	if (!Seek(m_file[slot], adr))
		return 0;
	return filestream_write(m_file[slot], m_cardData[slot] + adr, MC2_ERASE_SIZE) == (int64_t)(MC2_ERASE_SIZE);
}

u64 FileMemoryCard::GetCRC(uint slot)
{
	if (!m_file[slot])
		return 0;
	if (m_ispsx[slot])
	{
		/* The XOR of the card's 64-bit words, over whole runs of eight
		 * 528-byte sectors, taken from the image held in RAM. */
		const size_t run = 528 * 8 * sizeof(u64);
		const size_t len = m_cardData[slot] ? m_cardSize[slot] / run * run : 0;
		u64 retval = 0;
		for (size_t i = 0; i < len; i += sizeof(u64))
		{
			u64 w;
			memcpy(&w, m_cardData[slot] + i, sizeof(w));
			retval ^= w;
		}
		return retval;
	}
	return m_chksum[slot];
}

// --------------------------------------------------------------------------------------
//  MemoryCard Component API Bindings
// --------------------------------------------------------------------------------------
namespace Mcd
{
	FileMemoryCard impl; // class-based implementations we refer to when API is invoked
}; // namespace Mcd

uint FileMcd_ConvertToSlot(uint port, uint slot)
{
	if (slot == 0)
		return port;
	if (port == 0)
		return slot + 1; // multitap 1
	return slot + 4;     // multitap 2
}

void FileMcd_EmuOpen(void)
{
	if(FileMcd_Open)
		return;
	FileMcd_Open = true;
	// detect inserted memory card types
	for (uint slot = 0; slot < 8; ++slot)
	{
		if (EmuConfig.Mcd[slot].Filename[0] == '\0')
			EmuConfig.Mcd[slot].Type = MemoryCardType::Empty;
		else if (EmuConfig.Mcd[slot].Enabled)
			EmuConfig.Mcd[slot].Type = MemoryCardType::File;
	}

	Mcd::impl.Open();
}

void FileMcd_EmuClose(void)
{
	if(!FileMcd_Open)
		return;
	FileMcd_Open = false;
	Mcd::impl.Close();
}

const u8* FileMcd_GetMagicGateId(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	return (combinedSlot < 8 && s_mcd_has_mg_id[combinedSlot]) ? s_mcd_mg_id[combinedSlot] : NULL;
}

s32 FileMcd_IsPresent(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.IsPresent(combinedSlot);
	return false;
}

void FileMcd_GetSizeInfo(uint port, uint slot, McdSizeInfo* outways)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		Mcd::impl.GetSizeInfo(combinedSlot, *outways);
}

bool FileMcd_IsPSX(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.IsPSX(combinedSlot);
	return false;
}

s32 FileMcd_Read(uint port, uint slot, u8* dest, u32 adr, int size)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.Read(combinedSlot, dest, adr, size);
	return 0;
}

s32 FileMcd_Save(uint port, uint slot, const u8* src, u32 adr, int size)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.Save(combinedSlot, src, adr, size);
	return 0;
}

s32 FileMcd_EraseBlock(uint port, uint slot, u32 adr)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.EraseBlock(combinedSlot, adr);
	return 0;
}

u64 FileMcd_GetCRC(uint port, uint slot)
{
	const uint combinedSlot = FileMcd_ConvertToSlot(port, slot);
	if (EmuConfig.Mcd[combinedSlot].Type == MemoryCardType::File)
		return Mcd::impl.GetCRC(combinedSlot);
	return 0;
}

bool FileMcd_ReIndex(uint port, uint slot, const char* filter) { return false; }
