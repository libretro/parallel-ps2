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

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <retro_miscellaneous.h>

#include "ATA.h"

static void ata_init_sparse_support(ata_state_t* ata);
static void ata_clear_hob(ata_state_t* ata);

static void ata_reset_begin(ata_state_t* ata)
{
	ata_pre_cmd_execute_device_diag(ata);
}

static void ata_reset_end(ata_state_t* ata, bool hard)
{
	ata->curHeads = 16;
	ata->curSectors = 63;
	ata->curCylinders = 0;
	ata->curMultipleSectorsSetting = 128;

	/* UDMA Mode setting is preserved
	 * across SRST */
	if (hard)
	{
		ata->pioMode = 4;
		ata->sdmaMode = -1;
		ata->mdmaMode = 2;
		ata->udmaMode = -1;
	}
	else
	{
		ata->pioMode = 4;
		if (ata->udmaMode == -1)
		{
			ata->sdmaMode = -1;
			ata->mdmaMode = 2;
		}
	}

	ata->regControlEnableIRQ = false;
	ata_hdd_execute_device_diag(ata);
	ata->regControlEnableIRQ = true;
}

ata_state_t* ata_new(void)
{
	ata_state_t* ata = (ata_state_t*)calloc(1, sizeof(*ata));

	if (!ata)
		return NULL;

	/* Defaults the C++ member initializers used to provide; everything
	 * else is zero. */
	ata->curHeads = 16;
	ata->curSectors = 63;
	ata->curMultipleSectorsSetting = 128;
	ata->fetSmartEnabled = true;
	ata->fetWriteCacheEnabled = true;
	ata->smartAutosave = true;

	/* Power on, Would do self-Diag + Hardware Init */
	ata_reset_begin(ata);
	ata_reset_end(ata, true);

	return ata;
}

void ata_free(ata_state_t* ata)
{
	if (!ata)
		return;
	ata_close(ata);
	free(ata);
}

int ata_open(ata_state_t* ata, const char* hddPath, uint64_t size_sectors)
{
	int64_t size;

	ata->hddSizeSectors = size_sectors;

	ata->readBufferLen = 256 * 512;
	ata->readBuffer = (uint8_t*)malloc(ata->readBufferLen);
	if (!ata->readBuffer)
		return -1;

	/* Open File */
	if (!path_is_valid(hddPath))
		return -1;

	if (hdd_image_kind(hddPath) != HDD_IMAGE_RAW)
	{
		/* A VHD or CHD is the disk it describes, whatever the size
		 * option says. */
		if (!(ata->hddImg = hdd_image_open(hddPath)))
			return -1;
		ata->hddImageSize = hdd_image_size(ata->hddImg);
		ata->hddSizeSectors = ata->hddImageSize / 512;
		ata->hddSparse = false;
		ata->hddSparseBlockValid = false;
	}
	else
	{
		ata->hddImage = filestream_open(hddPath,
				RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
				RETRO_VFS_FILE_ACCESS_HINT_NONE);
		size = ata->hddImage ? filestream_get_size(ata->hddImage) : -1;
		if (!ata->hddImage || size < 0)
		{
			log_cb(RETRO_LOG_ERROR, "Failed to open HDD image '%s'\n", hddPath);
			return -1;
		}

		/* Store HddImage size for later check */
		ata->hddImageSize = (uint64_t)size;

		ata_init_sparse_support(ata);
	}

	ata_create_hdd_info(ata, ata->hddSizeSectors);

	ata_write_queue_init(&ata->writeQueue);

	if (!retro_eventcount_init(&ata->ioReadyEc) || !retro_eventcount_init(&ata->ioIdleEc))
		return -1;

	retro_atomic_int_init(&ata->ioClose, 0);
	retro_atomic_int_init(&ata->ioRead, 0);
	retro_atomic_int_init(&ata->ioWrite, 0);
	retro_atomic_int_init(&ata->ioThreadIdle, 0);
	retro_atomic_int_init(&ata->ioWriteFailed, 0);
	ata->ioReadFailed  = false;
	ata->ioErrorLogged = false;

	ata->ioThread = sthread_create(ata_io_thread_entry, ata);
	if (!ata->ioThread)
		return -1;
	ata->ioRunning = true;

	return 0;
}

