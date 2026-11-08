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
#include <retro_atomic.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>

#include "ATA.h"

/* ---- Write queue ---------------------------------------------------- */

/* The C lowering of DEV9's SimpleQueue<T>: one producer, one consumer,
 * a sentinel node always at head. The producer swaps a fresh sentinel
 * into head and fills in the old one; the ready flag publishes the
 * fill to the consumer. */

static ata_queue_node_t* ata_write_queue_new_node(void)
{
	ata_queue_node_t* node = (ata_queue_node_t*)calloc(1, sizeof(ata_queue_node_t));
	if (!node)
		abort();
	retro_atomic_int_init(&node->ready, 0);
	return node;
}

void ata_write_queue_init(ata_write_queue_t* q)
{
	q->tail = ata_write_queue_new_node();
	retro_atomic_ptr_init(&q->head, q->tail);
	retro_atomic_store_release_ptr(&q->head, q->tail);
}

/* Used by single queue thread (i.e. EE) */
void ata_write_queue_enqueue(ata_write_queue_t* q, const ata_write_entry_t* entry)
{
	/* Allocate next entry, and assign to head */
	ata_queue_node_t* newHead = ata_write_queue_new_node();
	ata_queue_node_t* newEntry = (ata_queue_node_t*)retro_atomic_exchange_ptr(&q->head, newHead);

	/* Fill in */
	newEntry->value = *entry;
	newEntry->next = newHead;

	/* Set ready (can be dequeued) */
	retro_atomic_store_release_int(&newEntry->ready, 1);
}

/* Used by single worker thread (i.e. IO) */
bool ata_write_queue_dequeue(ata_write_queue_t* q, ata_write_entry_t* entry)
{
	ata_queue_node_t* retEntry;

	if (!retro_atomic_load_acquire_int(&q->tail->ready))
		return false;

	retEntry = q->tail;
	q->tail = retEntry->next;

	*entry = retEntry->value;
	free(retEntry);
	return true;
}

/* Note, next entry may not be ready to dequeue.
 * May return false negative when another thread is mid enqueue.
 * Intended to only be used from queue thread */
bool ata_write_queue_is_empty(ata_write_queue_t* q)
{
	return (ata_queue_node_t*)retro_atomic_load_acquire_ptr(&q->head) == q->tail;
}

void ata_write_queue_destroy(ata_write_queue_t* q)
{
	if (retro_atomic_load_acquire_ptr(&q->head))
	{
		if (!ata_write_queue_is_empty(q))
		{
			ata_write_entry_t entry;

			log_cb(RETRO_LOG_ERROR, "DEV9: Queue not empty\n");

			/* Empty queue. Unlike the templated original this knows
			 * what the payload is, so drained entries free their
			 * data instead of leaking it. */
			while (!ata_write_queue_is_empty(q))
			{
				if (ata_write_queue_dequeue(q, &entry))
					free(entry.data);
			}
		}

		/* One sentinel node always remains. */
		free(q->tail);
		retro_atomic_store_release_ptr(&q->head, NULL);
		q->tail = NULL;
	}
}

/* ---- IO thread ------------------------------------------------------ */

static void ata_io_read(ata_state_t* ata);
static bool ata_io_write(ata_state_t* ata);
static bool ata_io_sparse_cache_load(ata_state_t* ata);
static void ata_io_sparse_cache_update_location(ata_state_t* ata, uint64_t byteOffset);
static bool ata_io_sparse_zero(ata_state_t* ata, uint64_t byteOffset, uint64_t byteSize);
static bool ata_is_all_zero(const void* data, size_t len);

