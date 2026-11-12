/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "../../common/Pcsx2Defs.h"
#include "../../common/Pcsx2Types.h"

#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every potential jump point in the PS2's addressable memory has a BASEBLOCK */
/* associated with it. So that means a BASEBLOCK for every 4 bytes of PS2 */
/* addressable memory.  Yay! */
typedef struct BASEBLOCK
{
	uptr m_pFnptr;
} BASEBLOCK;

/* extra block info (only valid for start of fn) */
typedef struct BASEBLOCKEX
{
	uptr fnptr;
	u32 startpc;
	u32 size;    /* The size in dwords (equivalent to the number of instructions) */
	u32 x86size; /* The size in byte of the translated x86 instructions */
} BASEBLOCKEX;

/* Growable, startpc-ordered array of BASEBLOCKEX. C89 shape: plain struct,
 * malloc/realloc-backed (the old new[]/delete[] copied only the live prefix
 * anyway, which is exactly what realloc does). */
typedef struct BaseBlockArray
{
	s32 _Reserved;
	s32 _Size;
	BASEBLOCKEX* blocks;
} BaseBlockArray;

static __fi void BaseBlockArray_reserve(struct BaseBlockArray* a, u32 size)
{
	a->blocks    = (BASEBLOCKEX*)realloc(a->blocks, size * sizeof(BASEBLOCKEX));
	a->_Reserved = size;
}

static __fi void BaseBlockArray_init(struct BaseBlockArray* a, s32 size)
{
	a->_Reserved = 0;
	a->_Size     = 0;
	a->blocks    = NULL;
	BaseBlockArray_reserve(a, size);
}

static __fi void BaseBlockArray_destroy(struct BaseBlockArray* a)
{
	free(a->blocks);
	a->blocks = NULL;
}

static BASEBLOCKEX* BaseBlockArray_insert(struct BaseBlockArray* a, u32 startpc, uptr fnptr)
{
	int imin = 0, imax, imid;

	if (a->_Size + 1 >= a->_Reserved)
		BaseBlockArray_reserve(a, a->_Reserved + 0x2000); /* some games requires even more! */

	/* Insert the the new BASEBLOCKEX by startpc order */
	imax = a->_Size;
	while (imin < imax)
	{
		imid = (imin + imax) >> 1;

		if (a->blocks[imid].startpc > startpc)
			imax = imid;
		else
			imin = imid + 1;
	}

	if (imin < a->_Size)
	{
		/* make a hole for a new block. */
		memmove(a->blocks + imin + 1, a->blocks + imin, (a->_Size - imin) * sizeof(BASEBLOCKEX));
	}

	memset((a->blocks + imin), 0, sizeof(BASEBLOCKEX));
	a->blocks[imin].startpc = startpc;
	a->blocks[imin].fnptr   = fnptr;

	a->_Size++;
	return &a->blocks[imin];
}

#define BaseBlockArray_at(a, idx) ((a)->blocks[(idx)])

static __fi void BaseBlockArray_clear(struct BaseBlockArray* a)
{
	a->_Size = 0;
}

static __fi u32 BaseBlockArray_size(const struct BaseBlockArray* a)
{
	return a->_Size;
}

static __fi void BaseBlockArray_erase(struct BaseBlockArray* a, s32 first, s32 last)
{
	const int range = last - first;

	if (last < a->_Size)
		memmove(a->blocks + first, a->blocks + last, (a->_Size - last) * sizeof(BASEBLOCKEX));

	a->_Size -= range;
}

/* Maps a guest start PC to the set of host jump sites (u32* patch locations,
 * stored as uptr) that target it. Chained hash: insert-with-duplicates,
 * "walk all jump sites for a given PC", and clear, all the callers need.
 * Entries live in a single growable POD pool and are chained by index (not
 * pointer) so a realloc of the pool never invalidates the chains. */
typedef struct BlockLinkEntry
{
	u32  pc;
	uptr jumpptr;
	s32  next; /* pool index of next entry in this bucket, or -1 */
} BlockLinkEntry;

typedef struct BlockLinkMap
{
	struct BlockLinkEntry* m_entries;
	s32    m_count;
	s32    m_capacity;
	s32*   m_buckets;
	u32    m_bucket_count; /* power of two */
	u32    m_bucket_mask;
} BlockLinkMap;

/* startpc is 4-byte aligned; drop the low 2 bits before masking so
 * adjacent block PCs spread across buckets. */
#define BlockLinkMap_bucket_of(m, pc) (((pc) >> 2) & (m)->m_bucket_mask)

static __fi void BlockLinkMap_init(struct BlockLinkMap* m)
{
	m->m_entries      = NULL;
	m->m_count        = 0;
	m->m_capacity     = 0;
	m->m_bucket_count = 0x10000; /* 64k buckets, covers typical live-link counts */
	m->m_bucket_mask  = m->m_bucket_count - 1;
	m->m_buckets      = (s32*)malloc(m->m_bucket_count * sizeof(s32));
	memset(m->m_buckets, 0xFF, m->m_bucket_count * sizeof(s32)); /* all -1 */
}

static __fi void BlockLinkMap_destroy(struct BlockLinkMap* m)
{
	free(m->m_entries);
	free(m->m_buckets);
	m->m_entries = NULL;
	m->m_buckets = NULL;
}

