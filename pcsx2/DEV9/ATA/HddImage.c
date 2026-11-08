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

/* VHD and CHD HDD images. See HddImage.h. C89.
 *
 * VHD, after Microsoft's Virtual Hard Disk Image Format Specification:
 * a 512-byte big-endian footer at the end of the file ("conectix"). A
 * fixed disk is its data followed by the footer. A dynamic disk has a
 * header ("cxsparse") where the footer points, naming a block allocation
 * table: one sector number per block of the disk, 0xFFFFFFFF for a block
 * never written, which reads as zeros. A block is a sector bitmap padded
 * to whole sectors, then the block's data. A block first written is put
 * where the footer was, its bitmap all set and its data zeros until
 * written, the footer moved past it and the table entry written last.
 * Differencing disks are refused.
 *
 * CHD, through libretro-common's rchd, which does no I/O: its requests
 * are read from the file here. The image is not written. A write goes to
 * <image>.overlay: a header, a bitmap of the 4 KiB blocks written, then
 * those blocks at their place in the disk, the rest of the file left as
 * holes. A block only partly written takes the rest from the CHD first.
 * The header names the CHD (its SHA-1 and size), and an overlay of
 * another image is refused rather than laid over this one. */

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/rchd.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <libretro.h>

#include "HddImage.h"

extern retro_log_printf_t log_cb;

#define VHD_FOOTER_SIZE   512
#define VHD_DYN_HDR_SIZE  1024
#define VHD_TYPE_FIXED    2
#define VHD_TYPE_DYNAMIC  3
#define VHD_TYPE_DIFF     4
#define VHD_UNALLOCATED   0xFFFFFFFFu

#define OVL_HDR_SIZE      512
#define OVL_BLOCK         4096u
#define OVL_MAGIC         "LRPS2HDO"
#define OVL_VERSION       1

#define CHD_FEED          (64u * 1024u)

struct hdd_image
{
	int      kind;
	uint64_t size;
	RFILE*   fp;

	/* VHD */
	int       vhd_dynamic;
	uint8_t   vhd_footer[VHD_FOOTER_SIZE];
	uint64_t  vhd_footer_pos; /* where the trailing footer sits */
	uint64_t  vhd_bat_off;
	uint32_t  vhd_block_size;
	uint32_t  vhd_bitmap_bytes;
	uint32_t  vhd_entries;
	uint32_t* vhd_bat;        /* host order */

	/* CHD */
	rchd_t*   chd;
	uint32_t  hunk_bytes;
	uint8_t*  hunk;
	int64_t   hunk_index;     /* of what hunk holds, -1 none */
	uint8_t*  feed;
	RFILE*    ovl;
	uint8_t*  ovl_map;
	uint64_t  ovl_blocks;
	uint64_t  ovl_map_bytes;
	uint64_t  ovl_data_off;
	uint64_t  ovl_dirty_lo;   /* map bytes [lo, hi) not yet written */
	uint64_t  ovl_dirty_hi;
	uint8_t*  ovl_block;      /* one block, for a part write */
};

