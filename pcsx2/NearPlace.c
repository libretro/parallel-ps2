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
#endif

/* Where the image cannot be measured, it is assumed to span this much either
 * side of the anchor. */
#define NEAR_MODULE_SPAN ((uintptr_t)0x10000000)
/* Nothing is placed in the bottom of the address space. */
#define NEAR_FLOOR ((uintptr_t)0x10000)
/* Probe stride where the free ranges cannot be enumerated. */
#define NEAR_STEP ((uintptr_t)0x04000000)
/* Strides each way the quick pass tries before the address space is read. */
#define NEAR_QUICK_STEPS 2
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
	const near_range_t *except;
	uintptr_t anchor;
	int found;
} near_phdr_ctx_t;

static void near_phdr_add(near_range_t *r, uintptr_t lo, uintptr_t hi)
{
	if (lo >= hi)
		return;
	if (lo < r->lo)
		r->lo = lo;
	if (hi > r->hi)
		r->hi = hi;
}

static int near_phdr_cb(struct dl_phdr_info *info, size_t info_size, void *data)
{
	near_phdr_ctx_t *c = (near_phdr_ctx_t *)data;
	const near_range_t *x = c->except;
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
		if (x && x->lo < hi && x->hi > lo)
		{
			near_phdr_add(&r, lo, x->lo);
			near_phdr_add(&r, x->hi, hi);
		}
		else
			near_phdr_add(&r, lo, hi);
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
	near_image_extent_except(anchor, NULL, out);
}

void near_image_extent_except(const void *anchor, const near_range_t *except, near_range_t *out)
{
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION mbi;
	(void)except;
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
	c.except = except;
	c.anchor = (uintptr_t)anchor;
	c.found = 0;
	dl_iterate_phdr(near_phdr_cb, &c);
	if (c.found)
	{
		*out = c.r;
		return;
	}
#else
	(void)except;
#endif
	near_extent_around((uintptr_t)anchor, out);
}

/* ---- room in the image --------------------------------------------------- */

#ifdef NEAR_HAVE_ROOM
#define NEAR_STR_(x) #x
#define NEAR_STR(x) NEAR_STR_(x)
/* .lbss is NOBITS and goes after .bss in the linkers' default layouts, so the
 * room costs no file size and is the top of the image. */
__asm__(".pushsection .lbss,\"aw\",@nobits\n"
		".p2align 16\n"
		".globl near_room_space\n"
		".hidden near_room_space\n"
		".type near_room_space,@object\n"
		".size near_room_space," NEAR_STR(NEAR_ROOM_BYTES) "\n"
		"near_room_space:\n"
		".zero " NEAR_STR(NEAR_ROOM_BYTES) "\n"
		".popsection\n");
extern unsigned char near_room_space[] __attribute__((visibility("hidden")));

static int near_room_lost;

/* The loader commits the room as writable data; drop that at once. */
__attribute__((constructor)) static void near_room_drop(void)
{
	near_room_return((uintptr_t)near_room_space, NEAR_ROOM_SIZE);
}
#endif

int near_room_extent(near_range_t *out)
{
#ifdef NEAR_HAVE_ROOM
	out->lo = (uintptr_t)near_room_space;
	out->hi = out->lo + NEAR_ROOM_SIZE;
	return 1;
#else
	(void)out;
	return 0;
#endif
}

int near_room_return(uintptr_t lo, size_t size)
{
#ifdef NEAR_HAVE_ROOM
	/* Read-only: uncommitted and outside VmData, yet still readable by
	 * whatever scans the image's data, as LeakSanitizer does. */
	if (mmap((void *)lo, size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
			-1, 0) != MAP_FAILED)
		return 1;
	near_room_lost = 1;
#else
	(void)lo;
	(void)size;
#endif
	return 0;
}

