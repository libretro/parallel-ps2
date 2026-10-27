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

/* Grows r to cover add. */
void near_hull(near_range_t *r, const near_range_t *add);

/* The loaded image containing anchor. Where the platform cannot report it,
 * a conservative span around anchor. */
void near_image_extent(const void *anchor, near_range_t *out);

/* Sets up a search for a size-byte reservation whose hull with target spans at
 * most limit bytes, on align boundaries, ending at or below ceiling (0: no
 * ceiling). Returns 0 when no base can satisfy that. */
int near_search_init(near_search_t *s, const near_range_t *target, size_t size,
		uintptr_t limit, uintptr_t align, uintptr_t ceiling);

/* Offers the free range [lo, hi): the best base inside it, if any, is kept. */
void near_search_gap(near_search_t *s, uintptr_t lo, uintptr_t hi);

/* Offers every free range of this process's address space in the window. */
void near_search_scan(near_search_t *s);

/* Whether a size-byte reservation can be placed now with its hull with target
 * within limit: each candidate is reserved and released again until one sticks. */
int near_place_room(const near_range_t *target, size_t size, uintptr_t limit,
		uintptr_t align);

#ifdef __cplusplus
}
#endif

#endif