static uint32_t be32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t* p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void put_be32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void put_le32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t* p, uint64_t v)
{
	put_le32(p, (uint32_t)v);
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static bool pread_at(RFILE* fp, uint64_t off, void* dst, uint64_t len)
{
	return filestream_seek(fp, (int64_t)off, RETRO_VFS_SEEK_POSITION_START) >= 0 &&
		filestream_read(fp, dst, (int64_t)len) == (int64_t)len;
}

static bool pwrite_at(RFILE* fp, uint64_t off, const void* src, uint64_t len)
{
	return filestream_seek(fp, (int64_t)off, RETRO_VFS_SEEK_POSITION_START) >= 0 &&
		filestream_write(fp, src, (int64_t)len) == (int64_t)len;
}

int hdd_image_kind(const char* path)
{
	const char* ext = path_get_extension(path);
	if (ext && string_is_equal_noncase(ext, "vhd"))
		return HDD_IMAGE_VHD;
	if (ext && string_is_equal_noncase(ext, "chd"))
		return HDD_IMAGE_CHD;
	return HDD_IMAGE_RAW;
}

uint64_t hdd_image_size(const hdd_image_t* img)
{
	return img->size;
}

/* ---- VHD ----------------------------------------------------------------- */

static bool vhd_checksum_ok(const uint8_t* p, int len, int sum_at)
{
	uint32_t sum = 0;
	int i;
	for (i = 0; i < len; i++)
		if (i < sum_at || i >= sum_at + 4)
			sum += p[i];
	return (uint32_t)~sum == be32(p + sum_at);
}

static bool vhd_open(hdd_image_t* img, const char* path)
{
	const int64_t fsize = filestream_get_size(img->fp);
	const uint8_t* f = img->vhd_footer;
	uint8_t dyn[VHD_DYN_HDR_SIZE];
	uint32_t type, i;

	if (fsize < VHD_FOOTER_SIZE || !pread_at(img->fp, (uint64_t)fsize - VHD_FOOTER_SIZE, img->vhd_footer, VHD_FOOTER_SIZE)
		|| memcmp(f, "conectix", 8) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s has no VHD footer\n", path);
		return false;
	}
	if (!vhd_checksum_ok(f, VHD_FOOTER_SIZE, 64))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s: the VHD footer's checksum is wrong\n", path);
		return false;
	}
	img->vhd_footer_pos = (uint64_t)fsize - VHD_FOOTER_SIZE;
	img->size = be64(f + 48);
	type = be32(f + 60);

	if (type == VHD_TYPE_FIXED)
	{
		if (img->size > img->vhd_footer_pos)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: %s: a fixed VHD shorter than its disk\n", path);
			return false;
		}
		return true;
	}
	if (type != VHD_TYPE_DYNAMIC)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s: VHD disk type %u is not supported (fixed and dynamic are)\n",
			path, (unsigned)type);
		return false;
	}

	if (!pread_at(img->fp, be64(f + 16), dyn, sizeof(dyn)) || memcmp(dyn, "cxsparse", 8) != 0
		|| !vhd_checksum_ok(dyn, VHD_DYN_HDR_SIZE, 36))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s: no valid VHD dynamic header\n", path);
		return false;
	}
	img->vhd_dynamic = 1;
	img->vhd_bat_off = be64(dyn + 16);
	img->vhd_entries = be32(dyn + 28);
	img->vhd_block_size = be32(dyn + 32);
	if (img->vhd_block_size < 512 || (img->vhd_block_size & (img->vhd_block_size - 1))
		|| (uint64_t)img->vhd_entries * img->vhd_block_size < img->size)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s: the VHD's block table does not cover the disk\n", path);
		return false;
	}
	img->vhd_bitmap_bytes = ((img->vhd_block_size / 512 / 8) + 511) & ~511u;
	if (!(img->vhd_bat = (uint32_t*)malloc((size_t)img->vhd_entries * 4)))
		return false;
	if (!pread_at(img->fp, img->vhd_bat_off, img->vhd_bat, (uint64_t)img->vhd_entries * 4))
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s: cannot read the VHD's block table\n", path);
		return false;
	}
	for (i = 0; i < img->vhd_entries; i++)
		img->vhd_bat[i] = be32((const uint8_t*)&img->vhd_bat[i]);
	return true;
}

/* A dynamic disk's new block, where the footer is; false if the file
 * cannot take it. */
static bool vhd_allocate(hdd_image_t* img, uint32_t block)
{
	const uint64_t at = img->vhd_footer_pos;
	const uint64_t footer_at = at + img->vhd_bitmap_bytes + img->vhd_block_size;
	uint8_t entry[4];
	uint8_t* bitmap;
	bool ok;

	if (at % 512)
		return false;
	if (!(bitmap = (uint8_t*)malloc(img->vhd_bitmap_bytes)))
		return false;
	memset(bitmap, 0xFF, img->vhd_bitmap_bytes);
	/* The data between bitmap and footer is a hole: zeros. */
	ok = pwrite_at(img->fp, at, bitmap, img->vhd_bitmap_bytes) &&
		pwrite_at(img->fp, footer_at, img->vhd_footer, VHD_FOOTER_SIZE);
	free(bitmap);
	if (!ok)
		return false;
	img->vhd_footer_pos = footer_at;
	put_be32(entry, (uint32_t)(at / 512));
	if (!pwrite_at(img->fp, img->vhd_bat_off + (uint64_t)block * 4, entry, 4))
		return false;
	img->vhd_bat[block] = (uint32_t)(at / 512);
	return true;
}

