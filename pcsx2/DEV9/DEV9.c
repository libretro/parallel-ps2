/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2020  PCSX2 Dev Team
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

#include <file/file_path.h>
#include "common/Pcsx2Defs.h"
#include <retro_miscellaneous.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <compat/strl.h>
#include <errno.h>
#include <stdarg.h>
#include "DEV9.h"
#include "Config.h"
#include "smap.h"

#ifdef _MSC_VER
#pragma warning(disable : 4244)
#endif

dev9Struct dev9;

/* #define HDD_48BIT */

/* clang-format off */
u8 eeprom[] = {
	/* 0x6D, 0x76, 0x63, 0x61, 0x31, 0x30, 0x08, 0x01, */
	0x76, 0x6D, 0x61, 0x63, 0x30, 0x31, 0x07, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* clang-format on */

/* eeprom.dat, in the working directory, mapped over the built-in EEPROM
 * when it is there and holds all 64 bytes. */
#define EEPROM_BYTES 64
#ifdef _WIN32
static HANDLE hEeprom = INVALID_HANDLE_VALUE;
static HANDLE mapping;
#else
static int hEeprom = -1;
#endif

static bool isRunning = false;

/* What a game reads of the EEPROM: the file's when it can be mapped
 * whole, else the built-in one. A map that failed, or of a file shorter
 * than the EEPROM, was taken as the EEPROM and faulted at the first read
 * (mmap's failure is MAP_FAILED, not NULL; past a file's end is SIGBUS). */
static void eeprom_map(void)
{
#ifndef _WIN32
	struct stat st;
#endif
	dev9.eeprom = (u16*)eeprom;
#ifdef _WIN32
	hEeprom = CreateFileW(L"eeprom.dat", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, NULL);
	if (hEeprom == INVALID_HANDLE_VALUE)
		return;
	if (GetFileSize(hEeprom, NULL) >= EEPROM_BYTES)
	{
		mapping = CreateFileMappingW(hEeprom, NULL, PAGE_READWRITE, 0, 0, NULL);
		if (mapping)
		{
			void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, EEPROM_BYTES);
			if (view)
			{
				dev9.eeprom = (u16*)view;
				return;
			}
			CloseHandle(mapping);
			mapping = NULL;
		}
	}
	CloseHandle(hEeprom);
	hEeprom = INVALID_HANDLE_VALUE;
#else
	hEeprom = open("eeprom.dat", O_RDWR, 0);
	if (hEeprom < 0)
		return;
	if (fstat(hEeprom, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= EEPROM_BYTES)
	{
		void* view = mmap(NULL, EEPROM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, hEeprom, 0);
		if (view != MAP_FAILED)
		{
			dev9.eeprom = (u16*)view;
			return;
		}
	}
	close(hEeprom);
	hEeprom = -1;
#endif
}

static void eeprom_unmap(void)
{
	if (dev9.eeprom && dev9.eeprom != (u16*)eeprom)
	{
#ifdef _WIN32
		UnmapViewOfFile(dev9.eeprom);
#else
		munmap(dev9.eeprom, EEPROM_BYTES);
#endif
	}
	dev9.eeprom = (u16*)eeprom;
#ifdef _WIN32
	if (mapping)
		CloseHandle(mapping);
	mapping = NULL;
	if (hEeprom != INVALID_HANDLE_VALUE)
		CloseHandle(hEeprom);
	hEeprom = INVALID_HANDLE_VALUE;
#else
	if (hEeprom >= 0)
		close(hEeprom);
	hEeprom = -1;
#endif
}

/* The HDD image's path, UTF-8, under the settings folder unless given
 * absolute; static, as a path is too large for a frame. */
static char s_hdd_path[PATH_MAX_LENGTH];

static const char* GetHDDPath(void)
{
	const char* file = EmuConfig.DEV9.HddFile;
	if (!file[0])
		EmuConfig.DEV9.HddEnable = false;
	if (path_is_absolute(file))
		strlcpy(s_hdd_path, file, sizeof(s_hdd_path));
	else
		fill_pathname_join(s_hdd_path, EmuFolders.Settings, file, sizeof(s_hdd_path));
	return s_hdd_path;
}

s32 DEV9init(void)
{
	int rxbi;
	log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9init\n");

	eeprom_unmap();
	memset(&dev9, 0, sizeof(dev9));
	dev9.ata = ata_new();
	if (!dev9.ata)
		return -1;
	FLASHinit();
	eeprom_map();

	for (rxbi = 0; rxbi < (SMAP_BD_SIZE / 8); rxbi++)
	{
		smap_bd_t* pbd = (smap_bd_t*)&dev9.dev9R[SMAP_BD_RX_BASE & 0xffff];
		pbd = &pbd[rxbi];

		pbd->ctrl_stat = SMAP_BD_RX_EMPTY;
		pbd->length = 0;
	}

	log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9init ok\n");

	return 0;
}

void DEV9shutdown(void)
{
	log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9shutdown\n");
	FLASHsave();
	ata_free(dev9.ata);
	dev9.ata = NULL;
	eeprom_unmap();
}

/* ata_open, then the drive's identity: the configured file, or one beside
 * the image with the extension .hddid. */
static int dev9_ata_open(const char* hddPath)
{
	if (ata_open(dev9.ata, hddPath, EmuConfig.DEV9.HddSizeSectors) != 0)
		return -1;
	if (EmuConfig.DEV9.HddIdFile[0])
		ata_load_hdd_id(dev9.ata, EmuConfig.DEV9.HddIdFile);
	else
	{
		char* id = (char*)malloc(strlen(hddPath) + 8);
		if (id)
		{
			strcpy(id, hddPath);
			path_remove_extension(id);
			strcat(id, ".hddid");
		}
		ata_load_hdd_id(dev9.ata, id);
		free(id);
	}
	return 0;
}

s32 DEV9open(void)
{
	const char* hddPath;
	log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9open\n");
	log_cb(RETRO_LOG_DEBUG, "DEV9: open r+: %s\n", EmuConfig.DEV9.HddFile);

	hddPath = GetHDDPath();

	if (EmuConfig.DEV9.HddEnable)
	{
		if (dev9_ata_open(hddPath) != 0)
			EmuConfig.DEV9.HddEnable = false;
	}

	if (EmuConfig.DEV9.EthEnable)
		InitNet();

	isRunning = true;
	return 0;
}

void DEV9close(void)
{
	log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9close\n");

	ata_close(dev9.ata);
	TermNet();
	isRunning = false;
}

int DEV9irqHandler(void)
{
	/* dev9Ru16(SPD_R_INTR_STAT)|= dev9.irqcause; */
	/* log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9irqHandler %x, %x\n", dev9.irqcause, dev9.irqmask); */
	if (dev9.irqcause & dev9.irqmask)
		return 1;
	return 0;
}

void _DEV9irq(int cause, int cycles)
{
	/* log_cb(RETRO_LOG_DEBUG, "DEV9: _DEV9irq %x, %x\n", cause, dev9.irqmask); */

	dev9.irqcause |= cause;

	if (cycles < 1)
		dev9Irq(1);
	else
		dev9Irq(cycles);
}

/* What the ATA device needs of dev9, through ATA.h. */
void dev9_irq_cause_clear(int bits)
{
	dev9.irqcause &= (u16)~bits;
}

int dev9_ata_dma_enabled(void)
{
	return (dev9.if_ctrl & SPD_IF_ATA_DMAEN) != 0;
}

/* Fakes SPEED FIFO */
static void HDDWriteFIFO(void)
{
	if (dev9.ata->dmaReady && (dev9.if_ctrl & SPD_IF_ATA_DMAEN))
	{
		const int unread = (dev9.fifo_bytes_write - dev9.fifo_bytes_read);
		const int spaceSectors = (SPD_DBUF_AVAIL_MAX * 512 - unread) / 512;
		int readSectors;
		if (spaceSectors < 0)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: No Space on SPEED FIFO\n");
			abort();
		}

		readSectors = dev9.ata->nsectorLeft < spaceSectors ? dev9.ata->nsectorLeft : spaceSectors;
		dev9.fifo_bytes_write += readSectors * 512;
		dev9.ata->nsectorLeft -= readSectors;
	}
	/* FIFOIntr(); */
}
static void HDDReadFIFO(void)
{
	if (dev9.ata->dmaReady && (dev9.if_ctrl & SPD_IF_ATA_DMAEN))
	{
		const int writeSectors = (dev9.fifo_bytes_write - dev9.fifo_bytes_read) / 512;
		dev9.fifo_bytes_read += writeSectors * 512;
		dev9.ata->nsectorLeft -= writeSectors;
	}
	/* FIFOIntr(); */
}
void IOPReadFIFO(int bytes);
void IOPReadFIFO(int bytes)
{
	dev9.fifo_bytes_read += bytes;
	if (dev9.fifo_bytes_read > dev9.fifo_bytes_write)
		log_cb(RETRO_LOG_ERROR, "DEV9: UNDERFLOW BY IOP\n");
	/* FIFOIntr(); */
}
void IOPWriteFIFO(int bytes);
void IOPWriteFIFO(int bytes)
{
	dev9.fifo_bytes_write += bytes;
	if (dev9.fifo_bytes_write - SPD_DBUF_AVAIL_MAX * 512 > dev9.fifo_bytes_read)
		log_cb(RETRO_LOG_ERROR, "DEV9: OVERFLOW BY IOP\n");
	/* FIFOIntr(); */
}
static void FIFOIntr(void)
{
	/* FIFO Buffer Full/Empty */
	const int unread = (dev9.fifo_bytes_write - dev9.fifo_bytes_read);

	if (unread == 0)
	{
		if ((dev9.irqcause & SPD_INTR_ATA_FIFO_EMPTY) == 0)
			_DEV9irq(SPD_INTR_ATA_FIFO_EMPTY, 1);
	}
	if (unread == SPD_DBUF_AVAIL_MAX * 512)
	{
		/* Log_Error("FIFO Full"); */
		/* INTR Full? */
	}
}

