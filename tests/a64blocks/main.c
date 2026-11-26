/* The arm64 recompilers' block index: see build.sh. */

#include <stdio.h>
#include <string.h>

#include "arm64/aBlocks.h"

#define RECS  8
#define LINKS 6
#define HASH  4
#define PAGES 16
#define SRC   10

static a64_block     s_recs[RECS];
static a64_page_link s_links[LINKS];
static u32           s_hash[HASH];
static u32           s_page_head[PAGES];
static u32           s_src[SRC];
static a64_blocks    s_b = { s_recs, s_links, s_hash, s_page_head, s_src, RECS, LINKS, HASH - 1, PAGES, SRC, 0, 0, 0, 0, 0, 0, 0 };

static int s_fail;

#define CHECK(c) do { if (!(c)) { printf("  FAIL line %d: %s\n", __LINE__, #c); s_fail = 1; } } while (0)

/* pcs listed on a page, in list order */
static int page_pcs(u32 pg, u32* out)
{
	int n = 0;
	u32 l = s_page_head[pg];
	while (l)
	{
		out[n++] = s_links[l - 1].pc;
		l = s_links[l - 1].next;
	}
	return n;
}

static int listed(u32 pg, u32 pc)
{
	u32 pcs[LINKS];
	int i, n = page_pcs(pg, pcs);
	for (i = 0; i < n; i++)
		if (pcs[i] == pc)
			return 1;
	return 0;
}

int main(void)
{
	a64_block *a, *b, *c, *d;
	u32 i, *link;

	/* garbage in the storage must not matter after a clear */
	memset(s_recs, 0x5a, sizeof(s_recs));
	memset(s_hash, 0x5a, sizeof(s_hash));
	memset(s_page_head, 0x5a, sizeof(s_page_head));
	a64_blocks_clear(&s_b);
	CHECK(s_b.nrecs == 0 && s_b.nlinks == 0 && s_b.src_top == 0);
	CHECK(a64_blocks_find(&s_b, 0x1000) == NULL);

	/* 0x1000, 0x1010, 0x1020 share a hash chain (mask 3 over pc >> 2) */
	a = a64_blocks_get(&s_b, 0x1000);
	b = a64_blocks_get(&s_b, 0x1010);
	c = a64_blocks_get(&s_b, 0x1020);
	d = a64_blocks_get(&s_b, 0x1004);
	CHECK(a && b && c && d && s_b.nrecs == 4);
	CHECK(a->pc == 0x1000 && a->fn == NULL && !a->live);
	a->live = 1;
	CHECK(a64_blocks_get(&s_b, 0x1000) == a && s_b.nrecs == 4); /* found, not a second record */
	CHECK(a->live == 1);                                         /* and not zeroed */
	CHECK(a64_blocks_find(&s_b, 0x1010) == b && a64_blocks_find(&s_b, 0x1020) == c);
	CHECK(a64_blocks_find(&s_b, 0x1004) == d && a64_blocks_find(&s_b, 0x1030) == NULL);

	/* erasing the middle of a chain keeps the rest reachable, in place */
	a64_blocks_erase(&s_b, 0x1010);
	CHECK(s_b.nrecs == 3);
	CHECK(a64_blocks_find(&s_b, 0x1010) == NULL);
	CHECK(a64_blocks_find(&s_b, 0x1000) == a && a64_blocks_find(&s_b, 0x1020) == c);
	a64_blocks_erase(&s_b, 0x1030); /* absent: nothing happens */
	CHECK(s_b.nrecs == 3);

	/* the freed record is the next one handed out, zeroed */
	b->live = 1;
	CHECK(a64_blocks_get(&s_b, 0x2000) == b && b->pc == 0x2000 && !b->live);
	CHECK(a64_blocks_find(&s_b, 0x1000) == a && a64_blocks_find(&s_b, 0x1004) == d);

	/* fill to the cap: room says no once every record is taken */
	CHECK(a64_blocks_room(&s_b, 0, 0));
	for (i = 0; s_b.nrecs < RECS; i++)
		a64_blocks_get(&s_b, 0x3000 + i * 4);
	CHECK(!a64_blocks_room(&s_b, 0, 0));
	CHECK(a64_blocks_find(&s_b, 0x1000) == a && a->pc == 0x1000); /* nothing moved */
	a64_blocks_erase(&s_b, 0x3000);
	CHECK(a64_blocks_room(&s_b, 0, 0));

	/* page lists: a pc once per page, links given back on unlink */
	a64_blocks_list(&s_b, 3, 0x1000);
	a64_blocks_list(&s_b, 3, 0x1000);
	a64_blocks_list(&s_b, 3, 0x1004);
	a64_blocks_list(&s_b, 3, 0x1020);
	a64_blocks_list(&s_b, 4, 0x1000);
	CHECK(s_b.nlinks == 4);
	CHECK(listed(3, 0x1000) && listed(3, 0x1004) && listed(3, 0x1020) && listed(4, 0x1000) && !listed(4, 0x1004));
	CHECK(a64_blocks_room(&s_b, 0, 2) && !a64_blocks_room(&s_b, 0, 3));

	/* walk page 3 dropping 0x1004, which has links on both sides */
	link = &s_page_head[3];
	while (*link)
	{
		if (s_links[*link - 1].pc == 0x1004)
			a64_blocks_unlink(&s_b, link);
		else
			link = &s_links[*link - 1].next;
	}
	CHECK(s_b.nlinks == 3 && !listed(3, 0x1004) && listed(3, 0x1000) && listed(3, 0x1020) && listed(4, 0x1000));
	/* the freed link is reused before the pool grows */
	i = s_b.link_top;
	a64_blocks_list(&s_b, 5, 0x1020);
	CHECK(s_b.link_top == i && s_b.nlinks == 4 && listed(5, 0x1020));
	/* emptying a list */
	while (s_page_head[4])
		a64_blocks_unlink(&s_b, &s_page_head[4]);
	CHECK(s_b.nlinks == 3 && s_page_head[4] == 0);
	/* fill the links */
	for (i = 0; s_b.nlinks < LINKS; i++)
		a64_blocks_list(&s_b, 6 + i, 0x1000);
	CHECK(!a64_blocks_room(&s_b, 0, 1));

	/* snapshot words: handed out in order, room counts them */
	a64_blocks_clear(&s_b);
	CHECK(a64_blocks_src(&s_b, 4) == 0 && a64_blocks_src(&s_b, 3) == 4);
	CHECK(a64_blocks_room(&s_b, 3, 0) && !a64_blocks_room(&s_b, 4, 0));

	/* a clear forgets everything */
	a64_blocks_get(&s_b, 0x1000);
	a64_blocks_list(&s_b, 1, 0x1000);
	a64_blocks_clear(&s_b);
	CHECK(s_b.nrecs == 0 && s_b.nlinks == 0 && s_b.src_top == 0);
	CHECK(a64_blocks_find(&s_b, 0x1000) == NULL && s_page_head[1] == 0);
	CHECK(a64_blocks_get(&s_b, 0x4000) == &s_recs[0]);

	printf(s_fail ? "a64blocks: FAIL\n" : "a64blocks: ok\n");
	return s_fail;
}
