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

#ifndef __DEV9_ATA_H__
#define __DEV9_ATA_H__

#include <stddef.h>
#include <stdint.h>

#include <boolean.h>
#include <retro_common_api.h>
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <streams/file_stream.h>

#include "HddImage.h"

/* The emulated SPEED ATA hard drive, in C89. State lives in a plain
 * struct and every function takes it explicitly; the member names
 * match the ones upstream PCSX2 uses so field-level diffs against the
 * donor tree still line up. */

/* ATA register offsets within the DEV9 HDD register window. The host
 * (DEV9.cpp) owns the bus map and passes (addr - ATA_DEV9_HDD_BASE)
 * into ata_read16/ata_write16. */
#define ATA_R_DATA       0x00
#define ATA_R_ERROR      0x02 /* On Read */
#define ATA_R_FEATURE    0x02 /* On Write */
#define ATA_R_NSECTOR    0x04
#define ATA_R_SECTOR     0x06
#define ATA_R_LCYL       0x08
#define ATA_R_HCYL       0x0a
#define ATA_R_SELECT     0x0c
#define ATA_R_STATUS     0x0e /* On Read */
#define ATA_R_CMD        0x0e /* On Write */
#define ATA_R_ALT_STATUS 0x1c /* On Read */
#define ATA_R_CONTROL    0x1c /* On Write */

#define ATA_ERR_MARK   0x01
#define ATA_ERR_TRACK0 0x02
#define ATA_ERR_ABORT  0x04
#define ATA_ERR_MCR    0x08
#define ATA_ERR_ID     0x10
#define ATA_ERR_MC     0x20
#define ATA_ERR_ECC    0x40
#define ATA_ERR_ICRC   0x80

#define ATA_STAT_ERR   0x01
#define ATA_STAT_INDEX 0x02
#define ATA_STAT_ECC   0x04
#define ATA_STAT_DRQ   0x08
#define ATA_STAT_SEEK  0x10
#define ATA_STAT_WRERR 0x20
#define ATA_STAT_READY 0x40
#define ATA_STAT_BUSY  0x80

#define ATA_INTR_INTRQ (1 << 0)

/* SPEED interrupt bit the ATA FIFO raises; guarded because DEV9.h
 * historically carried the definition. */
#ifndef SPD_INTR_ATA_FIFO_DATA
#define SPD_INTR_ATA_FIFO_DATA (1 << 1)
#endif

RETRO_BEGIN_DECLS

/* ---- Interface provided by the host --------------------------------- */

/* Raise a DEV9 interrupt after the given cycle delay. Implemented in
 * DEV9.cpp with C linkage. */
void _DEV9irq(int cause, int cycles);
/* dev9.irqcause &= ~bits. The irq cause register belongs to the host;
 * the ATA device only ever clears its own bits in it. */
void dev9_irq_cause_clear(int bits);
/* (dev9.if_ctrl & SPD_IF_ATA_DMAEN) != 0 */
int dev9_ata_dma_enabled(void);

/* The frontend's logger, defined in libretro/main.cpp with C linkage;
 * callers put the newline in the format. */
#include <libretro.h>
extern retro_log_printf_t log_cb;

/* ---- Write queue ---------------------------------------------------- */

/* One EE-thread producer (command completion), one IO-thread consumer.
 * The C lowering of DEV9's SimpleQueue<T> for the one instantiation the
 * ATA device needs; SimpleQueue.h itself remains for its C++ users. */
typedef struct ata_write_entry
{
	uint8_t* data;
	uint64_t sector;
	uint32_t length;
} ata_write_entry_t;

typedef struct ata_queue_node
{
	struct ata_queue_node* next;
	ata_write_entry_t value;
	retro_atomic_int_t ready;
} ata_queue_node_t;

typedef struct ata_write_queue
{
	retro_atomic_ptr_t head;
	ata_queue_node_t* tail;
} ata_write_queue_t;

void ata_write_queue_init(ata_write_queue_t* q);
void ata_write_queue_destroy(ata_write_queue_t* q);
/* Used by single queue thread (i.e. EE) */
void ata_write_queue_enqueue(ata_write_queue_t* q, const ata_write_entry_t* entry);
/* Used by single worker thread (i.e. IO) */
bool ata_write_queue_dequeue(ata_write_queue_t* q, ata_write_entry_t* entry);
/* May return false negative when another thread is mid enqueue.
 * Intended to only be used from queue thread */
bool ata_write_queue_is_empty(ata_write_queue_t* q);

/* ---- Device state --------------------------------------------------- */

struct ata_state;
typedef void (*ata_cmd_fn)(struct ata_state* ata);

