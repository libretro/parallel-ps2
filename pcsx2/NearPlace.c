/* See NearPlace.h. C89. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* dl_iterate_phdr, MAP_ANONYMOUS */
#endif

#include <stdio.h>
#include <string.h>

#include "NearPlace.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
	defined(__OpenBSD__) || defined(__DragonFly__)
#define NEAR_HAVE_PHDR 1
#include <link.h>
#ifndef ElfW
#if UINTPTR_MAX > 0xffffffffu
#define ElfW(type) Elf64_##type
#else
#define ElfW(type) Elf32_##type
#endif
#endif
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

/* Where the image cannot be measured, it is assumed to span this much either
 * side of the anchor. */
#define NEAR_MODULE_SPAN ((uintptr_t)0x10000000)
/* Nothing is placed in the bottom of the address space. */
#define NEAR_FLOOR ((uintptr_t)0x10000)
/* Probe stride where the free ranges cannot be enumerated. */
#define NEAR_STEP ((uintptr_t)0x04000000)
#define NEAR_TOP (~(uintptr_t)0)

void near_hull(near_range_t *r, const near_range_t *add)
{
	if (add->lo < r->lo)
		r->lo = add->lo;
	if (add->hi > r->hi)
		r->hi = add->hi;
}

static void near_extent_around(uintptr_t a, near_range_t *out)
{
	out->lo = a > NEAR_MODULE_SPAN ? a - NEAR_MODULE_SPAN : 0;
	out->hi = a < NEAR_TOP - NEAR_MODULE_SPAN ? a + NEAR_MODULE_SPAN : NEAR_TOP;
}

#ifdef NEAR_HAVE_PHDR
typedef struct near_phdr_ctx
{
	near_range_t r;
	uintptr_t anchor;
	int found;
} near_phdr_ctx_t;

static int near_phdr_cb(struct dl_phdr_info *info, size_t info_size, void *data)
{
	near_phdr_ctx_t *c = (near_phdr_ctx_t *)data;
	near_range_t r;
	unsigned i;
	int hit = 0;

	(void)info_size;
	r.lo = NEAR_TOP;
	r.hi = 0;
	for (i = 0; i < (unsigned)info->dlpi_phnum; i++)
	{
		const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
		uintptr_t lo;
		uintptr_t hi;
		if (ph->p_type != PT_LOAD)
			continue;
		lo = (uintptr_t)info->dlpi_addr + (uintptr_t)ph->p_vaddr;
		hi = lo + (uintptr_t)ph->p_memsz;
		if (c->anchor >= lo && c->anchor < hi)
			hit = 1;
		if (lo < r.lo)
			r.lo = lo;
		if (hi > r.hi)
			r.hi = hi;
	}
	if (!hit)
		return 0;
	c->r = r;
	c->found = 1;
	return 1;
}
#endif

void near_image_extent(const void *anchor, near_range_t *out)
{
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(anchor, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.AllocationBase)
	{
		const unsigned char *base = (const unsigned char *)mbi.AllocationBase;
		const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
		if (dos->e_magic == IMAGE_DOS_SIGNATURE)
		{
			const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
			if (nt->Signature == IMAGE_NT_SIGNATURE)
			{
				out->lo = (uintptr_t)base;
				out->hi = out->lo + (uintptr_t)nt->OptionalHeader.SizeOfImage;
				return;
			}
		}
	}
#elif defined(NEAR_HAVE_PHDR)
	near_phdr_ctx_t c;
	c.r.lo = 0;
	c.r.hi = 0;
	c.anchor = (uintptr_t)anchor;
	c.found = 0;
	dl_iterate_phdr(near_phdr_cb, &c);
	if (c.found)
	{
		*out = c.r;
		return;
	}
#endif
	near_extent_around((uintptr_t)anchor, out);
}

