/* One file of a FAT volume, in place. See fatimg.h. */
#include "fatimg.h"

#include <stdlib.h>
#include <string.h>

static u32 le16(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 le32(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

typedef struct fat_vol
{
	u64 base;      /* the volume's first byte in the image */
	u32 bps;       /* bytes per sector */
	u32 spc;       /* sectors per cluster */
	u32 fat_off;   /* the first FAT, bytes into the volume */
	u64 root_off;  /* FAT12/16: the fixed root directory */
	u32 root_ents;
	u64 data_off;  /* cluster 2 */
	u32 clusters;  /* counted from 2 */
	u32 root_clus; /* FAT32 */
	int type;      /* 12, 16 or 32 */
} fat_vol;

/* The boot sector at base, if it is a FAT one. */
static int read_bpb(fat_file* f, u64 base, u64 image_size, fat_vol* v)
{
	u8 b[512];
	u32 reserved, nfats, fat_sz, tot, root_secs, data_secs;

	if (!f->io(f->ctx, base, b, sizeof(b), 0) || b[510] != 0x55 || b[511] != 0xaa)
		return 0;
	if (b[0] != 0xeb && b[0] != 0xe9)
		return 0;
	v->base = base;
	v->bps = le16(b + 11);
	v->spc = b[13];
	reserved = le16(b + 14);
	nfats = b[16];
	v->root_ents = le16(b + 17);
	tot = le16(b + 19);
	if (!tot)
		tot = le32(b + 32);
	fat_sz = le16(b + 22);
	if (!fat_sz)
		fat_sz = le32(b + 36);
	if ((v->bps != 512 && v->bps != 1024 && v->bps != 2048 && v->bps != 4096)
		|| !v->spc || (v->spc & (v->spc - 1)) || !reserved || !nfats || !fat_sz || !tot)
		return 0;
	root_secs = (v->root_ents * 32 + v->bps - 1) / v->bps;
	if ((u64)reserved + (u64)nfats * fat_sz + root_secs >= tot)
		return 0;
	data_secs = tot - reserved - nfats * fat_sz - root_secs;
	v->clusters = data_secs / v->spc;
	v->type = v->clusters < 4085 ? 12 : v->clusters < 65525 ? 16 : 32;
	v->fat_off = reserved * v->bps;
	v->root_off = (u64)(reserved + nfats * fat_sz) * v->bps;
	v->data_off = (u64)(reserved + nfats * fat_sz + root_secs) * v->bps;
	v->root_clus = v->type == 32 ? le32(b + 44) : 0;
	if (base + (u64)tot * v->bps > image_size)
		return 0;
	return 1;
}

static int find_volume(fat_file* f, u64 image_size, fat_vol* v)
{
	u8 mbr[512];
	int i;

	if (read_bpb(f, 0, image_size, v))
		return 1;
	if (!f->io(f->ctx, 0, mbr, sizeof(mbr), 0) || mbr[510] != 0x55 || mbr[511] != 0xaa)
		return 0;
	for (i = 0; i < 4; i++)
	{
		const u8* e = mbr + 446 + i * 16;
		const u32 lba = le32(e + 8);
		if (e[4] && lba && read_bpb(f, (u64)lba * 512, image_size, v))
			return 1;
	}
	return 0;
}

static u64 cluster_offset(const fat_vol* v, u32 c)
{
	return v->base + v->data_off + (u64)(c - 2) * v->spc * v->bps;
}

/* One byte of the FAT, through a one-sector cache: the chain is walked
 * an entry at a time. */
static int fat_byte(fat_file* f, u64 at, u8* out)
{
	const u64 sec = at & ~(u64)(sizeof(f->cache) - 1);
	if (!f->cache_valid || f->cache_off != sec)
	{
		if (!f->io(f->ctx, sec, f->cache, sizeof(f->cache), 0))
			return 0;
		f->cache_off = sec;
		f->cache_valid = 1;
	}
	*out = f->cache[at - sec];
	return 1;
}

/* The FAT entry of cluster c; 0xffffffff when it cannot be read. */
static u32 fat_next(fat_file* f, const fat_vol* v, u32 c)
{
	const u32 width = v->type == 12 ? 2 : v->type == 16 ? 2 : 4;
	const u64 at = v->base + v->fat_off + (v->type == 12 ? (u64)c + c / 2 : (u64)c * width);
	u8 b[4];
	u32 i;
	for (i = 0; i < width; i++)
		if (!fat_byte(f, at + i, &b[i]))
			return 0xffffffffu;
	switch (v->type)
	{
		case 12:
			return (c & 1) ? le16(b) >> 4 : le16(b) & 0xfff;
		case 16:
			return le16(b);
		default:
			return le32(b) & 0x0fffffffu;
	}
}

static int is_end(const fat_vol* v, u32 c)
{
	return c >= (v->type == 12 ? 0xff8u : v->type == 16 ? 0xfff8u : 0x0ffffff8u);
}

static int valid_cluster(const fat_vol* v, u32 c)
{
	return c >= 2 && c - 2 < v->clusters;
}

/* The chain from first as runs of adjacent clusters, enough of it to
 * hold bytes; at most max_runs (the chain cannot be longer than the
 * volume, so a loop ends there). */
static int build_runs(fat_file* f, const fat_vol* v, u32 first, u64 bytes, fat_run** out, u32* nout)
{
	const u32 cbytes = v->spc * v->bps;
	u64 need = bytes;
	u32 c = first, steps = 0, n = 0, cap = 16;
	fat_run* runs = (fat_run*)malloc(cap * sizeof(fat_run));

	if (!runs)
		return 0;
	while (need)
	{
		const u64 off = cluster_offset(v, c);
		const u32 take = need < cbytes ? (u32)need : cbytes;
		if (!valid_cluster(v, c) || ++steps > v->clusters)
			break;
		if (n && runs[n - 1].offset + runs[n - 1].bytes == off && runs[n - 1].bytes <= 0xffffffffu - cbytes)
			runs[n - 1].bytes += take;
		else
		{
			if (n == cap)
			{
				fat_run* more = (fat_run*)realloc(runs, cap * 2 * sizeof(fat_run));
				if (!more)
					break;
				runs = more;
				cap *= 2;
			}
			runs[n].offset = off;
			runs[n].bytes = take;
			n++;
		}
		need -= take;
		if (!need)
			break;
		c = fat_next(f, v, c);
		if (c == 0xffffffffu || is_end(v, c))
			break;
	}
	if (need)
	{
		free(runs);
		return 0;
	}
	*out = runs;
	*nout = n;
	return 1;
}

static void dotted_name(const u8* e, char* out)
{
	int i, n = 0;
	for (i = 0; i < 8 && e[i] != ' '; i++)
		out[n++] = (char)e[i];
	if (e[8] != ' ')
	{
		out[n++] = '.';
		for (i = 8; i < 11 && e[i] != ' '; i++)
			out[n++] = (char)e[i];
	}
	out[n] = '\0';
}

/* The entry at e, if it is the file: 1 found, 0 go on, -1 the end. */
static int consider(fat_file* f, const fat_vol* v, const u8* e)
{
	u32 first, size;
	if (e[0] == 0)
		return -1;
	if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 0x18))
		return 0;
	size = le32(e + 28);
	first = le16(e + 26) | (v->type == 32 ? le16(e + 20) << 16 : 0);
	if (!size || !valid_cluster(v, first))
		return 0;
	if (!build_runs(f, v, first, size, &f->runs, &f->nruns))
		return 0;
	f->size = size;
	dotted_name(e, f->name);
	return 1;
}

