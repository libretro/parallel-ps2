/* A state load writes only the VRAM pages whose bytes it changes: every
 * page written is a page whose texture cache and super samples go, and,
 * on a GPU with VRAM of its own, a page uploaded. C89; tests/pgs holds
 * the runs against a page-by-page compare. */
#ifndef PGS_STATE_VRAM_H
#define PGS_STATE_VRAM_H

#include <stddef.h>
#include <string.h>

/* The first run of whole pages at or after from in which next differs
 * from cur: its offset, and its size in *run_size. size when there is
 * none. size and from are multiples of page. */
static size_t pgs_next_changed_run(const unsigned char *cur, const unsigned char *next,
	size_t size, size_t page, size_t from, size_t *run_size)
{
	size_t start, end;

	for (start = from; start < size; start += page)
		if (memcmp(cur + start, next + start, page) != 0)
			break;
	for (end = start; end < size; end += page)
		if (memcmp(cur + end, next + end, page) == 0)
			break;
	*run_size = end - start;
	return start;
}

#endif