static void ata_init_sparse_support(ata_state_t* ata)
{
	int64_t granularity;

	/* The VFS answers both questions the old native-handle code
	 * existed for: whether this file lives somewhere holes can be
	 * punched, and the smallest span a punch actually deallocates (on
	 * NTFS the compression unit, not the cluster). 0 means the backend
	 * cannot say -- a frontend-supplied VFS has no descriptor to ask --
	 * and is treated as no sparse support, never as "no alignment
	 * needed". Whether a punch really works is only knowable by
	 * trying; ata_io_write() drops to plain writes on the first
	 * failure. */
	ata->hddSparse = false;
	ata->hddSparseBlockValid = false;

	granularity = filestream_get_sparse_granularity(ata->hddImage);
	if (granularity <= 0)
		return;

	ata->hddSparseBlockSize = (uint64_t)granularity;
	ata->hddSparseBlock = (uint8_t*)malloc(ata->hddSparseBlockSize);
	if (!ata->hddSparseBlock)
		return;
	ata->hddSparse = true;
}

void ata_close(ata_state_t* ata)
{
	/* Wait for async code to finish */
	if (ata->ioRunning)
	{
		retro_atomic_store_release_int(&ata->ioClose, 1);
		ata_io_request(ata, &ata->ioWrite);

		sthread_join(ata->ioThread);
		ata->ioThread = NULL;
		ata->ioRunning = false;
	}

	/* verify queue */
	if (!ata_write_queue_is_empty(&ata->writeQueue))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ATA: Write queue not empty, possible data loss\n");
		abort(); /* All data must be written at this point */
	}
	ata_write_queue_destroy(&ata->writeQueue);

	retro_eventcount_free(&ata->ioReadyEc);
	retro_eventcount_free(&ata->ioIdleEc);

	/* Close File Handle */
	if (ata->hddSparse)
	{
		ata->hddSparse = false;
		ata->hddSparseBlockValid = false;
	}
	if (ata->hddSparseBlock)
	{
		free(ata->hddSparseBlock);
		ata->hddSparseBlock = NULL;
	}
	if (ata->hddImage)
	{
		filestream_close(ata->hddImage);
		ata->hddImage = NULL;
	}
	if (ata->hddImg)
	{
		hdd_image_close(ata->hddImg);
		ata->hddImg = NULL;
	}

	free(ata->readBuffer);
	ata->readBuffer = NULL;
}

void ata_hard_reset(ata_state_t* ata)
{
	/* log_cb(RETRO_LOG_DEBUG, "DEV9: *ATA_HARD RESET\n"); */
	ata_reset_begin(ata);
	ata_reset_end(ata, true);
}

uint8_t ata_get_selected_device(const ata_state_t* ata)
{
	return (ata->regSelect >> 4) & 1;
}

void ata_set_selected_device(ata_state_t* ata, uint8_t value)
{
	if (value == 1)
		ata->regSelect |= (1 << 4);
	else
		ata->regSelect &= ~(1 << 4);
}

uint16_t ata_read16(ata_state_t* ata, uint32_t addr)
{
	switch (addr)
	{
		case ATA_R_DATA:
			return ata_read_pio(ata);
		case ATA_R_ERROR:
			if (ata_get_selected_device(ata) != 0)
				return 0;
			return ata->regError;
		case ATA_R_NSECTOR:
			if (ata_get_selected_device(ata) != 0)
				return 0;
			if (!ata->regControlHOBRead)
				return ata->regNsector;
			return ata->regNsectorHOB;
		case ATA_R_SECTOR:
			if (ata_get_selected_device(ata) != 0)
				return 0;
			if (!ata->regControlHOBRead)
				return ata->regSector;
			return ata->regSectorHOB;
		case ATA_R_LCYL:
			if (ata_get_selected_device(ata) != 0)
				return 0;
			if (!ata->regControlHOBRead)
				return ata->regLcyl;
			return ata->regLcylHOB;
		case ATA_R_HCYL:
			if (ata_get_selected_device(ata) != 0)
				return 0;
			if (!ata->regControlHOBRead)
				return ata->regHcyl;
			return ata->regHcylHOB;
		case ATA_R_SELECT:
			return ata->regSelect;
		case ATA_R_STATUS:
			/* Clear irqcause */
			dev9_irq_cause_clear(ATA_INTR_INTRQ);
			/* fallthrough */
		case ATA_R_ALT_STATUS:
			/* raise IRQ? */
			if (ata_get_selected_device(ata) != 0)
				return 0;
			return ata->regStatus;
		default:
			log_cb(RETRO_LOG_ERROR, "DEV9: ATA: Unknown 16bit read at address %x\n", addr);
			return 0xff;
	}
}