int fat_open_first_file(fat_file* f, fat_io_fn io, void* ctx, u64 image_size)
{
	fat_vol v;
	u8 e[32];
	u32 i;
	int r;

	memset(f, 0, sizeof(*f));
	f->io = io;
	f->ctx = ctx;
	if (!find_volume(f, image_size, &v))
		return 0;

	if (v.type != 32)
	{
		for (i = 0; i < v.root_ents; i++)
		{
			if (!io(ctx, v.base + v.root_off + (u64)i * 32, e, 32, 0))
				return 0;
			if ((r = consider(f, &v, e)) != 0)
				return r > 0;
		}
		return 0;
	}
	else
	{
		/* The FAT32 root is a cluster chain like any directory. */
		const u32 per = v.spc * v.bps / 32;
		u32 c = v.root_clus, steps = 0;
		while (valid_cluster(&v, c) && ++steps <= v.clusters)
		{
			for (i = 0; i < per; i++)
			{
				if (!io(ctx, cluster_offset(&v, c) + (u64)i * 32, e, 32, 0))
					return 0;
				if ((r = consider(f, &v, e)) != 0)
					return r > 0;
			}
			c = fat_next(f, &v, c);
			if (c == 0xffffffffu || is_end(&v, c))
				break;
		}
		return 0;
	}
}

void fat_close(fat_file* f)
{
	free(f->runs);
	f->runs = NULL;
	f->nruns = 0;
	f->size = 0;
}

int fat_rw(fat_file* f, u64 offset, void* buf, u32 len, int write)
{
	u8* p = (u8*)buf;
	u64 at = 0;
	u32 i;

	if (!f->runs || offset > f->size || len > f->size - offset)
		return 0;
	for (i = 0; i < f->nruns && len; i++)
	{
		const fat_run* r = &f->runs[i];
		if (offset < at + r->bytes)
		{
			const u64 in = offset - at;
			const u32 n = (u32)(r->bytes - in < len ? r->bytes - in : len);
			if (!f->io(f->ctx, r->offset + in, p, n, write))
				return 0;
			p += n;
			offset += n;
			len -= n;
		}
		at += r->bytes;
	}
	return len == 0;
}