typedef struct ata_state
{
	uint64_t hddImageSize;
	uint64_t hddSizeSectors; /* Config value handed to ata_open(). */
	uint64_t hddSparseBlockSize;
	uint64_t HddSparseStart;
	uint64_t currentWriteSectors;

	RFILE* hddImage;       /* a raw image */
	hdd_image_t* hddImg;   /* a VHD or CHD; one of the two is open */
	uint8_t* hddSparseBlock;
	uint8_t* readBuffer;
	uint8_t* currentWrite; /* array */

	sthread_t* ioThread;
	retro_eventcount_t ioReadyEc; /* EE -> IO thread: a request is set */
	retro_eventcount_t ioIdleEc;  /* IO thread -> EE: it went idle   */

	ata_cmd_fn waitingCmd;
	ata_cmd_fn pioDRQEndTransferFunc;

	ata_write_queue_t writeQueue;

	int nsector;     /* sector count */
	int nsectorLeft; /* sectors left to transfer */
	uint32_t currentWriteLength;

	int pioMode;
	int sdmaMode;
	int mdmaMode;
	int udmaMode;

	/* Max tranfer on 24bit is 256*512 = 128KB
	 * Max tranfer on 48bit is 65536*512 = 32MB */
	int rdTransferred;
	int wrTransferred;
	int readBufferLen;

	/* PIO Buffer */
	int pioPtr;
	int pioEnd;
	int sectorsPerInterrupt;

	/* IO thread handshake; see ATA_Transfer.c. */
	retro_atomic_int_t ioClose;
	retro_atomic_int_t ioRead;
	retro_atomic_int_t ioWrite;
	retro_atomic_int_t ioThreadIdle;
	/* The host failed a queued write since the last flush reported;
	 * set by the IO thread, taken by the flush's completion. */
	retro_atomic_int_t ioWriteFailed;

	uint16_t curCylinders;
	/* WriteOnly, Only to be written BSY and DRQ are cleared, DMACK is
	 * not set and device is not sleeping, except for DEVICE RESET */
	uint16_t regCommand;

	uint8_t curHeads;
	uint8_t curSectors;
	uint8_t curMultipleSectorsSetting;

	uint8_t regError; /* ReadOnly */
	/* DEVICE REG (Read/Write)
	 * Bit 0-3: LBA Bits 24-27 (Unused in 48bit) or Command Dependent
	 * Bit 4: Selected Device
	 * Bit 5: Obsolete (All?)
	 * Bit 6: Command Dependent
	 * Bit 7: Obsolete (All?) */
	uint8_t regSelect;
	/* WriteOnly, Only to be written BSY and DRQ are cleared and DMACK
	 * is not set */
	uint8_t regFeature;
	uint8_t regFeatureHOB;
	/* Following regs are Read/Write, Only to be written BSY and DRQ
	 * are cleared and DMACK is not set */
	uint8_t regSector; /* Sector Number or LBA Low */
	uint8_t regSectorHOB;
	uint8_t regLcyl; /* LBA Mid */
	uint8_t regLcylHOB;
	uint8_t regHcyl; /* LBA High */
	uint8_t regHcylHOB;
	/* TODO handle nsector code */
	uint8_t regNsector;
	uint8_t regNsectorHOB;
	/* ReadOnly. When read via AlternateStatus pending interrupts are
	 * not cleared */
	uint8_t regStatus;

	uint8_t smartSelfTestCount;

	uint8_t identifyData[512];
	uint8_t pioBuffer[512];
	uint8_t sceSec[256 * 2];

	bool dmaReady;
	bool lba48Supported;

	bool hddSparse;
	bool hddSparseBlockValid;
	/* The host failed the last read; written before the read request
	 * is cleared, read once it is. */
	bool ioReadFailed;
	/* The host's last access to the image failed: logged once until
	 * one succeeds. */
	bool ioErrorLogged;

	/* LBA48 in use? */
	bool lba48;

	/* Enable/disable features */
	bool fetSmartEnabled;
	bool fetSecurityEnabled;
	bool fetWriteCacheEnabled;
	bool fetHostProtectedAreaEnabled;

	bool regControlEnableIRQ; /* Bit 1 = 1 Disable Interrupt */
	/* Bit 7 = HOB (cleared by any write to RegCommand, Sets if Low
	 * order or High order bytes are read in ata_read16) */
	bool regControlHOBRead;

	bool awaitFlush;
	bool ioRunning;

	/* Smart */
	bool smartAutosave;
	bool smartErrors;
} ata_state_t;

#ifdef __cplusplus
/* The struct is shared between C and C++ translation units, so the
 * atomic members must have identical layout under both languages'
 * retro_atomic backends. Every implementation we target satisfies
 * this; make a violation a compile error rather than silent state
 * corruption. */
