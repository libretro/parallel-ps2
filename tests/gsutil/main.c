/* The GS's pixel-format tables (GS/GSUtil.c) against the relationships
 * the GS relies on, its swizzle tables (GS/GSTables.c) against the ones
 * the C++ templates built at compile time, the addressing on them
 * (GS/GSOffset.c) against the C++ GSOffset it came from, and the texture cache's hash - libretro-common's XXH3
 * - against the xxHash it replaced, one-shot and streamed in GS blocks
 * the way the cache streams them: the same texture keeps its key. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/GSUtil.h"
#include "GS/GSTables.h"
#include "GS/GSOffset.h"
#include "GS/GSPsm.h"
#include "GS/GSDrawingContext.h"
#include "GS/Renderers/Common/GSDirtyRect.h"
#include "GS/GSClut.h"
#include "GS/GSLocalMemoryMaps.h"
#include "GS/Renderers/Common/GSFlatMap.h"
#include <encodings/xxh3.h>
#define XXH_STATIC_LINKING_ONLY 1
#define XXH_INLINE_ALL 1
#include <xxhash.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void check_hash(const u8* buf, size_t len)
{
	u64 want = XXH3_64bits(buf, len);
	xxh3_state_t st;
	size_t off;

	CHECK(xxh3_64(buf, len, 0) == want);
	xxh3_reset(&st, 0);
	for (off = 0; off + GS_BLOCK_SIZE <= len; off += GS_BLOCK_SIZE)
		xxh3_update(&st, buf + off, GS_BLOCK_SIZE);
	xxh3_update(&st, buf + off, len - off);
	CHECK(xxh3_digest(&st) == want);
}

/* XXH3 of each table as GSTables.cpp's constexpr templates built it */
static void check_tables(void)
{
#define TABLE(t, size, sum) do { \
	CHECK(sizeof(t) == (size)); \
	CHECK(xxh3_64(&(t), sizeof(t), 0) == (sum)); \
} while (0)
	TABLE(gs_block_table32,         64, 0x70225d52284824e2ULL);
	TABLE(gs_block_table16,         64, 0x4224a8ef5c78dbf3ULL);
	TABLE(gs_block_table16S,        64, 0xc2a3083f883c586cULL);
	TABLE(gs_block_table8,          64, 0x70225d52284824e2ULL);
	TABLE(gs_block_table4,          64, 0x4224a8ef5c78dbf3ULL);
	TABLE(gs_pixel_col_offset32,   128, 0x5720bb321a4fd6bcULL);
	TABLE(gs_pixel_col_offset16,   256, 0x1e92bff8fecdea71ULL);
	TABLE(gs_pixel_col_offset16S,  256, 0x4c3f54d26d5b2bddULL);
	TABLE(gs_pixel_col_offset8,    256, 0x5c9ba151ba8304c1ULL);
	TABLE(gs_pixel_col_offset4,    512, 0xd8180dec19436216ULL);
	TABLE(gs_pixel_row_offset32,   16384, 0x7e81141ca6badc82ULL);
	TABLE(gs_pixel_row_offset16,   16384, 0x07281a61c09964b4ULL);
	TABLE(gs_pixel_row_offset16S,  16384, 0x7c6886fb8e006a1fULL);
	TABLE(gs_pixel_row_offset8[0], 16384, 0xc08a47d02d89d370ULL);
	TABLE(gs_pixel_row_offset8[1], 16384, 0xec30581c4ddbb781ULL);
	TABLE(gs_pixel_row_offset4[0], 16384, 0xb7841a73a109201eULL);
	TABLE(gs_pixel_row_offset4[1], 16384, 0xe9286578e07818daULL);
	TABLE(gs_column_table32,        64, 0x84b72c3af69721eaULL);
	TABLE(gs_column_table16,       128, 0xa55630e1352bd88cULL);
	TABLE(gs_column_table8,        256, 0x2cf149bff1645635ULL);
	TABLE(gs_column_table4,       1024, 0x77d5c89fe163f319ULL);
	TABLE(gs_clut_table_t32i8,     128, 0xb0f48ca80efd3c0aULL);
	TABLE(gs_clut_table_t32i4,      16, 0x09b4011b229729c6ULL);
	TABLE(gs_clut_table_t16i8,      32, 0x5672f4b553bcb63aULL);
	TABLE(gs_clut_table_t16i4,      16, 0x5bfccf1957bc1963ULL);