u8 DEV9read8(u32 addr)
{
	u8 hard;
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return 0;

	if (addr >= ATA_DEV9_HDD_BASE && addr < ATA_DEV9_HDD_END)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ATA does not support 8bit reads %x\n", addr);
		return 0;
	}
	if (addr >= SMAP_REGBASE && addr < FLASH_REGBASE)
	{
		/* smap */
		return smap_read8(addr);
	}
	if ((addr >= FLASH_REGBASE) && (addr < (FLASH_REGBASE + FLASH_REGSIZE)))
	{
		return (u8)FLASHread32(addr, 1);
	}

	switch (addr)
	{
		case SPD_R_PIO_DATA:

			/*if(dev9.eeprom_dir!=1)
			{
				hard=0;
				break;
			}*/

			if (dev9.eeprom_state == EEPROM_TDATA)
			{
				if (dev9.eeprom_command == 2) /* read */
				{
					if (dev9.eeprom_bit == 0xFF)
						hard = 0;
					else
						hard = ((dev9.eeprom[dev9.eeprom_address] << dev9.eeprom_bit) & 0x8000) >> 11;
					dev9.eeprom_bit++;
					if (dev9.eeprom_bit == 16)
					{
						dev9.eeprom_address++;
						dev9.eeprom_bit = 0;
					}
				}
				else
					hard = 0;
			}
			else
				hard = 0;
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_DATA 8bit read %x\n", hard); */
			return hard;

		case DEV9_R_REV:
			hard = 0x32; /* expansion bay */
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9_R_REV 8bit read %x\n", hard); */
			return hard;

		default:
			hard = dev9Ru8(addr);
			log_cb(RETRO_LOG_ERROR, "DEV9: Unknown 8bit read at address %x value %x\n", addr, hard);
			return hard;
	}
}