/* ioRead and ioWrite are requests: the EE sets one and announces it on
 * ioReadyEc, the IO thread clears it once served. ioThreadIdle is set
 * by the IO thread whenever it is between requests and announced on
 * ioIdleEc. Neither side takes a lock; each sleeps on its eventcount
 * only while the flag it waits for is still clear.
 *
 * ata_hdd_read_sync() withdraws a pending write and then waits for the
 * IO thread to be idle, while the IO thread marks itself busy and then
 * reads which request to serve. Each side stores, fences seq_cst and
 * loads the other's flag, so either the IO thread sees the write
 * withdrawn or the EE sees it busy and waits: a write never runs under
 * the EE's synchronous read. */

static INLINE bool ata_io_pending(ata_state_t* ata)
{
	return (retro_atomic_load_acquire_int(&ata->ioRead)
		| retro_atomic_load_acquire_int(&ata->ioWrite)) != 0;
}

void ata_io_request(ata_state_t* ata, retro_atomic_int_t* request)
{
	retro_atomic_store_release_int(request, 1);
	retro_eventcount_notify(&ata->ioReadyEc);
}

static void ata_io_wait_idle(ata_state_t* ata)
{
	while (!retro_atomic_load_acquire_int(&ata->ioThreadIdle))
	{
		int key = retro_eventcount_prepare_wait(&ata->ioIdleEc);
		if (retro_atomic_load_acquire_int(&ata->ioThreadIdle))
		{
			retro_eventcount_cancel_wait(&ata->ioIdleEc);
			break;
		}
		retro_eventcount_commit_wait(&ata->ioIdleEc, key);
	}
}

void ata_io_thread_entry(void* userdata)
{
	ata_state_t* ata = (ata_state_t*)userdata;

	for (;;)
	{
		retro_atomic_store_release_int(&ata->ioThreadIdle, 1);
		retro_eventcount_notify(&ata->ioIdleEc);

		while (!ata_io_pending(ata))
		{
			int key = retro_eventcount_prepare_wait(&ata->ioReadyEc);
			if (ata_io_pending(ata))
			{
				retro_eventcount_cancel_wait(&ata->ioReadyEc);
				break;
			}
			retro_eventcount_commit_wait(&ata->ioReadyEc, key);
		}

		retro_atomic_store_release_int(&ata->ioThreadIdle, 0);
		retro_atomic_thread_fence_seq_cst();

		/* Read or Write */
		if (retro_atomic_load_acquire_int(&ata->ioRead))
			ata_io_read(ata);
		else if (retro_atomic_load_acquire_int(&ata->ioWrite))
		{
			if (!ata_io_write(ata) && retro_atomic_load_acquire_int(&ata->ioClose))
			{
				retro_atomic_store_release_int(&ata->ioClose, 0);
				retro_atomic_store_release_int(&ata->ioThreadIdle, 1);
				return;
			}
		}
	}
}

/* A failed access to the image is logged once, until one succeeds. */
static void ata_io_result(ata_state_t* ata, bool ok, const char* what)
{
	if (ok)
		ata->ioErrorLogged = false;
	else if (!ata->ioErrorLogged)
	{
		ata->ioErrorLogged = true;
		log_cb(RETRO_LOG_ERROR, "DEV9: ATA: HDD image %s failed\n", what);
	}
}

/* A read the host fails reads as zeroes past what it did read, and the
 * command reports it (ata_hdd_read_failed) instead of handing them over. */
static void ata_io_read(ata_state_t* ata)
{
	const int64_t lba   = ata_hdd_get_lba(ata);
	const int64_t bytes = (int64_t)ata->nsector * 512;
	int64_t got         = -1;

	if (lba == -1)
		;
	else if (ata->hddImg)
		got = hdd_image_read(ata->hddImg, (uint64_t)lba * 512, ata->readBuffer, (uint32_t)bytes) ? bytes : -1;
	else if (filestream_seek(ata->hddImage, lba * 512, RETRO_VFS_SEEK_POSITION_START) >= 0)
		got = filestream_read(ata->hddImage, ata->readBuffer, bytes);
	ata->ioReadFailed = got != bytes;
	if (ata->ioReadFailed)
		memset(ata->readBuffer + (got > 0 ? got : 0), 0, (size_t)(bytes - (got > 0 ? got : 0)));
	ata_io_result(ata, !ata->ioReadFailed, "read");
	retro_atomic_store_release_int(&ata->ioRead, 0);
}

