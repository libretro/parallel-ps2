/* The DEV9 HDD's VHD and CHD images (pcsx2/DEV9/ATA/HddImage.c), on
 * images written here from their formats (images.h):
 *
 *  - a fixed VHD reads its data and keeps what is written over it;
 *  - a dynamic VHD reads zeros where no block is, puts a block where its
 *    footer was when first written, moving the footer on and naming the
 *    block in its table, and reads it all back once reopened;
 *  - a CHD reads its hunks, holes as zeros, is never written, and keeps
 *    its writes in <image>.overlay, whole 4 KiB blocks made of the write
 *    and the CHD around it; they read back after a reopen, and an
 *    overlay of another image is refused rather than laid over this one;
 *  - accesses past the end, and differencing VHDs, are refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>

#include "DEV9/ATA/HddImage.h"
#include "images.h"

static void quiet(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = quiet;

static int failures;

static void check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static uint8_t pattern(uint64_t off)
{
	return (uint8_t)((off * 7 + (off >> 9) * 13 + 1) & 0xFF);
}

static long file_size(const char* path)
{
	FILE* f = fopen(path, "rb");
	long n;
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

static int read_file(const char* path, uint8_t** out, long* len)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	*out = (uint8_t*)malloc((size_t)*len + 1);
	if (!*out || fread(*out, 1, (size_t)*len, f) != (size_t)*len)
	{
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

static int image_is(hdd_image_t* img, const uint8_t* want, uint64_t size)
{
	static uint8_t buf[65536];
	uint64_t off;
	for (off = 0; off < size; off += sizeof(buf))
	{
		const uint32_t n = (uint32_t)((size - off) < sizeof(buf) ? (size - off) : sizeof(buf));
		if (!hdd_image_read(img, off, buf, n) || memcmp(buf, want + off, n) != 0)
			return 0;
	}
	return 1;
}

static void test_kind(void)
{
	check(hdd_image_kind("a/b/DEV9hdd.vhd") == HDD_IMAGE_VHD &&
		hdd_image_kind("DEV9HDD.CHD") == HDD_IMAGE_CHD &&
		hdd_image_kind("DEV9hdd.raw") == HDD_IMAGE_RAW &&
		hdd_image_kind("noext") == HDD_IMAGE_RAW, "kind by extension: .vhd, .chd, anything else raw");
	check(hdd_image_open("DEV9hdd.raw") == NULL, "a raw image is not opened here");
}

static void test_fixed_vhd(const char* dir)
{
	const uint64_t size = 64 * 1024;
	char path[1024];
	uint8_t* data = (uint8_t*)malloc((size_t)size);
	uint8_t sec[1024];
	hdd_image_t* img;
	uint64_t i;

	for (i = 0; i < size; i++)
		data[i] = pattern(i);
	sprintf(path, "%s/fixed.vhd", dir);
	img_write_fixed_vhd(path, data, size);

	img = hdd_image_open(path);
	check(img && hdd_image_size(img) == size, "fixed VHD: opens, the size its footer gives");
	if (!img)
		return;
	check(image_is(img, data, size), "fixed VHD: reads its data");
	memset(sec, 0xA5, sizeof(sec));
	memcpy(data + 3 * 512, sec, sizeof(sec));
	check(hdd_image_write(img, 3 * 512, sec, sizeof(sec)) && hdd_image_flush(img), "fixed VHD: takes a write");
	check(!hdd_image_read(img, size - 512, sec, 1024) && !hdd_image_write(img, size, sec, 512),
		"fixed VHD: refuses an access past the end");
	hdd_image_close(img);
	check(file_size(path) == (long)(size + 512), "fixed VHD: the file stays its data and footer");

	img = hdd_image_open(path);
	check(img && image_is(img, data, size), "fixed VHD: the write is there after a reopen");
	hdd_image_close(img);
	free(data);
}

static void test_dynamic_vhd(const char* dir)
{
	const uint64_t size = 256 * 1024;
	const uint32_t block = 32 * 1024;
	char path[1024];
	uint8_t* want = (uint8_t*)calloc(1, (size_t)size);
	uint8_t buf[3 * 512];
	uint8_t* file;
	long len, before;
	hdd_image_t* img;
	uint32_t i;

	sprintf(path, "%s/dynamic.vhd", dir);
	img_write_dynamic_vhd(path, size, block);
	before = file_size(path);

	img = hdd_image_open(path);
	check(img && hdd_image_size(img) == size, "dynamic VHD: opens, the size its footer gives");
	if (!img)
		return;
	check(image_is(img, want, size), "dynamic VHD: no block yet, all zeros");

	/* Into block 2, then across blocks 4 and 5. */
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)(i * 3 + 1);
	memcpy(want + 2 * block + 1024, buf, sizeof(buf));
	memcpy(want + 5 * block - 512, buf, sizeof(buf));
	check(hdd_image_write(img, 2 * block + 1024, buf, sizeof(buf)) &&
		hdd_image_write(img, 5 * block - 512, buf, sizeof(buf)) && hdd_image_flush(img),
		"dynamic VHD: takes writes, one across two blocks");
	check(image_is(img, want, size), "dynamic VHD: reads them back, zeros around them");
	hdd_image_close(img);

	if (!read_file(path, &file, &len))
	{
		check(0, "dynamic VHD: the file reads back");
		return;
	}
	{
		/* Three blocks, each a sector of bitmap and the block, where the
		 * footer was; the footer at the end again; the table names them
		 * in the order they were written. */
		const long grown = 3 * (512 + (long)block);
		const uint32_t first = (uint32_t)((before - 512) / 512);
		const uint8_t* bat = file + 512 + 1024;
		check(len == before + grown && memcmp(file + len - 512, file, 512) == 0,
			"dynamic VHD: three blocks put where the footer was, the footer after them");
		check(img_rd_be32(bat + 2 * 4) == first && img_rd_be32(bat + 4 * 4) == first + 1 + block / 512 &&
			img_rd_be32(bat + 5 * 4) == first + 2 * (1 + block / 512) && img_rd_be32(bat + 0) == 0xFFFFFFFFu,
			"dynamic VHD: the table names the blocks written and only those");
		check(file[(long)first * 512] == 0xFF && file[(long)first * 512 + 511] == 0xFF,
			"dynamic VHD: a new block's sector bitmap is all set");
		free(file);
	}

	img = hdd_image_open(path);
	check(img && image_is(img, want, size), "dynamic VHD: the writes are there after a reopen");
	if (img)
	{
		/* A block already there takes a write in place. */
		memset(buf, 0x77, sizeof(buf));
		memcpy(want + 2 * block, buf, sizeof(buf));
		hdd_image_write(img, 2 * block, buf, sizeof(buf));
		hdd_image_close(img);
		check(file_size(path) == before + 3 * (512 + (long)block),
			"dynamic VHD: a write to a block it has does not grow the file");
		img = hdd_image_open(path);
		check(img && image_is(img, want, size), "dynamic VHD: and reads back");
		hdd_image_close(img);
	}
	free(want);

	/* A differencing disk needs a parent: refused. */
	sprintf(path, "%s/diff.vhd", dir);
	{
		uint8_t f[512];
		FILE* fp = fopen(path, "wb");
		img_vhd_footer(f, 512 * 16, 4, 512);
		fwrite(f, 1, 512, fp);
		fwrite(f, 1, 512, fp);
		fclose(fp);
		check(hdd_image_open(path) == NULL, "differencing VHD: refused");
	}
}

