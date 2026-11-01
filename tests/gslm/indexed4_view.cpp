/* The 4-bit view of a 32-bit render target, as the Vulkan conversion
 * builds it, against GSLocalMemory.
 *
 * A PSMT4 read of a PSMCT32 target used to mean a CPU round trip, or,
 * without the CPU conversion hack, no look at the target at all: the
 * texture came from local memory, which the target had not been written
 * back to. ps_convert_rgba_8i now has a 4-bit mode on Vulkan that maps
 * every texel of the 4-bit page to the pixel and nibble it aliases, with
 * four tables (block4, column4, invblock32, invcolumn32) embedded in the
 * shader source.
 *
 * This reads those tables out of convert.glsl itself, runs the shader's
 * arithmetic over them, and compares every texel with what ReadPixel4
 * gives for the same memory that ReadPixel32 was read from: local memory
 * is filled with random words, so only the exact mapping matches. It
 * covers a texture from the target's first page and from later pages
 * (PageOffset), target widths that differ from the texture's (SBW vs
 * DBW), and texture widths from 0 up, odd ones included: GSLocalMemory
 * puts TBW / 2 4-bit pages in a row, as the shader's DBW / 128 does.
 *
 * Two negative controls: a swapped column4 entry, and a PageOffset one
 * page out, must both be caught.
 *
 * Usage: indexed4_view path/to/Vulkan/convert.glsl
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "GS/GSLocalMemory.h"

static unsigned block4[32];
static unsigned column4[512];
static unsigned invblock32[32];
static unsigned invcolumn32[64];

static long checks;
static long failures;

/* The values of "const uint NAME[N] = uint[N](" up to ");" in the shader
 * source, which is a C string literal file: quotes, \n and \t escapes and
 * the u suffixes are skipped. */
static int parse_table(const char* src, const char* name, unsigned* out, int n)
{
	char key[64];
	const char* p;
	int count = 0;

	snprintf(key, sizeof(key), "const uint %s[%d] = uint[%d](", name, n, n);
	p = strstr(src, key);
	if (!p)
	{
		printf("FAIL: %s not found in the shader\n", key);
		return 0;
	}
	p += strlen(key);
	while (*p && !(p[0] == ')' && p[1] == ';'))
	{
		if (*p == '\\')
			p += 2;
		else if (*p >= '0' && *p <= '9')
		{
			char* end;
			unsigned long v = strtoul(p, &end, 10);
			if (count < n)
				out[count] = (unsigned)v;
			count++;
			p = end;
		}
		else
			p++;
	}
	if (count != n)
	{
		printf("FAIL: %s has %d values, not %d\n", name, count, n);
		return 0;
	}
	return 1;
}

static u32 page_offset_of(u32 offset_x, u32 offset_y, u32 sbw)
{
	/* GSDevice::IndexedConversionPageOffset */
	u32 w = sbw < 64 ? 64 : sbw;
	return (offset_y / 32) * (w / 64) + offset_x / 64;
}

/* ps_convert_rgba_8i with DPSM == PSMT4, at scale 1: the pixel of the
 * PSMCT32 texture and the nibble of it that texel (x, y) reads. */
static void shader4(u32 x, u32 y, u32 sbw, u32 dbw, u32 page_offset, u32* px, u32* py, u32* nib)
{
	const u32 tx = x & 127, ty = y & 127;
	const u32 page = (y >> 7) * (dbw / 128) + (x >> 7) + page_offset;
	const u32 col = column4[((ty & 15) << 5) | (tx & 31)];
	const u32 word = block4[((ty >> 4) << 2) | (tx >> 5)] * 64 + (col >> 3);
	const u32 bxy = invblock32[word >> 6];
	const u32 pxy = invcolumn32[word & 63];

	*px = (((bxy & 7) << 3) | (pxy & 7)) + (page % (sbw / 64)) * 64;
	*py = (((bxy >> 3) << 3) | (pxy >> 3)) + (page / (sbw / 64)) * 32;
	*nib = col & 7;
}

/* Every texel of a TW x TH texture at TBP0 = target base + first_page
 * pages, width tbw, against ReadPixel4. Returns the mismatches. */
