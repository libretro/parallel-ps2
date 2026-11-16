/* The CDVD image readers hand back exactly the bytes the image holds. C89.
 *
 * Every compressed format serves sectors out of the reader's chunk cache:
 * a sector read produces the chunk containing it, and later sectors in
 * that chunk come from the cache. That is where the interesting failures
 * live - a stale chunk, a sector served from the wrong one, a boundary
 * crossing that takes half from each - and none of them crash. They hand
 * back plausible wrong data, which a game sees as a corrupt disc.
 *
 * The drive's read lends the sector where the reader holds it, and hands
 * it over later: a read in between must not change what is handed over.
 *
 *   - a flat image, mapped and served directly, and the drive's split
 *     read on it, its modes and a read past the end;
 *   - a gzip whose quick-access index holds values no reader writes;
 *   - CSO (python3) and CHD, DVD and CD (chdman), through the cache:
 *     forwards, backwards, alternating ends, three interleaved streams,
 *     a read between the drive's fetch and its hand-over, and readSector's
 *     part-sector modes through CDVDapi_Iso;
 *   - a CSO whose index runs backwards, through a VFS without mappings;
 *   - the read-ahead hints.
 * Without python3 or chdman the formats they make are skipped and said
 * so. Usage: cdvd_read <scratch dir> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <libretro.h>
#include <encodings/crc32.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

#include "CDVD/cdvd_iso.h"
#include "CDVD/cdvd_reader.h"
#include "CDVD/zlib_indexed.h"
#include "CDVD/CDVDcommon.h"

#define SECTOR   2048
#define SECTORS  512

static void quiet_log(enum retro_log_level level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = quiet_log;

static int s_ok = 1;

/* Deterministic, and compressible in parts and not in others, so the CSO
 * writer both compresses and stores frames. */
static void fill(u8 *p, unsigned lsn)
{
	unsigned i;
	for (i = 0; i < SECTOR; i++)
	{
		if ((lsn & 3) == 0)
			p[i] = (u8)(lsn & 0xFF);
		else
			p[i] = (u8)((lsn * 2654435761u + i * 40503u) >> 13);
	}
}

static int same(const u8 *got, const u8 *image, unsigned lsn)
{
	return got && !memcmp(got, image + (size_t)lsn * SECTOR, SECTOR);
}

/* The sector at lsn through cdvd_iso_read, which places it in a raw
 * frame - at the block offset the detector chose - so its 2048 data
 * bytes start 24 bytes in whatever the image's sector size. */
static int read_ok(cdvd_iso_t *in, const u8 *image, unsigned lsn)
{
	u8 got[2456];
	return cdvd_iso_read(in, got, lsn) >= 0 && same(got + 24, image, lsn);
}

/* The drive's read: fetch, then hand over the 2048-byte data. */
static int drive_ok(cdvd_iso_t *in, const u8 *image, unsigned lsn)
{
	u8 got[2456];
	cdvd_iso_begin_read(in, lsn);
	return cdvd_iso_finish_read(in, got, CDVD_MODE_2048) == 0 && same(got, image, lsn);
}

/* The VFS a frontend hands the core, over libretro-common's own files:
 * each of the interface's own type (a call through a pointer of another
 * type is undefined, so the impls are wrapped rather than cast) */
typedef libretro_vfs_implementation_file vfs_file;
static const char *RETRO_CALLCONV vfs_get_path(struct retro_vfs_file_handle *f) { return retro_vfs_file_get_path_impl((vfs_file *)f); }
static struct retro_vfs_file_handle *RETRO_CALLCONV vfs_open(const char *path, unsigned mode, unsigned hints)
{
	return (struct retro_vfs_file_handle *)retro_vfs_file_open_impl(path, mode, hints);
}
static int RETRO_CALLCONV vfs_close(struct retro_vfs_file_handle *f) { return retro_vfs_file_close_impl((vfs_file *)f); }
static int64_t RETRO_CALLCONV vfs_size(struct retro_vfs_file_handle *f) { return retro_vfs_file_size_impl((vfs_file *)f); }
static int64_t RETRO_CALLCONV vfs_tell(struct retro_vfs_file_handle *f) { return retro_vfs_file_tell_impl((vfs_file *)f); }
static int64_t RETRO_CALLCONV vfs_seek(struct retro_vfs_file_handle *f, int64_t offset, int whence)
{
	return retro_vfs_file_seek_impl((vfs_file *)f, offset, whence);
}
static int64_t RETRO_CALLCONV vfs_read(struct retro_vfs_file_handle *f, void *s, uint64_t len)
{
	return retro_vfs_file_read_impl((vfs_file *)f, s, len);
}