/* A write the host failed: the flush that follows reports it. */
static void ata_io_write_failed(ata_state_t* ata)
{
	retro_atomic_store_release_int(&ata->ioWriteFailed, 1);
	ata_io_result(ata, false, "write");
}

static bool ata_io_write(ata_state_t* ata)
{
	ata_write_entry_t entry;
	uint64_t imagePos;

	if (!ata_write_queue_dequeue(&ata->writeQueue, &entry))
	{
		/* A swap rather than a store: a request ata_close() set after
		 * the failed dequeue is read here, and with it the ioClose it
		 * published first. */
		retro_atomic_exchange_int(&ata->ioWrite, 0);
		return false;
	}

	imagePos = entry.sector * 512;
	if (ata->hddImg)
	{
		if (!hdd_image_write(ata->hddImg, imagePos, entry.data, entry.length) || !hdd_image_flush(ata->hddImg))
			ata_io_write_failed(ata);
		else
			ata_io_result(ata, true, "write");
		free(entry.data);
		return true;
	}
	if (filestream_seek(ata->hddImage, (int64_t)imagePos, RETRO_VFS_SEEK_POSITION_START) < 0)
	{
		ata_io_write_failed(ata);
		free(entry.data);
		return true;
	}
	if (ata->hddSparse)
	{
		uint32_t written = 0;
		while (written != entry.length)
		{
			uint32_t writeSize;
			bool sparseWrite;

			ata_io_sparse_cache_update_location(ata, imagePos + written);
			/* Align to sparse block size. */
			writeSize = (uint32_t)(ata->hddSparseBlockSize - ((imagePos + written) % ata->hddSparseBlockSize));
			/* Limit to size of write. */
			writeSize = MIN(writeSize, entry.length - written);

			sparseWrite = ata_is_all_zero(&entry.data[written], writeSize);

			if (sparseWrite)
			{
				if (!ata_io_sparse_zero(ata, imagePos + written, writeSize))
				{
					log_cb(RETRO_LOG_ERROR, "DEV9: ATA: File sparse write error\n");

					ata->hddSparse = false;
					free(ata->hddSparseBlock);
					ata->hddSparseBlock = NULL;
					ata->hddSparseBlockValid = false;

					/* Fallthough into other if statment. */
					sparseWrite = false;
				}
			}

			/* Also handles sparse write failures. */
			if (!sparseWrite)
			{
				/* Update cache. */
				if (ata->hddSparseBlockValid)
					memcpy(&ata->hddSparseBlock[(imagePos + written) - ata->HddSparseStart], &entry.data[written], writeSize);

				if (filestream_write(ata->hddImage, &entry.data[written], (int64_t)writeSize) != (int64_t)writeSize ||
					filestream_flush(ata->hddImage) != 0)
				{
					ata_io_write_failed(ata);
					break;
				}
			}
			written += writeSize;
		}
	}
	else
	{
		if (filestream_write(ata->hddImage, entry.data, (int64_t)entry.length) != (int64_t)entry.length ||
			filestream_flush(ata->hddImage) != 0)
			ata_io_write_failed(ata);
		else
			ata_io_result(ata, true, "write");
	}
	free(entry.data);
	return true;
}