#undef TABLE
	/* PSMT8 and PSMT4 rows go a a b b b b a a; the rest one table */
	CHECK(gs_pixel_row_list8[0] == &gs_pixel_row_offset8[0] && gs_pixel_row_list8[1] == &gs_pixel_row_offset8[0]);
	CHECK(gs_pixel_row_list8[2] == &gs_pixel_row_offset8[1] && gs_pixel_row_list8[5] == &gs_pixel_row_offset8[1]);
	CHECK(gs_pixel_row_list8[6] == &gs_pixel_row_offset8[0] && gs_pixel_row_list8[7] == &gs_pixel_row_offset8[0]);
	CHECK(gs_pixel_row_list4[3] == &gs_pixel_row_offset4[1] && gs_pixel_row_list4[7] == &gs_pixel_row_offset4[0]);
	CHECK(gs_pixel_row_list32[7] == &gs_pixel_row_offset32 && gs_pixel_row_list16S[4] == &gs_pixel_row_offset16S);
	/* The alignment the vector loads assume */
	CHECK(((size_t)&gs_block_table16 & 63) == 0);
	CHECK(((size_t)gs_pixel_col_offset4 & 127) == 0 && ((size_t)&gs_pixel_row_offset8[1] & 127) == 0);
}

/* Block numbers, pixel addresses and the pages and blocks of rectangles,
 * for each swizzle at random bp, bw and places, hashed: the sums are what
 * the C++ GSOffset gave for the same draws */
static u32 s_rng = 1;
static u32 next_rand(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return s_rng >> 8;
}

#define OUT_LEN (1 << 16)
static u32 s_out[OUT_LEN];