static void report(int good, const char *what, const char *how)
{
	printf("  %s: %s -- %s\n", good ? "ok" : "FAIL", what, how);
	if (!good)
		s_ok = 0;
}

static void check_pass(cdvd_iso_t *in, const u8 *image, const char *what)
{
	unsigned lsn;
	int good = 1;

	for (lsn = 0; good && lsn < SECTORS; lsn++)
		good = read_ok(in, image, lsn);
	report(good, what, "sectors in order");

	/* Backwards, so every read misses the chunk the last one used. */
	for (lsn = SECTORS, good = 1; good && lsn-- > 0; )
		good = read_ok(in, image, lsn);
	report(good, what, "sectors backwards");

	/* Two ends alternating: each evicts the other's chunk unless the
	 * cache keeps both, and a chunk reused without its offset checked
	 * hands back the wrong one. */
	for (lsn = 0, good = 1; good && lsn < 64; lsn++)
		good = read_ok(in, image, lsn) && read_ok(in, image, SECTORS - 1 - lsn);
	report(good, what, "two ends alternating");

	/* Three streams in turn, as a game reading a movie, its audio and
	 * its data does. */
	for (lsn = 0, good = 1; good && lsn < 120; lsn++)
		good = drive_ok(in, image, lsn) && drive_ok(in, image, 170 + lsn)
			&& drive_ok(in, image, 340 + lsn);
	report(good, what, "three interleaved streams through the drive");

	/* The drive fetches a sector and hands it over later; reads of other
	 * sectors in between, far enough to evict every chunk, must not
	 * change what it hands over. */
	for (lsn = 0, good = 1; good && lsn < SECTORS; lsn += 37)
	{
		u8 got[2456];
		unsigned k;
		cdvd_iso_begin_read(in, lsn);
		for (k = 0; good && k < 2 * CDVD_CACHE_SLOTS; k++)
			good = read_ok(in, image, (lsn + 61 * (k + 1)) % SECTORS)
				&& cdvd_iso_lend(in, (lsn + 29 * (k + 1)) % SECTORS) != NULL;
		good = good && cdvd_iso_finish_read(in, got, CDVD_MODE_2048) == 0
			&& same(got, image, lsn);
	}
	report(good, what, "a fetched sector survives reads before its hand-over");
}

/* readSector's modes through the API the drive uses: a part-sector is
 * the raw frame's bytes from its offset. */
static void check_read_sector(const char *path, const u8 *image, const char *what)
{
	u8 got[2456];
	unsigned lsn;
	int good;

	cdvd_iso_set_gz_index("$(f).pindex.tmp", "");
	good = CDVDapi_Iso.open(path) == 0;
	for (lsn = 0; good && lsn < SECTORS; lsn += 13)
	{
		memset(got, 0xee, sizeof(got));
		good = CDVDapi_Iso.readSector(got, lsn, CDVD_MODE_2048) == 0 && same(got, image, lsn)
			&& CDVDapi_Iso.readSector(got, lsn, CDVD_MODE_2328) == 0 && same(got, image, lsn);
	}
	if (good)
	{
		/* Two sectors fetched by readTrack and a readSector in between:
		 * the fetched one is what getBuffer hands over. */
		CDVDapi_Iso.readTrack(7, CDVD_MODE_2048);
		CDVDapi_Iso.readSector(got, 300, CDVD_MODE_2048);
		good = CDVDapi_Iso.getBuffer(got) == 0 && same(got, image, 7);
	}
	CDVDapi_Iso.close();
	report(good, what, "readSector's modes, and a readSector inside a drive read");
}

static int open_iso(cdvd_iso_t *in, const char *path)
{
	cdvd_iso_init(in);
	return cdvd_iso_open(in, path, "$(f).pindex.tmp", "");
}