/* False when the block cannot be read; the cache is then not valid. */
static bool ata_io_sparse_cache_load(ata_state_t* ata)
{
	/* Reads are bounds checked, but for the sectors read only.
	 * Need to bounds check for sparse block, to handle an edge case
	 * of a user providing a file with a size that dosn't align with
	 * the sparse block size. Normally that won't happen as we
	 * generate files of exact Gib size. */
	uint64_t readSize = ata->hddSparseBlockSize;
	const uint64_t posEnd = ata->HddSparseStart + ata->hddSparseBlockSize;
	int64_t orgPos;

	if (posEnd > ata->hddImageSize)
	{
		readSize = ata->hddSparseBlockSize - (posEnd - ata->hddImageSize);
		/* Zero cache for data beyond end of file. */
		memset(&ata->hddSparseBlock[readSize], 0, ata->hddSparseBlockSize - readSize);
	}

	/* Store file pointer. */
	orgPos = filestream_tell(ata->hddImage);

	/* The native-handle code asked the OS whether this block was still
	 * a hole (FSCTL_QUERY_ALLOCATED_RANGES, SEEK_HOLE) so it could skip
	 * the read. The VFS has no such query, so the block is always read;
	 * a hole reads back as zeroes straight out of the page cache, so
	 * this costs a copy rather than I/O and the cache contents come out
	 * identical. The flush keeps the read coherent with our own
	 * buffered writes on whatever backend the VFS resolved to. */
	filestream_flush(ata->hddImage);

	/* Load into cache. */
	if (orgPos < 0 ||
		filestream_seek(ata->hddImage, (int64_t)ata->HddSparseStart, RETRO_VFS_SEEK_POSITION_START) < 0 ||
		filestream_read(ata->hddImage, ata->hddSparseBlock, (int64_t)readSize) != (int64_t)readSize ||
		filestream_seek(ata->hddImage, orgPos, RETRO_VFS_SEEK_POSITION_START) < 0) /* Restore file pointer. */
		return false;

	ata->hddSparseBlockValid = true;
	return true;
}

static void ata_io_sparse_cache_update_location(ata_state_t* ata, uint64_t byteOffset)
{
	const uint64_t currentBlockStart = (byteOffset / ata->hddSparseBlockSize) * ata->hddSparseBlockSize;
	if (currentBlockStart != ata->HddSparseStart)
	{
		ata->HddSparseStart = currentBlockStart;
		ata->hddSparseBlockValid = false;
		/* Only update cache when we perform a sparse write. */
	}
}

/* Also sets hddImage write ptr. */
static bool ata_io_sparse_zero(ata_state_t* ata, uint64_t byteOffset, uint64_t byteSize)
{
	/* An unreadable block falls back to plain writes. */
	if (!ata->hddSparseBlockValid && !ata_io_sparse_cache_load(ata))
		return false;

	/* Write to cache */
	memset(&ata->hddSparseBlock[byteOffset - ata->HddSparseStart], 0, byteSize);

	/* Is block non-zero? */
	if (!ata_is_all_zero(ata->hddSparseBlock, ata->hddSparseBlockSize))
	{
		/* No, do normal write */
		if (filestream_write(ata->hddImage, &ata->hddSparseBlock[byteOffset - ata->HddSparseStart], (int64_t)byteSize) != (int64_t)byteSize ||
			filestream_flush(ata->hddImage) != 0)
			ata_io_write_failed(ata);
		return true;
	}

	/* Yes, try sparse write. filestream_punch_hole carries the whole
	 * platform matrix the native-handle code open-coded here --
	 * FSCTL_SET_ZERO_DATA (arming FSCTL_SET_SPARSE itself first),
	 * fallocate(FALLOC_FL_PUNCH_HOLE), fcntl(F_PUNCHHOLE) -- and
	 * returns -1 where the backend or filesystem cannot punch at all,
	 * upon which the caller falls back to plain writes for good. */
	if (filestream_punch_hole(ata->hddImage, (int64_t)ata->HddSparseStart, (int64_t)ata->hddSparseBlockSize) != 0)
		return false;

	if (filestream_seek(ata->hddImage, (int64_t)(byteOffset + byteSize), RETRO_VFS_SEEK_POSITION_START) < 0)
		ata_io_write_failed(ata);
	return true;
}

