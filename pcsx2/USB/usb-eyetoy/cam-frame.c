/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#include "cam-frame.h"
#include "cam-jpeg.h"
#include "jo_mpeg.h"

#include <stdlib.h>
#include <string.h>

static u8 clamp_u8(int v)
{
	return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void eyetoy_xrgb_to_rgb(const u32* src, int src_w, int src_h, size_t src_pitch, u8* dst, int dst_w, int dst_h, bool mirror)
{
	int x, y;
	for (y = 0; y < dst_h; y++)
	{
		const u32* row = (const u32*)((const u8*)src + (size_t)(y * src_h / dst_h) * src_pitch);
		for (x = 0; x < dst_w; x++)
		{
			const u32 p = row[(mirror ? dst_w - 1 - x : x) * src_w / dst_w];
			*dst++ = (u8)(p >> 16);
			*dst++ = (u8)(p >> 8);
			*dst++ = (u8)p;
		}
	}
}

void eyetoy_yuyv_to_rgb(const u8* src, int src_w, int src_h, u8* dst, int dst_w, int dst_h, bool mirror)
{
	int x, y;
	for (y = 0; y < dst_h; y++)
	{
		const u8* row = src + (y * src_h / dst_h) * src_w * 2;
		for (x = 0; x < dst_w; x++)
		{
			const int sx = (mirror ? dst_w - 1 - x : x) * src_w / dst_w;
			const u8* pair = row + (sx & ~1) * 2;
			/* BT.601 limited range */
			const int c = 298 * (row[sx * 2] - 16) + 128;
			const int d = pair[1] - 128;
			const int e = pair[3] - 128;
			*dst++ = clamp_u8((c + 409 * e) >> 8);
			*dst++ = clamp_u8((c - 100 * d - 208 * e) >> 8);
			*dst++ = clamp_u8((c + 516 * d) >> 8);
		}
	}
}

/* The Capture Eye's 80x64 greyscale, sampled every fourth pixel and laid
 * out in 8x8 blocks. */
static u32 encode_yuv400(u8* out, const u8* rgb, int width, int height)
{
	u32 pos = 0;
	int mx, my, x, y;
	for (my = 0; my < 8; my++)
		for (mx = 0; mx < 10; mx++)
			for (y = 0; y < 8; y++)
				for (x = 0; x < 8; x++)
				{
					const int sx = 4 * (8 * mx + x);
					const int sy = 4 * (8 * my + y);
					const u8* p;
					if (sx >= width || sy >= height)
					{
						out[pos++] = 0x01;
						continue;
					}
					p = rgb + (sy * width + sx) * 3;
					out[pos++] = (u8)(0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]);
				}
	return pos;
}

eyetoy_frame_encoder* eyetoy_encoder_create(void)
{
	eyetoy_frame_encoder* enc = (eyetoy_frame_encoder*)malloc(sizeof(*enc));
	if (enc)
		eyetoy_jpeg_tables_init(&enc->jpeg);
	return enc;
}

void eyetoy_encoder_free(eyetoy_frame_encoder* enc)
{
	free(enc);
}

u32 eyetoy_encode_frame(eyetoy_frame_encoder* enc, u8* out, const u8* rgb, int width, int height, enum eyetoy_frame_format format)
{
	switch (format)
	{
		case EYETOY_FORMAT_MPEG:
			return (u32)jo_write_mpeg(out, rgb, width, height, JO_RGB24, JO_NONE, JO_NONE, enc->mpeg_work);
		case EYETOY_FORMAT_JPEG:
			return eyetoy_compress_jpeg(&enc->jpeg, out, EYETOY_MAX_FRAME_SIZE, rgb, width, height, 80);
		case EYETOY_FORMAT_YUV400:
			return encode_yuv400(out, rgb, width, height);
	}
	return 0;
}

u32 eyetoy_encode_test_pattern(eyetoy_frame_encoder* enc, u8* out, int width, int height, enum eyetoy_frame_format format)
{
	int x, y;
	if (format == EYETOY_FORMAT_YUV400)
	{
		for (y = 0; y < 64; y++)
			for (x = 0; x < 80; x++)
				out[80 * y + x] = (u8)(255 * y / 80);
		return 80 * 64;
	}

	/* The picture is drawn in the encoder's own RGB, which holds the
	 * largest, so drawing it allocates nothing. */
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			u8* p = enc->rgb + (y * width + x) * 3;
			p[0] = (u8)(255 * y / height);
			p[1] = (u8)(255 * x / width);
			p[2] = (u8)(255 * y / height);
		}
	}
	return eyetoy_encode_frame(enc, out, enc->rgb, width, height, format);
}