u16 DEV9read16(u32 addr)
{
	u16 hard;
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return 0;

	if (addr >= ATA_DEV9_HDD_BASE && addr < ATA_DEV9_HDD_END)
	{
		return ata_read16(dev9.ata, addr - ATA_DEV9_HDD_BASE);
	}
	if (addr >= SMAP_REGBASE && addr < FLASH_REGBASE)
	{
		/* smap */
		return smap_read16(addr);
	}
	if ((addr >= FLASH_REGBASE) && (addr < (FLASH_REGBASE + FLASH_REGSIZE)))
	{
		return (u16)FLASHread32(addr, 2);
	}

	switch (addr)
	{
		case SPD_R_INTR_STAT:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_INTR_STAT 16bit read %x\n", dev9.irqcause); */
			return dev9.irqcause;

		case SPD_R_INTR_MASK:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_INTR_MASK 16bit read %x\n", dev9.irqmask); */
			return dev9.irqmask;

		case SPD_R_PIO_DATA:

			/*if(dev9.eeprom_dir!=1)
			{
				hard=0;
				break;
			}*/

			if (dev9.eeprom_state == EEPROM_TDATA)
			{
				if (dev9.eeprom_command == 2) /* read */
				{
					if (dev9.eeprom_bit == 0xFF)
						hard = 0;
					else
						hard = ((dev9.eeprom[dev9.eeprom_address] << dev9.eeprom_bit) & 0x8000) >> 11;
					dev9.eeprom_bit++;
					if (dev9.eeprom_bit == 16)
					{
						dev9.eeprom_address++;
						dev9.eeprom_bit = 0;
					}
				}
				else
					hard = 0;
			}
			else
				hard = 0;
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_DATA 16bit read %x\n", hard); */
			return hard;

		case DEV9_R_REV:
			/* hard = 0x0030; // expansion bay */
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: DEV9_R_REV 16bit read %x\n", dev9.irqmask); */
			hard = 0x0032;
			return hard;

		case SPD_R_REV_1:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_REV_1 16bit read %x\n", 0); */
			return 0;

		case SPD_R_REV_2:
			hard = 0x0011;
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: STD_R_REV_2 16bit read %x\n", hard); */
			return hard;

		case SPD_R_REV_3:
			hard = 0;
			if (EmuConfig.DEV9.HddEnable)
				hard |= SPD_CAPS_ATA;
			if (EmuConfig.DEV9.EthEnable)
				hard |= SPD_CAPS_SMAP;
			hard |= SPD_CAPS_FLASH;
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_REV_3 16bit read %x\n", hard); */
			return hard;

		case SPD_R_0e:
			hard = 0x0002; /* Have HDD inserted */
			log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_0e 16bit read %x\n", hard);
			return hard;
		case SPD_R_XFR_CTRL:
			log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL 16bit read %x\n", dev9.xfr_ctrl);
			return dev9.xfr_ctrl;
		case SPD_R_DBUF_STAT:
		{
			u8 count;
			hard = 0;
			if (dev9.if_ctrl & SPD_IF_READ) /* Semi async */
			{
				HDDWriteFIFO(); /* Yes this is not a typo */
			}
			else
			{
				HDDReadFIFO();
			}
			FIFOIntr();

			count = (u8)((dev9.fifo_bytes_write - dev9.fifo_bytes_read) / 512);
			if (dev9.xfr_ctrl & SPD_XFR_WRITE) /* or ifRead? */
			{
				hard = (u8)(SPD_DBUF_AVAIL_MAX - count);
				hard |= (count == 0) ? SPD_DBUF_STAT_1 : (u16)0;
				hard |= (count > 0) ? SPD_DBUF_STAT_2 : (u16)0;
			}
			else
			{
				hard = count;
				hard |= (count < SPD_DBUF_AVAIL_MAX) ? SPD_DBUF_STAT_1 : (u16)0;
				hard |= (count == 0) ? SPD_DBUF_STAT_2 : (u16)0;
				/* If overflow (HDD->SPEED), set both SPD_DBUF_STAT_2 & SPD_DBUF_STAT_FULL */
				/* and overflow INTR set */
			}

			if (count == SPD_DBUF_AVAIL_MAX)
			{
				hard |= SPD_DBUF_STAT_FULL;
			}

			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DBUF_STAT 16bit read %x\n", hard); */
			return hard;
		}
		case SPD_R_IF_CTRL:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_IF_CTRL 16bit read %x\n", dev9.if_ctrl); */
			return dev9.if_ctrl;
		default:
			hard = dev9Ru16(addr);
			log_cb(RETRO_LOG_ERROR, "DEV9: Unknown 16bit read at address %x value %x\n", addr, hard);
			return hard;
	}
}