static void check_offsets(void)
{
	static const gs_swizzle_info* const info[8] = {&gs_swizzle32, &gs_swizzle32Z,
		&gs_swizzle16, &gs_swizzle16S, &gs_swizzle16Z, &gs_swizzle16SZ, &gs_swizzle8, &gs_swizzle4};
	static const u32 want[8][2] = {
		{0xed562ad4U, 0x12e3a432U},
		{0xb6b77a6eU, 0x910de15eU},
		{0xcac616f5U, 0x3d6ae983U},
		{0x928de963U, 0xc63f4d67U},
		{0x10748befU, 0xe39e5d67U},
		{0x443088dbU, 0xdaab5f6bU},
		{0x24cfe28fU, 0xc31976e7U},
		{0xfb6c9cadU, 0x84d935b4U}
	};
	int i, k;
	for (i = 0; i < 8; i++)
	{
		u32 hb = 0, hl = 0, n = 0;
		for (k = 0; k < 20000; k++)
		{
			u32 bp = next_rand() & 0x3fff, bw = next_rand() % 33;
			int x = (int)(next_rand() % 4096), y = (int)(next_rand() % 4096);
			gs_offset off = gs_offset_make(info[i], bp, bw, 0);
			gs_offset fresh;
			gs_bn_helper bn;
			gs_pa_helper pa;
			gs_bn_init(&bn, &off, x, y);
			gs_pa_init(&pa, &off, x & 2047, y); /* its row reads past x + 63 */
			s_out[n++] = gs_offset_bn(&off, x, y);
			s_out[n++] = gs_offset_bn_nowrap(&off, x, y);
			s_out[n++] = gs_offset_pa(&off, x, y);
			s_out[n++] = gs_pa_value(&pa, (int)(next_rand() % 64));
			gs_bn_next_x(&bn); gs_bn_next_x(&bn); gs_bn_next_y(&bn); gs_bn_next_x(&bn);
			s_out[n++] = gs_bn_value(&bn);
			fresh = gs_offset_make(info[i], bp, bw, 0);
			s_out[n++] = gs_offset_bn(&fresh, x, y);
			s_out[n++] = gs_offset_pa(&fresh, x, y);
			s_out[n++] = gs_offset_bw(&off);
			if (n > OUT_LEN - 16) { hb ^= (u32)xxh3_64(s_out, n * 4, hb); n = 0; }
		}
		hb ^= (u32)xxh3_64(s_out, n * 4, hb);
		n = 0;
		for (k = 0; k < 3000; k++)
		{
			u32 bp = next_rand() & 0x3fff, bw = 1 + (next_rand() % 32);
			int l = (int)(next_rand() % 2048), t = (int)(next_rand() % 2048);
			int r = l + 1 + (int)(next_rand() % ((k & 7) ? 300 : 2048));
			int b = t + 1 + (int)(next_rand() % ((k & 7) ? 300 : 2048));
			gs_offset off = gs_offset_make(info[i], bp, bw, 0);
			gs_page_looper lp;
			gs_page_rows rows;
			gs_bn_helper bn;
			u32 start, end, pos;
			u32 touched[GS_MAX_PAGES / 32];
			int br, bb;
			s_out[n++] = 0xffffffffu;
			gs_offset_page_looper(&lp, &off, l, t, r, b);
			gs_page_rows_init(&rows, &lp);
			memset(touched, 0, sizeof(touched));
			while (gs_page_rows_next(&rows, &start, &end))
				for (pos = start; pos < end; pos++)
				{
					u32 page = pos % GS_MAX_PAGES;
					if (lp.slow && (touched[page / 32] & (1u << (page % 32))))
						continue;
					touched[page / 32] |= 1u << (page % 32);
					if (n < OUT_LEN - 2)
						s_out[n++] = page;
				}
			/* the blocks of a smaller rectangle at the same corner */
			br = (l + (r - l) % 200 + 1 + off.swz.block_mask_x) >> off.swz.block_shift_x;
			bb = (t + (b - t) % 200 + 1 + off.swz.block_mask_y) >> off.swz.block_shift_y;
			for (gs_bn_init(&bn, &off, l, t); bn.blk_y < bb; gs_bn_next_y(&bn))
				for (; bn.blk_x < br; gs_bn_next_x(&bn))
					if (n < OUT_LEN - 2)
						s_out[n++] = gs_bn_value(&bn);
			s_out[n++] = (u32)gs_rect_is_aligned(l, t, r, b, off.swz.block_mask_x, off.swz.block_mask_y)
				| (u32)gs_rect_is_aligned(l, t, r, b, off.swz.page_mask_x, off.swz.page_mask_y) << 1;
			if (n > OUT_LEN - 4096) { hl ^= (u32)xxh3_64(s_out, n * 4, hl); n = 0; }
		}
		hl ^= (u32)xxh3_64(s_out, n * 4, hl);
		if (hb != want[i][0] || hl != want[i][1])
		{
			printf("  FAIL swizzle %d: 0x%08x 0x%08x, want 0x%08x 0x%08x\n", i,
				(unsigned)hb, (unsigned)hl, (unsigned)want[i][0], (unsigned)want[i][1]);
			fails++;
		}
	}
}

/* Each mode's attributes, and the block ranges of random rectangles and
 * whether pairs of them share a page, hashed: the sums are what
 * GSLocalMemory's constructor and its block helpers gave */