static bool vhd_io(hdd_image_t* img, uint64_t offset, uint8_t* buf, uint32_t len, bool write)
{
	while (len)
	{
		const uint32_t block = (uint32_t)(offset / img->vhd_block_size);
		const uint32_t in = (uint32_t)(offset % img->vhd_block_size);
		const uint32_t n = (img->vhd_block_size - in) < len ? (img->vhd_block_size - in) : len;

		if (img->vhd_bat[block] == VHD_UNALLOCATED)
		{
			if (!write)
				memset(buf, 0, n);
			else if (!vhd_allocate(img, block))
				return false;
		}
		if (img->vhd_bat[block] != VHD_UNALLOCATED)
		{
			const uint64_t at = (uint64_t)img->vhd_bat[block] * 512 + img->vhd_bitmap_bytes + in;
			if (write ? !pwrite_at(img->fp, at, buf, n) : !pread_at(img->fp, at, buf, n))
				return false;
		}
		offset += n;
		buf += n;
		len -= n;
	}
	return true;
}

/* ---- CHD ----------------------------------------------------------------- */

/* Feeds the decoder what it asks for from the file until it is done. */
static int chd_drive(hdd_image_t* img, int (*step)(rchd_t*, rchd_request_t*))
{
	rchd_request_t req;
	for (;;)
	{
		int64_t got;
		size_t want;
		const int err = step(img->chd, &req);
		if (err != RCHD_PENDING)
			return err;
		if (req.source != RCHD_SOURCE_SELF)
			return RCHD_ERROR_NO_PARENT;
		want = req.length < CHD_FEED ? req.length : CHD_FEED;
		if (filestream_seek(img->fp, (int64_t)req.offset, RETRO_VFS_SEEK_POSITION_START) < 0
			|| (got = filestream_read(img->fp, img->feed, (int64_t)want)) <= 0)
			return RCHD_ERROR_DATA;
		if (rchd_feed(img->chd, img->feed, (size_t)got) < 0)
			return RCHD_ERROR_DATA;
	}
}

static bool chd_read_base(hdd_image_t* img, uint64_t offset, uint8_t* dst, uint32_t len)
{
	while (len)
	{
		const int64_t h = (int64_t)(offset / img->hunk_bytes);
		const uint32_t in = (uint32_t)(offset % img->hunk_bytes);
		const uint32_t n = (img->hunk_bytes - in) < len ? (img->hunk_bytes - in) : len;

		if (offset + n > img->size)
			return false;
		if (h != img->hunk_index)
		{
			img->hunk_index = -1;
			if (rchd_read_hunk_begin(img->chd, (uint32_t)h, img->hunk) != RCHD_OK
				|| chd_drive(img, rchd_read_step) != RCHD_OK)
				return false;
			img->hunk_index = h;
		}
		memcpy(dst, img->hunk + in, n);
		offset += n;
		dst += n;
		len -= n;
	}
	return true;
}

static bool ovl_present(const hdd_image_t* img, uint64_t block)
{
	return (img->ovl_map[block >> 3] >> (block & 7)) & 1;
}

/* The overlay of path, opened, or made when there is none; false when one
 * there belongs to another image. */
