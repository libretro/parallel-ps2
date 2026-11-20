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

#include "BaseblockEx.h"
#include "../HostMem.h"

/* Room made once, mapped and committed as it is touched (the heap, at
 * boot, if the mapping is refused) */
static void* BaseBlocks_room(size_t bytes)
{
	const PageProtectionMode rw = {true, true, false};
	void* p = host_mmap(NULL, bytes, rw);
	return p ? p : malloc(bytes);
}

/* Called on every recompiler reset (_DynGen_Dispatchers): the room is made
 * on the first call, for one block per 64 bytes of code and one link per
 * 32, far denser than real code, and emptied on later ones. */
void BaseBlocks_init(struct BaseBlocks* b, size_t code_size)
{
	struct BlockLinkMap* m = &b->links;
	if (!m->m_buckets)
	{
		m->m_bucket_count = 0x10000; /* 64k buckets, covers typical live-link counts */
		m->m_bucket_mask  = m->m_bucket_count - 1;
		m->m_buckets      = (s32*)malloc(m->m_bucket_count * sizeof(s32));
		m->m_capacity     = (s32)(code_size / 32);
		m->m_entries      = (struct BlockLinkEntry*)BaseBlocks_room((size_t)m->m_capacity * sizeof(struct BlockLinkEntry));
		if (!m->m_entries)
			m->m_capacity = 0;
	}
	m->m_count = 0;
	memset(m->m_buckets, 0xFF, m->m_bucket_count * sizeof(s32)); /* all -1 */

	b->recompiler = 0;

	if (!b->blocks.blocks)
	{
		b->blocks._Reserved = (s32)(code_size / 64);
		b->blocks.blocks    = (BASEBLOCKEX*)BaseBlocks_room((size_t)b->blocks._Reserved * sizeof(BASEBLOCKEX));
		if (!b->blocks.blocks)
			b->blocks._Reserved = 0;
	}
	b->blocks._Size = 0;
}

BASEBLOCKEX* BaseBlocks_New(struct BaseBlocks* b, u32 startpc, uptr fnptr)
{
	BlockLinkMap_patch_links(&b->links, startpc, fnptr);

	return BaseBlockArray_insert(&b->blocks, startpc, fnptr);
}

int BaseBlocks_LastIndex(const struct BaseBlocks* b, u32 startpc)
{
	int imin, imax;

	if (0 == BaseBlockArray_size(&b->blocks))
		return -1;

	imin = 0;
	imax = BaseBlockArray_size(&b->blocks) - 1;

	while (imin != imax)
	{
		const int imid = (imin + imax + 1) >> 1;

		if (b->blocks.blocks[imid].startpc > startpc)
			imax = imid - 1;
		else
			imin = imid;
	}

	return imin;
}

void BaseBlocks_Link(struct BaseBlocks* b, u32 pc, s32* jumpptr)
{
	BASEBLOCKEX* targetblock = BaseBlocks_Get(b, pc);
	/* (a site the links cannot hold goes through the recompiler, which is
	 * always right, rather than to a block it could not be moved off) */
	const int recorded = BlockLinkMap_insert(&b->links, pc, (uptr)jumpptr);
	/* jumpptr aims into the code buffer at arbitrary alignment. */
	const s32 rel32_ = (recorded && targetblock && targetblock->startpc == pc)
		? (s32)(targetblock->fnptr - (sptr)(jumpptr + 1))
		: (s32)(b->recompiler - (sptr)(jumpptr + 1));
	memcpy(jumpptr, &rel32_, sizeof(s32));
}