static void check_psm(void)
{
	const GSBlockSwizzleTable* tables[5] = {&gs_block_table32, &gs_block_table16,
		&gs_block_table16S, &gs_block_table8, &gs_block_table4};
	u32 n = 0, h = 0, i, j;
	int k;

	for (i = 0; i < 64; i++)
	{
		const gs_psm_attr* p = &gs_psm[i];
		u32 w = 9;
		s_out[n++] = p->bpp; s_out[n++] = p->trbpp; s_out[n++] = p->pal; s_out[n++] = p->fmt;
		s_out[n++] = p->bs.x; s_out[n++] = p->bs.y; s_out[n++] = p->pgs.x; s_out[n++] = p->pgs.y;
		s_out[n++] = p->msk; s_out[n++] = p->depth; s_out[n++] = p->fmsk;
		s_out[n++] = p->info.page_shift_x | p->info.page_shift_y << 8 | p->info.block_address_xor << 16;
		for (j = 0; j < 5; j++)
			if (p->info.block_swizzle == tables[j])
				w = j;
		s_out[n++] = w;
	}
	CHECK(xxh3_64(s_out, n * 4, 0) == 0x5e6bfac109054401ULL);

	s_rng = 7;
	n = 0;
	for (k = 0; k < 40000; k++)
	{
		u32 psm = next_rand() % 64, bp = next_rand() & 0x3fff, bw = 1 + next_rand() % 32;
		int r[4], r2[4];
		u32 psm2, bp2, bw2;
		r[0] = (int)(next_rand() % 2048);
		r[1] = (int)(next_rand() % 2048);
		r[2] = r[0] + 1 + (int)(next_rand() % ((k & 3) ? 64 : 1024));
		r[3] = r[1] + 1 + (int)(next_rand() % ((k & 3) ? 64 : 1024));
		if (k & 1)
		{
			r[0] &= ~63; r[1] &= ~31; r[2] = (r[2] + 63) & ~63; r[3] = (r[3] + 31) & ~31;
		}
		psm2 = next_rand() % 64; bp2 = next_rand() & 0x3fff; bw2 = 1 + next_rand() % 32;
		r2[0] = (int)(next_rand() % 512);
		r2[1] = (int)(next_rand() % 512);
		r2[2] = 513 + (int)(next_rand() % 512);
		r2[3] = 513 + (int)(next_rand() % 512);
		s_out[n++] = gs_psm_start_block(bp, bw, psm, r[0], r[1]);
		s_out[n++] = gs_psm_end_block(bp, bw, psm, r[2], r[3]);
		s_out[n++] = gs_psm_end_block_unwrapped(bp, bw, psm, r[2], r[3]);
		s_out[n++] = (u32)gs_psm_has_overlap(bp, bw, psm, r, bp2, bw2, psm2, r2);
		if (n > OUT_LEN - 8) { h ^= (u32)xxh3_64(s_out, n * 4, h); n = 0; }
	}
	h ^= (u32)xxh3_64(s_out, n * 4, h);
	CHECK(h == 0xe0d6ac4fU);
}

/* A drawing context's texture size fix-up and scissor, for random
 * registers and coordinates (NaN, infinities and huge ones among them),
 * hashed: the sum is what the C++ GSDrawingContext gave */
static u32 ctx_rand(void)
{
	return next_rand();
}

static u64 ctx_rand64(void)
{
	u64 hi = ctx_rand();
	u64 lo = ctx_rand();
	return hi << 32 | lo;
}

static float ctx_float(void)
{
	u32 k = ctx_rand() % 16;
	if (k == 0)
	{
		u32 b = ctx_rand() << 8;
		float f;
		b |= ctx_rand() & 0xff;
		memcpy(&f, &b, 4);
		return f;
	}
	if (k < 4)
	{
		float i = (float)((int)(ctx_rand() % 4096) - 1024);
		return i + (float)(ctx_rand() % 16) / 16.0f;
	}
	{
		float i = (float)(ctx_rand() % 1100);
		return i + (float)(ctx_rand() % 256) / 256.0f - 0.5f;
	}
}

