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

int main(void)
{
	static u8 buf[1 << 16];
	size_t i;
	u32 psm;

	gs_util_init();
	check_tables();
	check_offsets();

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
