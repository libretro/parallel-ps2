/* The recompilers' block array and link pool (pcsx2/x86/BaseblockEx.c):
 * their room is made by the first BaseBlocks_init, sized from the code
 * cache, and never grows; a full table refuses rather than reallocates,
 * BaseBlocks_NearlyFull says when to reset first, a later init empties
 * them in place, and a link the pool cannot hold goes through the
 * recompiler. */
#include <stdio.h>
#include <string.h>

#include "BaseblockEx.h"

#define CODE_SIZE (64 * 1024) /* 1024 blocks, 2048 links */

static int failures;

#define CHECK(c) \
	do \
	{ \
		if (!(c)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++; \
		} \
	} while (0)

static s32 rel_of(const s32* site)
{
	s32 v;
	memcpy(&v, site, sizeof(v));
	return v;
}

static s32 rel_to(const s32* site, uptr target)
{
	return (s32)(target - (uptr)(site + 1));
}

static struct BaseBlocks b;
static s32 sites[4096];
static u8 code[256];

int main(void)
{
	BASEBLOCKEX* blocks;
	BlockLinkEntry* entries;
	s32* buckets;
	const uptr rec = (uptr)&code[0];
	const uptr fn  = (uptr)&code[128];
	s32 i;

	BaseBlocks_init(&b, CODE_SIZE);
	BaseBlocks_SetJITCompile(&b, rec);
	CHECK(b.blocks._Reserved == CODE_SIZE / 64);
	CHECK(b.links.m_capacity == CODE_SIZE / 32);
	CHECK(b.blocks.blocks && b.links.m_entries && b.links.m_buckets);
	blocks  = b.blocks.blocks;
	entries = b.links.m_entries;
	buckets = b.links.m_buckets;
	CHECK(!BaseBlocks_NearlyFull(&b, 1));

	/* a link to a block not compiled yet goes to the recompiler, and is
	 * moved onto the block when it is made */
	BaseBlocks_Link(&b, 0x1000, &sites[0]);
	CHECK(rel_of(&sites[0]) == rel_to(&sites[0], rec));
	CHECK(BaseBlocks_New(&b, 0x1000, fn) != NULL);
	CHECK(rel_of(&sites[0]) == rel_to(&sites[0], fn));
	/* and to a block that is there, straight to it */
	BaseBlocks_Link(&b, 0x1000, &sites[1]);
	CHECK(rel_of(&sites[1]) == rel_to(&sites[1], fn));
	CHECK(b.links.m_count == 2);

	/* blocks fill to the room made and no further, in place */
	for (i = 1; i < CODE_SIZE / 64; i++)
	{
		if (!BaseBlocks_New(&b, 0x2000 + (u32)i * 8, fn))
		{
			CHECK(!"block refused before full");
			break;
		}
	}
	CHECK(b.blocks._Size == CODE_SIZE / 64);
	CHECK(b.blocks.blocks == blocks && b.blocks._Reserved == CODE_SIZE / 64);
	CHECK(BaseBlocks_NearlyFull(&b, 1));
	CHECK(BaseBlocks_New(&b, 0x100000, fn) == NULL);
	CHECK(b.blocks._Size == CODE_SIZE / 64);
	/* still ordered by start pc */
	for (i = 1; i < b.blocks._Size; i++)
		CHECK(b.blocks.blocks[i - 1].startpc < b.blocks.blocks[i].startpc);

	/* a later init empties both in place */
	BaseBlocks_init(&b, CODE_SIZE);
	BaseBlocks_SetJITCompile(&b, rec);
	CHECK(b.blocks._Size == 0 && b.links.m_count == 0);
	CHECK(b.blocks.blocks == blocks && b.links.m_entries == entries && b.links.m_buckets == buckets);
	CHECK(b.blocks._Reserved == CODE_SIZE / 64 && b.links.m_capacity == CODE_SIZE / 32);
	CHECK(!BaseBlocks_NearlyFull(&b, 1));

	/* NearlyFull says so exactly when less than room is left */
	CHECK(!BaseBlocks_NearlyFull(&b, CODE_SIZE / 64));
	CHECK(BaseBlocks_NearlyFull(&b, CODE_SIZE / 64 + 1));

	/* links fill to the room made; one past it is not recorded and goes
	 * through the recompiler even to a block that is there, so it is
	 * never left on a block it cannot be moved off */
	CHECK(BaseBlocks_New(&b, 0x3000, fn) != NULL);
	for (i = 0; i < CODE_SIZE / 32; i++)
		BaseBlocks_Link(&b, 0x3000, &sites[i]);
	CHECK(b.links.m_count == CODE_SIZE / 32);
	CHECK(b.links.m_entries == entries);
	CHECK(rel_of(&sites[CODE_SIZE / 32 - 1]) == rel_to(&sites[CODE_SIZE / 32 - 1], fn));
	CHECK(BaseBlocks_NearlyFull(&b, 1));
	BaseBlocks_Link(&b, 0x3000, &sites[CODE_SIZE / 32]);
	CHECK(b.links.m_count == CODE_SIZE / 32);
	CHECK(rel_of(&sites[CODE_SIZE / 32]) == rel_to(&sites[CODE_SIZE / 32], rec));

	/* removing the block moves every recorded link back to the recompiler */
	BaseBlocks_Remove(&b, 0, 0);
	CHECK(rel_of(&sites[0]) == rel_to(&sites[0], rec));
	CHECK(rel_of(&sites[CODE_SIZE / 32 - 1]) == rel_to(&sites[CODE_SIZE / 32 - 1], rec));

	if (failures)
	{
		printf("baseblocks: %d failure(s)\n", failures);
		return 1;
	}
	printf("baseblocks: ok\n");
	return 0;
}
