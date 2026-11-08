/*  PCSX2 - PS2 Emulator for PCs
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

/* HDD images other than a raw file, for the ATA drive: VHD (fixed and
 * dynamic, read and write) and CHD (read only, its writes kept in an
 * overlay file beside it, <image>.overlay). A raw image stays on the
 * ATA's own path, which knows how to punch holes in it.
 *
 * Offsets and lengths are in bytes and whole 512-byte sectors. Used from
 * one thread at a time (the ATA's I/O thread). C89. */
#ifndef HDDIMAGE_H
#define HDDIMAGE_H

#include <stdint.h>
#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	HDD_IMAGE_RAW = 0,
	HDD_IMAGE_VHD = 1,
	HDD_IMAGE_CHD = 2
};

/* The suffix of a CHD's write overlay, after the image's own name. */
#define HDD_IMAGE_OVERLAY_SUFFIX ".overlay"

typedef struct hdd_image hdd_image_t;

/* Which kind path is, by its extension: .vhd, .chd, anything else raw. */
int hdd_image_kind(const char* path);

/* NULL, logged, when path cannot be opened as its kind (a raw path is
 * never opened here). */
hdd_image_t* hdd_image_open(const char* path);
void hdd_image_close(hdd_image_t* img);

/* The size of the disk the image holds, in bytes. */
uint64_t hdd_image_size(const hdd_image_t* img);

bool hdd_image_read(hdd_image_t* img, uint64_t offset, void* dst, uint32_t len);
bool hdd_image_write(hdd_image_t* img, uint64_t offset, const void* src, uint32_t len);
/* What is written so far, on the medium. */
bool hdd_image_flush(hdd_image_t* img);

#ifdef __cplusplus
}
#endif

#endif
