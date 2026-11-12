/* The disc playlist behind libretro's disk control interface: the paths,
 * which one the tray points at, the disc the frontend asked to resume on,
 * and the M3U and cue sheets that fill it. No VM state: inserting and
 * ejecting a disc stays with the caller. */
#ifndef LRPS2_DISK_LIST_H
#define LRPS2_DISK_LIST_H

#include <stddef.h>
#include <boolean.h>

/* An M3U lists at most this many discs; the frontend can append more,
 * up to DISK_LIST_SLOTS. */
#define DISK_LIST_M3U_MAX 10
#define DISK_LIST_SLOTS   64

#ifdef __cplusplus
extern "C" {
#endif

void        disk_list_clear(void);
unsigned    disk_list_count(void);
/* NULL past the end; "" for a slot appended and not yet filled. */
const char* disk_list_path(unsigned index);
/* The disc the tray points at: -1 once the last one is removed. */
int         disk_list_index(void);
bool        disk_list_set_index(unsigned index);
/* NULL or "" appends an empty slot. False when the list is full. */
bool        disk_list_append(const char* path);
/* NULL removes the entry, shifting the rest down. */
bool        disk_list_replace(unsigned index, const char* path);

/* set_initial_image: kept until the playlist is read, then applied with
 * libretro's fallback to the first disc when the list disagrees. */
void        disk_list_set_initial(unsigned index, const char* path);
void        disk_list_apply_initial(void);

/* path is a cue sheet; on return it names the sheet's first track. */
void        disk_list_cue_first_track(char* path, size_t size);
/* Reads an M3U into the list, applies the initial disc and writes the
 * one to boot into boot. False if the playlist cannot be read. */
bool        disk_list_load_m3u(const char* m3u_path, char* boot, size_t size);

#ifdef __cplusplus
}
#endif

#endif