bool eyetoy_mailbox_init(eyetoy_frame_mailbox* m)
{
	u8* mem = (u8*)malloc((size_t)EYETOY_MAX_FRAME_SIZE * 3);
	int i;
	if (!mem)
		return false;
	for (i = 0; i < 3; i++)
	{
		m->buf[i].data = mem + (size_t)EYETOY_MAX_FRAME_SIZE * i;
		m->buf[i].size = 0;
		m->buf[i].generation = 0;
	}
	retro_atomic_int_init(&m->slot, 0);
	m->work = 1;
	m->hold = 2;
	return true;
}

void eyetoy_mailbox_free(eyetoy_frame_mailbox* m)
{
	free(m->buf[0].data);
	m->buf[0].data = m->buf[1].data = m->buf[2].data = NULL;
}

void eyetoy_mailbox_reset(eyetoy_frame_mailbox* m)
{
	int i;
	for (i = 0; i < 3; i++)
		m->buf[i].size = 0;
	retro_atomic_store_release_int(&m->slot, retro_atomic_load_acquire_int(&m->slot) & ~EYETOY_MAILBOX_FRESH);
}

u8* eyetoy_mailbox_work(const eyetoy_frame_mailbox* m)
{
	return m->buf[m->work].data;
}

void eyetoy_mailbox_publish(eyetoy_frame_mailbox* m, u32 size, u32 generation)
{
	m->buf[m->work].size = size;
	m->buf[m->work].generation = generation;
	/* acq_rel: the frame's bytes are visible to whoever takes the slot. */
	m->work = retro_atomic_exchange_int(&m->slot, m->work | EYETOY_MAILBOX_FRESH) & ~EYETOY_MAILBOX_FRESH;
}

const eyetoy_frame_buffer* eyetoy_mailbox_take(eyetoy_frame_mailbox* m)
{
	const int got = retro_atomic_exchange_int(&m->slot, m->hold);
	m->hold = got & ~EYETOY_MAILBOX_FRESH;
	return (got & EYETOY_MAILBOX_FRESH) ? &m->buf[m->hold] : NULL;
}

void eyetoy_ring_reset(eyetoy_sample_ring* r)
{
	retro_atomic_int_init(&r->head, 0);
	retro_atomic_int_init(&r->tail, 0);
}

u32 eyetoy_ring_space(eyetoy_sample_ring* r)
{
	const u32 h = (u32)retro_atomic_load_acquire_int(&r->head);
	const u32 t = (u32)retro_atomic_load_relaxed_int(&r->tail);
	return EYETOY_RING_MASK - ((t - h) & EYETOY_RING_MASK);
}

bool eyetoy_ring_push(eyetoy_sample_ring* r, s16 sample)
{
	const u32 t = (u32)retro_atomic_load_relaxed_int(&r->tail);
	if (((t + 1) & EYETOY_RING_MASK) == ((u32)retro_atomic_load_acquire_int(&r->head) & EYETOY_RING_MASK))
		return false;
	r->data[t & EYETOY_RING_MASK] = sample;
	retro_atomic_store_release_int(&r->tail, (int)((t + 1) & EYETOY_RING_MASK));
	return true;
}

u32 eyetoy_ring_count(eyetoy_sample_ring* r)
{
	const u32 h = (u32)retro_atomic_load_relaxed_int(&r->head);
	const u32 t = (u32)retro_atomic_load_acquire_int(&r->tail);
	return (t - h) & EYETOY_RING_MASK;
}

void eyetoy_ring_trim_to(eyetoy_sample_ring* r, u32 keep)
{
	const u32 n = eyetoy_ring_count(r);
	if (n > keep)
	{
		const u32 h = (u32)retro_atomic_load_relaxed_int(&r->head);
		retro_atomic_store_release_int(&r->head, (int)((h + n - keep) & EYETOY_RING_MASK));
	}
}

u32 eyetoy_ring_pop(eyetoy_sample_ring* r, s16* samples, u32 max)
{
	/* One read of the count: the producer may add between two. */
	const u32 count = eyetoy_ring_count(r);
	const u32 n = count < max ? count : max;
	const u32 h = (u32)retro_atomic_load_relaxed_int(&r->head);
	u32 i;
	for (i = 0; i < n; i++)
		samples[i] = r->data[(h + i) & EYETOY_RING_MASK];
	retro_atomic_store_release_int(&r->head, (int)((h + n) & EYETOY_RING_MASK));
	return n;
}