u32 DEV9read32(u32 addr)
{
	u32 hard;
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return 0;

	if (addr >= ATA_DEV9_HDD_BASE && addr < ATA_DEV9_HDD_END)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ATA does not support 32bit reads %x\n", addr);
		return 0;
	}
	if (addr >= SMAP_REGBASE && addr < FLASH_REGBASE)
	{
		/* smap */
		return smap_read32(addr);
	}
	if ((addr >= FLASH_REGBASE) && (addr < (FLASH_REGBASE + FLASH_REGSIZE)))
	{
		return (u32)FLASHread32(addr, 4);
	}

	hard = dev9Ru32(addr);
	log_cb(RETRO_LOG_ERROR, "DEV9: Unknown 32bit read at address %x value %x\n", addr, hard);
	return hard;
}

void DEV9write8(u32 addr, u8 value)
{
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return;

	if (addr >= ATA_DEV9_HDD_BASE && addr < ATA_DEV9_HDD_END)
	{
#ifdef ENABLE_ATA
		ata_write<1>(addr, value);
#endif
		return;
	}
	if (addr >= SMAP_REGBASE && addr < FLASH_REGBASE)
	{
		/* smap */
		smap_write8(addr, value);
		return;
	}
	if ((addr >= FLASH_REGBASE) && (addr < (FLASH_REGBASE + FLASH_REGSIZE)))
	{
		FLASHwrite32(addr, (u32)value, 1);
		return;
	}

	switch (addr)
	{
		case 0x10000020:
			log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_INTR_CAUSE, WTFH ?\n");
			dev9.irqcause = 0xff;
			break;
		case SPD_R_INTR_STAT:
			log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_INTR_STAT,  WTFH ?\n");
			dev9.irqcause = value;
			return;
		case SPD_R_INTR_MASK:
			log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_INTR_MASK8, WTFH ?\n");
			break;

		case SPD_R_PIO_DIR:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_DIR 8bit write %x\n", value); */

			if ((value & 0xc0) != 0xc0)
				return;

			if ((value & 0x30) == 0x20)
			{
				dev9.eeprom_state = 0;
			}
			dev9.eeprom_dir = (value >> 4) & 3;

			return;

		case SPD_R_PIO_DATA:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_DATA 8bit write %x\n", value); */

			if ((value & 0xc0) != 0xc0)
				return;

			switch (dev9.eeprom_state)
			{
				case EEPROM_READY:
					dev9.eeprom_command = 0;
					dev9.eeprom_state++;
					break;
				case EEPROM_OPCD0:
					dev9.eeprom_command = (value >> 4) & 2;
					dev9.eeprom_state++;
					dev9.eeprom_bit = 0xFF;
					break;
				case EEPROM_OPCD1:
					dev9.eeprom_command |= (value >> 5) & 1;
					dev9.eeprom_state++;
					break;
				case EEPROM_ADDR0:
				case EEPROM_ADDR1:
				case EEPROM_ADDR2:
				case EEPROM_ADDR3:
				case EEPROM_ADDR4:
				case EEPROM_ADDR5:
					dev9.eeprom_address =
						(dev9.eeprom_address & (63 ^ (1 << (dev9.eeprom_state - EEPROM_ADDR0)))) |
						((value >> (dev9.eeprom_state - EEPROM_ADDR0)) & (0x20 >> (dev9.eeprom_state - EEPROM_ADDR0)));
					dev9.eeprom_state++;
					break;
				case EEPROM_TDATA:
				{
					if (dev9.eeprom_command == 1) /* write */
					{
						dev9.eeprom[dev9.eeprom_address] =
							(dev9.eeprom[dev9.eeprom_address] & (63 ^ (1 << dev9.eeprom_bit))) |
							((value >> dev9.eeprom_bit) & (0x8000 >> dev9.eeprom_bit));
						dev9.eeprom_bit++;
						if (dev9.eeprom_bit == 16)
						{
							dev9.eeprom_address++;
							dev9.eeprom_bit = 0;
						}
					}
				}
				break;
				default:
					log_cb(RETRO_LOG_ERROR, "DEV9: Unknown EEPROM COMMAND\n");
					break;
			}
			return;
		default:
			dev9Ru8(addr) = value;
			log_cb(RETRO_LOG_ERROR, "DEV9: Unknown 8bit write at address %x value %x\n", addr, value);
			return;
	}
}