static long run_view(GSLocalMemory* mem, u32 target_bp, u32 target_bw, u32 first_page, u32 tbw, u32 w, u32 h, int po_skew)
{
	const u32 tbp = target_bp + first_page * 32;
	const u32 pgw = target_bw < 1 ? 1 : target_bw;
	const u32 off_x = (first_page % pgw) * 64;
	const u32 off_y = (first_page / pgw) * 32;
	const u32 po = page_offset_of(off_x, off_y, pgw * 64) + po_skew;
	/* The texture cache passes max(TBW, 1) * 64 for both widths. */
	const u32 dbw = (tbw < 1 ? 1 : tbw) * 64;
	long bad = 0;
	u32 x, y;

	for (y = 0; y < h; y++)
	{
		for (x = 0; x < w; x++)
		{
			u32 px, py, nib, word, got, want;

			shader4(x, y, pgw * 64, dbw, po, &px, &py, &nib);
			word = mem->ReadPixel32(px, py, target_bp, target_bw);
			got = (word >> (nib * 4)) & 15;
			want = mem->ReadPixel4(GSLocalMemory::PixelAddress4(x, y, tbp, tbw));
			if (got != want)
				bad++;
		}
	}
	return bad;
}

static void expect(int ok, const char* what)
{
	checks++;
	if (!ok)
	{
		failures++;
		printf("FAIL: %s\n", what);
	}
}

int main(int argc, char** argv)
{
	static const u32 target_widths[] = {1, 2, 3, 4, 5, 8, 10};
	static const u32 tex_widths[] = {0, 1, 2, 3, 4, 5, 6, 8, 10};
	static const u32 first_pages[] = {0, 1, 3, 7};
	GSLocalMemory mem;
	char what[160];
	char* src;
	long size;
	FILE* f;
	u32 i, a, b, c;

	if (argc < 2 || !(f = fopen(argv[1], "rb")))
	{
		printf("FAIL: usage: indexed4_view path/to/convert.glsl\n");
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	src = (char*)calloc(1, (size_t)size + 1);
	if (!src || fread(src, 1, (size_t)size, f) != (size_t)size)
	{
		printf("FAIL: cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(f);

	if (!parse_table(src, "block4", block4, 32) || !parse_table(src, "column4", column4, 512) ||
		!parse_table(src, "invblock32", invblock32, 32) || !parse_table(src, "invcolumn32", invcolumn32, 64))
		return 1;
	free(src);

	srand(4);
	for (i = 0; i < GSLocalMemory::m_vmsize; i++)
		mem.m_vm8[i] = (u8)(rand() >> 7);

	/* The texture cache's gate: a 32-bit target and a texture starting on
	 * one of its pages. 512x256 texels
	 * is four 4-bit pages across and two down at the widest, so the page
	 * arithmetic past the first row and column is in it. */
	for (a = 0; a < sizeof(target_widths) / sizeof(target_widths[0]); a++)
		for (b = 0; b < sizeof(tex_widths) / sizeof(tex_widths[0]); b++)
			for (c = 0; c < sizeof(first_pages) / sizeof(first_pages[0]); c++)
			{
				const u32 tw = tex_widths[b] >= 1 && tex_widths[b] * 64 < 512 ? tex_widths[b] * 64 : (tex_widths[b] ? 512 : 64);
				const long bad = run_view(&mem, 0x400, target_widths[a], first_pages[c], tex_widths[b], tw, 256, 0);
				snprintf(what, sizeof(what), "target TBW %u, texture TBW %u from page %u: %ld texels wrong",
					target_widths[a], tex_widths[b], first_pages[c], bad);
				expect(bad == 0, what);
			}

	/* Negative control: the texture taken a page late. */
	{
		const long bad = run_view(&mem, 0x400, 4, 1, 4, 256, 128, 1);
		snprintf(what, sizeof(what), "a PageOffset one page out is caught (%ld texels differ)", bad);
		expect(bad > 0, what);
	}

	/* Negative control: one swapped column4 pair must show. */
	{
		const unsigned t = column4[1];
		long bad;
		column4[1] = column4[2];
		column4[2] = t;
		bad = run_view(&mem, 0x400, 4, 0, 4, 256, 128, 0);
		snprintf(what, sizeof(what), "swapped column4 entries are caught (%ld texels differ)", bad);
		expect(bad > 0, what);
		column4[2] = column4[1];
		column4[1] = t;
	}

	printf("indexed4_view: %ld checks, %ld failures\n", checks, failures);
	return failures ? 1 : 0;
}