void ata_write16(ata_state_t* ata, uint32_t addr, uint16_t value)
{
	if (addr != ATA_R_CMD && (ata->regStatus & (ATA_STAT_BUSY | ATA_STAT_DRQ)) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: ATA: DEVICE BUSY, DROPPING WRITE\n");
		return;
	}
	switch (addr)
	{
		case ATA_R_FEATURE:
			ata_clear_hob(ata);
			ata->regFeatureHOB = ata->regFeature;
			ata->regFeature = (uint8_t)value;
			break;
		case ATA_R_NSECTOR:
			ata_clear_hob(ata);
			ata->regNsectorHOB = ata->regNsector;
			ata->regNsector = (uint8_t)value;
			break;
		case ATA_R_SECTOR:
			ata_clear_hob(ata);
			ata->regSectorHOB = ata->regSector;
			ata->regSector = (uint8_t)value;
			break;
		case ATA_R_LCYL:
			ata_clear_hob(ata);
			ata->regLcylHOB = ata->regLcyl;
			ata->regLcyl = (uint8_t)value;
			break;
		case ATA_R_HCYL:
			ata_clear_hob(ata);
			ata->regHcylHOB = ata->regHcyl;
			ata->regHcyl = (uint8_t)value;
			break;
		case ATA_R_SELECT:
			ata->regSelect = (uint8_t)value;
			break;
		case ATA_R_CONTROL:
			if ((value & 0x2) != 0)
			{
				/* Supress all IRQ */
				dev9_irq_cause_clear(ATA_INTR_INTRQ);
				ata->regControlEnableIRQ = false;
			}
			else
				ata->regControlEnableIRQ = true;

			if ((value & 0x4) != 0)
			{
				log_cb(RETRO_LOG_DEBUG, "DEV9: *ATA_R_CONTROL RESET\n");
				ata_reset_begin(ata);
				ata_reset_end(ata, false);
			}
			if ((value & 0x80) != 0)
				ata->regControlHOBRead = true;

			break;
		case ATA_R_CMD:
			ata->regCommand = value;
			ata->regControlHOBRead = false;
			dev9_irq_cause_clear(ATA_INTR_INTRQ);
			ata_ide_exec_cmd(ata, value);
			break;
		default:
			log_cb(RETRO_LOG_ERROR, "DEV9: ATA: UNKNOWN 16bit write at address %x, value %x\n", addr, value);
			break;
	}
}

void ata_async(ata_state_t* ata, uint32_t cycles)
{
	(void)cycles;

	if (!ata->hddImage && !ata->hddImg)
		return;

	if ((ata->regStatus & (ATA_STAT_BUSY | ATA_STAT_DRQ)) == 0 ||
		ata->awaitFlush || (ata->waitingCmd != NULL))
	{
		if (retro_atomic_load_acquire_int(&ata->ioRead)
			| retro_atomic_load_acquire_int(&ata->ioWrite))
			return; /* IO Running */

		/* Note, ioThread may still be working. */
		if (ata->waitingCmd != NULL) /* Are we waiting to continue a command? */
		{
			ata_cmd_fn cmd = ata->waitingCmd;
			ata->waitingCmd = NULL;
			if (ata->ioReadFailed)
				ata_hdd_read_failed(ata);
			else
				cmd(ata);
		}
		else if (!ata_write_queue_is_empty(&ata->writeQueue)) /* Flush cache */
			ata_io_request(ata, &ata->ioWrite);
		else if (ata->awaitFlush) /* Fire IRQ on flush completion? */
		{
			ata->awaitFlush = false;
			/* A write the host failed since the last flush is this
			 * flush's error: the data did not reach the image. */
			if (retro_atomic_exchange_int(&ata->ioWriteFailed, 0))
			{
				ata->regStatus |= (uint8_t)(ATA_STAT_ERR | ATA_STAT_WRERR);
				ata->regError  |= (uint8_t)ATA_ERR_ABORT;
			}
			ata_post_cmd_no_data(ata);
		}
	}
}

