/* Placement of the host memory reservations within rel32 reach of each other
 * and of the core's image. C89.
 *
 * The recompilers emit RIP-relative / abs32 operands to the core's globals and,
 * for constant-address loads and stores, straight into guest memory. Every byte
 * of the code reservation therefore has to sit within +-2GB of every byte of the
 * loaded image and of the main reservation. That holds when all three lie inside
 * one window of `limit` bytes, which is the invariant this unit places by: a
 * reservation is admitted only where the hull of it and its target ranges spans
 * at most `limit`. */
#ifndef PCSX2_NEARPLACE_H
#define PCSX2_NEARPLACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEAR_MAX_CANDIDATES 32
#define NEAR_MAX_GAPS 64

typedef struct near_range
{
	uintptr_t lo; /* first byte */
	uintptr_t hi; /* one past the last byte */
} near_range_t;

/* The best NEAR_MAX_CANDIDATES bases seen so far, ordered by the span of the
 * hull they produce with the target: the tightest placement is tried first, so
 * the next reservation keeps the most room. */
typedef struct near_search
{
	uintptr_t base[NEAR_MAX_CANDIDATES];
	uintptr_t span[NEAR_MAX_CANDIDATES];
	near_range_t target;
	near_range_t window; /* admissible bases, both ends inclusive */
	size_t size;
	uintptr_t align;
	unsigned count;
} near_search_t;

/* The free ranges of the address space near the image, read once and
 * searched as often as needed: placing main and then code beside it costs
 * one read. Ranges smaller than min_size are not kept. */
typedef struct near_space
{
	near_range_t gap[NEAR_MAX_GAPS];
	size_t min_size;
	unsigned count;
} near_space_t;

/* Grows r to cover add. */
void near_hull(near_range_t *r, const near_range_t *add);

/* The loaded image containing anchor. Where the platform cannot report it,
 * a conservative span around anchor. */
void near_image_extent(const void *anchor, near_range_t *out);

/* As near_image_extent, less except (NULL: nothing) where that is an end of
 * the image. */
void near_image_extent_except(const void *anchor, const near_range_t *except, near_range_t *out);

/* Room for both maps kept in the module's own image, for when no range in
 * reach is free, as in hosts that have mapped a lot around the module.
 * It sits at the top of the image in a section of its own and is read-only,
 * uncommitted zeroes until something is mapped over it. */
#if defined(__linux__) && defined(__x86_64__) && defined(__GNUC__)
#define NEAR_HAVE_ROOM 1
#define NEAR_ROOM_BYTES 0x27100000 /* main plus code, Memory.h */
#define NEAR_ROOM_SIZE ((size_t)NEAR_ROOM_BYTES)
#endif

/* The room's range, used or not. Returns 0 where the build keeps none. */
int near_room_extent(near_range_t *out);

/* Hands [lo, lo + size) of the room back as uncommitted, read-only zeroes.
 * Returns 0 when it cannot: the range may then be unmapped and anyone's, so
 * the room is given up for good. */
int near_room_return(uintptr_t lo, size_t size);

/* Places the pair in the room instead: main at its start, code after main.
 * The ops map exactly over a base and unmap_main hands main back with
 * near_room_return. image is the whole image, room included. Returns 0
 * without a usable room, when the pair does not fit it, or when image and
 * room span more than limit. */
typedef struct near_pair_ops near_pair_ops_t;
int near_place_pair_in_room(const near_range_t *image, size_t main_size, size_t code_size,
		uintptr_t limit, uintptr_t align, const near_pair_ops_t *ops);

/* Sets up a search for a size-byte reservation whose hull with target spans at
 * most limit bytes, on align boundaries, ending at or below ceiling (0: no
 * ceiling). Returns 0 when no base can satisfy that. */
int near_search_init(near_search_t *s, const near_range_t *target, size_t size,
		uintptr_t limit, uintptr_t align, uintptr_t ceiling);

/* Offers the free range [lo, hi): the best base inside it, if any, is kept. */
void near_search_gap(near_search_t *s, uintptr_t lo, uintptr_t hi);

/* Offers every free range of this process's address space in the window. */
void near_search_scan(near_search_t *s);

/* Offers the few bases right outside the target without reading the address
 * space; most placements succeed here. Run near_search_scan on a fresh
 * search when none of them can be mapped. */
void near_search_quick(near_search_t *s);

/* Records the free ranges in [lo, hi). Returns 0 where the address space
 * cannot be enumerated: near_search_scan then steps through hints. */
int near_space_read(near_space_t *sp, uintptr_t lo, uintptr_t hi, size_t min_size);

/* Offers the recorded ranges, less exclude when it is not NULL (a
 * reservation placed since the read). */
void near_search_space(near_search_t *s, const near_space_t *sp, const near_range_t *exclude);

/* Places a main reservation and a code reservation so that the hull of the
 * image, main and code spans at most limit: main within limit - code_size of
 * the image and below main_ceiling (0: none), then code against the hull of
 * image and main. The bases right outside the image are tried first; when
 * that neighbourhood is taken, the address space is read once into sp and
 * both are placed from it. The ops map each reservation exactly at a base
 * (returning nonzero on success) and undo main when code does not fit
 * beside it. Returns nonzero with both mapped. */
struct near_pair_ops
{
	int (*map_main)(void *ctx, uintptr_t base);
	void (*unmap_main)(void *ctx);
	int (*map_code)(void *ctx, uintptr_t base);
	void *ctx;
};

int near_place_pair(near_space_t *sp, const near_range_t *image, size_t main_size,
		uintptr_t main_ceiling, size_t code_size, uintptr_t limit, uintptr_t align,
		const near_pair_ops_t *ops);

/* near_place_pair over an address space the caller has filled sp with. */
int near_place_pair_in(const near_space_t *sp, const near_range_t *image, size_t main_size,
		uintptr_t main_ceiling, size_t code_size, uintptr_t limit, uintptr_t align,
		const near_pair_ops_t *ops);

#ifdef __cplusplus
}
#endif

#endif
