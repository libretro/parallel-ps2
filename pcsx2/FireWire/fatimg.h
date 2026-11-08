/* One file in a FAT volume, read and written in place: the Python 1's
 * CF card is a FAT volume whose one file holds the game's own file
 * system, and the board reads and writes that file's sectors as a disk.
 *
 * The volume is the whole image or its first partition. FAT12, FAT16
 * and FAT32; the file is the root directory's first regular file that is
 * not empty. Its clusters are found once, at open, and kept as runs; the
 * file never changes size, so the FAT is never written. C89. */
#ifndef FATIMG_H
#define FATIMG_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads (write 0) or writes len bytes at offset of the image; nonzero
 * when it all went. */
typedef int (*fat_io_fn)(void* ctx, u64 offset, void* buf, u32 len, int write);

typedef struct fat_run
{
	u64 offset; /* in the image */
	u32 bytes;
} fat_run;

typedef struct fat_file
{
	fat_io_fn io;
	void* ctx;
	char name[13]; /* 8.3, dotted */
	u32 size;
	fat_run* runs;
	u32 nruns;
	/* The FAT sector last read, while the chain is walked. */
	u64 cache_off;
	int cache_valid;
	u8 cache[512];
} fat_file;

/* Finds the file; nonzero when there is one. Close it either way. */
int fat_open_first_file(fat_file* f, fat_io_fn io, void* ctx, u64 image_size);
void fat_close(fat_file* f);

/* len bytes at offset of the file; nonzero when all of it is inside the
 * file and went. */
int fat_rw(fat_file* f, u64 offset, void* buf, u32 len, int write);

#ifdef __cplusplus
}
#endif

#endif