static void check_context(void)
{
	GIFRegTEX0 tex0;
	GIFRegCLAMP clamp;
	GIFRegSCISSOR scissor;
	GIFRegXYOFFSET xyoffset;
	u32 n = 0, h = 0;
	int k, i;

	memset(&xyoffset, 0, sizeof(xyoffset));
	s_rng = 3;
	for (k = 0; k < 200000; k++)
	{
		float st[4];
		int linear, mip;
		int in[4], cull[4], xyof[4];
		GIFRegTEX0 r;
		tex0.U64 = ctx_rand64();
		tex0.TW = ctx_rand() % 16;
		tex0.TH = ctx_rand() % 16;
		clamp.U64 = ctx_rand64();
		st[0] = ctx_float();
		st[1] = ctx_float();
		st[2] = ctx_float();
		st[3] = ctx_float();
		linear = ctx_rand() & 1;
		mip = (ctx_rand() % 8) == 0;
		r = gs_ctx_size_fixed_tex0(&tex0, &clamp, st, linear, mip);
		s_out[n++] = (u32)r.U64;
		s_out[n++] = (u32)(r.U64 >> 32);
		scissor.U64 = ctx_rand64();
		xyoffset.OFX = ctx_rand();
		xyoffset.OFY = ctx_rand();
		gs_ctx_scissor(&scissor, &xyoffset, in, cull, xyof);
		for (i = 0; i < 4; i++)
		{
			s_out[n++] = (u32)in[i];
			s_out[n++] = (u32)cull[i];
			s_out[n++] = (u32)xyof[i];
		}
		if (n > OUT_LEN - 32) { h ^= (u32)xxh3_64(s_out, n * 4, h); n = 0; }
	}
	h ^= (u32)xxh3_64(s_out, n * 4, h);
	CHECK(h == 0x417e5a7cU);
}

/* The texture cache's dirty rectangles: each in another format's pixels,
 * their union and channels, and one clamped, for random lists, hashed:
 * the sum is what the C++ GSDirtyRect and GSDirtyRectList gave */
static void put_rect(u32* n, gs_vec4i v)
{
	union gs_v4i_view w;
	int i;
	w.v = v;
	for (i = 0; i < 4; i++)
		s_out[(*n)++] = w.u32[i];
}

static void check_dirty(void)
{
	static const u32 psms[15] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H,
		PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S, PSGPU24, 5};
	gs_dirty_rect list[6];
	u32 n = 0, h = 0, j;
	int k;

	s_rng = 11;
	for (k = 0; k < 20000; k++)
	{
		u32 cnt = next_rand() % 6, tex_psm;
		int sw, sh;
		union gs_v4i_view tot;
		for (j = 0; j < cnt; j++)
		{
			int l = (int)(next_rand() % 2048);
			int t = (int)(next_rand() % 2048);
			int w = (int)(next_rand() % 600);
			int hh = (int)(next_rand() % 600);
			list[j].r = gs_v4i_set4(l, t, l + w, t + hh);
			list[j].psm = psms[next_rand() % 15];
			list[j].bw = next_rand() % 33;
			list[j].rgba._u32 = next_rand() & 15;
		}
		tex_psm = psms[next_rand() % 15];
		sw = (int)(next_rand() % 2100);
		sh = (int)(next_rand() % 2100);
		tot.v = gs_dirty_total_rect(list, cnt, tex_psm, sw, sh);
		put_rect(&n, tot.v);
		s_out[n++] = gs_dirty_channels(list, cnt);
		for (j = 0; j < cnt; j++)
		{
			int c0 = (int)(next_rand() % 1024);
			int c1 = (int)(next_rand() % 1024);
			int c2 = c0 + (int)(next_rand() % 1500);
			int c3 = c1 + (int)(next_rand() % 1500);
			int al = (int)(next_rand() & 1);
			union gs_v4i_view a, b;
			int i;
			a.v = gs_dirty_rect_clamped(&list[j], tex_psm, gs_v4i_set4(c0, c1, c2, c3), al);
			b.v = gs_dirty_rect_get(&list[j], tex_psm, !al);
			for (i = 0; i < 4; i++)
			{
				s_out[n++] = a.u32[i];
				s_out[n++] = b.u32[i];
			}
		}
		if (n > OUT_LEN - 128) { h ^= (u32)xxh3_64(s_out, n * 4, h); n = 0; }
	}
	h ^= (u32)xxh3_64(s_out, n * 4, h);
	CHECK(h == 0xc9bf6558U);
}