int near_search_init(near_search_t *s, const near_range_t *target, size_t size,
		uintptr_t limit, uintptr_t align, uintptr_t ceiling)
{
	uintptr_t lo;
	uintptr_t hi;

	s->target    = *target;
	s->size      = size;
	s->align     = align ? align : 1;
	s->count     = 0;
	s->window.lo = 1;
	s->window.hi = 0;
	if (!size || (uintptr_t)size > limit || target->hi < target->lo
			|| target->hi - target->lo > limit)
		return 0;

	/* base >= target.hi - limit keeps the target's far end in reach of the
	 * base; base + size <= target.lo + limit keeps the reservation's far end
	 * in reach of the target's start. Together they bound the hull. */
	lo = target->hi > limit ? target->hi - limit : 0;
	if (lo < NEAR_FLOOR)
		lo = NEAR_FLOOR;
	hi = target->lo <= NEAR_TOP - limit ? target->lo + limit - size : NEAR_TOP - size;
	if (ceiling)
	{
		if (ceiling < (uintptr_t)size)
			return 0;
		if (hi > ceiling - size)
			hi = ceiling - size;
	}
	if (lo > NEAR_TOP - (s->align - 1))
		return 0;
	lo = (lo + s->align - 1) & ~(s->align - 1);
	hi &= ~(s->align - 1);
	if (lo > hi)
		return 0;
	s->window.lo = lo;
	s->window.hi = hi;
	return 1;
}

/* The hull is tightest with the base anywhere from here up to
 * max(target.lo, target.hi - size): the reservation then lies against the
 * target or covers it. */
static uintptr_t near_pref(const near_search_t *s)
{
	uintptr_t p = s->target.hi > (uintptr_t)s->size ? s->target.hi - s->size : 0;
	return p < s->target.lo ? p : s->target.lo;
}

void near_search_gap(near_search_t *s, uintptr_t lo, uintptr_t hi)
{
	uintptr_t mask = ~(s->align - 1);
	uintptr_t a;
	uintptr_t b;
	uintptr_t p;
	uintptr_t up;
	uintptr_t start;
	uintptr_t end;
	uintptr_t span;
	unsigned i;

	if (hi <= lo || hi - lo < (uintptr_t)s->size || s->window.lo > s->window.hi)
		return;
	a = lo > s->window.lo ? lo : s->window.lo;
	b = hi - s->size;
	if (b > s->window.hi)
		b = s->window.hi;
	if (a > NEAR_TOP - (s->align - 1))
		return;
	a = (a + s->align - 1) & mask;
	b &= mask;
	if (a > b)
		return;

	p = near_pref(s);
	if (p < a)
		p = a;
	else if (p > b)
		p = b;
	up = (p + s->align - 1) & mask;
	p  = up <= b ? up : (p & mask);

	start = p < s->target.lo ? p : s->target.lo;
	end   = p + s->size > s->target.hi ? p + s->size : s->target.hi;
	span  = end - start;

	if (s->count == NEAR_MAX_CANDIDATES && span >= s->span[NEAR_MAX_CANDIDATES - 1])
		return;
	i = s->count < NEAR_MAX_CANDIDATES ? s->count++ : NEAR_MAX_CANDIDATES - 1;
	while (i > 0 && s->span[i - 1] > span)
	{
		s->base[i] = s->base[i - 1];
		s->span[i] = s->span[i - 1];
		i--;
	}
	s->base[i] = p;
	s->span[i] = span;
}

#if defined(_WIN32) || (defined(__linux__) && !defined(NEAR_FORCE_STEPPED))
/* The last address a candidate's reservation can reach. */
static uintptr_t near_scan_top(const near_search_t *s)
{
	return s->window.hi <= NEAR_TOP - s->size ? s->window.hi + s->size : NEAR_TOP;
}
#endif

#if !defined(_WIN32)
/* Without a map of the address space, offer size-byte ranges stepping outward
 * from the target's edges; the kernel's acceptance of the hint is what tells
 * them apart. The target itself is occupied, so nothing is offered inside it. */
