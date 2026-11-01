/* The VRAM pages a paraLLEl-GS state load writes (PGSStateVRAM.h).
 *
 * Defrost writes, page run by page run, only where the state's VRAM
 * differs from what is there: each page written loses its texture cache
 * and super samples, and on a discrete GPU is uploaded. Held here
 * against a page-by-page compare, over a VRAM-sized buffer with pages
 * changed alone, in runs, at both ends, everywhere and nowhere, and with
 * only the first or last byte of a page changed. Every changed page must
 * be in exactly one run, no unchanged page in any, and two runs never
 * touch (they would be one).
 *
 * A walk that drops a run ending at the last page is the negative
 * control: it must be caught. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../pcsx2/GS/Renderers/parallel-gs/PGSStateVRAM.h"

#define VRAM  (4u * 1024u * 1024u)
#define PAGE  (8u * 1024u)
#define PAGES (VRAM / PAGE)

static unsigned char s_cur[VRAM];
static unsigned char s_next[VRAM];
static unsigned      s_rng = 1;

static unsigned rnd(void)
{
	s_rng = s_rng * 1103515245u + 12345u;
	return s_rng >> 8;
}

/* 0 good; 1 the control, which stops before a run that reaches the end. */
static int check(int control)
{
	static unsigned char seen[PAGES];
	size_t offset = 0, size = 0, prev_end = (size_t)-1;
	unsigned p;
	int bad = 0;

	memset(seen, 0, sizeof(seen));
	while ((offset = pgs_next_changed_run(s_cur, s_next, VRAM, PAGE, offset, &size)) < VRAM)
	{
		if (control && offset + size == VRAM)
			break;
		if (size == 0 || offset % PAGE || size % PAGE || offset == prev_end)
			bad = 1;
		for (p = (unsigned)(offset / PAGE); p < (unsigned)((offset + size) / PAGE); p++)
			seen[p]++;
		prev_end = offset + size;
		offset += size;
	}
	for (p = 0; p < PAGES; p++)
	{
		const int changed = memcmp(s_cur + p * PAGE, s_next + p * PAGE, PAGE) != 0;
		if (seen[p] != (changed ? 1 : 0))
			bad = 1;
	}
	return bad;
}

int main(void)
{
	unsigned t, i, p;
	int bad = 0, caught = 0, cases = 0;

	for (i = 0; i < VRAM; i++)
		s_cur[i] = (unsigned char)rnd();

	for (t = 0; t < 64; t++)
	{
		memcpy(s_next, s_cur, VRAM);
		switch (t % 8)
		{
			case 0: break;                                             /* nothing */
			case 1: s_next[0] ^= 1; break;                             /* first byte */
			case 2: s_next[VRAM - 1] ^= 1; break;                      /* last byte */
			case 3: for (i = 0; i < VRAM; i += PAGE) s_next[i] ^= 0x80; break; /* everywhere */
			case 4: s_next[(rnd() % PAGES) * PAGE + PAGE - 1] ^= 1; break; /* a page's last byte */
			default:
				for (i = 0; i < 1 + rnd() % 8; i++)
				{
					const unsigned first = rnd() % PAGES;
					const unsigned n = 1 + rnd() % 6;
					for (p = first; p < first + n && p < PAGES; p++)
						s_next[p * PAGE + rnd() % PAGE] ^= (unsigned char)(1 + rnd() % 255);
				}
				break;
		}
		cases++;
		if (check(0))
		{
			printf("  case %u: runs differ from the page compare\n", t);
			bad = 1;
		}
		if (check(1))
			caught++;
	}
	printf("  page runs: %d VRAM images, runs match the page compare: %s\n", cases, bad ? "no" : "yes");
	if (!caught)
	{
		printf("  the control that drops a run at the end was never caught\n");
		bad = 1;
	}
	else
		printf("  control (run at the end dropped): caught on %d\n", caught);
	printf(bad ? "FAIL\n" : "PASS\n");
	return bad;
}