/* The CLUT: palettes loaded from random local memory in every mode and
 * read out, its CLD bookkeeping and invalidation, hashed: the sum is what
 * the C++ GSClut gave for the same calls */
static u64 clut_rand64(void)
{
	u64 hi = next_rand();
	u64 lo = next_rand();
	return hi << 32 | lo;
}

static void check_clut(void)
{
	static const u32 psms[9] = {PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH, PSMCT32, PSMCT16, 11, 12};
	static const u32 cpsms[6] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, 3, 9};
	const size_t vm_size = 4 * 1024 * 1024;
	gs_clut c;
	u8* vm;
	u32 n = 0, h = 0, i;
	int k;

	s_rng = 5;
	/* local memory is mapped wrapped: past the end reads the start */
	vm = (u8*)malloc(4 * vm_size);
	CHECK(vm != NULL);
	if (!vm)
		return;
	for (i = 0; i < vm_size / 4; i++)
	{
		u32 a = next_rand();
		u32 b = next_rand();
		((u32*)vm)[i] = a ^ (b << 16);
	}
	for (i = 1; i < 4; i++)
		memcpy(vm + i * vm_size, vm, vm_size);

	gs_clut_init(&c);
	gs_clut_reset(&c);
	for (k = 0; k < 60000; k++)
	{
		GIFRegTEX0 TEX0;
		GIFRegTEXCLUT TEXCLUT;
		GIFRegTEXA TEXA;
		u32 op;
		TEX0.U64 = clut_rand64();
		TEXCLUT.U64 = clut_rand64();
		TEXA.U64 = clut_rand64();
		TEX0.PSM = psms[next_rand() % 9];
		TEX0.CPSM = cpsms[next_rand() % 6];
		TEX0.CSM = (next_rand() % 4) == 0;
		TEX0.CLD = next_rand() % 8;
		TEXCLUT.CBW = next_rand() % 8;
		TEXCLUT.COU = next_rand() % 64;
		TEXCLUT.COV = next_rand() % 1024;
		op = next_rand() % 8;
		if (op == 0)
		{
			u32 st = next_rand() & 0x3fff;
			u32 e = st + (next_rand() % 64);
			s_out[n++] = (u32)gs_clut_invalidate_range(&c, st, e & 0x3fff, (int)(next_rand() & 1));
		}
		else if (op == 1)
			s_out[n++] = (u32)gs_clut_can_load(&c, &TEX0, (int)(next_rand() & 1));
		else if (op == 2)
		{
			c.write.next_tex0 = clut_rand64();
			s_out[n++] = c.write.dirty;
			if (c.write.dirty & 2)
				c.write.dirty = 1;
		}
		else if (op == 3)
			s_out[n++] = (u32)gs_clut_write_test(&c, &TEX0, &TEXCLUT);
		else if (op < 6)
			gs_clut_write(&c, vm, &TEX0, &TEXCLUT);
		else
		{
			int a, b;
			gs_clut_read32(&c, &TEX0, &TEXA);
			for (i = 0; i < 256; i++)
				s_out[n++] = c.buff32[i];
			gs_clut_alpha_minmax(&c, &a, &b);
			s_out[n++] = (u32)a;
			s_out[n++] = (u32)b;
		}
		s_out[n++] = (u32)c.write.TEX0.CBP | (u32)c.write.TEX0.CPSM << 16;
		if (n > OUT_LEN - 600) { h ^= (u32)xxh3_64(s_out, n * 4, h); n = 0; }
		if (k % 20000 == 19999)
			gs_clut_reset(&c);
	}
	h ^= (u32)xxh3_64(s_out, n * 4, h);
	CHECK(h == 0x2dd8493cU);
	gs_clut_deinit(&c);
	free(vm);
}