static void test_chd(const char* dir)
{
	const uint64_t size = 96 * 1024;
	const uint32_t hunk = 8192;
	char path[1024], ovl[1100], other[1024];
	uint8_t* data = (uint8_t*)calloc(1, (size_t)size);
	uint8_t* want = (uint8_t*)malloc((size_t)size);
	uint8_t* chd_before;
	uint8_t* chd_after;
	long chd_len, chd_len2;
	uint8_t sec[512 * 3];
	hdd_image_t* img;
	uint64_t i;

	/* Hunks 0-3 and 6-11 hold data; 4 and 5 are holes. */
	for (i = 0; i < size; i++)
		if (i < 4 * hunk || i >= 6 * hunk)
			data[i] = pattern(i);
	memcpy(want, data, (size_t)size);
	sprintf(path, "%s/disk.chd", dir);
	sprintf(ovl, "%s/disk.chd" HDD_IMAGE_OVERLAY_SUFFIX, dir);
	remove(ovl);
	img_write_chd(path, data, size, hunk, 1);
	read_file(path, &chd_before, &chd_len);

	img = hdd_image_open(path);
	check(img && hdd_image_size(img) == size, "CHD: opens, the size of its disk");
	if (!img)
		return;
	check(image_is(img, want, size), "CHD: reads its hunks, holes as zeros");

	/* A sector inside a block, three across a block edge, one into a
	 * hole. */
	memset(sec, 0xC3, sizeof(sec));
	memcpy(want + 4096 + 512, sec, 512);
	memcpy(want + 3 * 4096 - 512, sec, sizeof(sec));
	memcpy(want + 4 * hunk + 1024, sec, 512);
	check(hdd_image_write(img, 4096 + 512, sec, 512) &&
		hdd_image_write(img, 3 * 4096 - 512, sec, sizeof(sec)) &&
		hdd_image_write(img, 4 * hunk + 1024, sec, 512) && hdd_image_flush(img),
		"CHD: takes writes");
	check(image_is(img, want, size), "CHD: reads them back over the CHD around them");
	check(!hdd_image_write(img, size, sec, 512), "CHD: refuses a write past the end");
	hdd_image_close(img);

	read_file(path, &chd_after, &chd_len2);
	check(chd_len == chd_len2 && memcmp(chd_before, chd_after, (size_t)chd_len) == 0,
		"CHD: the image itself is not written");
	free(chd_before);
	free(chd_after);
	check(file_size(ovl) > 0, "CHD: the writes are in <image>.overlay");

	img = hdd_image_open(path);
	check(img && image_is(img, want, size), "CHD: the writes are there after a reopen");
	hdd_image_close(img);

	/* The overlay of another image: refused, and left alone. */
	sprintf(other, "%s/other.chd", dir);
	img_write_chd(other, data, size, hunk, 2);
	{
		char other_ovl[1100];
		uint8_t* o;
		long olen, olen2;
		uint8_t* o2;
		sprintf(other_ovl, "%s/other.chd" HDD_IMAGE_OVERLAY_SUFFIX, dir);
		remove(other_ovl);
		read_file(ovl, &o, &olen);
		{
			FILE* f = fopen(other_ovl, "wb");
			fwrite(o, 1, (size_t)olen, f);
			fclose(f);
		}
		check(hdd_image_open(other) == NULL, "CHD: an overlay made for another image is refused");
		read_file(other_ovl, &o2, &olen2);
		check(olen == olen2 && memcmp(o, o2, (size_t)olen) == 0, "CHD: and is left as it was");
		free(o);
		free(o2);
		remove(other_ovl);
	}
	remove(other);
	remove(ovl);
	free(data);
	free(want);
}