static_assert(sizeof(retro_atomic_int_t) == sizeof(int) &&
		alignof(retro_atomic_int_t) == alignof(int),
	"retro_atomic_int_t layout differs between C and C++ TUs");
static_assert(sizeof(retro_atomic_ptr_t) == sizeof(void*) &&
		alignof(retro_atomic_ptr_t) == alignof(void*),
	"retro_atomic_ptr_t layout differs between C and C++ TUs");
#endif

/* ---- Host-facing API ------------------------------------------------ */

/* Allocate and power on (self-diag + hardware init). */
ata_state_t* ata_new(void);
/* ata_close() then free. */
void ata_free(ata_state_t* ata);

/* size_sectors is the configured HDD size (was EmuConfig.DEV9.
 * HddSizeSectors); the device keeps it for IDENTIFY DEVICE and the
 * access checks. Returns 0 on success. */
int ata_open(ata_state_t* ata, const char* hddPath, uint64_t size_sectors);
void ata_close(ata_state_t* ata);

void ata_hard_reset(ata_state_t* ata);

/* addr is the offset within the ATA register window (ATA_R_*). */
uint16_t ata_read16(ata_state_t* ata, uint32_t addr);
void ata_write16(ata_state_t* ata, uint32_t addr, uint16_t value);

void ata_async(ata_state_t* ata, uint32_t cycles);

void ata_read_dma8_mem(ata_state_t* ata, uint8_t* pMem, int size);
void ata_write_dma8_mem(ata_state_t* ata, uint8_t* pMem, int size);

uint16_t ata_read_pio(ata_state_t* ata);

/* ---- Internals shared between the ATA translation units ------------- */

/* State */
uint8_t ata_get_selected_device(const ata_state_t* ata);
void ata_set_selected_device(ata_state_t* ata, uint8_t value);
int64_t ata_hdd_get_lba(ata_state_t* ata);
void ata_hdd_set_lba(ata_state_t* ata, int64_t sectorNum);
bool ata_hdd_can_seek(ata_state_t* ata);
bool ata_hdd_can_access(ata_state_t* ata, int* sectors);

/* Info */
void ata_create_hdd_info(ata_state_t* ata, uint64_t sizeSectors);

/* Transfer */
void ata_io_thread_entry(void* userdata); /* sthread entry; userdata is the ata_state_t* */
/* Set an IO request flag (ioRead or ioWrite) and wake the IO thread. */
void ata_io_request(ata_state_t* ata, retro_atomic_int_t* request);
void ata_hdd_read_async(ata_state_t* ata, ata_cmd_fn drqCMD);
void ata_hdd_read_sync(ata_state_t* ata, ata_cmd_fn drqCMD);
/* Ends a read command the host failed to read the image for: an
 * uncorrectable-data error, and no data. */
void ata_hdd_read_failed(ata_state_t* ata);
bool ata_hdd_can_assess_or_set_error(ata_state_t* ata);
void ata_hdd_set_error_at_transfer_end(ata_state_t* ata);

/* Commands */
void ata_ide_exec_cmd(ata_state_t* ata, uint16_t value);
bool ata_pre_cmd(ata_state_t* ata);
void ata_ide_cmd_lba48_transform(ata_state_t* ata, bool islba48);
void ata_cmd_no_data_abort(ata_state_t* ata);
void ata_post_cmd_no_data(ata_state_t* ata);
void ata_pre_cmd_execute_device_diag(ata_state_t* ata);
void ata_hdd_execute_device_diag(ata_state_t* ata);
void ata_hdd_flush_cache(ata_state_t* ata);
void ata_hdd_init_dev_parameters(ata_state_t* ata);
void ata_hdd_read_verify_sectors(ata_state_t* ata, bool isLBA48);
void ata_hdd_seek_cmd(ata_state_t* ata);
void ata_hdd_set_features(ata_state_t* ata);
void ata_hdd_set_multiple_mode(ata_state_t* ata);
void ata_hdd_nop(ata_state_t* ata);
void ata_hdd_idle(ata_state_t* ata);
void ata_hdd_idle_immediate(ata_state_t* ata);
void ata_drq_cmd_pio_data_to_host(ata_state_t* ata, uint8_t* buff, int buffLen,
		int buffIndex, int size, bool sendIRQ);
void ata_hdd_identify_device(ata_state_t* ata);
void ata_hdd_read_multiple(ata_state_t* ata, bool isLBA48);
void ata_hdd_read_sectors(ata_state_t* ata, bool isLBA48);
void ata_hdd_read_dma(ata_state_t* ata, bool isLBA48);
void ata_hdd_write_dma(ata_state_t* ata, bool isLBA48);
void ata_hdd_smart(ata_state_t* ata);
void ata_hdd_sce(ata_state_t* ata);

RETRO_END_DECLS

#endif /* __DEV9_ATA_H__ */
