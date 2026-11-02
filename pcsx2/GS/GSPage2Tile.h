/*
 * GSPage2TileMap's contents, built in C: which tiles of a texture live in
 * each GS page.
 *
 * The caller walks the texture's blocks and writes one key a block,
 * GS_PAGE2TILE_KEY(page, addr), addr being the block's (y << 7) + x. A
 * page's entries are then its rows of 32 addresses (addr >> 5), each
 * with the inverted mask of the addresses it holds (~(1 << (addr & 31))
 * or'ed), in order of row: what the std::unordered_map of
 * std::unordered_sets, a std::unordered_map per page and a std::sort
 * made before.
 *
 * Here the keys are radix sorted, three passes of nine bits, and runs of
 * the same page and row are merged as they come. Cold: once per texture
 * shape, on first use.
 */

#ifndef GS_PAGE2TILE_H
#define GS_PAGE2TILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pages up to 512, addresses below 65536: 25 bits. */
#define GS_PAGE2TILE_KEY(page, addr) (((unsigned)(page) << 16) | (unsigned)(addr))

/* Sorts n keys (scratch: n more); returns the entries they make. */
size_t gs_page2tile_sort(unsigned *keys, unsigned *scratch, size_t n);
/* From keys sorted by gs_page2tile_sort: entries as int pairs (row,
 * mask), as many as it returned, and first[0..pages], where page p's run
 * starts (first[pages] is the end). */
void   gs_page2tile_fill(const unsigned *keys, size_t n, unsigned pages, int *entries, unsigned *first);

#ifdef __cplusplus
}
#endif

#endif