int64_t ata_hdd_get_lba(ata_state_t* ata)
{
	if ((ata->regSelect & 0x40) != 0)
	{
		if (!ata->lba48)
		{
			return (ata->regSector |
					(ata->regLcyl << 8) |
					(ata->regHcyl << 16) |
					((ata->regSelect & 0x0f) << 24));
		}
		else
		{
			return ((int64_t)ata->regHcylHOB << 40) |
				   ((int64_t)ata->regLcylHOB << 32) |
				   ((int64_t)ata->regSectorHOB << 24) |
				   ((int64_t)ata->regHcyl << 16) |
				   ((int64_t)ata->regLcyl << 8) |
				   ata->regSector;
		}
	}
	else
	{
		ata->regStatus |= (uint8_t)ATA_STAT_ERR;
		ata->regError |= (uint8_t)ATA_ERR_ABORT;

		log_cb(RETRO_LOG_ERROR, "DEV9: ATA: Tried to get LBA address while LBA mode disabled\n");
		return -1;
	}
}

void ata_hdd_set_lba(ata_state_t* ata, int64_t sectorNum)
{
	if ((ata->regSelect & 0x40) != 0)
	{
		if (!ata->lba48)
		{
			ata->regSelect = (uint8_t)((ata->regSelect & 0xf0) | (int)((sectorNum >> 24) & 0x0f));
			ata->regHcyl = (uint8_t)(sectorNum >> 16);
			ata->regLcyl = (uint8_t)(sectorNum >> 8);
			ata->regSector = (uint8_t)(sectorNum);
		}
		else
		{
			ata->regSector = (uint8_t)sectorNum;
			ata->regLcyl = (uint8_t)(sectorNum >> 8);
			ata->regHcyl = (uint8_t)(sectorNum >> 16);
			ata->regSectorHOB = (uint8_t)(sectorNum >> 24);
			ata->regLcylHOB = (uint8_t)(sectorNum >> 32);
			ata->regHcylHOB = (uint8_t)(sectorNum >> 40);
		}
	}
	else
	{
		ata->regStatus |= ATA_STAT_ERR;
		ata->regError |= ATA_ERR_ABORT;

		log_cb(RETRO_LOG_ERROR, "DEV9: ATA: Tried to set LBA address while LBA mode disabled\n");
	}
}

bool ata_hdd_can_seek(ata_state_t* ata)
{
	int sectors = 0;
	return ata_hdd_can_access(ata, &sectors);
}

bool ata_hdd_can_access(ata_state_t* ata, int* sectors)
{
	int64_t lba;
	int64_t posStart;
	int64_t posEnd;
	int64_t maxLBA;

	maxLBA = MIN((int64_t)ata->hddSizeSectors, (int64_t)(ata->hddImageSize / 512)) - 1;
	if ((ata->regSelect & 0x40) == 0) /* CHS mode */
		maxLBA = MIN(maxLBA, (int64_t)ata->curCylinders * ata->curHeads * ata->curSectors);

	lba = ata_hdd_get_lba(ata);
	if (lba == -1)
		return false;

	posStart = lba;

	if (posStart > maxLBA)
	{
		*sectors = -1;
		return false;
	}

	posEnd = posStart + *sectors;

	if (posEnd > maxLBA)
	{
		const int64_t overshoot = posEnd - maxLBA;
		int64_t space = *sectors - overshoot;
		*sectors = (int)space;
		return false;
	}

	return true;
}

/* QEMU stuff */
static void ata_clear_hob(ata_state_t* ata)
{
	/* any write clears HOB high bit of device control register */
	ata->regControlHOBRead = false;
}