/* For build.sh's checks against other tools' images: "cat <image>
 * <raw>" copies the disk out, "write <image> <offset> <len> <first>"
 * writes len bytes counting up from first. */
static int tool(int argc, char** argv)
{
	static uint8_t buf[65536];
	hdd_image_t* img;
	uint64_t off, size;
	int ok = 1;

	if (argc < 4 || !(img = hdd_image_open(argv[2])))
		return 2;
	size = hdd_image_size(img);
	if (!strcmp(argv[1], "cat"))
	{
		FILE* f = fopen(argv[3], "wb");
		for (off = 0; f && ok && off < size; off += sizeof(buf))
		{
			const uint32_t n = (uint32_t)((size - off) < sizeof(buf) ? (size - off) : sizeof(buf));
			ok = hdd_image_read(img, off, buf, n) && fwrite(buf, 1, n, f) == n;
		}
		ok = f && fclose(f) == 0 && ok;
	}
	else if (argc >= 6)
	{
		const uint32_t n = (uint32_t)strtoul(argv[4], NULL, 0);
		const unsigned first = (unsigned)strtoul(argv[5], NULL, 0);
		uint8_t* p = (uint8_t*)malloc(n);
		uint32_t i;
		for (i = 0; p && i < n; i++)
			p[i] = (uint8_t)(first + i);
		ok = p && hdd_image_write(img, (uint64_t)strtoul(argv[3], NULL, 0), p, n) && hdd_image_flush(img);
		free(p);
	}
	hdd_image_close(img);
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	const char* dir = argc > 1 ? argv[1] : ".";

	if (argc > 1 && (!strcmp(argv[1], "cat") || !strcmp(argv[1], "write")))
		return tool(argc, argv);

	test_kind();
	test_fixed_vhd(dir);
	test_dynamic_vhd(dir);
	test_chd(dir);
	printf(failures ? "hddimage: %d failure(s)\n" : "hddimage: ok\n", failures);
	return failures != 0;
}