/* The pixel offsets of random frame and Z buffers and the page-to-tile
 * maps of random textures, each made once and found again, hashed: the
 * sum is what GSLocalMemory gave */
static void check_maps(void)
{
	static const u32 psms[15] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H,
		PSMT4HL, PSMT4HH, PSMZ32, PSMZ24, PSMZ16, PSMZ16S, PSGPU24, 7};
	gs_flat_map_t po4, p2t;
	u64 h = 0;
	int k;

	s_rng = 9;
	gs_flat_map_init(&po4, sizeof(u64), sizeof(GSPixelOffset4*));
	gs_flat_map_init(&p2t, sizeof(u64), sizeof(GSPage2TileMap*));
	for (k = 0; k < 600; k++)
	{
		GIFRegFRAME F;
		GIFRegZBUF Z;
		GIFRegTEX0 T;
		GSPixelOffset4* p;
		GSPage2TileMap* m;
		F.U64 = 0;
		Z.U64 = 0;
		T.U64 = 0;
		F.FBP = next_rand() % 512;
		F.FBW = next_rand() % 64;
		F.PSM = psms[next_rand() % 15];
		Z.ZBP = next_rand() % 512;
		Z.PSM = psms[next_rand() % 15];
		p = gs_pixel_offset4_get(&po4, &F, &Z);
		h = xxh3_64(p->row, sizeof(p->row), h);
		h = xxh3_64(p->col, sizeof(p->col), h);
		h ^= p->hash + p->fbp * 3 + p->zbp * 5 + p->fpsm * 7 + p->zpsm * 11 + p->bw * 13;
		T.TBP0 = next_rand() % 16384;
		T.TBW = next_rand() % 64;
		T.PSM = psms[next_rand() % 15];
		T.TW = next_rand() % 12;
		T.TH = next_rand() % 12;
		m = gs_page2tile_map_get(&p2t, &T);
		h = xxh3_64(m->first, sizeof(m->first), h);
		h = xxh3_64(m->entries, m->first[GS_MAX_PAGES] * 8, h);
		if (gs_pixel_offset4_get(&po4, &F, &Z) != p || gs_page2tile_map_get(&p2t, &T) != m)
			h ^= 1;
	}
	CHECK(h == 0x85f715263c20ec07ULL);
	gs_local_memory_map_free(&po4);
	gs_local_memory_map_free(&p2t);
}