static bool ovl_open_at(hdd_image_t* img, const char* ovl_path)
{
	const rchd_info_t* info = rchd_info(img->chd);
	uint8_t hdr[OVL_HDR_SIZE];
	uint8_t have[OVL_HDR_SIZE];

	img->ovl_blocks = (img->size + OVL_BLOCK - 1) / OVL_BLOCK;
	img->ovl_map_bytes = (img->ovl_blocks + 7) / 8;
	img->ovl_data_off = (OVL_HDR_SIZE + img->ovl_map_bytes + OVL_BLOCK - 1) / OVL_BLOCK * OVL_BLOCK;
	if (!(img->ovl_map = (uint8_t*)calloc(1, (size_t)img->ovl_map_bytes + 1))
		|| !(img->ovl_block = (uint8_t*)malloc(OVL_BLOCK)))
		return false;

	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, OVL_MAGIC, 8);
	put_le32(hdr + 8, OVL_VERSION);
	put_le32(hdr + 12, OVL_BLOCK);
	put_le64(hdr + 16, img->size);
	memcpy(hdr + 24, info->sha1, 20);

	if (path_is_valid(ovl_path))
	{
		if (!(img->ovl = filestream_open(ovl_path, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING,
				RETRO_VFS_FILE_ACCESS_HINT_NONE))
			|| !pread_at(img->ovl, 0, have, sizeof(have)))
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: cannot read the overlay %s\n", ovl_path);
			return false;
		}
		if (memcmp(have, hdr, 44) != 0)
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: %s is the overlay of another image; move it away to start a new one\n", ovl_path);
			return false;
		}
		if (!pread_at(img->ovl, OVL_HDR_SIZE, img->ovl_map, img->ovl_map_bytes))
		{
			log_cb(RETRO_LOG_ERROR, "DEV9: the overlay %s is cut short\n", ovl_path);
			return false;
		}
		log_cb(RETRO_LOG_INFO, "DEV9: HDD writes go to %s\n", ovl_path);
		return true;
	}

	if (!(img->ovl = filestream_open(ovl_path, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE))
		|| !pwrite_at(img->ovl, 0, hdr, sizeof(hdr))
		|| !pwrite_at(img->ovl, OVL_HDR_SIZE, img->ovl_map, img->ovl_map_bytes)
		|| filestream_flush(img->ovl) != 0)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: cannot make the overlay %s\n", ovl_path);
		return false;
	}
	log_cb(RETRO_LOG_INFO, "DEV9: HDD writes go to a new overlay, %s\n", ovl_path);
	return true;
}

static bool ovl_open(hdd_image_t* img, const char* path)
{
	const size_t n = strlen(path) + sizeof(HDD_IMAGE_OVERLAY_SUFFIX);
	char* ovl_path = (char*)malloc(n);
	bool ok;

	if (!ovl_path)
		return false;
	strlcpy(ovl_path, path, n);
	strlcat(ovl_path, HDD_IMAGE_OVERLAY_SUFFIX, n);
	ok = ovl_open_at(img, ovl_path);
	free(ovl_path);
	return ok;
}

static bool chd_open(hdd_image_t* img, const char* path)
{
	const rchd_info_t* info;
	int err;

	if (!(img->chd = rchd_new()) || !(img->feed = (uint8_t*)malloc(CHD_FEED)))
		return false;
	if ((err = chd_drive(img, rchd_open_step)) != RCHD_OK)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s is not a CHD this core reads (%d)\n", path, err);
		return false;
	}
	info = rchd_info(img->chd);
	if (info->has_parent)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: %s needs a parent CHD; HDD images with one are not supported\n", path);
		return false;
	}
	img->size = info->logical_bytes & ~(uint64_t)511;
	img->hunk_bytes = info->hunk_bytes;
	img->hunk_index = -1;
	if (!img->size || !(img->hunk = (uint8_t*)malloc(img->hunk_bytes)))
		return false;
	return ovl_open(img, path);
}

static bool chd_read(hdd_image_t* img, uint64_t offset, uint8_t* dst, uint32_t len)
{
	while (len)
	{
		const uint64_t block = offset / OVL_BLOCK;
		const uint32_t in = (uint32_t)(offset % OVL_BLOCK);
		const uint32_t n = (OVL_BLOCK - in) < len ? (OVL_BLOCK - in) : len;

		if (ovl_present(img, block)
				? !pread_at(img->ovl, img->ovl_data_off + offset, dst, n)
				: !chd_read_base(img, offset, dst, n))
			return false;
		offset += n;
		dst += n;
		len -= n;
	}
	return true;
}

static bool chd_write(hdd_image_t* img, uint64_t offset, const uint8_t* src, uint32_t len)
{
	while (len)
	{
		const uint64_t block = offset / OVL_BLOCK;
		const uint32_t in = (uint32_t)(offset % OVL_BLOCK);
		const uint32_t n = (OVL_BLOCK - in) < len ? (OVL_BLOCK - in) : len;
		const uint64_t block_at = img->ovl_data_off + block * OVL_BLOCK;

		if (!ovl_present(img, block))
		{
			/* The rest of the block, from the CHD, written with it. */
			uint32_t have = OVL_BLOCK;
			if (block * OVL_BLOCK + have > img->size)
				have = (uint32_t)(img->size - block * OVL_BLOCK);
			memset(img->ovl_block, 0, OVL_BLOCK);
			if (n != have && !chd_read_base(img, block * OVL_BLOCK, img->ovl_block, have))
				return false;
			memcpy(img->ovl_block + in, src, n);
			if (!pwrite_at(img->ovl, block_at, img->ovl_block, have))
				return false;
			img->ovl_map[block >> 3] |= (uint8_t)(1u << (block & 7));
			if (img->ovl_dirty_lo == img->ovl_dirty_hi)
			{
				img->ovl_dirty_lo = block >> 3;
				img->ovl_dirty_hi = (block >> 3) + 1;
			}
			else
			{
				if ((block >> 3) < img->ovl_dirty_lo)
					img->ovl_dirty_lo = block >> 3;
				if ((block >> 3) + 1 > img->ovl_dirty_hi)
					img->ovl_dirty_hi = (block >> 3) + 1;
			}
		}
		else if (!pwrite_at(img->ovl, block_at + in, src, n))
			return false;
		offset += n;
		src += n;
		len -= n;
	}
	return true;
}