static void near_scan_stepped(near_search_t *s)
{
	uintptr_t mask = ~(s->align - 1);
	uintptr_t down = s->target.lo >= (uintptr_t)s->size ? (s->target.lo - s->size) & mask : 0;
	uintptr_t up   = s->target.hi <= NEAR_TOP - (s->align - 1) ? (s->target.hi + s->align - 1) & mask : NEAR_TOP;
	uintptr_t off  = 0;

	for (;;)
	{
		int d = s->target.lo >= (uintptr_t)s->size && off <= down && down - off >= s->window.lo;
		int u = up <= s->window.hi && off <= s->window.hi - up;
		if (!d && !u)
			break;
		if (d)
			near_search_gap(s, down - off, down - off + s->size);
		if (u)
			near_search_gap(s, up + off, up + off + s->size);
		if (off > NEAR_TOP - NEAR_STEP)
			break;
		off += NEAR_STEP;
	}
}
#endif

/* NEAR_FORCE_STEPPED: the harness's lane for the platforms without a map. */
#if defined(__linux__) && !defined(NEAR_FORCE_STEPPED)
static uintptr_t near_hex(const char **pp)
{
	const char *p = *pp;
	uintptr_t v   = 0;
	for (;;)
	{
		char c = *p;
		if (c >= '0' && c <= '9')
			v = (v << 4) | (uintptr_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = (v << 4) | (uintptr_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v = (v << 4) | (uintptr_t)(c - 'A' + 10);
		else
			break;
		p++;
	}
	*pp = p;
	return v;
}

/* /proc/self/maps lists the mappings in address order; the free ranges are
 * the spaces between them. Returns 0 when it cannot be read. */
static int near_scan_maps(near_search_t *s)
{
	char line[256];
	uintptr_t prev = 0;
	uintptr_t top  = near_scan_top(s);
	FILE *f        = fopen("/proc/self/maps", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
	{
		const char *p = line;
		size_t n      = strlen(line);
		int whole     = n && line[n - 1] == '\n';
		uintptr_t lo  = near_hex(&p);
		uintptr_t hi;

		/* A long path leaves the rest of its line for the next reads. */
		while (!whole && fgets(line, sizeof(line), f))
		{
			n     = strlen(line);
			whole = n && line[n - 1] == '\n';
		}
		if (*p != '-')
			continue;
		p++;
		hi = near_hex(&p);
		if (lo > prev)
			near_search_gap(s, prev, lo);
		if (hi > prev)
			prev = hi;
		if (prev >= top)
			break;
	}
	fclose(f);
	if (prev < top)
		near_search_gap(s, prev, top);
	return 1;
}
#endif

#if defined(_WIN32)
static void near_scan_query(near_search_t *s)
{
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = s->window.lo;
	uintptr_t top  = near_scan_top(s);

	while (addr < top && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		uintptr_t rlo = (uintptr_t)mbi.BaseAddress;
		uintptr_t rhi = rlo + (uintptr_t)mbi.RegionSize;
		if (mbi.State == MEM_FREE)
			near_search_gap(s, rlo, rhi);
		if (rhi <= addr)
			break;
		addr = rhi;
	}
}
#endif

void near_search_scan(near_search_t *s)
{
	if (s->window.lo > s->window.hi)
		return;
#if defined(_WIN32)
	near_scan_query(s);
#else
#if defined(__linux__) && !defined(NEAR_FORCE_STEPPED)
	if (near_scan_maps(s))
		return;
#endif
	near_scan_stepped(s);
#endif
}

static int near_probe(uintptr_t base, size_t size)
{
#if defined(_WIN32)
	void *p = VirtualAlloc((LPVOID)base, size, MEM_RESERVE, PAGE_NOACCESS);
	if (!p)
		return 0;
	VirtualFree(p, 0, MEM_RELEASE);
	return (uintptr_t)p == base;
#else
	void *p = mmap((void *)base, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED)
		return 0;
	munmap(p, size);
	return (uintptr_t)p == base;
#endif
}

int near_place_room(const near_range_t *target, size_t size, uintptr_t limit,
		uintptr_t align)
{
	near_search_t s;
	unsigned i;

	if (!near_search_init(&s, target, size, limit, align, 0))
		return 0;
	near_search_scan(&s);
	for (i = 0; i < s.count; i++)
		if (near_probe(s.base[i], size))
			return 1;
	return 0;
}
