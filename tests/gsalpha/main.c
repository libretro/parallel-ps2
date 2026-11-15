/* GS memory's backing and the texture cache's alpha ranges, from C89:
 *
 * GSAllocateWrappedMemory (GS/GSWrappedMemory.c): each view is the same
 * memory, a write through one seen through all, made and freed twice; a
 * size it cannot map gives NULL. Before, a failed mmap on POSIX gave
 * MAP_FAILED, which the caller took for memory and cleared.
 *
 * gs_rgba8_alpha_minmax (GS/GSAlphaRange.c): the least and greatest alpha
 * byte of rows of RGBA8 texels, any width (the last 1-3 texels of a row
 * left out of the vector loop), any stride and alignment, against the
 * bytes; and gs_lm_alpha_minmax's paletted answer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/Pcsx2Defs.h"
#include "GS/GSAlphaRange.h"

static void RETRO_CALLCONV quiet(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}
retro_log_printf_t log_cb = quiet;

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); fails++; } } while (0)

static u32 s_rng = 12345u;
static u32 rnd(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static void wrapped(void)
{
	const size_t size = 4u * 1024u * 1024u;
	int round;

	for (round = 0; round < 2; round++)
	{
		u8* p = (u8*)GSAllocateWrappedMemory(size, 4);
		u32 v, i, ok = 1;
		CHECK(p != NULL, "4MB four times: made");
		if (!p)
			return;
		for (i = 0; i < 64; i++)
		{
			const size_t at = (size_t)(rnd() % (size / 4)) * 4;
			v = rnd();
			*(volatile u32*)(p + (i & 3) * size + at) = v;
			{
				u32 view;
				for (view = 0; view < 4; view++)
					ok &= *(volatile u32*)(p + view * size + at) == v;
			}
		}
		CHECK(ok, "a write through one view is seen through all four");
		/* across the end of one view into the next: the wrap (volatile:
		 * to the compiler p[7] and p[size + 7] are different bytes, and
		 * it may read the one before writing the other) */
		{
			volatile u8* q = p;
			q[size - 2] = 0xA1;
			q[size - 1] = 0xB2;
			q[size]     = 0xC3;
			q[size + 1] = 0xD4;
			CHECK(q[0] == 0xC3 && q[1] == 0xD4 && q[2 * size - 2] == 0xA1 && q[3 * size + 1] == 0xD4,
				"a write over a view's end lands at the start");
		}
		GSFreeWrappedMemory(p, size, 4);
	}

	/* what cannot be mapped, or overflows: NULL, never a pointer that
	 * is not one */
	CHECK(GSAllocateWrappedMemory((size_t)1 << (sizeof(size_t) * 8 - 4), 4) == NULL, "too big: NULL");
	CHECK(GSAllocateWrappedMemory((size_t)-1 / 2, 4) == NULL, "size times repeat overflows: NULL");
	CHECK(GSAllocateWrappedMemory(size, 0) == NULL, "no views: NULL");
	/* and a good one after the failures */
	{
		volatile u8* p = (volatile u8*)GSAllocateWrappedMemory(size, 2);
		CHECK(p != NULL, "made again after failing");
		if (p)
		{
			p[size + 7] = 0x5a;
			CHECK(p[7] == 0x5a, "the two views of the remade memory");
			GSFreeWrappedMemory((void*)p, size, 2);
		}
	}
}

static void rgba8(void)
{
	static u8 buf[64 * 16 * 4 + 64];
	int n, bad = 0;

	for (n = 0; n < 4000; n++)
	{
		const u32 width  = rnd() % 41;
		const u32 height = 1 + rnd() % 9;
		const u32 stride = width * 4 + (rnd() % 4) * 4 + (rnd() % 2) * 2;
		u8* data         = buf + rnd() % 4;
		u8 want_min = 255, want_max = 0;
		GSAlphaMinMax got;
		u32 x, y, i;

		for (i = 0; i < sizeof(buf); i++)
			buf[i] = (u8)rnd();
		/* sometimes one alpha throughout, sometimes the extremes */
		if (n % 7 == 0)
			for (y = 0; y < height; y++)
				for (x = 0; x < width; x++)
					data[y * stride + x * 4 + 3] = 0x80;
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
			{
				const u8 a = data[y * stride + x * 4 + 3];
				want_min   = a < want_min ? a : want_min;
				want_max   = a > want_max ? a : want_max;
			}
		got = gs_rgba8_alpha_minmax(data, width, height, stride);
		if (got.min != want_min || got.max != want_max)
		{
			if (bad < 5)
				printf("  %ux%u stride %u: %02x..%02x, the bytes %02x..%02x\n", (unsigned)width, (unsigned)height,
					(unsigned)stride, got.min, got.max, want_min, want_max);
			bad++;
		}
	}
	CHECK(bad == 0, "RGBA8 alpha range: the bytes' least and greatest");
}

static void paletted(void)
{
	static u32 pal[256];
	gs_local_memory mem;
	gs_offset off;
	GIFRegTEXA ta;
	int r[4] = {0, 0, 16, 16};
	u8 amin = 0, amax = 0;
	u32 i;

	memset(&mem, 0, sizeof(mem));
	memset(&off, 0, sizeof(off));
	ta.U64 = 0;
	for (i = 0; i < 256; i++)
		pal[i] = 0x40000000u | i;
	mem.m_clut32 = pal;
	CHECK(gs_lm_alpha_minmax(&mem, &off, r, PSMT8, &ta, &amin, &amax) && amin == 0x40 && amax == 0x40,
		"8 bits, one alpha: it");
	pal[200] = 0x7f000000u;
	CHECK(!gs_lm_alpha_minmax(&mem, &off, r, PSMT8, &ta, &amin, &amax), "8 bits, two alphas: declined");
	CHECK(gs_lm_alpha_minmax(&mem, &off, r, PSMT4HH, &ta, &amin, &amax) && amin == 0x40,
		"4 bits read 16 entries only");
	CHECK(!gs_lm_alpha_minmax(&mem, &off, r, 5, &ta, &amin, &amax), "a format it does not read: declined");
}

int main(void)
{
	wrapped();
	rgba8();
	paletted();
	printf(fails ? "gsalpha: FAILED (%d)\n" : "gsalpha: ok\n", fails);
	return fails != 0;
}