int near_place_pair_in_room(const near_range_t *image, size_t main_size, size_t code_size,
		uintptr_t limit, uintptr_t align, const near_pair_ops_t *ops)
{
#ifdef NEAR_HAVE_ROOM
	near_range_t room;
	near_range_t hull;
	uintptr_t main_base;
	uintptr_t code_base;

	if (near_room_lost || !near_room_extent(&room))
		return 0;
	if (!align)
		align = 1;
	main_base = (room.lo + align - 1) & ~(align - 1);
	code_base = (main_base + main_size + align - 1) & ~(align - 1);
	if (code_base > room.hi || room.hi - code_base < code_size)
		return 0;
	hull = *image;
	near_hull(&hull, &room);
	if (hull.hi - hull.lo > limit)
		return 0;
	if (!ops->map_main(ops->ctx, main_base))
		return 0;
	if (ops->map_code(ops->ctx, code_base))
		return 1;
	ops->unmap_main(ops->ctx);
	return 0;
#else
	(void)image;
	(void)main_size;
	(void)code_size;
	(void)limit;
	(void)align;
	(void)ops;
	return 0;
#endif
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

/* The last address a candidate's reservation can reach. */
static uintptr_t near_scan_top(const near_search_t *s)
{
	return s->window.hi <= NEAR_TOP - s->size ? s->window.hi + s->size : NEAR_TOP;
}

/* Offer size-byte ranges stepping outward from the target's edges, at most
 * steps strides each way (0: across the whole window); the kernel's
 * acceptance of the hint is what tells them apart. The target itself is
 * occupied, so nothing is offered inside it. */
static void near_scan_stepped(near_search_t *s, unsigned steps)
{
	uintptr_t mask = ~(s->align - 1);
	uintptr_t down = s->target.lo >= (uintptr_t)s->size ? (s->target.lo - s->size) & mask : 0;
	uintptr_t up   = s->target.hi <= NEAR_TOP - (s->align - 1) ? (s->target.hi + s->align - 1) & mask : NEAR_TOP;
	uintptr_t off  = 0;
	unsigned n     = 0;

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
		if (++n == steps || off > NEAR_TOP - NEAR_STEP)
			break;
		off += NEAR_STEP;
	}
}

void near_search_quick(near_search_t *s)
{
	if (s->window.lo <= s->window.hi)
		near_scan_stepped(s, NEAR_QUICK_STEPS);
}

typedef void (*near_free_fn)(void *ctx, uintptr_t lo, uintptr_t hi);

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
#endif

/* Calls fn for each free range of this process's address space that meets
 * [lo, top). Returns 0 where the address space cannot be enumerated.
 * /proc/self/maps lists the mappings in address order, so the free ranges
 * are the spaces between them; Windows walks them with VirtualQuery. */
