/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2021 PCSX2 Dev Team
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

/* A disc image, read a sector at a time. C89.
 *
 * Every format (flat, CSO/ZSO, CHD, gzip) is a cdvd_reader with an ops
 * table. A format is either directly addressable - a mapped flat image,
 * whose sectors are read straight out of the mapping - or chunked: it
 * names the chunk an offset lies in and produces that chunk's bytes,
 * and the reader keeps the last few chunks in a small cache.
 *
 * cdvd_reader_lend hands out a sector where it already lies - in the
 * mapping, in a cached chunk, or in a chunk the format can lend in
 * place - so a caller that only copies it on is spared a copy;
 * cdvd_reader_read copies into the caller's buffer. */
#ifndef PCSX2_CDVD_READER_H
#define PCSX2_CDVD_READER_H

#include <boolean.h>
#include <streams/file_stream.h>

#include "../../common/Pcsx2Defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 *  Read-ahead of a mapped image
 * --------------------------------------------------------------------------
 * Windows starts no read-ahead on a mapped view: a reader that goes on in
 * order hints the next CDVD_RA_AHEAD bytes past each read, again once half
 * of that is used, and the file's read-ahead thread brings them in. A read
 * within CDVD_RA_GAP of where the last ended goes on in order. */
#define CDVD_RA_AHEAD (4u * 1024u * 1024u)
#define CDVD_RA_GAP   (256u * 1024u)

typedef struct cdvd_readahead
{
	u64 next;
	u64 last;
} cdvd_readahead_t;

/* true: hint [pos + len, pos + len + CDVD_RA_AHEAD) */
bool cdvd_readahead_step(cdvd_readahead_t* ra, u64 pos, u64 len);

#ifdef _WIN32
void cdvd_readahead_note(cdvd_readahead_t* ra, RFILE* fp, u64 pos, u64 len);
#else
#define cdvd_readahead_note(ra, fp, pos, len) ((void)0)
#endif

/* --------------------------------------------------------------------------
 *  Reader
 * -------------------------------------------------------------------------- */

/* The chunk an offset lies in; id < 0 past the end. */
typedef struct cdvd_chunk
{
	s64 id;
	u64 offset;
	u32 length;
} cdvd_chunk_t;

typedef struct cdvd_reader cdvd_reader_t;

typedef struct cdvd_reader_ops
{
	cdvd_chunk_t (*chunk_for_offset)(cdvd_reader_t* r, u64 offset);
	/* Produces the chunk into dst; returns its bytes, or <= 0. */
	int (*read_chunk)(cdvd_reader_t* r, void* dst, s64 id);
	/* Optional: the chunk's bytes where they already lie, valid while the
	 * image is open; returns their count, or 0 to have it read. */
	int (*lend_chunk)(cdvd_reader_t* r, s64 id, const u8** ptr);
	bool (*open)(cdvd_reader_t* r, const char* filename);
	void (*close)(cdvd_reader_t* r);
	u32 (*block_count)(const cdvd_reader_t* r);
} cdvd_reader_ops_t;

/* Chunks kept: enough that a few interleaved streams each keep theirs. */
#define CDVD_CACHE_SLOTS 8

typedef struct cdvd_cache_slot
{
	u64       offset;
	const u8* data;  /* buf, or the format's own bytes */
	u8*       buf;   /* owned */
	u32       size;  /* bytes valid at data; 0 empty */
	u32       cap;
	u32       used;  /* stamp of the last hit, for eviction */
} cdvd_cache_slot_t;

struct cdvd_reader
{
	const cdvd_reader_ops_t* ops;
	/* Directly addressable image: the whole of it, and its read-ahead. */
	const u8*                direct;
	u64                      direct_size;
	RFILE*                   direct_file;
	cdvd_readahead_t         direct_ra;
	cdvd_cache_slot_t        cache[CDVD_CACHE_SLOTS];
	u32                      clock;
	u32                      dataoffset;
	/* The sector size the caller reads. */
	u32                      blocksize;
	/* Nonzero where sectors are stored this far apart rather than
	 * blocksize (CHD units); a sector is its first blocksize bytes. */
	u32                      internal_block_size;
	char                     filename[PCSX2_PATH_MAX];
};

/* The reader for path, chosen by extension, opened; NULL when it does not
 * open. gz_index_template and data_root place a gzip image's quick-access
 * index (EmuConfig.GzipIsoIndexTemplate, EmuFolders.DataRoot). */
cdvd_reader_t* cdvd_reader_open(const char* path,
		const char* gz_index_template, const char* data_root);
/* Closes and frees; NULL is fine. */
void cdvd_reader_free(cdvd_reader_t* r);

#define cdvd_reader_block_count(r) ((r)->ops->block_count(r))

/* count sectors from sector into dst, blocksize bytes each. Returns the
 * bytes copied, or -1. */
int cdvd_reader_read(cdvd_reader_t* r, void* dst, u32 sector, u32 count);

/* One sector's blocksize bytes where they lie, or NULL when they cannot be
 * lent (past the end, or split across chunks): read them instead. Valid
 * until the next call on r. */
const u8* cdvd_reader_lend(cdvd_reader_t* r, u32 sector);

/* Shared by the formats: zeroed state and the ops, before open. */
void cdvd_reader_init(cdvd_reader_t* r, const cdvd_reader_ops_t* ops);
/* Frees the cache; the format's close is the caller's. */
void cdvd_reader_drop_cache(cdvd_reader_t* r);

/* The formats. Each returns a reader that is not yet open. */
cdvd_reader_t* cdvd_flat_new(void);
cdvd_reader_t* cdvd_cso_new(void);
cdvd_reader_t* cdvd_chd_new(void);
cdvd_reader_t* cdvd_gz_new(const char* index_template, const char* data_root);

#ifdef __cplusplus
}
#endif

#endif
