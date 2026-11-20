/* The buddy block (GS/Renderers/Common/GSBuddy.c): pieces of a power of
 * two, aligned, never overlapping, refused when no free piece is big
 * enough, and joined again as they come back, so after any mix of
 * allocations and frees the whole block is one free piece again. A
 * random run is checked against a byte map of what is out. */
#include <stdio.h>
#include <string.h>

#include "GS/Renderers/Common/GSBuddy.h"

static int fails;

#define CHECK(c) \
	do \
	{ \
		if (!(c)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			fails++; \
		} \
	} while (0)

#define SHIFT 20 /* 1 MiB */
#define PIECE 12 /* 4 KiB */
#define LIVE 64

static unsigned char owner[1 << (SHIFT - PIECE)]; /* which live slot + 1 */
static unsigned rnd = 12345u;

static unsigned next_rand(void)
{
	rnd = rnd * 1103515245u + 12345u;
	return rnd >> 8;
}

static size_t round_up(size_t n)
{
	size_t p = (size_t)1 << PIECE;
	while (p < n)
		p <<= 1;
	return p;
}

int main(void)
{
	gs_buddy b;
	unsigned char* p[LIVE];
	size_t sz[LIVE];
	unsigned char *a, *c, *d;
	size_t i, k;
	int step;

	CHECK(!gs_buddy_init(&b, 20, 5)); /* (pieces under a cache line) */
	CHECK(gs_buddy_alloc(&b, 1) == NULL);
	CHECK(!gs_buddy_free(&b, &b));

	CHECK(gs_buddy_init(&b, SHIFT, PIECE));
	CHECK(b.used == 0);

	/* the whole, then nothing more */
	a = (unsigned char*)gs_buddy_alloc(&b, (size_t)1 << SHIFT);
	CHECK(a == b.base && b.used == ((size_t)1 << SHIFT));
	CHECK(gs_buddy_alloc(&b, 1) == NULL);
	CHECK(gs_buddy_free(&b, a));
	CHECK(b.used == 0);
	CHECK(gs_buddy_alloc(&b, ((size_t)1 << SHIFT) + 1) == NULL);

	/* small ones round up and split, and come back whole */
	a = (unsigned char*)gs_buddy_alloc(&b, 1);
	c = (unsigned char*)gs_buddy_alloc(&b, 5000);
	d = (unsigned char*)gs_buddy_alloc(&b, 4096);
	CHECK(a && c && d);
	CHECK(b.used == 4096 + 8192 + 4096);
	CHECK(((size_t)(a - b.base) & 4095) == 0 && ((size_t)(c - b.base) & 8191) == 0);
	CHECK(a != d && (c + 8192 <= a || a + 4096 <= c) && (c + 8192 <= d || d + 4096 <= c));
	CHECK(gs_buddy_free(&b, c));
	CHECK(gs_buddy_free(&b, c)); /* (a second free is ignored) */
	CHECK(b.used == 8192);
	CHECK(gs_buddy_free(&b, a) && gs_buddy_free(&b, d));
	CHECK(b.used == 0);
	a = (unsigned char*)gs_buddy_alloc(&b, (size_t)1 << SHIFT);
	CHECK(a == b.base);
	CHECK(gs_buddy_free(&b, a));
	CHECK(!gs_buddy_free(&b, (unsigned char*)&b));
	CHECK(!gs_buddy_free(&b, b.base + b.size));

	/* a piece freed after its twin joins it; freed again, it is ignored */
	a = (unsigned char*)gs_buddy_alloc(&b, 4096);
	d = (unsigned char*)gs_buddy_alloc(&b, 4096);
	CHECK(a == b.base && d == b.base + 4096);
	CHECK(gs_buddy_free(&b, a) && gs_buddy_free(&b, d));
	CHECK(b.used == 0);
	CHECK(gs_buddy_free(&b, d) && gs_buddy_free(&b, a));
	CHECK(b.used == 0);
	a = (unsigned char*)gs_buddy_alloc(&b, (size_t)1 << SHIFT);
	CHECK(a == b.base);
	CHECK(gs_buddy_free(&b, a));

	/* a random run against a map of what is out */
	memset(p, 0, sizeof(p));
	for (step = 0; step < 200000; step++)
	{
		const size_t s = (size_t)(next_rand() % LIVE);
		if (p[s])
		{
			for (k = (size_t)(p[s] - b.base) >> PIECE; k < (((size_t)(p[s] - b.base) + sz[s]) >> PIECE); k++)
			{
				CHECK(owner[k] == s + 1);
				owner[k] = 0;
			}
			CHECK(p[s][0] == (unsigned char)s && p[s][sz[s] - 1] == (unsigned char)s);
			CHECK(gs_buddy_free(&b, p[s]));
			p[s] = NULL;
		}
		else
		{
			const size_t want = 1 + (size_t)(next_rand() % (next_rand() & 1 ? 9000 : 120000));
			p[s] = (unsigned char*)gs_buddy_alloc(&b, want);
			if (!p[s])
				continue;
			sz[s] = round_up(want);
			CHECK(((size_t)(p[s] - b.base) & (sz[s] - 1)) == 0);
			for (k = (size_t)(p[s] - b.base) >> PIECE; k < (((size_t)(p[s] - b.base) + sz[s]) >> PIECE); k++)
			{
				CHECK(owner[k] == 0);
				owner[k] = (unsigned char)(s + 1);
			}
			p[s][0] = p[s][sz[s] - 1] = (unsigned char)s;
		}
	}
	k = 0;
	for (i = 0; i < LIVE; i++)
		if (p[i])
		{
			k += sz[i];
			CHECK(gs_buddy_free(&b, p[i]));
		}
	CHECK(b.used == 0);
	a = (unsigned char*)gs_buddy_alloc(&b, (size_t)1 << SHIFT);
	CHECK(a == b.base);
	gs_buddy_deinit(&b);
	CHECK(b.base == NULL && gs_buddy_alloc(&b, 1) == NULL);

	if (fails)
	{
		printf("gsbuddy: %d failed\n", fails);
		return 1;
	}
	printf("gsbuddy: ok\n");
	return 0;
}