static __fi void BlockLinkMap_insert(struct BlockLinkMap* m, u32 pc, uptr jumpptr)
{
	u32 b;
	struct BlockLinkEntry* e;

	if (m->m_count == m->m_capacity)
	{
		const s32 newcap = m->m_capacity ? m->m_capacity * 2 : 4096;
		m->m_entries  = (struct BlockLinkEntry*)realloc(m->m_entries, newcap * sizeof(struct BlockLinkEntry));
		m->m_capacity = newcap;
	}

	b            = BlockLinkMap_bucket_of(m, pc);
	e            = &m->m_entries[m->m_count];
	e->pc        = pc;
	e->jumpptr   = jumpptr;
	e->next      = m->m_buckets[b];
	m->m_buckets[b] = m->m_count;
	m->m_count++;
}

/* Walks every jump site registered for pc and rewrites each one to a 32-bit
 * rel32 displacement pointing at target_addr (target_addr - (site + 4)). */
static __fi void BlockLinkMap_patch_links(const struct BlockLinkMap* m, u32 pc, uptr target_addr)
{
	s32 i = m->m_buckets[BlockLinkMap_bucket_of(m, pc)];
	while (i != -1)
	{
		const struct BlockLinkEntry* e = &m->m_entries[i];
		if (e->pc == pc)
		{
			const u32 rel32_ = (u32)(target_addr - (e->jumpptr + 4));
			memcpy((void*)e->jumpptr, &rel32_, sizeof(u32));
		}
		i = e->next;
	}
}

static __fi void BlockLinkMap_clear(struct BlockLinkMap* m)
{
	m->m_count = 0;
	memset(m->m_buckets, 0xFF, m->m_bucket_count * sizeof(s32));
}

typedef struct BaseBlocks
{
	struct BlockLinkMap links;
	uptr recompiler;
	struct BaseBlockArray blocks;
} BaseBlocks;

void BaseBlocks_init(struct BaseBlocks* b);
BASEBLOCKEX* BaseBlocks_New(struct BaseBlocks* b, u32 startpc, uptr fnptr);
int BaseBlocks_LastIndex(const struct BaseBlocks* b, u32 startpc);
void BaseBlocks_Link(struct BaseBlocks* b, u32 pc, s32* jumpptr);

#define BaseBlocks_SetJITCompile(b, recompiler_) ((b)->recompiler = (uptr)(recompiler_))

static __fi int BaseBlocks_Index(const struct BaseBlocks* b, u32 startpc)
{
	const int idx = BaseBlocks_LastIndex(b, startpc);

	if ((idx == -1) || (startpc < b->blocks.blocks[idx].startpc) ||
		((b->blocks.blocks[idx].size) && (startpc >= b->blocks.blocks[idx].startpc + b->blocks.blocks[idx].size * 4)))
		return -1;
	return idx;
}

static __fi BASEBLOCKEX* BaseBlocks_At(struct BaseBlocks* b, int idx)
{
	if (idx < 0 || idx >= (int)BaseBlockArray_size(&b->blocks))
		return 0;

	return &b->blocks.blocks[idx];
}

static __fi BASEBLOCKEX* BaseBlocks_Get(struct BaseBlocks* b, u32 startpc)
{
	return BaseBlocks_At(b, BaseBlocks_Index(b, startpc));
}

static __fi void BaseBlocks_Remove(struct BaseBlocks* b, int first, int last)
{
	int idx = first;
	do
	{
		BlockLinkMap_patch_links(&b->links, b->blocks.blocks[idx].startpc, b->recompiler);
	} while (idx++ < last);

	/* TODO: remove links from this block? */
	BaseBlockArray_erase(&b->blocks, first, last + 1);
}

static __fi void BaseBlocks_Reset(struct BaseBlocks* b)
{
	BaseBlockArray_clear(&b->blocks);
	BlockLinkMap_clear(&b->links);
}

#define PC_GETBLOCK_(x, reclut) ((BASEBLOCK*)(reclut[((u32)(x)) >> 16] + (x) * (sizeof(BASEBLOCK) / 4)))

/**
 * Add a page to the recompiler lookup table
 *
 * Will associate `reclut[pagebase + pageidx]` with `mapbase[mappage << 14]`
 * Will associate `hwlut[pagebase + pageidx]` with `pageidx << 16`
 */
static inline void recLUT_SetPage(uptr reclut[0x10000], u32 hwlut[0x10000],
                                  BASEBLOCK* mapbase, uint pagebase, uint pageidx, uint mappage)
{
	/* this value is in 64k pages! */
	uint page = pagebase + pageidx;

	/* Integer arithmetic on uptr, not pointer arithmetic: mapbase may be
	 * the null sentinel and the page delta is negative for pages after
	 * the mapped one - '&mapbase[negative]' on null and '<<' on a
	 * negative value are both UB.  The stored value is bit-identical. */
	reclut[page] = (uptr)mapbase +
		(uptr)(((sptr)mappage - (sptr)page) * (sptr)0x4000 * (sptr)sizeof(BASEBLOCK));
	if (hwlut)
		hwlut[page] = 0u - (pagebase << 16);
}
#ifdef __cplusplus
}
#endif
