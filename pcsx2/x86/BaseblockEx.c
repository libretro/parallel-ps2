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

/* The old class ran its constructor once, at static-init time. The call
 * sites that replace it (_DynGen_Dispatchers) run on every recompiler
 * reset, so this is idempotent: allocate on the first call, and on later
 * calls just reset the contents the way the destructor-then-constructor
 * pair never did. Re-allocating here would leak a 256KB bucket table per
 * reset. */
void BaseBlocks_init(struct BaseBlocks* b)
{
	if (!b->links.m_buckets)
		BlockLinkMap_init(&b->links);
	else
		BlockLinkMap_clear(&b->links);

	b->recompiler = 0;

	if (!b->blocks.blocks)
		BaseBlockArray_init(&b->blocks, 0x4000);
	else
		BaseBlockArray_clear(&b->blocks);
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
	/* jumpptr aims into the code buffer at arbitrary alignment. */
	const s32 rel32_ = (targetblock && targetblock->startpc == pc)
		? (s32)(targetblock->fnptr - (sptr)(jumpptr + 1))
		: (s32)(b->recompiler - (sptr)(jumpptr + 1));
	memcpy(jumpptr, &rel32_, sizeof(s32));
	BlockLinkMap_insert(&b->links, pc, (uptr)jumpptr);
}