static int near_enum_free(uintptr_t lo, uintptr_t top, near_free_fn fn, void *ctx)
{
#if defined(_WIN32)
	MEMORY_BASIC_INFORMATION mbi;
	uintptr_t addr = lo;

	while (addr < top && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		uintptr_t rlo = (uintptr_t)mbi.BaseAddress;
		uintptr_t rhi = rlo + (uintptr_t)mbi.RegionSize;
		if (mbi.State == MEM_FREE)
			fn(ctx, rlo, rhi);
		if (rhi <= addr)
			break;
		addr = rhi;
	}
	return 1;
#elif defined(__linux__) && !defined(NEAR_FORCE_STEPPED)
	char line[256];
	uintptr_t prev = 0;
	FILE *f        = fopen("/proc/self/maps", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
	{
		const char *p = line;
		size_t n      = strlen(line);
		int whole     = n && line[n - 1] == '\n';
		uintptr_t rlo = near_hex(&p);
		uintptr_t rhi;

		/* A long path leaves the rest of its line for the next reads. */
		while (!whole && fgets(line, sizeof(line), f))
		{
			n     = strlen(line);
			whole = n && line[n - 1] == '\n';
		}
		if (*p != '-')
			continue;
		p++;
		rhi = near_hex(&p);
		if (rlo > prev && rlo > lo)
			fn(ctx, prev, rlo);
		if (rhi > prev)
			prev = rhi;
		if (prev >= top)
			break;
	}
	fclose(f);
	if (prev < top)
		fn(ctx, prev, top);
	return 1;
#else
	(void)lo;
	(void)top;
	(void)fn;
	(void)ctx;
	return 0;
#endif
}

static void near_gap_to_search(void *ctx, uintptr_t lo, uintptr_t hi)
{
	near_search_gap((near_search_t *)ctx, lo, hi);
}

void near_search_scan(near_search_t *s)
{
	if (s->window.lo > s->window.hi)
		return;
	if (!near_enum_free(s->window.lo, near_scan_top(s), near_gap_to_search, s))
		near_scan_stepped(s, 0);
}

static void near_gap_to_space(void *ctx, uintptr_t lo, uintptr_t hi)
{
	near_space_t *sp = (near_space_t *)ctx;
	if (hi - lo < sp->min_size || sp->count == NEAR_MAX_GAPS)
		return;
	sp->gap[sp->count].lo = lo;
	sp->gap[sp->count].hi = hi;
	sp->count++;
}

int near_space_read(near_space_t *sp, uintptr_t lo, uintptr_t hi, size_t min_size)
{
	sp->count    = 0;
	sp->min_size = min_size;
	return near_enum_free(lo, hi, near_gap_to_space, sp);
}

void near_search_space(near_search_t *s, const near_space_t *sp, const near_range_t *exclude)
{
	unsigned i;
	for (i = 0; i < sp->count; i++)
	{
		const near_range_t *g = &sp->gap[i];
		if (!exclude || exclude->hi <= g->lo || exclude->lo >= g->hi)
		{
			near_search_gap(s, g->lo, g->hi);
			continue;
		}
		if (exclude->lo > g->lo)
			near_search_gap(s, g->lo, exclude->lo);
		if (exclude->hi < g->hi)
			near_search_gap(s, exclude->hi, g->hi);
	}
}

/* Pass 0 offers the quick bases; pass 1 the address space, or where it
 * cannot be read the whole stepped window. */
static void near_offer(near_search_t *s, unsigned pass, const near_space_t *sp, int have_space,
		const near_range_t *exclude)
{
	if (pass == 0)
		near_search_quick(s);
	else if (have_space)
		near_search_space(s, sp, exclude);
	else
		near_search_scan(s);
}

/* One pass of near_place_pair. */
static int near_place_pass(unsigned pass, const near_space_t *sp, int have_space,
		const near_range_t *image, size_t main_size, uintptr_t main_ceiling, size_t code_size,
		uintptr_t limit, uintptr_t align, const near_pair_ops_t *ops)
{
	near_search_t ms;
	near_search_t cs;
	near_range_t target;
	near_range_t main_r;
	unsigned i;
	unsigned j;

	if ((uintptr_t)code_size >= limit
			|| !near_search_init(&ms, image, main_size, limit - code_size, align, main_ceiling))
		return 0;
	near_offer(&ms, pass, sp, have_space, NULL);
	for (i = 0; i < ms.count; i++)
	{
		if (!ops->map_main(ops->ctx, ms.base[i]))
			continue;
		main_r.lo = ms.base[i];
		main_r.hi = ms.base[i] + main_size;
		target    = *image;
		near_hull(&target, &main_r);
		if (near_search_init(&cs, &target, code_size, limit, align, 0))
		{
			near_offer(&cs, pass, sp, have_space, &main_r);
			for (j = 0; j < cs.count; j++)
				if (ops->map_code(ops->ctx, cs.base[j]))
					return 1;
		}
		ops->unmap_main(ops->ctx);
		/* A crowded neighbourhood: on to the address space. */
		if (pass == 0)
			break;
	}
	return 0;
}

int near_place_pair(near_space_t *sp, const near_range_t *image, size_t main_size,
		uintptr_t main_ceiling, size_t code_size, uintptr_t limit, uintptr_t align,
		const near_pair_ops_t *ops)
{
	int have_space;
	if (near_place_pass(0, sp, 0, image, main_size, main_ceiling, code_size, limit, align, ops))
		return 1;
	have_space = near_space_read(sp, image->hi > limit ? image->hi - limit : 0,
			image->lo <= NEAR_TOP - limit ? image->lo + limit : NEAR_TOP,
			code_size < main_size ? code_size : main_size);
	return near_place_pass(1, sp, have_space, image, main_size, main_ceiling, code_size, limit, align, ops);
}

int near_place_pair_in(const near_space_t *sp, const near_range_t *image, size_t main_size,
		uintptr_t main_ceiling, size_t code_size, uintptr_t limit, uintptr_t align,
		const near_pair_ops_t *ops)
{
	if (near_place_pass(0, sp, 0, image, main_size, main_ceiling, code_size, limit, align, ops))
		return 1;
	return near_place_pass(1, sp, 1, image, main_size, main_ceiling, code_size, limit, align, ops);
}