static bool ata_is_all_zero(const void* data, size_t len)
{
	/* Check with the biggest int available most of the array, but
	 * without aligning it, then check the end of the non aligned
	 * array bytewise. */
	const int64_t* pbi = (const int64_t*)data;
	const int64_t* pbiUpper = ((const int64_t*)(((const char*)data) + len)) - 1;
	const char* p;

	for (; pbi <= pbiUpper; pbi++)
		if (*pbi)
			return false;
	for (p = (const char*)pbi; p < ((const char*)data) + len; p++)
		if (*p)
			return false;
	return true;
}

/* ---- Read entry points (device thread) ------------------------------ */

void ata_hdd_read_async(ata_state_t* ata, ata_cmd_fn drqCMD)
{
	ata->nsectorLeft = 0;

	if (!ata_hdd_can_assess_or_set_error(ata))
		return;

	ata->nsectorLeft = ata->nsector;
	if (ata->readBufferLen < ata->nsector * 512)
	{
		free(ata->readBuffer);
		ata->readBuffer = (uint8_t*)malloc((size_t)ata->nsector * 512);
		if (!ata->readBuffer)
			abort();
		ata->readBufferLen = ata->nsector * 512;
	}
	ata->waitingCmd = drqCMD;

	ata_io_request(ata, &ata->ioRead);
}

/* Note, we don't expect both Async & Sync Reads
 * Do one of the other */
void ata_hdd_read_sync(ata_state_t* ata, ata_cmd_fn drqCMD)
{
	bool ioWritePaused;

	/* Withdraw any write request to prevent reading & writing at the
	 * same time, then wait until the IO thread is idle. */
	ioWritePaused = retro_atomic_exchange_int(&ata->ioWrite, 0) != 0;
	retro_atomic_thread_fence_seq_cst();
	ata_io_wait_idle(ata);

	ata->nsectorLeft = 0;

	if (!ata_hdd_can_assess_or_set_error(ata))
	{
		if (ioWritePaused)
			ata_io_request(ata, &ata->ioWrite);
		return;
	}

	ata->nsectorLeft = ata->nsector;
	if (ata->readBufferLen < ata->nsector * 512)
	{
		free(ata->readBuffer);
		ata->readBuffer = (uint8_t*)malloc((size_t)ata->nsector * 512);
		if (!ata->readBuffer)
			abort();
		ata->readBufferLen = ata->nsector * 512;
	}

	ata_io_read(ata);

	if (ioWritePaused)
		ata_io_request(ata, &ata->ioWrite);

	if (ata->ioReadFailed)
		ata_hdd_read_failed(ata);
	else
		drqCMD(ata);
}

void ata_hdd_read_failed(ata_state_t* ata)
{
	ata->ioReadFailed = false;
	ata->nsectorLeft  = 0;
	ata->regStatus   |= (uint8_t)ATA_STAT_ERR;
	ata->regError    |= (uint8_t)ATA_ERR_ECC; /* uncorrectable data */
	ata_post_cmd_no_data(ata);
}

bool ata_hdd_can_assess_or_set_error(ata_state_t* ata)
{
	if (!ata_hdd_can_access(ata, &ata->nsector))
	{
		/* Read what we can */
		ata->regStatus |= (uint8_t)ATA_STAT_ERR;
		ata->regError |= (uint8_t)ATA_ERR_ID;
		if (ata->nsector == -1)
		{
			ata_post_cmd_no_data(ata);
			return false;
		}
	}
	return true;
}

void ata_hdd_set_error_at_transfer_end(ata_state_t* ata)
{
	uint64_t currSect = (uint64_t)ata_hdd_get_lba(ata);
	currSect += (uint64_t)ata->nsector;
	if ((ata->regStatus & ATA_STAT_ERR) != 0)
	{
		/* Error condition
		 * Write errored sector to LBA */
		currSect++;
		ata_hdd_set_lba(ata, currSect);
	}
}
