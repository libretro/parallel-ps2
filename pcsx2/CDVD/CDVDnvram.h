/* The mechacon NVRAM file (CDVD.c): loaded after each BIOS load, saved on
 * shutdown. C, so Memory.c reaches it; CDVD.h includes this. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void cdvdLoadNVRAM(void);
extern void cdvdSaveNVRAM(void);

/* Where the NVRAM lives and what a new one's i.Link ID is, for content
 * that brings its own (a Konami Python 2 game's machine). nvram replaces
 * the BIOS's .nvm beside it, loaded and saved; ilink_id is an 8-byte ID
 * or a 1 KiB NVRAM dump to take it from, used when the NVRAM is made
 * afresh. NULL or empty puts each back to the default. */
extern void cdvd_set_nvram_files(const char *nvram, const char *ilink_id);

#define CDVD_NVRAM_STORE_MAX 1024

/* Writes the size bytes at data to path, unless path already holds
 * exactly them. The file is replaced whole (filestream_write_file_atomic),
 * so an interrupted save leaves the previous contents. Returns 0 when it
 * was unchanged, 1 when written, -1 on failure or size above
 * CDVD_NVRAM_STORE_MAX. */
int cdvd_nvram_store(const char *path, const void *data, size_t size);

#ifdef __cplusplus
}
#endif
