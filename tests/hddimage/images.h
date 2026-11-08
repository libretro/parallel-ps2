/* Writers of the HDD image formats pcsx2/DEV9/ATA/HddImage.c reads, for
 * the tests: VHD after Microsoft's Virtual Hard Disk Image Format
 * Specification, and an uncompressed version 5 CHD (a flat map of hunk
 * indices, zero for a hunk that reads as zeros). Written here from the
 * formats, not by the code under test. C89. */
#ifndef TESTS_HDDIMAGE_IMAGES_H
#define TESTS_HDDIMAGE_IMAGES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void img_be32(uint8_t* p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static void img_be64(uint8_t* p, uint64_t v)
{
	img_be32(p, (uint32_t)(v >> 32));
	img_be32(p + 4, (uint32_t)v);
}

static uint32_t img_rd_be32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* One's complement of the byte sum, the checksum field counted as zero. */
static void img_vhd_checksum(uint8_t* p, int len, int at)
{
	uint32_t sum = 0;
	int i;
	memset(p + at, 0, 4);
	for (i = 0; i < len; i++)
		sum += p[i];
	img_be32(p + at, ~sum);
}

static void img_vhd_footer(uint8_t f[512], uint64_t size, uint32_t type, uint64_t data_offset)
{
	memset(f, 0, 512);
	memcpy(f, "conectix", 8);
	img_be32(f + 8, 2);                /* features: reserved bit */
	img_be32(f + 12, 0x00010000u);
	img_be64(f + 16, data_offset);
	memcpy(f + 28, "lrp2", 4);
	img_be32(f + 32, 0x00010000u);
	memcpy(f + 36, "Wi2k", 4);
	img_be64(f + 40, size);
	img_be64(f + 48, size);
	img_be32(f + 56, (uint32_t)(((size / 512 / 63 / 16) & 0xFFFF) << 16 | 16u << 8 | 63u));
	img_be32(f + 60, type);
	memcpy(f + 68, "lrps2-test-uuid!", 16);
	img_vhd_checksum(f, 512, 64);
}

/* data (size bytes) then the footer. */
static int img_write_fixed_vhd(const char* path, const uint8_t* data, uint64_t size)
{
	uint8_t f[512];
	FILE* fp = fopen(path, "wb");
	int ok;
	if (!fp)
		return 0;
	img_vhd_footer(f, size, 2, 0xFFFFFFFFFFFFFFFFull);
	ok = fwrite(data, 1, (size_t)size, fp) == size && fwrite(f, 1, 512, fp) == 512;
	return fclose(fp) == 0 && ok;
}

/* Footer copy, dynamic header, a block table with nothing allocated,
 * footer. */
static int img_write_dynamic_vhd(const char* path, uint64_t size, uint32_t block_size)
{
	const uint32_t entries = (uint32_t)((size + block_size - 1) / block_size);
	const uint32_t bat_bytes = (entries * 4 + 511) & ~511u;
	uint8_t f[512], h[1024];
	uint8_t* bat = (uint8_t*)malloc(bat_bytes);
	FILE* fp = fopen(path, "wb");
	int ok;
	if (!fp || !bat)
	{
		if (fp)
			fclose(fp);
		free(bat);
		return 0;
	}
	img_vhd_footer(f, size, 3, 512);
	memset(h, 0, sizeof(h));
	memcpy(h, "cxsparse", 8);
	img_be64(h + 8, 0xFFFFFFFFFFFFFFFFull);
	img_be64(h + 16, 512 + 1024);
	img_be32(h + 24, 0x00010000u);
	img_be32(h + 28, entries);
	img_be32(h + 32, block_size);
	img_vhd_checksum(h, 1024, 36);
	memset(bat, 0xFF, bat_bytes);
	ok = fwrite(f, 1, 512, fp) == 512 && fwrite(h, 1, 1024, fp) == 1024 &&
		fwrite(bat, 1, bat_bytes, fp) == bat_bytes && fwrite(f, 1, 512, fp) == 512;
	free(bat);
	return fclose(fp) == 0 && ok;
}

/* An uncompressed version 5 CHD of data (size bytes; NULL for zeros).
 * A hunk all zeros is a hole in the map, stored nowhere. tag goes into
 * the header's SHA-1, so two images can be told apart. */
static int img_write_chd(const char* path, const uint8_t* data, uint64_t size, uint32_t hunk, uint8_t tag)
{
	const uint32_t hunks = (uint32_t)((size + hunk - 1) / hunk);
	const uint64_t map_at = 124;
	uint8_t h[124];
	uint8_t* map = (uint8_t*)calloc(hunks, 4);
	uint8_t* buf = (uint8_t*)calloc(1, hunk);
	uint64_t at;
	uint32_t i, next;
	FILE* fp = fopen(path, "wb");
	int ok = 1;

	if (!fp || !map || !buf)
	{
		if (fp)
			fclose(fp);
		free(map);
		free(buf);
		return 0;
	}
	memset(h, 0, sizeof(h));
	memcpy(h, "MComprHD", 8);
	img_be32(h + 8, 124);
	img_be32(h + 12, 5);
	img_be64(h + 32, size);
	img_be64(h + 40, map_at);
	img_be64(h + 48, 0);
	img_be32(h + 56, hunk);
	img_be32(h + 60, 512);
	h[84] = tag;
	h[85] = 0x5a;

	/* Hunks start at the first hunk boundary past the map. */
	next = (uint32_t)((map_at + (uint64_t)hunks * 4 + hunk - 1) / hunk);
	for (i = 0; i < hunks; i++)
	{
		const uint64_t off = (uint64_t)i * hunk;
		const uint32_t n = (uint32_t)((size - off) < hunk ? (size - off) : hunk);
		uint32_t j, any = 0;
		if (data)
			for (j = 0; j < n; j++)
				any |= data[off + j];
		if (any)
			img_be32(map + 4 * i, next++);
	}
	ok = fwrite(h, 1, 124, fp) == 124 && fwrite(map, 1, (size_t)hunks * 4, fp) == (size_t)hunks * 4;
	for (i = 0; ok && i < hunks; i++)
	{
		const uint32_t idx = img_rd_be32(map + 4 * i);
		const uint64_t off = (uint64_t)i * hunk;
		const uint32_t n = (uint32_t)((size - off) < hunk ? (size - off) : hunk);
		if (!idx)
			continue;
		memset(buf, 0, hunk);
		memcpy(buf, data + off, n);
		at = (uint64_t)idx * hunk;
		ok = fseek(fp, (long)at, SEEK_SET) == 0 && fwrite(buf, 1, hunk, fp) == hunk;
	}
	free(map);
	free(buf);
	return fclose(fp) == 0 && ok;
}

#endif
