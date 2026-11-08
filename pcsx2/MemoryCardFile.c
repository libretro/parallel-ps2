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

/* Memory card images. See MemoryCardFile.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <libretro.h>

#include "MemoryCardFile.h"
#include "memcard_ecc.h"

extern retro_log_printf_t log_cb;

#define MCD_SLOTS 8
#define MC2_MBSIZE 1081344  /* a megabyte of card data: 1024 * 528 * 2 */
#define MC2_ERASE_SIZE 8448 /* 528 * 16 */
#define PSX_CARD_SIZE 0x20000

/* A card per combined slot, the whole image in RAM: reads come from it,
 * writes go to it and through to the file at once. */
typedef struct mcd_card
{
	RFILE* file;
	s64 file_size;
	char* filename; /* NULL when closed */
	u8* data;
	size_t size;
	u64 chksum;
	bool ispsx;
	/* Its MagicGate ID, from <image>.id or the slot's named file. */
	bool has_mg_id;
	u8 mg_id[8];
} mcd_card;

static mcd_card s_cards[MCD_SLOTS];
static bool s_open;
/* The checksum word's place in a PS2 card. */
static const u32 s_chkaddr = 0x210;
/* An ID file named for the slot, in place of the one beside its image. */
static char* s_id_path[MCD_SLOTS];

uint FileMcd_GetMtapPort(uint slot)
{
	switch (slot)
	{
		case 1:
		case 5:
		case 6:
		case 7:
			return 1;
		default:
			return 0;
	}
}

/* The multitap slot, 1 to 3; 0 is the standard player slots'. */
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
		default:
			return 0;
	}
}

bool FileMcd_IsMultitapSlot(uint slot)
{
	return slot > 1;
}

void FileMcd_GetDefaultName(char* out, size_t out_size, uint slot)
{
	/* Small numbers, and no snprintf before C99. */
	char name[32];
	if (FileMcd_IsMultitapSlot(slot))
		sprintf(name, "Mcd-Multitap%u-Slot%02u.ps2",
			(FileMcd_GetMtapPort(slot) + 1) % 10, (FileMcd_GetMtapSlot(slot) + 1) % 100);
	else
		sprintf(name, "Mcd%03u.ps2", (slot + 1) % 1000);
	strlcpy(out, name, out_size);
}

uint FileMcd_ConvertToSlot(uint port, uint slot)
{
	if (slot == 0)
		return port;
	if (port == 0)
		return slot + 1; /* multitap 1 */
	return slot + 4;     /* multitap 2 */
}

void FileMcd_SetIdPath(uint slot, const char* path)
{
	if (slot >= MCD_SLOTS)
		return;
	free(s_id_path[slot]);
	s_id_path[slot] = NULL;
	if (path && path[0])
	{
		const size_t n = strlen(path) + 1;
		if ((s_id_path[slot] = (char*)malloc(n)))
			memcpy(s_id_path[slot], path, n);
	}
}

static void load_card_id(uint slot, const char* image)
{
	/* Off the stack, like open's paths. */
	char* path = (char*)malloc(PCSX2_PATH_MAX);
	void* buf = NULL;
	int64_t len = 0;

	if (!path)
		return;
	if (s_id_path[slot])
		strlcpy(path, s_id_path[slot], PCSX2_PATH_MAX);
	else
	{
		strlcpy(path, image, PCSX2_PATH_MAX);
		path_remove_extension(path);
		strlcat(path, ".id", PCSX2_PATH_MAX);
	}
	if (path_is_valid(path) && filestream_read_file(path, &buf, &len))
	{
		if (len == 8)
		{
			memcpy(s_cards[slot].mg_id, buf, 8);
			s_cards[slot].has_mg_id = true;
			log_cb(RETRO_LOG_INFO, "Memcard: slot %d answers MagicGate with the ID in %s\n", (int)slot, path);
		}
		else
			log_cb(RETRO_LOG_WARN, "Memcard: %s is %d bytes, not 8; ignored\n", path, (int)len);
		free(buf);
	}
	free(path);
}

/* A blank card of size_mb, false when it cannot be made. Written beside
 * the path and renamed into place whole, so a full disk cannot leave a
 * short card that later opens as a smaller one. */