int main(void)
{
	static u8 buf[1 << 16];
	size_t i;
	u32 psm;

	gs_util_init();
	check_tables();
	check_offsets();
	check_psm();
	check_context();
	check_dirty();
	check_clut();
	check_maps();

	/* Primitive classes. */
	CHECK(gs_util_get_prim_class(GS_POINTLIST) == GS_POINT_CLASS);
	CHECK(gs_util_get_prim_class(GS_LINESTRIP) == GS_LINE_CLASS);
	CHECK(gs_util_get_prim_class(GS_TRIANGLEFAN) == GS_TRIANGLE_CLASS);
	CHECK(gs_util_get_prim_class(GS_SPRITE) == GS_SPRITE_CLASS);
	CHECK(gs_util_get_prim_class(GS_INVALID) == GS_INVALID_CLASS);

	/* Every format is compatible and swizzles with itself, and shares
	 * no bits with itself. */
	for (psm = 0; psm < 64; psm++)
	{
		CHECK(gs_util_has_compatible_bits(psm, psm));
		CHECK(gs_util_has_same_swizzle_bits(psm, psm));
		CHECK(gs_util_has_shared_bits(psm, psm));
	}
	/* 24 and 32 bits cross, 16 and 16S cross, colour and depth do not. */
	CHECK(gs_util_has_compatible_bits(PSMCT32, PSMCT24) && gs_util_has_compatible_bits(PSMCT24, PSMCT32));
	CHECK(gs_util_has_compatible_bits(PSMZ16, PSMZ16S));
	CHECK(!gs_util_has_compatible_bits(PSMCT32, PSMCT16));
	CHECK(!gs_util_has_compatible_bits(PSMCT32, PSMZ32));
	/* The 8H/4HL/4HH formats live in a 32-bit word's alpha bits. */
	CHECK(gs_util_has_same_swizzle_bits(PSMT8H, PSMCT32) && gs_util_has_same_swizzle_bits(PSMCT32, PSMT4HH));
	CHECK(!gs_util_has_same_swizzle_bits(PSMT8, PSMCT32));
	/* ...so they coexist with CT24, which the shared-bits test says by
	 * returning false; and 4HL with 4HH. */
	CHECK(!gs_util_has_shared_bits(PSMT8H, PSMCT24) && !gs_util_has_shared_bits(PSMCT24, PSMT8H));
	CHECK(!gs_util_has_shared_bits(PSMT4HL, PSMT4HH) && !gs_util_has_shared_bits(PSMT4HH, PSMT4HL));
	CHECK(!gs_util_has_shared_bits(PSMT4HH, PSMZ24));
	CHECK(gs_util_has_shared_bits(PSMCT32, PSMCT24));
	CHECK(gs_util_has_shared_bits_ptr(PSMCT32, gs_util_shared_bits_ptr(PSMCT24)));
	CHECK(!gs_util_has_shared_bits_ptr(PSMT8H, gs_util_shared_bits_ptr(PSMCT24)));
	CHECK(gs_util_has_shared_bits_bp(0x100, PSMCT32, 0x100, PSMCT24));
	CHECK(!gs_util_has_shared_bits_bp(0x100, PSMCT32, 0x200, PSMCT24));
	/* Channels. */
	CHECK(gs_util_channel_mask(PSMCT32) == 0xf && gs_util_channel_mask(PSMCT24) == 0x7);
	CHECK(gs_util_channel_mask(PSMT8H) == 0x8 && gs_util_channel_mask(PSMZ24) == 0x7);
	CHECK(gs_util_channel_mask_fbmsk(PSMCT32, 0xFF0000FF) == 0x6);
	CHECK(gs_util_channel_mask_fbmsk(PSMCT24, 0x00FF0000) == 0x3);

	/* Short and long inputs, at odd lengths and offsets; 64 bytes and
	 * 1 KiB are the 16- and 256-entry CLUT keys. */
	srand(1);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (u8)rand();
	for (i = 0; i < 300; i += 7)
		check_hash(buf + 1, i);
	check_hash(buf, 64);
	check_hash(buf, 1024);
	check_hash(buf, 241);
	check_hash(buf + 3, 4096);
	check_hash(buf, sizeof(buf));
	check_hash(buf + 5, sizeof(buf) - 5);
	/* The cache's CLUT keys as it writes them: lengths known at compile
	 * time, which xxh3.h hashes in place up to 128 octets. */
	CHECK(xxh3_64(buf + 2, sizeof(u32) * 16, 0) == XXH3_64bits(buf + 2, 64));
	CHECK(xxh3_64(buf + 2, sizeof(u32) * 256, 0) == XXH3_64bits(buf + 2, 1024));

	if (fails)
	{
		printf("gsutil: %d failed\n", fails);
		return 1;
	}
	printf("gsutil: ok\n");
	return 0;
}
