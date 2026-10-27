/* Placement of the host reservations (pcsx2/NearPlace.c). C89.
 *
 * Synthetic lanes feed address-space layouts straight to the search and check
 * the reach invariant on every candidate it offers: the hull of the image,
 * the main reservation and the code reservation spans at most RIP_LIMIT, so
 * every rel32 operand the recompilers emit is encodable.
 *
 * Live lanes run against this process: the measured image covers a 320MB
 * .bss array (an image well past any fixed span estimate), a main + code pair
 * placed for real keeps that invariant, and with the address space around the
 * image reserved except for one hole off any fixed stride grid, the search
 * still finds the hole. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "NearPlace.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

#define RIP_LIMIT     ((uintptr_t)0x7C000000)
#define CODE_SIZE     ((size_t)0x13100000)
#define MAIN_SIZE     ((size_t)0x14000000)
#define RESERVE_ALIGN ((uintptr_t)0x10000)
#define STRIDE        ((uintptr_t)0x04000000)
#define TOP           (~(uintptr_t)0)
/* 64-bit constants without C99 integer literals */
#define HI(hi32, lo32) (((uintptr_t)(hi32) << 32) | (uintptr_t)(lo32))

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { failures++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

#if UINTPTR_MAX > 0xffffffffu

/* ---- synthetic layouts ------------------------------------------------- */

static uintptr_t rng_state = HI(0x9E3779B9u, 0x7F4A7C15u);

static uintptr_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static uintptr_t hull_span(const near_range_t *a, uintptr_t base, size_t size)
{
	uintptr_t lo = base < a->lo ? base : a->lo;
	uintptr_t hi = base + size > a->hi ? base + size : a->hi;
	return hi - lo;
}

#define MAX_GAPS 64

typedef struct layout
{
	near_range_t gap[MAX_GAPS];
	unsigned count;
} layout_t;

/* Busy mappings at random places around the image; everything between them
 * is free. */
static void make_layout(layout_t *l, const near_range_t *image)
{
	uintptr_t busy_lo[MAX_GAPS];
	uintptr_t busy_hi[MAX_GAPS];
	uintptr_t prev;
	unsigned nbusy = 1 + (unsigned)(rng() % (MAX_GAPS - 2));
	unsigned i;
	unsigned j;

	busy_lo[0] = image->lo;
	busy_hi[0] = image->hi;
	for (i = 1; i < nbusy; i++)
	{
		uintptr_t span = HI(3, 0);
		uintptr_t c    = image->lo - span / 2 + rng() % span;
		busy_lo[i]     = c & ~(uintptr_t)0xFFF;
		busy_hi[i]     = busy_lo[i] + 0x1000 + (rng() % 0x08000000 & ~(uintptr_t)0xFFF);
	}
	for (i = 1; i < nbusy; i++)
	{
		uintptr_t lo = busy_lo[i];
		uintptr_t hi = busy_hi[i];
		for (j = i; j > 0 && busy_lo[j - 1] > lo; j--)
		{
			busy_lo[j] = busy_lo[j - 1];
			busy_hi[j] = busy_hi[j - 1];
		}
		busy_lo[j] = lo;
		busy_hi[j] = hi;
	}
	l->count = 0;
	prev     = 0x10000;
	for (i = 0; i < nbusy; i++)
	{
		if (busy_lo[i] > prev)
		{
			l->gap[l->count].lo = prev;
			l->gap[l->count].hi = busy_lo[i];
			l->count++;
		}
		if (busy_hi[i] > prev)
			prev = busy_hi[i];
	}
	l->gap[l->count].lo = prev;
	l->gap[l->count].hi = HI(0x7FFF, 0xFFFF0000u);
	l->count++;
}

static void feed(near_search_t *s, const layout_t *l)
{
	unsigned i;
	for (i = 0; i < l->count; i++)
		near_search_gap(s, l->gap[i].lo, l->gap[i].hi);
}

static int inside_gap(const layout_t *l, uintptr_t base, size_t size)
{
	unsigned i;
	for (i = 0; i < l->count; i++)
		if (base >= l->gap[i].lo && base + size <= l->gap[i].hi)
			return 1;
	return 0;
}

/* Every candidate lies in free space, on the alignment, in order of hull
 * span, under the ceiling, and keeps the hull with its target within limit. */
static void check_candidates(const near_search_t *s, const layout_t *l, const near_range_t *target,
		size_t size, uintptr_t limit, uintptr_t ceiling)
{
	unsigned i;
	for (i = 0; i < s->count; i++)
	{
		uintptr_t b = s->base[i];
		CHECK(inside_gap(l, b, size), "candidate outside free space");
		CHECK((b & (RESERVE_ALIGN - 1)) == 0, "candidate misaligned");
		CHECK(hull_span(target, b, size) <= limit, "candidate out of reach of its target");
		CHECK(hull_span(target, b, size) == s->span[i], "recorded span wrong");
		CHECK(!ceiling || b + size <= ceiling, "candidate above the ceiling");
		CHECK(i == 0 || s->span[i - 1] <= s->span[i], "candidates out of order");
	}
}

/* An image of any size, main then code: whichever main candidate is taken
 * (the far end of its window included), every code candidate reaches both
 * main and every byte of the image. */
static void lane_fuzz(void)
{
	unsigned iter;
	unsigned with_code = 0;

	for (iter = 0; iter < 20000; iter++)
	{
		layout_t l;
		near_search_t m;
		near_search_t c;
		near_range_t image;
		near_range_t target;
		near_range_t main_r;
		uintptr_t ceiling = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
		uintptr_t pick;
		unsigned i;

		image.lo = (HI(0x7F00, 0) + (rng() % HI(1, 0))) & ~(uintptr_t)0xFFF;
		image.hi = image.lo + 0x100000 + (rng() % 0x30000000);
		make_layout(&l, &image);

		if (!near_search_init(&m, &image, MAIN_SIZE, RIP_LIMIT - CODE_SIZE, RESERVE_ALIGN, ceiling))
		{
			CHECK(image.hi - image.lo > RIP_LIMIT - CODE_SIZE - MAIN_SIZE, "main window refused for a small image");
			continue;
		}
		feed(&m, &l);
		check_candidates(&m, &l, &image, MAIN_SIZE, RIP_LIMIT - CODE_SIZE, ceiling);
		if (!m.count)
			continue;

		/* Take the worst-placed main, as a crowded host would force. */
		pick           = m.base[m.count - 1];
		main_r.lo = pick;
		main_r.hi = pick + MAIN_SIZE;
		target    = image;
		near_hull(&target, &main_r);
		CHECK(target.hi - target.lo <= RIP_LIMIT - CODE_SIZE, "main leaves no room for code");

		/* The main range is no longer free for the code. */
		for (i = 0; i < l.count; i++)
			if (pick >= l.gap[i].lo && pick + MAIN_SIZE <= l.gap[i].hi)
			{
				if (l.count < MAX_GAPS)
				{
					l.gap[l.count].lo = pick + MAIN_SIZE;
					l.gap[l.count].hi = l.gap[i].hi;
					l.count++;
				}
				l.gap[i].hi = pick;
				break;
			}

		CHECK(near_search_init(&c, &target, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "code window refused");
		feed(&c, &l);
		check_candidates(&c, &l, &target, CODE_SIZE, RIP_LIMIT, 0);
		for (i = 0; i < c.count; i++)
		{
			CHECK(hull_span(&image, c.base[i], CODE_SIZE) <= RIP_LIMIT, "code out of reach of the image");
			CHECK(hull_span(&main_r, c.base[i], CODE_SIZE) <= RIP_LIMIT, "code out of reach of main");
			CHECK(c.base[i] + CODE_SIZE <= pick || c.base[i] >= pick + MAIN_SIZE, "code overlaps main");
		}
		if (c.count)
			with_code++;
	}
	CHECK(with_code > 1000, "fuzz placed too few code reservations to mean anything");
	printf("fuzz: %u layouts with main and code placed\n", with_code);
}

/* One free hole near the image, off any fixed stride from it: the only
 * candidate is that hole. */
static void lane_one_hole(void)
{
	near_search_t s;
	near_range_t image;
	uintptr_t hole;

	image.lo = HI(0x7F12, 0x34560000u);
	image.hi = image.lo + 0x1A000000;
	hole     = image.lo - 0x48000000 - STRIDE / 2 - CODE_SIZE;
	CHECK(near_search_init(&s, &image, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "window refused");
	near_search_gap(&s, 0x10000, image.lo - 0x80000000u);
	near_search_gap(&s, hole, hole + CODE_SIZE);
	CHECK(s.count == 1 && s.base[0] == hole, "the one hole in reach was not found");

	/* Too small by a page, or entirely out of reach: nothing. */
	CHECK(near_search_init(&s, &image, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "window refused");
	near_search_gap(&s, hole, hole + CODE_SIZE - 0x1000);
	near_search_gap(&s, image.hi + 0x80000000u, image.hi + HI(1, 0));
	near_search_gap(&s, image.lo - HI(1, 0), image.lo - 0x7C000000u + 0x1A000000 - 1);
	CHECK(s.count == 0, "a hole too small or out of reach was offered");
}

/* Many holes, offered in random order: the closest NEAR_MAX_CANDIDATES are
 * kept, closest first. */
static void lane_best_k(void)
{
	near_search_t s;
	near_range_t image;
	uintptr_t order[200];
	unsigned i;

	image.lo = HI(0x7F00, 0);
	image.hi = image.lo + 0x02000000;
	for (i = 0; i < 200; i++)
		order[i] = i;
	for (i = 199; i > 0; i--)
	{
		unsigned j   = (unsigned)(rng() % (i + 1));
		uintptr_t t  = order[i];
		order[i]     = order[j];
		order[j]     = t;
	}
	CHECK(near_search_init(&s, &image, 0x100000, RIP_LIMIT, RESERVE_ALIGN, 0), "window refused");
	for (i = 0; i < 200; i++)
	{
		uintptr_t lo = image.hi + 0x01000000 + order[i] * 0x00400000;
		near_search_gap(&s, lo, lo + 0x00200000);
	}
	CHECK(s.count == NEAR_MAX_CANDIDATES, "best-K list not full");
	for (i = 0; i < s.count; i++)
		CHECK(s.base[i] == image.hi + 0x01000000 + (uintptr_t)i * 0x00400000, "best-K list not the closest, in order");
}

/* Targets against either end of the address space do not wrap. */
static void lane_edges(void)
{
	near_search_t s;
	near_range_t t;

	t.lo = 0x20000;
	t.hi = 0x01000000;
	CHECK(near_search_init(&s, &t, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "low window refused");
	CHECK(s.window.lo >= 0x10000, "window below the floor");
	near_search_gap(&s, 0, TOP);
	CHECK(s.count == 1 && hull_span(&t, s.base[0], CODE_SIZE) <= RIP_LIMIT, "low target misplaced");

	t.lo = TOP - 0x02000000;
	t.hi = TOP;
	CHECK(near_search_init(&s, &t, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "high window refused");
	near_search_gap(&s, 0, TOP);
	CHECK(s.count == 1 && hull_span(&t, s.base[0], CODE_SIZE) <= RIP_LIMIT
			&& s.base[0] + CODE_SIZE > s.base[0], "high target misplaced");

	CHECK(!near_search_init(&s, &t, MAIN_SIZE, RIP_LIMIT, RESERVE_ALIGN,
			(uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1)), "window under a ceiling far below the target");
}

/* ---- live lanes -------------------------------------------------------- */

/* Large enough that no fixed span estimate around one address covers it. */
static unsigned char big_bss[0x14000000];
static int data_global = 1;

static int reserve_at(uintptr_t base, size_t size)
{
#if defined(_WIN32)
	return VirtualAlloc((LPVOID)base, size, MEM_RESERVE, PAGE_NOACCESS) == (LPVOID)base;
#else
	void *p = mmap((void *)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return 0;
	if ((uintptr_t)p != base)
	{
		munmap(p, size);
		return 0;
	}
	return 1;
#endif
}

static void release_at(uintptr_t base, size_t size)
{
#if defined(_WIN32)
	(void)size;
	VirtualFree((LPVOID)base, 0, MEM_RELEASE);
#else
	munmap((void *)base, size);
#endif
}

static void lane_live_extent(near_range_t *image)
{
	near_image_extent((const void *)&data_global, image);
	CHECK(image->lo <= (uintptr_t)&lane_live_extent && (uintptr_t)&lane_live_extent < image->hi, "extent misses the text");
	CHECK(image->lo <= (uintptr_t)&data_global && (uintptr_t)&data_global < image->hi, "extent misses .data");
	CHECK(image->lo <= (uintptr_t)&big_bss[0] && (uintptr_t)&big_bss[sizeof(big_bss) - 1] < image->hi,
			"extent misses the end of .bss");
	big_bss[data_global] = 1;
}

static void lane_live_pair(const near_range_t *image)
{
	near_search_t m;
	near_search_t c;
	near_range_t target;
	near_range_t main_r;
	uintptr_t ceiling = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
	unsigned i;
	int placed = 0;

	CHECK(near_search_init(&m, image, MAIN_SIZE, RIP_LIMIT - CODE_SIZE, RESERVE_ALIGN, ceiling), "main window refused");
	near_search_scan(&m);
	for (i = 0; i < m.count && !placed; i++)
	{
		if (!reserve_at(m.base[i], MAIN_SIZE))
			continue;
		main_r.lo = m.base[i];
		main_r.hi = m.base[i] + MAIN_SIZE;
		target    = *image;
		near_hull(&target, &main_r);
		if (!near_place_room(&target, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN))
		{
			release_at(m.base[i], MAIN_SIZE);
			continue;
		}
		CHECK(near_search_init(&c, &target, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "code window refused");
		near_search_scan(&c);
		CHECK(c.count > 0, "room reported but no code candidate");
		if (c.count && reserve_at(c.base[0], CODE_SIZE))
		{
			CHECK(hull_span(&target, c.base[0], CODE_SIZE) <= RIP_LIMIT, "live code out of reach");
			release_at(c.base[0], CODE_SIZE);
			placed = 1;
		}
		release_at(m.base[i], MAIN_SIZE);
	}
	CHECK(placed, "no live main + code placement");
}

#ifndef NEAR_FORCE_STEPPED
/* Free ranges of this process, for the crowding lane. */
typedef void (*gap_fn)(uintptr_t lo, uintptr_t hi, void *ctx);

static void each_gap(uintptr_t lo, uintptr_t hi, gap_fn fn, void *ctx)
{
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = lo;
	while (addr < hi && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		uintptr_t rlo = (uintptr_t)mbi.BaseAddress;
		uintptr_t rhi = rlo + (uintptr_t)mbi.RegionSize;
		if (mbi.State == MEM_FREE)
			fn(rlo > lo ? rlo : lo, rhi < hi ? rhi : hi, ctx);
		if (rhi <= addr)
			break;
		addr = rhi;
	}
#else
	char line[512];
	uintptr_t prev = 0;
	FILE *f        = fopen("/proc/self/maps", "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f))
	{
		unsigned long a;
		unsigned long b;
		if (sscanf(line, "%lx-%lx", &a, &b) != 2)
			continue;
		if ((uintptr_t)a > prev && (uintptr_t)a > lo && prev < hi)
			fn(prev > lo ? prev : lo, (uintptr_t)a < hi ? (uintptr_t)a : hi, ctx);
		if ((uintptr_t)b > prev)
			prev = (uintptr_t)b;
	}
	fclose(f);
	if (prev < hi)
		fn(prev > lo ? prev : lo, hi, ctx);
#endif
}

#define MAX_HELD 4096

typedef struct crowd
{
	uintptr_t held_lo[MAX_HELD];
	uintptr_t held_len[MAX_HELD];
	uintptr_t down;
	uintptr_t up;
	uintptr_t win_lo;
	uintptr_t win_hi;
	uintptr_t hole;
	unsigned held;
} crowd_t;

/* A hole half a stride off the grid a fixed-stride probe would step through
 * outward from the image's edges. */
static void find_hole(uintptr_t lo, uintptr_t hi, void *ctx)
{
	crowd_t *c = (crowd_t *)ctx;
	uintptr_t h;
	if (c->hole || hi - lo < CODE_SIZE + 2 * RESERVE_ALIGN)
		return;
	h = (lo + RESERVE_ALIGN - 1) & ~(RESERVE_ALIGN - 1);
	if (h >= c->up)
		h += (STRIDE / 2 - (h - c->up) % STRIDE + STRIDE) % STRIDE;
	else if (h <= c->down)
		h += ((c->down - h) % STRIDE + STRIDE / 2) % STRIDE;
	if (h > c->down && h < c->up)
		return;
	if (h >= c->win_lo && h <= c->win_hi && h + CODE_SIZE <= hi)
		c->hole = h;
}

static void hold(crowd_t *c, uintptr_t lo, uintptr_t hi)
{
	lo = (lo + RESERVE_ALIGN - 1) & ~(RESERVE_ALIGN - 1);
	hi &= ~(RESERVE_ALIGN - 1);
	if (hi <= lo || c->held == MAX_HELD)
		return;
	c->held_lo[c->held]  = lo;
	c->held_len[c->held] = hi - lo;
	c->held++;
}

static void fill_gap(uintptr_t lo, uintptr_t hi, void *ctx)
{
	crowd_t *c = (crowd_t *)ctx;
	if (c->hole >= lo && c->hole < hi)
	{
		hold(c, lo, c->hole);
		hold(c, c->hole + CODE_SIZE, hi);
	}
	else
		hold(c, lo, hi);
}

static crowd_t crowd;

static void lane_live_crowded(const near_range_t *image)
{
	near_search_t s;
	unsigned i;

	CHECK(near_search_init(&s, image, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "window refused");
	crowd.down   = (image->lo - CODE_SIZE) & ~(RESERVE_ALIGN - 1);
	crowd.up     = (image->hi + RESERVE_ALIGN - 1) & ~(RESERVE_ALIGN - 1);
	crowd.win_lo = s.window.lo;
	crowd.win_hi = s.window.hi;
	crowd.hole   = 0;
	crowd.held   = 0;
	each_gap(s.window.lo, s.window.hi + CODE_SIZE, find_hole, &crowd);
	if (!crowd.hole)
	{
		printf("crowded: SKIP (no free range in reach to cut the hole from)\n");
		return;
	}
	each_gap(s.window.lo > 0x10000000 ? s.window.lo - 0x10000000 : 0x10000,
			s.window.hi + CODE_SIZE + 0x10000000, fill_gap, &crowd);
	/* Reserved only now: the enumeration reads the map these change. */
	for (i = 0; i < crowd.held; i++)
		if (!reserve_at(crowd.held_lo[i], crowd.held_len[i]))
			crowd.held_len[i] = 0;

	CHECK(near_search_init(&s, image, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN, 0), "window refused");
	near_search_scan(&s);
	CHECK(s.count >= 1 && s.base[0] == crowd.hole, "the only hole in reach was not found");
	CHECK(near_place_room(image, CODE_SIZE, RIP_LIMIT, RESERVE_ALIGN), "room not found in the only hole");
	printf("crowded: %u ranges held, hole %s\n", crowd.held,
			s.count && s.base[0] == crowd.hole ? "found" : "MISSED");

	for (i = 0; i < crowd.held; i++)
		if (crowd.held_len[i])
			release_at(crowd.held_lo[i], crowd.held_len[i]);
}
#endif

int main(void)
{
	near_range_t image;

	lane_fuzz();
	lane_one_hole();
	lane_best_k();
	lane_edges();
	lane_live_extent(&image);
	printf("image: %lu MB measured\n", (unsigned long)((image.hi - image.lo) >> 20));
	lane_live_pair(&image);
#ifdef NEAR_FORCE_STEPPED
	/* A fixed-stride probe finds a lone hole only by luck: that is what the
	 * address map is for. */
	printf("crowded: SKIP (stepped probe)\n");
#else
	lane_live_crowded(&image);
#endif

	if (failures)
	{
		printf("nearplace: %d FAILED\n", failures);
		return 1;
	}
	printf("nearplace: all passed\n");
	return 0;
}

#else

int main(void)
{
	printf("nearplace: SKIP (32-bit host)\n");
	return 0;
}

#endif
