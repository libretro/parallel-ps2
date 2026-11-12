/* SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team */
/* SPDX-License-Identifier: GPL-3.0+ */

#include "cam-jpeg.h"

#include <math.h>
#include <string.h>

/* Natural (row-major) index -> position in zig-zag order. */
static const u8 s_zigzag[64] = {
	0, 1, 5, 6, 14, 15, 27, 28, 2, 4, 7, 13, 16, 26, 29, 42,
	3, 8, 12, 17, 25, 30, 41, 43, 9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

/* ITU T.81 Annex K tables, natural order. */
static const u8 s_luma_quant[64] = {
	16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
	14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
	18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
	49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
static const u8 s_chroma_quant[64] = {
	17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

static const u8 s_dc_luma_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const u8 s_dc_chroma_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const u8 s_dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const u8 s_ac_luma_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const u8 s_ac_luma_vals[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa};
static const u8 s_ac_chroma_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const u8 s_ac_chroma_vals[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa};

static void build_huff_table(eyetoy_jpeg_huff_table* t, const u8* bits, const u8* vals)
{
	u16 code = 0;
	int k = 0, len, i;
	memset(t, 0, sizeof(*t));
	for (len = 1; len <= 16; len++)
	{
		for (i = 0; i < bits[len - 1]; i++, k++)
		{
			t->code[vals[k]] = code++;
			t->size[vals[k]] = (u8)len;
		}
		code <<= 1;
	}
}

void eyetoy_jpeg_tables_init(eyetoy_jpeg_tables* t)
{
	build_huff_table(&t->dc_luma, s_dc_luma_bits, s_dc_vals);
	build_huff_table(&t->ac_luma, s_ac_luma_bits, s_ac_luma_vals);
	build_huff_table(&t->dc_chroma, s_dc_chroma_bits, s_dc_vals);
	build_huff_table(&t->ac_chroma, s_ac_chroma_bits, s_ac_chroma_vals);
}

typedef struct jpeg_writer
{
	u8* out;
	u32 size;
	u32 pos;
	u32 bits;
	int count;
	bool overflow;
} jpeg_writer;

static void put_byte(jpeg_writer* w, u8 b)
{
	if (w->pos < w->size)
		w->out[w->pos++] = b;
	else
		w->overflow = true;
}

static void put_word(jpeg_writer* w, u16 v)
{
	put_byte(w, (u8)(v >> 8));
	put_byte(w, (u8)v);
}

static void put_bits(jpeg_writer* w, u32 value, int n)
{
	w->bits = (w->bits << n) | (value & ((1u << n) - 1));
	w->count += n;
	while (w->count >= 8)
	{
		const u8 b = (u8)(w->bits >> (w->count - 8));
		put_byte(w, b);
		if (b == 0xFF)
			put_byte(w, 0);
		w->count -= 8;
	}
}

static void flush_bits(jpeg_writer* w)
{
	if (w->count > 0)
		put_bits(w, 0x7F, 8 - w->count);
}

/* The AAN forward DCT, unscaled; the scale folds into the quantiser. */
static void fdct(float* d0, float* d1, float* d2, float* d3, float* d4, float* d5, float* d6, float* d7)
{
	const float tmp0 = *d0 + *d7, tmp7 = *d0 - *d7;
	const float tmp1 = *d1 + *d6, tmp6 = *d1 - *d6;
	const float tmp2 = *d2 + *d5, tmp5 = *d2 - *d5;
	const float tmp3 = *d3 + *d4, tmp4 = *d3 - *d4;
	float tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
	float tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
	float z1, z2, z3, z4, z5, z11, z13;

	*d0 = tmp10 + tmp11;
	*d4 = tmp10 - tmp11;
	z1 = (tmp12 + tmp13) * 0.707106781f;
	*d2 = tmp13 + z1;
	*d6 = tmp13 - z1;

	tmp10 = tmp4 + tmp5;
	tmp11 = tmp5 + tmp6;
	tmp12 = tmp6 + tmp7;
	z5 = (tmp10 - tmp12) * 0.382683433f;
	z2 = tmp10 * 0.541196100f + z5;
	z4 = tmp12 * 1.306562965f + z5;
	z3 = tmp11 * 0.707106781f;
	z11 = tmp7 + z3;
	z13 = tmp7 - z3;
	*d5 = z13 + z2;
	*d3 = z13 - z2;
	*d1 = z11 + z4;
	*d7 = z11 - z4;
}

static int magnitude(int v)
{
	const int a = v < 0 ? -v : v;
	int n = 0;
	while (a >> n)
		n++;
	return n;
}

/* Nearest, ties to even, as lrintf under the default rounding the capture
 * threads keep: C89 has no lrintf. */
static int round_even(float f)
{
	const double v = (double)f;
	double r = floor(v + 0.5);
	if (r - v == 0.5 && fmod(r, 2.0) != 0.0)
		r -= 1.0;
	return (int)r;
}

static int encode_block(jpeg_writer* w, float* block, const float* fdtbl, int dc, const eyetoy_jpeg_huff_table* hdc, const eyetoy_jpeg_huff_table* hac)
{
	int q[64];
	int i, diff, dc_size, last, run;
	for (i = 0; i < 64; i += 8)
		fdct(&block[i], &block[i + 1], &block[i + 2], &block[i + 3], &block[i + 4], &block[i + 5], &block[i + 6], &block[i + 7]);
	for (i = 0; i < 8; i++)
		fdct(&block[i], &block[i + 8], &block[i + 16], &block[i + 24], &block[i + 32], &block[i + 40], &block[i + 48], &block[i + 56]);

	for (i = 0; i < 64; i++)
		q[s_zigzag[i]] = round_even(block[i] * fdtbl[i]);

	diff = q[0] - dc;
	dc_size = magnitude(diff);
	put_bits(w, hdc->code[dc_size], hdc->size[dc_size]);
	put_bits(w, (u32)(diff < 0 ? diff - 1 : diff), dc_size);

	last = 63;
	while (last > 0 && q[last] == 0)
		last--;
	run = 0;
	for (i = 1; i <= last; i++)
	{
		int ac_size, sym;
		if (q[i] == 0)
		{
			run++;
			continue;
		}
		while (run > 15)
		{
			put_bits(w, hac->code[0xF0], hac->size[0xF0]);
			run -= 16;
		}
		ac_size = magnitude(q[i]);
		sym = (run << 4) | ac_size;
		put_bits(w, hac->code[sym], hac->size[sym]);
		put_bits(w, (u32)(q[i] < 0 ? q[i] - 1 : q[i]), ac_size);
		run = 0;
	}
	if (last != 63)
		put_bits(w, hac->code[0x00], hac->size[0x00]);
	return q[0];
}

static void write_quant(jpeg_writer* w, u8 id, const u8* table)
{
	u8 zz[64];
	int i;
	for (i = 0; i < 64; i++)
		zz[s_zigzag[i]] = table[i];
	put_byte(w, id);
	for (i = 0; i < 64; i++)
		put_byte(w, zz[i]);
}

static void write_huff(jpeg_writer* w, u8 id, const u8* bits, const u8* vals, int nvals)
{
	int i;
	put_byte(w, id);
	for (i = 0; i < 16; i++)
		put_byte(w, bits[i]);
	for (i = 0; i < nvals; i++)
		put_byte(w, vals[i]);
}

static void put_bytes(jpeg_writer* w, const u8* b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		put_byte(w, b[i]);
}

u32 eyetoy_compress_jpeg(const eyetoy_jpeg_tables* huff, u8* out, u32 out_size, const u8* rgb, int width, int height, int quality)
{
	static const float aasf[8] = {
		1.0f * 2.828427125f, 1.387039845f * 2.828427125f, 1.306562965f * 2.828427125f, 1.175875602f * 2.828427125f,
		1.0f * 2.828427125f, 0.785694958f * 2.828427125f, 0.541196100f * 2.828427125f, 0.275899379f * 2.828427125f};
	static const u8 jfif[] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
	static const u8 components[] = {1, 0x21, 0, 2, 0x11, 1, 3, 0x11, 1};
	static const u8 scan[] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
	u8 luma_q[64], chroma_q[64];
	float luma_fd[64], chroma_fd[64];
	int scale, i, mx, my, row, col;
	int dc_y = 0, dc_cb = 0, dc_cr = 0;
	jpeg_writer w;

	quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
	scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	for (i = 0; i < 64; i++)
	{
		const int l = (s_luma_quant[i] * scale + 50) / 100;
		const int c = (s_chroma_quant[i] * scale + 50) / 100;
		const float a = aasf[i >> 3] * aasf[i & 7];
		luma_q[i] = (u8)(l < 1 ? 1 : l > 255 ? 255 : l);
		chroma_q[i] = (u8)(c < 1 ? 1 : c > 255 ? 255 : c);
		luma_fd[i] = 1.0f / (luma_q[i] * a);
		chroma_fd[i] = 1.0f / (chroma_q[i] * a);
	}

	w.out = out;
	w.size = out_size;
	w.pos = 0;
	w.bits = 0;
	w.count = 0;
	w.overflow = false;
	put_word(&w, 0xFFD8);
	put_word(&w, 0xFFE0);
	put_word(&w, (u16)(2 + sizeof(jfif)));
	put_bytes(&w, jfif, sizeof(jfif));
	put_word(&w, 0xFFDB);
	put_word(&w, 2 + 65 * 2);
	write_quant(&w, 0, luma_q);
	write_quant(&w, 1, chroma_q);
	put_word(&w, 0xFFC0);
	put_word(&w, 17);
	put_byte(&w, 8);
	put_word(&w, (u16)height);
	put_word(&w, (u16)width);
	put_byte(&w, 3);
	put_bytes(&w, components, sizeof(components));
	put_word(&w, 0xFFC4);
	put_word(&w, 2 + (17 + 12) * 2 + (17 + 162) * 2);
	write_huff(&w, 0x00, s_dc_luma_bits, s_dc_vals, 12);
	write_huff(&w, 0x10, s_ac_luma_bits, s_ac_luma_vals, 162);
	write_huff(&w, 0x01, s_dc_chroma_bits, s_dc_vals, 12);
	write_huff(&w, 0x11, s_ac_chroma_bits, s_ac_chroma_vals, 162);
	put_word(&w, 0xFFDA);
	put_word(&w, 12);
	put_bytes(&w, scan, sizeof(scan));

	for (my = 0; my < height; my += 8)
	{
		for (mx = 0; mx < width; mx += 16)
		{
			float y[2][64], cb[64], cr[64];
			for (row = 0; row < 8; row++)
			{
				const int sy = my + row < height ? my + row : height - 1;
				for (col = 0; col < 16; col++)
				{
					const int sx = mx + col < width ? mx + col : width - 1;
					const u8* p = rgb + (sy * width + sx) * 3;
					const float r = p[0], g = p[1], b = p[2];
					const float u = -0.168736f * r - 0.331264f * g + 0.5f * b;
					const float v = 0.5f * r - 0.418688f * g - 0.081312f * b;
					y[col >> 3][row * 8 + (col & 7)] = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
					if (col & 1)
					{
						cb[row * 8 + (col >> 1)] = (cb[row * 8 + (col >> 1)] + u) * 0.5f;
						cr[row * 8 + (col >> 1)] = (cr[row * 8 + (col >> 1)] + v) * 0.5f;
					}
					else
					{
						cb[row * 8 + (col >> 1)] = u;
						cr[row * 8 + (col >> 1)] = v;
					}
				}
			}
			dc_y = encode_block(&w, y[0], luma_fd, dc_y, &huff->dc_luma, &huff->ac_luma);
			dc_y = encode_block(&w, y[1], luma_fd, dc_y, &huff->dc_luma, &huff->ac_luma);
			dc_cb = encode_block(&w, cb, chroma_fd, dc_cb, &huff->dc_chroma, &huff->ac_chroma);
			dc_cr = encode_block(&w, cr, chroma_fd, dc_cr, &huff->dc_chroma, &huff->ac_chroma);
		}
	}
	flush_bits(&w);
	put_word(&w, 0xFFD9);
	return w.overflow ? 0 : w.pos;
}