/* The map after the blocks it names: a block is never marked before it
 * is there. */
static bool chd_flush(hdd_image_t* img)
{
	if (filestream_flush(img->ovl) != 0)
		return false;
	if (img->ovl_dirty_lo != img->ovl_dirty_hi)
	{
		if (!pwrite_at(img->ovl, OVL_HDR_SIZE + img->ovl_dirty_lo, img->ovl_map + img->ovl_dirty_lo,
				img->ovl_dirty_hi - img->ovl_dirty_lo)
			|| filestream_flush(img->ovl) != 0)
			return false;
		img->ovl_dirty_lo = img->ovl_dirty_hi = 0;
	}
	return true;
}

/* ---- the image ----------------------------------------------------------- */

hdd_image_t* hdd_image_open(const char* path)
{
	hdd_image_t* img;
	const int kind = hdd_image_kind(path);
	bool ok;

	if (kind == HDD_IMAGE_RAW || !(img = (hdd_image_t*)calloc(1, sizeof(*img))))
		return NULL;
	img->kind = kind;
	img->fp = filestream_open(path,
		kind == HDD_IMAGE_VHD ? (RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING)
		                      : RETRO_VFS_FILE_ACCESS_READ,
		RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!img->fp)
	{
		log_cb(RETRO_LOG_ERROR, "DEV9: cannot open the HDD image %s\n", path);
		free(img);
		return NULL;
	}
	ok = kind == HDD_IMAGE_VHD ? vhd_open(img, path) : chd_open(img, path);
	if (!ok || (img->size % 512) != 0 || !img->size)
	{
		hdd_image_close(img);
		return NULL;
	}
	log_cb(RETRO_LOG_INFO, "DEV9: HDD image %s, %s, %u MiB\n", path,
		kind == HDD_IMAGE_CHD ? "CHD" : img->vhd_dynamic ? "dynamic VHD" : "fixed VHD",
		(unsigned)(img->size >> 20));
	return img;
}

void hdd_image_close(hdd_image_t* img)
{
	if (!img)
		return;
	if (img->ovl)
	{
		chd_flush(img);
		filestream_close(img->ovl);
	}
	if (img->chd)
		rchd_free(img->chd);
	if (img->fp)
		filestream_close(img->fp);
	free(img->vhd_bat);
	free(img->hunk);
	free(img->feed);
	free(img->ovl_map);
	free(img->ovl_block);
	free(img);
}

bool hdd_image_read(hdd_image_t* img, uint64_t offset, void* dst, uint32_t len)
{
	if (offset > img->size || len > img->size - offset)
		return false;
	if (img->kind == HDD_IMAGE_CHD)
		return chd_read(img, offset, (uint8_t*)dst, len);
	if (img->vhd_dynamic)
		return vhd_io(img, offset, (uint8_t*)dst, len, false);
	return pread_at(img->fp, offset, dst, len);
}

bool hdd_image_write(hdd_image_t* img, uint64_t offset, const void* src, uint32_t len)
{
	if (offset > img->size || len > img->size - offset)
		return false;
	if (img->kind == HDD_IMAGE_CHD)
		return chd_write(img, offset, (const uint8_t*)src, len);
	if (img->vhd_dynamic)
		return vhd_io(img, offset, (uint8_t*)src, len, true);
	return pwrite_at(img->fp, offset, src, len);
}

bool hdd_image_flush(hdd_image_t* img)
{
	if (img->kind == HDD_IMAGE_CHD)
		return chd_flush(img);
	return filestream_flush(img->fp) == 0;
}