void DEV9write16(u32 addr, u16 value)
{
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return;

	if (addr >= ATA_DEV9_HDD_BASE && addr < ATA_DEV9_HDD_END)
	{
		ata_write16(dev9.ata, addr - ATA_DEV9_HDD_BASE, value);
		return;
	}
	if (addr >= SMAP_REGBASE && addr < FLASH_REGBASE)
	{
		/* smap */
		smap_write16(addr, value);
		return;
	}
	if ((addr >= FLASH_REGBASE) && (addr < (FLASH_REGBASE + FLASH_REGSIZE)))
	{
		FLASHwrite32(addr, (u32)value, 2);
		return;
	}

	switch (addr)
	{
		case SPD_R_INTR_MASK:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_INTR_MASK 16bit write %x	, checking for masked/unmasked interrupts\n", value); */
			if ((dev9.irqmask != value) && ((dev9.irqmask | value) & dev9.irqcause))
			{
				/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_INTR_MASK16 firing unmasked interrupts\n"); */
				dev9Irq(1);
			}
			dev9.irqmask = value;
			break;

		case SPD_R_PIO_DIR:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_DIR 16bit write %x\n", value); */

			if ((value & 0xc0) != 0xc0)
				return;

			if ((value & 0x30) == 0x20)
			{
				dev9.eeprom_state = 0;
			}
			dev9.eeprom_dir = (value >> 4) & 3;

			return;

		case SPD_R_PIO_DATA:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_DATA 16bit write %x\n", value); */

			if ((value & 0xc0) != 0xc0)
				return;

			switch (dev9.eeprom_state)
			{
				case EEPROM_READY:
					dev9.eeprom_command = 0;
					dev9.eeprom_state++;
					break;
				case EEPROM_OPCD0:
					dev9.eeprom_command = (value >> 4) & 2;
					dev9.eeprom_state++;
					dev9.eeprom_bit = 0xFF;
					break;
				case EEPROM_OPCD1:
					dev9.eeprom_command |= (value >> 5) & 1;
					dev9.eeprom_state++;
					break;
				case EEPROM_ADDR0:
				case EEPROM_ADDR1:
				case EEPROM_ADDR2:
				case EEPROM_ADDR3:
				case EEPROM_ADDR4:
				case EEPROM_ADDR5:
					dev9.eeprom_address =
						(dev9.eeprom_address & (63 ^ (1 << (dev9.eeprom_state - EEPROM_ADDR0)))) |
						((value >> (dev9.eeprom_state - EEPROM_ADDR0)) & (0x20 >> (dev9.eeprom_state - EEPROM_ADDR0)));
					dev9.eeprom_state++;
					break;
				case EEPROM_TDATA:
				{
					if (dev9.eeprom_command == 1) /* write */
					{
						dev9.eeprom[dev9.eeprom_address] =
							(dev9.eeprom[dev9.eeprom_address] & (63 ^ (1 << dev9.eeprom_bit))) |
							((value >> dev9.eeprom_bit) & (0x8000 >> dev9.eeprom_bit));
						dev9.eeprom_bit++;
						if (dev9.eeprom_bit == 16)
						{
							dev9.eeprom_address++;
							dev9.eeprom_bit = 0;
						}
					}
				}
				break;
				default:
					log_cb(RETRO_LOG_ERROR, "DEV9: Unknown EEPROM COMMAND\n");
					break;
			}
			return;

		case SPD_R_DMA_CTRL:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_IF_CTRL 16bit write %x\n", value); */
			dev9.dma_ctrl = value;

			/* if (value & SPD_DMA_TO_SMAP) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DMA_CTRL DMA For SMAP\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DMA_CTRL DMA For ATA\n"); */

			/* if ((value & SPD_DMA_FASTEST) != 0) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DMA_CTRL Fastest DMA Mode\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DMA_CTRL Slower DMA Mode\n"); */

			/* if ((value & SPD_DMA_WIDE) != 0) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DMA_CTRL Wide(32bit) DMA Mode Set\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DMA_CTRL 16bit DMA Mode\n"); */

			if ((value & SPD_DMA_PAUSE) != 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_DMA_CTRL Pause DMA Not Implemented\n");

			if ((value & 0xFFE8) != 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_DMA_CTRL Unknown value written %x\n", value);

			break;
		case SPD_R_XFR_CTRL:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL 16bit write %x\n", value); */
			dev9.xfr_ctrl = value;

			/* if (value & SPD_XFR_WRITE) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL Set Write\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL Set Read\n"); */

			/* if ((value & (1 << 1)) != 0) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL Unknown Bit 1\n"); */

			/* if ((value & (1 << 2)) != 0) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL Unknown Bit 2\n"); */

			/* if (value & SPD_XFR_DMAEN) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL For DMA Enabled\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_XFR_CTRL For DMA Disabled\n"); */

			if ((value & 0xFF78) != 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_XFR_CTRL Unknown value written %x\n", value);

			break;
		case SPD_R_DBUF_STAT:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DBUF_STAT 16bit write %x\n", value); */

			if ((value & SPD_DBUF_RESET_FIFO) != 0)
			{
				/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_DBUF_STAT Reset FIFO\n"); */
				dev9.fifo_bytes_write = 0;
				dev9.fifo_bytes_read = 0;
				dev9.xfr_ctrl &= ~SPD_XFR_WRITE; /* ? */
				dev9.if_ctrl |= SPD_IF_READ; /* ? */

				FIFOIntr();
			}

			if (value != 3)
				log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_DBUF_STAT 16bit write %x Which != 3!!!\n", value);
			break;

		case SPD_R_IF_CTRL:
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_IF_CTRL 16bit write %x\n", value); */
			dev9.if_ctrl = value;

			/* if (value & SPD_IF_UDMA) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL UDMA Enabled\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL UDMA Disabled\n"); */
			/* if (value & SPD_IF_READ) */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL DMA Is ATA Read\n"); */
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL DMA Is ATA Write\n"); */

			if (value & SPD_IF_ATA_DMAEN)
			{
				/* log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL ATA DMA Enabled\n"); */
				if (value & SPD_IF_READ) /* Semi async */
				{
					HDDWriteFIFO(); /* Yes this is not a typo */
				}
				else
				{
					HDDReadFIFO();
				}
				FIFOIntr();
			}
			/* else */
			/* 	log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL ATA DMA Disabled\n"); */

			if (value & (1 << 3))
				log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL Unknown Bit 3 Set\n");

			if (value & (1 << 4))
				log_cb(RETRO_LOG_ERROR, "DEV9: IF_CTRL Unknown Bit 4 Set\n");
			if (value & (1 << 5))
				log_cb(RETRO_LOG_ERROR, "DEV9: IF_CTRL Unknown Bit 5 Set\n");

			if ((value & SPD_IF_HDD_RESET) == 0) /* Maybe? */
			{
				/* log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL HDD Hard Reset\n"); */
				ata_hard_reset(dev9.ata);
			}
			if ((value & SPD_IF_ATA_RESET) != 0)
			{
				log_cb(RETRO_LOG_DEBUG, "DEV9: IF_CTRL ATA Reset\n");
				/* 0x62        0x0020 */
				dev9.if_ctrl = 0x001A;
				/* 0x66        0x0001 */
				dev9.pio_mode = 0x24;
				dev9.mdma_mode = 0x45;
				dev9.udma_mode = 0x83;
				/* 0x76    0x4ABA (And consequently 0x78 = 0x4ABA.) */
			}

			if ((value & 0xFF00) > 0)
				log_cb(RETRO_LOG_ERROR, "DEV9: IF_CTRL Unknown Bit(s) %x\n", (value & 0xFF00));

			break;
		case SPD_R_PIO_MODE: /* ATA only? or includes EEPROM? */
			/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_MODE 16bit write %x\n", value); */
			dev9.pio_mode = value;

			switch (value)
			{
				case 0x92:
					/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_MODE 0\n"); */
					break;
				case 0x72:
					/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_MODE 1\n"); */
					break;
				case 0x32:
					/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_MODE 2\n"); */
					break;
				case 0x24:
					/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_MODE 3\n"); */
					break;
				case 0x23:
					/* log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_PIO_MODE 4\n"); */
					break;

				default:
					log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_PIO_MODE UNKNOWN MODE %x\n", value);
					break;
			}
			break;
		case SPD_R_MDMA_MODE: /* ATA only? or includes EEPROM? */
			log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_MDMA_MODE 16bit write %x\n", value);
			dev9.mdma_mode = value;

			switch (value)
			{
				case 0xFF:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_MDMA_MODE 0\n");
					break;
				case 0x45:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_MDMA_MODE 1\n");
					break;
				case 0x24:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_MDMA_MODE 2\n");
					break;
				default:
					log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_MDMA_MODE UNKNOWN MODE %x\n", value);
					break;
			}

			break;
		case SPD_R_UDMA_MODE: /* ATA only? */
			log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_UDMA_MODE 16bit write %x\n", value);
			dev9.udma_mode = value;

			switch (value)
			{
				case 0xa7:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_UDMA_MODE 0\n");
					break;
				case 0x85:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_UDMA_MODE 1\n");
					break;
				case 0x63:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_UDMA_MODE 2\n");
					break;
				case 0x62:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_UDMA_MODE 3\n");
					break;
				case 0x61:
					log_cb(RETRO_LOG_DEBUG, "DEV9: SPD_R_UDMA_MODE 4\n");
					break;
				default:
					log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_UDMA_MODE UNKNOWN MODE %x\n", value);
					break;
			}
			break;

		default:
			dev9Ru16(addr) = value;
			log_cb(RETRO_LOG_ERROR, "DEV9: *Unknown 16bit write at address %x value %x\n", addr, value);
			return;
	}
}

void DEV9write32(u32 addr, u32 value)
{
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return;

	if (addr >= ATA_DEV9_HDD_BASE && addr < ATA_DEV9_HDD_END)
	{
#ifdef ENABLE_ATA
		ata_write<4>(addr, value);
#endif
		return;
	}
	if (addr >= SMAP_REGBASE && addr < FLASH_REGBASE)
	{
		/* smap */
		smap_write32(addr, value);
		return;
	}
	if ((addr >= FLASH_REGBASE) && (addr < (FLASH_REGBASE + FLASH_REGSIZE)))
	{
		FLASHwrite32(addr, (u32)value, 4);
		return;
	}

	switch (addr)
	{
		case SPD_R_INTR_MASK:
			log_cb(RETRO_LOG_ERROR, "DEV9: SPD_R_INTR_MASK, WTFH ?\n");
			break;
		default:
			dev9Ru32(addr) = value;
			log_cb(RETRO_LOG_ERROR, "DEV9: Unknown 32bit write at address %x write %x\n", addr, value);
			return;
	}
}

void DEV9readDMA8Mem(u32* pMem, int size)
{
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return;

	size >>= 1;

	log_cb(RETRO_LOG_DEBUG, "DEV9: *DEV9readDMA8Mem: size %x\n", size);

	if (dev9.dma_ctrl & SPD_DMA_TO_SMAP)
		smap_readDMA8Mem(pMem, size);
	else
	{
		if (dev9.xfr_ctrl & SPD_XFR_DMAEN &&
			!(dev9.xfr_ctrl & SPD_XFR_WRITE))
		{
			HDDWriteFIFO();
			IOPReadFIFO(size);
			ata_read_dma8_mem(dev9.ata, (u8*)pMem, size);
			FIFOIntr();
		}
	}

	/* TODO, track if read was successful */
}

void DEV9writeDMA8Mem(u32* pMem, int size)
{
	if (!EmuConfig.DEV9.EthEnable && !EmuConfig.DEV9.HddEnable)
		return;

	size >>= 1;

	log_cb(RETRO_LOG_DEBUG, "DEV9: *DEV9writeDMA8Mem: size %x\n", size);

	if (dev9.dma_ctrl & SPD_DMA_TO_SMAP)
		smap_writeDMA8Mem(pMem, size);
	else
	{
		if (dev9.xfr_ctrl & SPD_XFR_DMAEN &&
			dev9.xfr_ctrl & SPD_XFR_WRITE)
		{
			IOPWriteFIFO(size);
			HDDReadFIFO();
			ata_write_dma8_mem(dev9.ata, (u8*)pMem, size);
			FIFOIntr();
		}
	}

	/* TODO, track if write was successful */
}

void DEV9async(u32 cycles)
{
	smap_async(cycles);
	ata_async(dev9.ata, cycles);
}

void DEV9CheckChanges(const Pcsx2Config* old_config)
{
	const char* hddPath;
	if (!isRunning)
		return;

	/* Eth */
	ReconfigureLiveNet(old_config);

	/* Hdd: the path, validated */
	hddPath = GetHDDPath();

	/* Hdd Compare with old config */
	if (EmuConfig.DEV9.HddEnable)
	{
		if (old_config->DEV9.HddEnable)
		{
			/* ATA::Open/Close dosn't set any regs */
			/* So we can close/open to apply settings */
			if (strcmp(EmuConfig.DEV9.HddFile, old_config->DEV9.HddFile) != 0 ||
				strcmp(EmuConfig.DEV9.HddIdFile, old_config->DEV9.HddIdFile) != 0 ||
				EmuConfig.DEV9.HddSizeSectors != old_config->DEV9.HddSizeSectors)
			{
				ata_close(dev9.ata);
				if (dev9_ata_open(hddPath) != 0)
					EmuConfig.DEV9.HddEnable = false;
			}
		}
		else if (dev9_ata_open(hddPath) != 0)
			EmuConfig.DEV9.HddEnable = false;
	}
	else if (old_config->DEV9.HddEnable)
		ata_close(dev9.ata);
}