static void put32(u8 *p, u32 v)
{
	p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* A gzip of the image in stored deflate blocks: no compressor needed. */
static int write_gz(const char *path, const u8 *data, size_t len)
{
	static const u8 head[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
	u8 hdr[5], tail[8];
	size_t done = 0;
	u32 crc = encoding_crc32(0, data, len);
	FILE *f = fopen(path, "wb");
	if (!f || fwrite(head, 1, sizeof(head), f) != sizeof(head))
		return 0;
	while (done < len)
	{
		const size_t n = (len - done) > 65535 ? 65535 : (len - done);
		hdr[0] = (u8)(done + n == len ? 1 : 0);
		hdr[1] = (u8)n; hdr[2] = (u8)(n >> 8);
		hdr[3] = (u8)~n; hdr[4] = (u8)(~n >> 8);
		if (fwrite(hdr, 1, 5, f) != 5 || fwrite(data + done, 1, n, f) != n)
			return 0;
		done += n;
	}
	put32(tail, crc);
	put32(tail + 4, (u32)len);
	if (fwrite(tail, 1, 8, f) != 8)
		return 0;
	return fclose(f) == 0;
}

/* A quick-access index beside the gzip, as the reader writes it, with
 * the span and size given. */
static int write_gz_index(const char *path, s32 span, s64 size)
{
	static const char id[] = "PCSX2.index.gzip.v1|";
	Access a;
	Point *pt = (Point*)calloc(1, sizeof(Point));
	FILE *f = fopen(path, "wb");
	int ok;
	memset(&a, 0, sizeof(a));
	a.have = 1;
	a.size = 1;
	a.span = span;
	a.uncompressed_size = size;
	ok = f && pt && fwrite(id, 1, sizeof(id) - 1, f) == sizeof(id) - 1
		&& fwrite(&a, sizeof(a), 1, f) == 1 && fwrite(pt, sizeof(*pt), 1, f) == 1;
	free(pt);
	if (f && fclose(f) != 0)
		ok = 0;
	return ok;
}

/* A CSO whose index runs backwards at its first frame, with 512 KB of
 * payload after it, read through a VFS without mappings - the way a
 * frontend VFS serves it, staging frames in the read buffer. The frame is
 * refused; nothing past the buffer is written. */
static void check_backwards_cso(const char *path)
{
	static struct retro_vfs_interface iface;
	struct retro_vfs_interface_info info;
	const size_t payload = 512 * 1024;
	const u32 frames = 4;
	const size_t index_at = 24, payload_at = 24 + (frames + 1) * 4;
	u8 *file = (u8*)calloc(1, payload_at + payload);
	u8 sector[SECTOR];
	cdvd_reader_t *r;
	int good = 1, got;
	FILE *f;

	if (!file)
	{
		report(0, "damaged CSO", "out of memory");
		return;
	}
	memcpy(file, "CISO", 4);
	put32(file + 4, 24);
	put32(file + 8, frames * SECTOR);
	put32(file + 12, 0);
	put32(file + 16, SECTOR);
	file[20] = 1;
	put32(file + index_at, (u32)payload_at);
	put32(file + index_at + 4, 1);
	put32(file + index_at + 8, (u32)(payload_at + payload));
	put32(file + index_at + 12, (u32)(payload_at + payload));
	put32(file + index_at + 16, (u32)(payload_at + payload));
	f = fopen(path, "wb");
	if (!f || fwrite(file, 1, payload_at + payload, f) != payload_at + payload)
	{
		free(file);
		report(0, "damaged CSO", "could not write it");
		return;
	}
	fclose(f);
	free(file);

	iface.get_path = vfs_get_path;
	iface.open     = vfs_open;
	iface.close    = vfs_close;
	iface.size     = vfs_size;
	iface.tell     = vfs_tell;
	iface.seek     = vfs_seek;
	iface.read     = vfs_read;
	info.required_interface_version = FILESTREAM_REQUIRED_VFS_VERSION;
	info.iface = &iface;
	filestream_vfs_init(&info);
	if (!(r = cdvd_reader_open(path, NULL, NULL)))
		good = 0;
	else
	{
		got = cdvd_reader_read(r, sector, 0, 1);
		good = got <= 0;
		cdvd_reader_free(r);
	}
	info.iface = NULL;
	filestream_vfs_init(&info);
	remove(path);
	report(good, "a CSO frame whose index runs backwards", "refused");
}

/* The mapped readers hint ahead of a reader that goes on in order; jumps
 * hint nothing. */
static void check_readahead(void)
{
	const u64 A = CDVD_RA_AHEAD;
	cdvd_readahead_t ra;
	int good = 1, hints = 0;
	u64 pos;

	memset(&ra, 0, sizeof(ra));
	good &= cdvd_readahead_step(&ra, 0, 2048);          /* first read in order */
	good &= !cdvd_readahead_step(&ra, 2048, 2048);      /* inside the hinted span */
	for (pos = 4096; pos < A; pos += 2048)
		hints += cdvd_readahead_step(&ra, pos, 2048);
	good &= hints == 1;                                 /* once more, half way */
	good &= !cdvd_readahead_step(&ra, 100 * A, 2048);   /* a jump */
	good &= cdvd_readahead_step(&ra, 100 * A + 2048, 2048); /* in order from there */
	pos = 100 * A + 4096 + CDVD_RA_GAP / 2;
	good &= !cdvd_readahead_step(&ra, pos, 2048);       /* a small skip is no jump, */
	good &= !cdvd_readahead_step(&ra, pos + 2048, 2048); /* so nothing hints yet */
	good &= !cdvd_readahead_step(&ra, 0, 2048);         /* back to the start */
	report(good, "read-ahead hints", "follow a reader in order");
}

static void check_image(const char *path, const u8 *image, const char *what)
{
	cdvd_iso_t in;
	if (!open_iso(&in, path))
	{
		report(0, what, "does not open");
		return;
	}
	if (in.blocks < SECTORS)
		report(0, what, "too few sectors");
	else
	{
		check_pass(&in, image, what);
		cdvd_iso_close(&in);
		check_read_sector(path, image, what);
	}
	cdvd_iso_close(&in);
}

/* A raw MODE2 CD image of the same data, for chdman createcd. */
static int write_cd(const char *bin, const char *cue, const u8 *image)
{
	u8 frame[2352];
	unsigned lsn;
	FILE *f = fopen(bin, "wb");
	if (!f)
		return 0;
	for (lsn = 0; lsn < SECTORS; lsn++)
	{
		memset(frame, 0, sizeof(frame));
		memset(frame + 1, 0xff, 10);
		frame[15] = 2;
		memcpy(frame + 24, image + (size_t)lsn * SECTOR, SECTOR);
		if (fwrite(frame, 1, sizeof(frame), f) != sizeof(frame))
			return 0;
	}
	if (fclose(f) != 0 || !(f = fopen(cue, "w")))
		return 0;
	/* by its leaf name: chdman looks beside the cue */
	fprintf(f, "FILE \"%s\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n",
			strrchr(bin, '/') ? strrchr(bin, '/') + 1 : bin);
	return fclose(f) == 0;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char iso[1024], cso[1100], gz[1100], idx[1200], chd[1100], cdbin[1100], cdcue[1100], cdchd[1100];
	char cmd[4200];
	u8 *image = (u8*)malloc(SECTOR * SECTORS);
	unsigned lsn;
	FILE *f;

	setvbuf(stdout, NULL, _IONBF, 0);
	printf("cdvd read\n");
	/* Absolute paths: a gzip's index is placed beside it only then. */
	sprintf(iso, "%s/cdvdread.iso", dir);
	sprintf(cso, "%s/cdvdread.cso", dir);
	sprintf(gz, "%s/cdvdread.iso.gz", dir);
	sprintf(idx, "%s.pindex.tmp", gz);
	sprintf(chd, "%s/cdvdread_dvd.chd", dir);
	sprintf(cdbin, "%s/cdvdread_cd.bin", dir);
	sprintf(cdcue, "%s/cdvdread_cd.cue", dir);
	sprintf(cdchd, "%s/cdvdread_cd.chd", dir);

	for (lsn = 0; lsn < SECTORS; lsn++)
		fill(image + (size_t)lsn * SECTOR, lsn);
	/* Sector 16 is the primary volume descriptor the geometry is detected
	 * from: "CD001" at byte 1 and the logical block size at 128. */
	{
		u8 *pvd = image + (size_t)16 * SECTOR;
		memset(pvd, 0, SECTOR);
		pvd[0] = 1;
		memcpy(pvd + 1, "CD001", 5);
		pvd[6] = 1;
		pvd[128] = (u8)(SECTOR & 0xFF);
		pvd[129] = (u8)((SECTOR >> 8) & 0xFF);
		pvd[130] = (u8)((SECTOR >> 8) & 0xFF);
		pvd[131] = (u8)(SECTOR & 0xFF);
	}
	f = fopen(iso, "wb");
	if (!f || fwrite(image, SECTOR, SECTORS, f) != SECTORS)
	{
		printf("  FAIL: could not write %s\n", iso);
		return 1;
	}
	fclose(f);

	/* Flat: served from the mapping. */
	{
		cdvd_iso_t in;
		u8 got[2456];
		int good, i;
		if (!open_iso(&in, iso))
		{
			printf("  FAIL: Open\n");
			return 1;
		}
		printf("  opened %u sectors, block offset %d\n", (unsigned)in.blocks, (int)in.blockofs);
		check_pass(&in, image, "flat, served directly");

		/* The same sector twice: the held result is still the right one. */
		for (lsn = 0, good = 1; good && lsn < SECTORS; lsn += 7)
			good = drive_ok(&in, image, lsn) && drive_ok(&in, image, lsn);
		report(good, "flat", "the drive's read, a sector read twice");

		/* The 2368-byte mode is the raw 2352: on a 2048-byte image the 24
		 * bytes before the data are zero and the data follows. A mode the
		 * reader does not know is an error. */
		memset(got, 0xee, sizeof(got));
		cdvd_iso_begin_read(&in, 20);
		good = cdvd_iso_finish_read(&in, got, CDVD_MODE_2368) == 0;
		for (i = 0; i < 24 && good; i++)
			good = got[i] == 0;
		good = good && same(got + 24, image, 20);
		cdvd_iso_begin_read(&in, 20);
		good = good && cdvd_iso_finish_read(&in, got, 99) == -1;
		report(good, "flat", "the 2368-byte mode reads the raw sector, an unknown mode fails");

		/* Past the end: several games do this and must not be told the
		 * disc is broken. */
		cdvd_iso_begin_read(&in, SECTORS + 16);
		report(cdvd_iso_finish_read(&in, got, CDVD_MODE_2048) == 0, "flat", "a read past the end reports 0");
		cdvd_iso_close(&in);
		check_read_sector(iso, image, "flat");
	}

	/* A gzip whose index holds a span or a size no reader wrote: the index
	 * is not used, a new one is built, and the sectors come back right. */
	if (!write_gz(gz, image, (size_t)SECTOR * SECTORS))
		report(0, "gzip", "could not write it");
	else
	{
		static const struct { s32 span; s64 size; const char *what; } bad[] = {
			{ 0,       (s64)SECTOR * SECTORS, "gzip, its index holding a zero span"     },
			{ -4096,   (s64)SECTOR * SECTORS, "gzip, its index holding a negative span" },
			{ 4 << 20, -1,                    "gzip, its index holding a negative size" },
		};
		unsigned b;
		for (b = 0; b < sizeof(bad) / sizeof(bad[0]); b++)
		{
			if (!write_gz_index(idx, bad[b].span, bad[b].size))
				report(0, bad[b].what, "could not write the index");
			else
				check_image(gz, image, bad[b].what);
		}
		remove(gz);
		remove(idx);
	}

	sprintf(cmd, "python3 '%s/../cdvd/make_cso.py' '%s' '%s' > /dev/null 2>&1", dir, iso, cso);
	if (system(cmd) == 0)
	{
		check_image(cso, image, "CSO, through the chunk cache");
		remove(cso);
	}
	else
		printf("  note: no python3, so CSO is not covered here\n");

	sprintf(cmd, "chdman createdvd -f -i '%s' -o '%s' > /dev/null 2>&1", iso, chd);
	if (system(cmd) == 0)
	{
		check_image(chd, image, "CHD, a DVD");
		remove(chd);
		if (write_cd(cdbin, cdcue, image))
		{
			sprintf(cmd, "chdman createcd -f -i '%s' -o '%s' > /dev/null 2>&1", cdcue, cdchd);
			if (system(cmd) == 0)
				check_image(cdchd, image, "CHD, a CD of 2448-byte units");
			else
				report(0, "CHD, a CD", "chdman could not make it");
			remove(cdchd);
		}
		remove(cdbin);
		remove(cdcue);
	}
	else
		printf("  note: no chdman, so CHD is not covered here\n");

	check_backwards_cso("cdvdread_backwards.cso");
	check_readahead();

	free(image);
	remove(iso);
	printf(s_ok ? "cdvd read: ok\n" : "cdvd read: FAILED\n");
	return s_ok ? 0 : 1;
}