static bool create_card(const char* path, uint size_mb)
{
	u8* buf = (u8*)malloc(MC2_ERASE_SIZE);
	RFILE* fp;
	bool ok;
	uint i;

	if (!buf)
		return false;
	fp = filestream_open_atomic(path);
	ok = fp != NULL;
	memset(buf, 0xff, MC2_ERASE_SIZE);
	for (i = 0; ok && i < (MC2_MBSIZE * size_mb) / MC2_ERASE_SIZE; i++)
		ok = filestream_write(fp, buf, MC2_ERASE_SIZE) == (int64_t)MC2_ERASE_SIZE;
	free(buf);
	if (!fp)
		return false;
	switch (filestream_commit_atomic(fp, path, ok))
	{
		case 0:
			return true;
		case 1:
		{
			char* tmp = (char*)malloc(PCSX2_PATH_MAX);
			if (tmp && filestream_atomic_temp_name(tmp, PCSX2_PATH_MAX, path))
				filestream_delete(tmp);
			free(tmp);
			return false;
		}
		default:
			return false;
	}
}

static char* dup_string(const char* s)
{
	const size_t n = strlen(s) + 1;
	char* d = (char*)malloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

static void open_card(uint slot, char* fname, char* staged)
{
	mcd_card* c = &s_cards[slot];

	if (path_get_size(fname) <= 0 && !create_card(fname, 8))
		log_cb(RETRO_LOG_ERROR, "Could not create a memory card: \n\n%s\n\n\n", fname);

	if (string_ends_with(fname, ".bin"))
	{
		/* An ECC-less image works on a raw copy beside it, converted back
		 * at close. */
		if (strlcpy(staged, fname, PCSX2_PATH_MAX) + 1 >= PCSX2_PATH_MAX
			|| strlcat(staged, "x", PCSX2_PATH_MAX) >= PCSX2_PATH_MAX)
		{
			/* Truncation folds the copy's name back onto the card's. */
			log_cb(RETRO_LOG_ERROR, "Memcard: path too long to stage ECC conversion: %s\n", fname);
			return;
		}
		if (!memcard_noecc_to_raw(fname, staged))
		{
			filestream_delete(staged);
			return;
		}
		c->file = filestream_open(staged, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	}
	else
		c->file = filestream_open(fname, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
			RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!c->file)
		return;

	load_card_id(slot, fname);
	c->file_size = filestream_get_size(c->file);
	c->filename = dup_string(fname);
	c->ispsx = c->file_size == PSX_CARD_SIZE;

	/* The whole card into RAM, once. */
	if (c->file_size > 0 && (c->data = (u8*)malloc((size_t)c->file_size)))
	{
		int64_t got;
		c->size = (size_t)c->file_size;
		filestream_seek(c->file, 0, RETRO_VFS_SEEK_POSITION_START);
		got = filestream_read(c->file, c->data, (int64_t)c->size);
		if (got != (int64_t)c->size)
		{
			/* What was not read is blank card, not whatever the
			 * allocation held. */
			const size_t have = got > 0 ? (size_t)got : 0;
			log_cb(RETRO_LOG_ERROR, "Error reading memcard.\n");
			memset(c->data + have, 0xff, c->size - have);
		}
	}

	if (!c->ispsx && c->data && s_chkaddr + sizeof(c->chksum) <= c->size)
		memcpy(&c->chksum, c->data + s_chkaddr, sizeof(c->chksum));
}

static void open_cards(void)
{
	/* The card's path and its staging name, off the stack. */
	char* paths = (char*)malloc(PCSX2_PATH_MAX);
	char* staged = (char*)malloc(PCSX2_PATH_MAX);
	uint slot;

	if (!paths || !staged)
	{
		free(paths);
		free(staged);
		return;
	}
	for (slot = 0; slot < MCD_SLOTS; ++slot)
	{
		mcd_card* c = &s_cards[slot];
		c->has_mg_id = false;
		if (FileMcd_IsMultitapSlot(slot) && !emu_config_multitap(FileMcd_GetMtapPort(slot)))
			continue;
		if (!emu_config_mcd(slot, paths, PCSX2_PATH_MAX) || !paths[0])
			continue;
		open_card(slot, paths, staged);
	}
	free(paths);
	free(staged);
}

static void close_cards(void)
{
	uint slot;
	for (slot = 0; slot < MCD_SLOTS; ++slot)
	{
		mcd_card* c = &s_cards[slot];
		if (!c->file)
			continue;

		/* The checksum word goes back. */
		if (!c->ispsx && filestream_seek(c->file, s_chkaddr, RETRO_VFS_SEEK_POSITION_START) == 0)
			filestream_write(c->file, &c->chksum, sizeof(c->chksum));
		filestream_close(c->file);
		c->file = NULL;

		if (c->filename && string_ends_with(c->filename, ".bin"))
		{
			char* raw = (char*)malloc(strlen(c->filename) + 2);
			if (raw)
			{
				strcpy(raw, c->filename);
				strcat(raw, "x");
				if (memcard_raw_to_noecc(raw, c->filename))
					filestream_delete(raw);
				free(raw);
			}
		}

		free(c->data);
		free(c->filename);
		c->data = NULL;
		c->filename = NULL;
		c->size = 0;
		c->file_size = -1;
	}
}

void FileMcd_EmuOpen(void)
{
	if (s_open)
		return;
	s_open = true;
	open_cards();
}

void FileMcd_EmuClose(void)
{
	if (!s_open)
		return;
	s_open = false;
	close_cards();
}

static mcd_card* card(uint port, uint slot)
{
	const uint combined = FileMcd_ConvertToSlot(port, slot);
	return combined < MCD_SLOTS ? &s_cards[combined] : NULL;
}

const u8* FileMcd_GetMagicGateId(uint port, uint slot)
{
	const mcd_card* c = card(port, slot);
	return c && c->has_mg_id ? c->mg_id : NULL;
}

s32 FileMcd_IsPresent(uint port, uint slot)
{
	const mcd_card* c = card(port, slot);
	return c && c->file != NULL;
}

void FileMcd_GetSizeInfo(uint port, uint slot, McdSizeInfo* out)
{
	const mcd_card* c = card(port, slot);
	u8 b[4];
	out->SectorSize = 512;            /* 0x0200 */
	out->EraseBlockSizeInSectors = 16; /* 0x0010 */
	out->Xor = 18;                     /* 0x12: XOR of 02 00 00 10 */
	if (c && c->file)
		out->McdSizeInSectors = (u32)c->file_size / (out->SectorSize + out->EraseBlockSizeInSectors);
	else
		out->McdSizeInSectors = 0x4000;
	memcpy(b, &out->McdSizeInSectors, 4);
	out->Xor ^= b[0] ^ b[1] ^ b[2] ^ b[3];
}

bool FileMcd_IsPSX(uint port, uint slot)
{
	const mcd_card* c = card(port, slot);
	return c && c->ispsx;
}

s32 FileMcd_Read(uint port, uint slot, u8* dest, u32 adr, int size)
{
	const mcd_card* c = card(port, slot);
	if (!c || !c->data)
	{
		memset(dest, 0, (size_t)size);
		return 1;
	}
	if ((size_t)adr + (size_t)size > c->size)
		return 0;
	memcpy(dest, c->data + adr, (size_t)size);
	return 1;
}

s32 FileMcd_Save(uint port, uint slot, const u8* src, u32 adr, int size)
{
	mcd_card* c = card(port, slot);
	u8* dst;
	int i;

	if (!c || !c->data)
		return 1;
	if ((size_t)adr + (size_t)size > c->size)
		return 0;
	dst = c->data + adr;
	if (c->ispsx)
		memcpy(dst, src, (size_t)size);
	else
	{
		/* A PS2 card's flash only clears bits: new data ANDs into what is
		 * there, and the merged region folds into the running checksum. */
		for (i = 0; i < size; i++)
			dst[i] &= src[i];
		for (i = 0; i + 8 <= size; i += 8)
		{
			u64 w;
			memcpy(&w, dst + i, 8);
			c->chksum ^= w;
		}
	}
	/* Just the changed region goes to the file, at once. */
	if (filestream_seek(c->file, adr, RETRO_VFS_SEEK_POSITION_START) != 0)
		return 0;
	return filestream_write(c->file, dst, size) == (int64_t)size;
}

s32 FileMcd_EraseBlock(uint port, uint slot, u32 adr)
{
	mcd_card* c = card(port, slot);
	if (!c || !c->data)
		return 1;
	if ((size_t)adr + MC2_ERASE_SIZE > c->size)
		return 0;
	/* The RAM image and the file both: a later read must not see what was
	 * there before. */
	memset(c->data + adr, 0xff, MC2_ERASE_SIZE);
	if (filestream_seek(c->file, adr, RETRO_VFS_SEEK_POSITION_START) != 0)
		return 0;
	return filestream_write(c->file, c->data + adr, MC2_ERASE_SIZE) == (int64_t)MC2_ERASE_SIZE;
}

u64 FileMcd_GetCRC(uint port, uint slot)
{
	const mcd_card* c = card(port, slot);
	if (!c || !c->file)
		return 0;
	if (c->ispsx)
	{
		/* The XOR of the card's 64-bit words, over whole runs of eight
		 * 528-byte sectors. */
		const size_t run = 528 * 8 * sizeof(u64);
		const size_t len = c->data ? c->size / run * run : 0;
		u64 crc = 0;
		size_t i;
		for (i = 0; i < len; i += sizeof(u64))
		{
			u64 w;
			memcpy(&w, c->data + i, sizeof(w));
			crc ^= w;
		}
		return crc;
	}
	return c->chksum;
}

bool FileMcd_ReIndex(uint port, uint slot, const char* filter)
{
	(void)port;
	(void)slot;
	(void)filter;
	return false;
}
