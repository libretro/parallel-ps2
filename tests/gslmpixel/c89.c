/* The C local memory (GS/GSLocalMemory.h) from C89, alone: each format's
 * writer and reader agree, frames convert both ways, texels expand by
 * TEXA and read through the palette, and the rectangle arithmetic gives
 * what the page sizes say. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "GS/GSLocalMemory.h"
#include "GS/GSTables.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

void* GSAllocateWrappedMemory(size_t size, size_t repeat) { return calloc(size, repeat); }
void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat) { (void)size; (void)repeat; free(ptr); }
void gs_local_memory_populate_transfers(void) {}
void gs_local_memory_read_texture_blocks(gs_local_memory* mem, const gs_offset* off, const int r[4], u8* dst,
	int dstpitch, const GIFRegTEXA* TEXA)
{
	(void)mem; (void)off; (void)r; (void)dst; (void)dstpitch; (void)TEXA;
	fails++;
	printf("  FAIL: whole blocks asked of a rectangle with none\n");
}

int main(void)
{
	static const u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
		PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
	static u32 pal[256];
	gs_local_memory m;
	GIFRegTEXA ta;
	GIFRegTEX0 t0;
	gs_offset off;
	int r[4], out[4];
	u32 i, k, v, want;
	u32 tex[4 * 3];

	gs_tables_init();
	gs_psm_init();
	CHECK(gs_local_memory_init(&m), "init");
	for (i = 0; i < 256; i++)
		pal[i] = 0xff000000u | i * 0x010101u;
	m.m_clut32 = pal;

	for (k = 0; k < sizeof(psms) / sizeof(psms[0]); k++)
	{
		const u32 psm  = psms[k];
		const u32 bits = gs_psm[psm].trbpp;
		const u32 mask = bits >= 32 ? 0xffffffffu : (1u << bits) - 1;
		int ok         = 1;
		for (i = 0; i < 64; i++)
		{
			const int x = (int)(i * 7 % 61), y = (int)(i * 13 % 47);
			v = (i * 0x9E3779B9u) & mask;
			gs_lm_pixel[psm].wp(&m, x, y, v, 0x100, 2);
			if (gs_lm_pixel[psm].rp(&m, x, y, 0x100, 2) != v)
				ok = 0;
		}
		if (!ok)
			printf("  FAIL: psm %u: what is written is not what is read\n", (unsigned)psm);
		fails += !ok;
	}

	/* a frame of 16 bits: 5 bits a channel and alpha's top, both ways */
	gs_lm_write_frame16(&m, 5, 0x80f8f8f8u);
	CHECK(gs_lm_read_pixel16(&m, 5) == 0xffff, "frame16 written");
	CHECK(gs_lm_read_frame16(&m, 5) == 0x80f8f8f8u, "frame16 read");
	/* texels: 24 bits take TA0, unless AEM and black; 16 bits by their top bit */
	ta.U64 = 0;
	ta.TA0 = 0x11;
	ta.TA1 = 0x22;
	CHECK(gs_lm_expand24(0x000000, &ta) == 0x11000000u, "24: TA0");
	ta.AEM = 1;
	CHECK(gs_lm_expand24(0x000000, &ta) == 0, "24: AEM black");
	CHECK(gs_lm_expand16(0x8000, &ta) == 0x22000000u, "16: TA1");
	CHECK(gs_lm_expand16(0x7fff, &ta) == 0x11f8f8f8u, "16: TA0");
	/* a paletted texel is its palette entry */
	gs_lm_write_pixel8(&m, 77, 200);
	CHECK(gs_lm_read_texel8(&m, 77, &ta) == pal[200], "8: the palette");

	/* pages of 32 bits are 64x32 */
	r[0] = 64; r[1] = 32; r[2] = 128; r[3] = 33;
	CHECK(gs_lm_page_aligned_mask(PSMCT32, r) == 0x0fff, "page aligned but the bottom");
	gs_lm_rect_for_page_offset(0, 3 * 32, 2, PSMCT32, out);
	CHECK(out[0] == 64 && out[1] == 32 && out[2] == 128 && out[3] == 64, "page 3 of a 2-page-wide buffer");
	gs_lm_rect_for_page_offset(0, 3 * 32, 0, PSMT8, out);
	CHECK(out[0] == 0 && out[1] == 3 * 64 && out[2] == 128 && out[3] == 4 * 64, "width 0: one page wide");

	/* a rectangle smaller than a block: texel by texel */
	t0.U64 = 0;
	t0.TBP0 = 0x100;
	t0.TBW  = 2;
	off = gs_offset_make(&gs_psm[PSMCT32].info, 0x100, 2, PSMCT32);
	r[0] = 3; r[1] = 1; r[2] = 7; r[3] = 4;
	memset(tex, 0, sizeof(tex));
	gs_local_memory_read_texture(&m, &off, r, (u8*)tex, 16, &ta);
	want = gs_lm_pixel[PSMCT32].rt(&m, 5, 2, &t0, &ta);
	CHECK(tex[1 * 4 + 2] == want, "a texel of a ragged rectangle");

	gs_local_memory_deinit(&m);
	CHECK(m.m_vm8 == NULL, "deinit");
	printf(fails ? "gslmpixel c89: FAILED (%d)\n" : "gslmpixel c89: ok\n", fails);
	return fails != 0;
}
